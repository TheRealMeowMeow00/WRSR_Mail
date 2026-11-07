// WRSR Mail - a postal service for Workers & Resources: Soviet Republic
// Copyright (C) 2026 TheRealMeowMeow00
// SPDX-License-Identifier: GPL-3.0-only
//
// Chain simulation: the plugin's real code (waste hook, home need, stop picking,
// dispatcher) runs against fake game objects at the real offsets, on a simulated clock.
// No game code runs. What the fake game does at a stop (driving, loading, the factory) is
// an assumption, calibrated on the RML run of 2026-11-06 05:39. So this finds mistakes in
// the plugin's logic and balance. It proves nothing about how the real game treats a stop.
//
// Build: see build.sh. Usage: sim_chain.exe

#include <windows.h>
#include <stdio.h>
static unsigned long long g_simMs;
static unsigned long long SimTick(void) { return g_simMs; }
#define GetTickCount64 SimTick
#include "../plugins/mail/mail.cpp"
#undef GetTickCount64

#include <math.h>
#include <stdarg.h>
#include <string>
#include <vector>

static const float kFactor = 0.35f;       // class factor of the eletronics template
static const float kTruckNominal = 4.5f;

struct Config
{
    const char* name;
    int   houses, residents, nIn, nOut;
    int   maxStops;        // 0 = the formula
    bool  rowsAtFactory;   // else both rows point at the warehouse
    bool  playerLine;      // warehouse <-> factory carried by a line the player made
    bool  breakAfterFirst; // a multi stop route ends after its first stop (0.4.1 run)
    bool  noOffice;        // no mail trucks anywhere: nobody is served
    bool  wasteOn;
    float factoryTpm;      // tonnes per real minute, an assumption
    int   minutes;
    float perWaste;
};

struct Bld
{
    BYTE  mem[0x1000];
    BYTE  type[0x400];
    std::vector<BYTE> stor;
    std::vector<StoredRes> entries;
    float x, y, accum;
    int   residents;
    bool  house;
};

struct Truck
{
    BYTE  mem[0x2000];
    BYTE  vt[0x9000];
    BYTE  stops[32 * 0x198];
    BYTE* stopB[32];
    void* stopRes[32];
    StoredRes cargo[1];
    bool  active;
    float x, y, tx, ty, wait;
    float routeLen;
    int   trips, loadedTrips;
    double moved, routeSum, idealSum;
    float lastLoad;
};

static BYTE g_res[4][0x340];
static BYTE g_game[0x13000];
static BYTE g_office[0x2000];
static std::vector<Bld*> g_all, g_houses;
static std::vector<Truck*> g_trucks;
static std::vector<BYTE*> g_people;
static std::vector<BYTE*> g_tv, g_tg;
static struct { BYTE* b; int id; int pad; } g_slots[2];
static Config g_cfg;
static int g_refused, g_routes, g_stopRefused;

static void SimLog(const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    if (strstr(buf, "route REFUSED")) ++g_refused;
    else if (strstr(buf, "refused as a stop")) ++g_stopRefused;
    else if (strstr(buf, "mail  route:")) ++g_routes;
}

static Truck* TruckOf(BYTE* v) { return (Truck*)(v - offsetof(Truck, mem)); }

static void FClear(BYTE* v, char)
{
    *(BYTE**)(v + kVehStopBegin) = TruckOf(v)->stops;
    *(BYTE**)(v + kVehStopEnd)   = TruckOf(v)->stops;
    *(int*)(v + kVehStopIndex)   = 0;
}
static char FAdd(BYTE* v, BYTE* b, int, char)
{
    Truck* t = TruckOf(v);
    int n = (int)((*(BYTE**)(v + kVehStopEnd) - t->stops) / kStopStride);
    if (n >= 32) return 0;
    t->stopB[n] = b; t->stopRes[n] = NULL;
    memset(t->stops + n * kStopStride, 0, kStopStride);
    *(BYTE**)(v + kVehStopEnd) += kStopStride;
    return 1;
}
static void FRes(void* inStop, ResPair* p)
{
    // inStop is stop + 8 of the truck whose stop buffer holds it.
    for (Truck* t : g_trucks)
        if ((BYTE*)inStop >= t->stops && (BYTE*)inStop < t->stops + sizeof(t->stops))
            t->stopRes[((BYTE*)inStop - t->stops) / kStopStride] = p->resource;
}
static Bld* BldOf(BYTE* b) { return (Bld*)(b - offsetof(Bld, mem)); }

static float Dist(float ax, float ay, float bx, float by) { return (float)hypot(ax - bx, ay - by); }

static void HeadFor(Truck* t, int idx)
{
    Bld* b = BldOf(t->stopB[idx]);
    t->tx = b->x; t->ty = b->y;
    t->routeLen += Dist(t->x, t->y, t->tx, t->ty);
}

// Buildings no truck can reach: a route with one of them does not start.
static std::vector<BYTE*> g_unreachable;
static int g_unreachableNext;

static char FStart(BYTE* v, char, char, char)
{
    Truck* t = TruckOf(v);
    {
        int stopsNow = (int)((*(BYTE**)(v + kVehStopEnd) - t->stops) / kStopStride);
        for (int i = 0; i < stopsNow; ++i)
            for (BYTE* u : g_unreachable)
                if (t->stopB[i] == u) return 0;
    }
    t->active = true; t->wait = 0; t->routeLen = 0;
    *(BYTE**)(v + kVehParkedAt) = NULL;
    *(int*)(v + kVehStopIndex) = 0;
    ++t->trips;
    // The route as ordered, against visiting the same stops nearest first.
    int n = (int)((*(BYTE**)(v + kVehStopEnd) - t->stops) / kStopStride);
    float px = t->x, py = t->y, len = 0;
    for (int i = 0; i < n; ++i) { Bld* b = BldOf(t->stopB[i]); len += Dist(px, py, b->x, b->y); px = b->x; py = b->y; }
    std::vector<int> left;
    for (int i = 0; i < n; ++i) left.push_back(i);
    px = t->x; py = t->y; float ideal = 0;
    while (!left.empty())
    {
        int bi = 0; float bd = 1e9f;
        for (int k = 0; k < (int)left.size(); ++k)
        {
            Bld* b = BldOf(t->stopB[left[k]]);
            float d = Dist(px, py, b->x, b->y);
            if (d < bd) { bd = d; bi = k; }
        }
        ideal += bd;
        Bld* b = BldOf(t->stopB[left[bi]]); px = b->x; py = b->y;
        left.erase(left.begin() + bi);
    }
    t->routeSum += len; t->idealSum += ideal;
    HeadFor(t, 0);
    return 1;
}
static void FAbort(BYTE* v) { TruckOf(v)->active = false; }
static void FOrigWaste(void*, void*, WasteList*, float) {}
static void FOrigHome(void*, BYTE*, void*, void*) {}

static void InitStor(Bld* b, BYTE* res[], float caps[], int n)
{
    b->stor.assign(n * kStorStride, 0);
    b->entries.assign(n, StoredRes{});
    for (int i = 0; i < n; ++i)
    {
        BYTE* s = b->stor.data() + i * kStorStride;
        b->entries[i].resource = res[i];
        *(StoredRes**)(s + 0) = &b->entries[i];
        *(StoredRes**)(s + 8) = &b->entries[i] + 1;
        *(float*)(s + kStorCapacity) = caps[i];
        *(int*)(s + kStorClass) = 0;
        s[kStorExport] = (i == 0 && res[i] == g_res[0]) ? 1 : 0;
    }
    *(BYTE**)(b->mem + kBldStorageBegin) = b->stor.data();
    *(BYTE**)(b->mem + kBldStorageEnd)   = b->stor.data() + n * kStorStride;
    *(BYTE**)(b->mem + kBuildingTypeOff) = b->type;
}

static Bld* NewBld(float x, float y, int typeNo)
{
    Bld* b = new Bld();
    memset(b->mem, 0, sizeof(b->mem));
    memset(b->type, 0, sizeof(b->type));
    b->x = x; b->y = y; b->accum = 0; b->residents = 0; b->house = false;
    *(int*)(b->type + kTypeNumberOff) = typeNo;
    g_all.push_back(b);
    return b;
}

static float CapOf(BYTE* s) { return Rd<float>(s, kStorCapacity) * kFactor; }

static void SimTruck(Truck* t, float dt)
{
    if (!t->active) return;
    BYTE* v = t->mem;
    if (t->wait > 0) { t->wait -= dt; return; }
    float d = Dist(t->x, t->y, t->tx, t->ty);
    float step = 15.0f * dt;                // calibrated: about 25 s for a one stop trip
    if (d > step)
    {
        t->x += (t->tx - t->x) / d * step;
        t->y += (t->ty - t->y) / d * step;
        return;
    }
    t->x = t->tx; t->y = t->ty;
    int idx = Rd<int>(v, kVehStopIndex);
    int n = (int)((Rd<BYTE*>(v, kVehStopEnd) - t->stops) / kStopStride);
    BYTE* stop = t->stops + idx * kStopStride;
    BYTE* b = t->stopB[idx];
    float cap = kTruckNominal * kFactor;

    if (stop[kStopLoad] && t->stopRes[idx])
    {
        BYTE* res = (BYTE*)t->stopRes[idx];
        StoredRes* st = OwnMail(b, res);
        if (st)
        {
            float room = cap - t->cargo[0].amount;
            float take = st->amount < room ? st->amount : room;
            if (take > 0) { st->amount -= take; t->cargo[0].amount += take; t->cargo[0].resource = res; t->moved += take; }
        }
        t->wait = 3.0f;
    }
    if (stop[kStopUnload] && t->cargo[0].amount > 0)
    {
        BYTE* res = t->cargo[0].resource;
        BYTE* sto = NULL;
        StoredRes* st = OwnMail(b, res, &sto);
        if (st)
        {
            float room = CapOf(sto) - st->amount;
            float give = t->cargo[0].amount < room ? t->cargo[0].amount : room;
            if (give > 0) { st->amount += give; t->cargo[0].amount -= give; }
        }
        t->wait = 3.0f;
    }
    if (idx >= n - 1)
    {
        t->active = false;
        *(BYTE**)(v + kVehParkedAt) = g_office;
        return;
    }
    int next = idx + 1;
    if (g_cfg.breakAfterFirst && n > 3 && idx == 0) next = n - 1;   // the 0.4.1 behaviour
    *(int*)(v + kVehStopIndex) = next;
    HeadFor(t, next);
}

struct Result { double fedShare, happy, rawFullShare, stuckCargo, massErr, callsPerPeriod, rawDepot, doneDepot; };

static void ResetStatics(void)
{
    memset(g_visits, 0, sizeof(g_visits));
    memset(g_wait, 0, sizeof(g_wait));
    memset(g_personSeen, 0, sizeof(g_personSeen));
    memset(g_homes, 0, sizeof(g_homes));
    memset(g_served, 0, sizeof(g_served));
    g_decayDay = 0;
    g_roleTrips[0] = g_roleTrips[1] = 0;
    memset(g_watchCargo, 0, sizeof(g_watchCargo));
    memset(g_officeMark, 0, sizeof(g_officeMark));
    g_homeCount = 0; g_stopForced = 0; g_multiAborts = 0;
    g_gameDay = 0; g_calendarDay = -1; g_calendarYear = -1; g_mailDecayed = 0; g_homeNeutral = 0;
    g_lastPriceMs = 0;
    memset(g_watch, 0, sizeof(g_watch));
    memset(g_probeOffice, 0, sizeof(g_probeOffice));
    g_dumpedN = 0;
    g_mailMade = g_mailUsed = g_homeFed = g_homeUnhappy = 0;
    g_refused = g_routes = g_stopRefused = 0;
    g_simMs = 1000;
}

static void Run(const Config& cfg)
{
    g_cfg = cfg;
    ResetStatics();
    g_all.clear(); g_houses.clear(); g_trucks.clear(); g_people.clear(); g_tv.clear(); g_tg.clear();
    memset(g_office, 0, sizeof(g_office));

    g_maxStops = cfg.maxStops;
    g_mailPerWaste = cfg.perWaste;
    g_mailCapacity = 0.6f; g_mailThreshold = 0.25f;
    g_mailPerPerson = cfg.perWaste * 0.000034f / 0.30f;   // waste in this sim has its own scale

    float caps2[2] = { 0.6f, 0.6f };
    BYTE* rr[2] = { g_res[0], g_res[1] };
    // A town: a grid of houses, about 80 m apart, deterministic.
    int side = (int)ceil(sqrt((double)cfg.houses));
    for (int i = 0; i < cfg.houses; ++i)
    {
        Bld* h = NewBld((i % side) * 80.0f, (i / side) * 80.0f, 2);
        InitStor(h, rr, caps2, 2);
        h->house = true; h->residents = cfg.residents;
        *(float*)(h->mem + kBldQuality) = 0.8f;
        h->accum = (float)(i % 37) / 37.0f;
        g_houses.push_back(h);
    }
    g_unreachable.clear();
    for (int i = 0; i < g_unreachableNext && i < (int)g_houses.size(); ++i) g_unreachable.push_back(g_houses[i]->mem);
    memset(g_badStops, 0, sizeof(g_badStops));
    float edge = side * 80.0f;
    Bld* factory = NewBld(edge + 200, edge / 2, 7);
    float capF = cfg.factoryTpm >= 4.0f ? 40.0f : 20.0f;   // the large office, or the small
    float capsF[2] = { capF, capF };
    InitStor(factory, rr, capsF, 2);
    factory->stor[0 * kStorStride + kStorImport] = 1;
    factory->stor[0 * kStorStride + kStorExport] = 0;
    factory->stor[1 * kStorStride + kStorExport] = 1;
    Bld* wh = NewBld(edge + 100, edge / 2 + 100, 8);
    float capsW[2] = { 300.0f, 300.0f };
    InitStor(wh, rr, capsW, 2);
    wh->stor[0 * kStorStride + kStorExport] = 0;
    Bld* base = (Bld*)nullptr;
    base = NewBld(edge / 2, edge + 100, 0x31);   // the office position only
    (void)base;

    static std::vector<BYTE*> allBuildings;
    allBuildings.clear();
    for (Bld* b : g_all) allBuildings.push_back(b->mem);
    *(BYTE***)(g_game + kGameBuildings)     = allBuildings.data();
    *(BYTE***)(g_game + kGameBuildings + 8) = allBuildings.data() + allBuildings.size();

    Bld* depot =cfg.rowsAtFactory ? factory : wh;
    g_slots[0].b = depot->mem; g_slots[0].id = 0x1A;
    g_slots[1].b = depot->mem; g_slots[1].id = 0x1B;
    static BYTE officeType[0x400];
    *(int*)(officeType + kTypeNumberOff) = kTypeTechOffice;
    *(BYTE**)(g_office + kBuildingTypeOff) = officeType;
    *(void**)(g_office + 0xF78) = &g_slots[0];
    *(void**)(g_office + 0xF80) = &g_slots[2];

    for (Bld* b : g_all) if (b != base) g_tg.push_back(b->mem);
    *(int*)(g_game + kGameDifficulty) = 2;
    *(BYTE***)(g_office + kOffTgtBegin) = g_tg.data();
    *(BYTE***)(g_office + kOffTgtEnd)   = g_tg.data() + g_tg.size();

    // The office's own position stands in for the base stop.
    Bld* officeBld = base;
    (void)officeBld;

    for (int i = 0; i < cfg.nIn + cfg.nOut; ++i)
    {
        Truck* t = new Truck();
        memset(t->mem, 0, sizeof(t->mem));
        memset(t->vt, 0, sizeof(t->vt));
        strcpy((char*)t->vt + kVtName, i < cfg.nIn ? "9000000002/mail_truck_in" : "9000000002/mail_truck_out");
        *(float*)(t->vt + kVtCapacity) = kTruckNominal;
        *(int*)(t->vt + kVtKind) = kVtKindRoad;
        *(BYTE**)(t->mem + kVehRecord) = t->vt;
        *(BYTE**)(t->mem + kVehOwner) = g_office;
        *(BYTE**)(t->mem + kVehParkedAt) = g_office;
        *(BYTE**)(t->mem + kVehStopBegin) = t->stops;
        *(BYTE**)(t->mem + kVehStopEnd) = t->stops;
        *(StoredRes**)(t->mem + kVehCargoBegin) = t->cargo;
        *(StoredRes**)(t->mem + kVehCargoEnd) = t->cargo + 1;
        t->cargo[0].resource = NULL; t->cargo[0].amount = 0;
        t->active = false; t->wait = 0; t->trips = 0; t->moved = 0; t->routeSum = t->idealSum = 0;
        t->x = base->x; t->y = base->y; t->tx = t->x; t->ty = t->y;
        g_trucks.push_back(t);
        g_tv.push_back(t->mem);
    }
    *(BYTE***)(g_office + kOffVehBegin) = g_tv.data();
    *(BYTE***)(g_office + kOffVehEnd)   = g_tv.data() + g_tv.size();

    for (Bld* h : g_houses)
        for (int r = 0; r < h->residents; ++r)
        {
            BYTE* p = (BYTE*)calloc(1, 0x750);
            *(BYTE**)(p + kPersonBuilding) = h->mem;
            *(float*)(p + kPersonHappiness) = 0.7f;
            g_people.push_back(p);
        }

    // The base stop in routes is the office building pointer: give it a position too.
    static Bld officeShell;
    memset(officeShell.mem, 0, sizeof(officeShell.mem));
    officeShell.x = base->x; officeShell.y = base->y;

    const float dt = 1.0f;
    const double wastePerResMin = 0.016 / 110.0;   // calibrated on the 05:39 run
    WasteEntry entry = { (void*)1, 0.0006f, 0.0f };
    WasteList list = { &entry, &entry + 1, &entry + 1 };
    size_t personAt = 0;
    const int perSecond = (int)(g_people.size() / 20) + 1;     // each resident every 20 s

    for (int sec = 0; sec < cfg.minutes * 60; ++sec)
    {
        g_simMs += 1000;
        if (sec % 60 == 0) ++*(int*)(g_game + 0x590);   // assumption: a game day a minute
        MailGameTick(g_game);
        if (cfg.wasteOn)
            for (Bld* h : g_houses)
            {
                h->accum += (float)(h->residents * wastePerResMin / 60.0 / 0.0006);
                while (h->accum >= 1.0f) { h->accum -= 1.0f; AddWasteDetour(NULL, h->mem, &list, 1.0f); }
            }
        for (int i = 0; i < perSecond && !g_people.empty(); ++i)
        {
            CitizenHomeDetour(g_game, g_people[personAt], NULL, NULL);
            personAt = (personAt + 1) % g_people.size();
        }
        // The factory, and the player's line between warehouse and factory if any.
        StoredRes* fr = OwnMail(factory->mem, g_res[0]); StoredRes* fd = OwnMail(factory->mem, g_res[1]);
        float conv = cfg.factoryTpm / 60.0f;
        float room = 40.0f * kFactor - fd->amount;
        if (conv > fr->amount) conv = fr->amount;
        if (conv > room) conv = room;
        if (conv > 0) { fr->amount -= conv; fd->amount += conv; }
        if (cfg.playerLine)
        {
            float rate = 2.0f / 60.0f;
            StoredRes* wr = OwnMail(wh->mem, g_res[0]); StoredRes* wd = OwnMail(wh->mem, g_res[1]);
            float a = wr->amount < rate ? wr->amount : rate;
            float r2 = 40.0f * kFactor - fr->amount;
            if (a > r2) a = r2;
            if (a > 0) { wr->amount -= a; fr->amount += a; }
            float c = fd->amount < rate ? fd->amount : rate;
            if (c > 0) { fd->amount -= c; wd->amount += c; }
        }
        for (Truck* t : g_trucks) SimTruck(t, dt);
        RunMailDispatch(g_office);
        for (Truck* t : g_trucks)
            if (*(float*)(t->mem + kVehCooldown) > 0) *(float*)(t->mem + kVehCooldown) -= dt;
    }

    // Results.
    double onHouses = 0, doneHouses = 0, stuck = 0, depots = 0;
    int rawFull = 0;
    for (Bld* h : g_houses)
    {
        BYTE* s = NULL;
        StoredRes* r = OwnMail(h->mem, g_res[0], &s);
        onHouses += r->amount;
        if (r->amount >= CapOf(s) * 0.99f) ++rawFull;
        doneHouses += OwnMail(h->mem, g_res[1])->amount;
    }
    int inTrips = 0, outTrips = 0; double movedIn = 0, movedOut = 0, routeLen = 0, ideal = 0;
    for (int i = 0; i < (int)g_trucks.size(); ++i)
    {
        Truck* t = g_trucks[i];
        stuck += t->cargo[0].amount;
        if (i < cfg.nIn) { inTrips += t->trips; movedIn += t->moved; } else { outTrips += t->trips; movedOut += t->moved; }
        routeLen += t->routeSum; ideal += t->idealSum;
    }
    double fr_ = OwnMail(factory->mem, g_res[0])->amount, fd_ = OwnMail(factory->mem, g_res[1])->amount;
    double wr_ = OwnMail(wh->mem, g_res[0])->amount, wd_ = OwnMail(wh->mem, g_res[1])->amount;
    depots = fr_ + fd_ + wr_ + wd_;
    double made = g_mailMade / kFixed, used = g_mailUsed / kFixed;
    double lost = g_mailDecayed / kFixed;
    double err = made - used - lost - onHouses - doneHouses - depots - stuck;
    double happy = 0;
    for (BYTE* p : g_people) { happy += Rd<float>(p, kPersonHappiness); free(p); }
    happy = g_people.empty() ? 0 : happy / g_people.size();
    double expectedCalls = (double)g_people.size() * g_gameDay;
    double calls = (double)g_homeFed + (double)g_homeUnhappy + (double)g_homeNeutral;

    printf("--- %s\n", cfg.name);
    printf("    town: %d houses, %d residents, trucks %d + %d, %d min\n",
           cfg.houses, (int)g_people.size(), cfg.nIn, cfg.nOut, cfg.minutes);
    printf("    made %.1f t, used %.1f t. raw in houses %.1f t (%d%% of houses full), done in houses %.1f t\n",
           made, used, onHouses, rawFull * 100 / (cfg.houses ? cfg.houses : 1), doneHouses);
    printf("    factory raw %.1f / done %.1f, warehouse raw %.1f / done %.1f, stuck on trucks %.2f t\n", fr_, fd_, wr_, wd_, stuck);
    printf("    trips %d (%.1f t), refused %d, stops refused %d; route length vs nearest first %.2f\n",
           inTrips + outTrips, movedIn + movedOut, g_refused, g_stopRefused, ideal > 0 ? routeLen / ideal : 0.0);
    printf("    resident days: with mail %.0f%%, without %.0f%%, not served %.0f%%; %.2f x one a day; happiness %.2f (start 0.70, limit 0.80, band 0.60..1.00), multi stop forced off: %s\n",
           calls > 0 ? g_homeFed * 100.0 / calls : 0.0, calls > 0 ? g_homeUnhappy * 100.0 / calls : 0.0,
           calls > 0 ? g_homeNeutral * 100.0 / calls : 0.0,
           expectedCalls > 0 ? calls / expectedCalls : 0.0, happy, g_stopForced ? "yes" : "no");
    printf("    lost to age %.1f t, mass balance error %.3f t\n", lost, err);
}

int main()
{
    static TsmHost host;
    host.log = SimLog;
    H = &host;
    g_logMs = 0;
    strcpy(g_vehicleName, "mail_truck");
    strcpy((char*)g_res[0], "mail_raw");
    strcpy((char*)g_res[1], "mail_done");
    strcpy((char*)g_res[2], "workers");
    strcpy((char*)g_res[3], "eletronics");
    *(int*)(g_res[2] + kResPriceKind) = -1;
    *(int*)(g_res[0] + kResPriceKind) = 1;
    *(int*)(g_res[1] + kResPriceKind) = 1;
    *(int*)(g_res[3] + kResPriceKind) = 1;
    // Illustrative prices: a wage of 5 and electronics at 1000.
    *(float*)(g_res[2] + kResBuyRub) = 5.0f;  *(float*)(g_res[2] + kResBuyUsd) = 1.0f;
    *(float*)(g_res[3] + kResBuyRub) = 1000.0f; *(float*)(g_res[3] + kResBuyUsd) = 200.0f;
    for (int i = 0; i < 4; ++i) { *(float*)(g_res[i] + kResSellMul) = 0.95f; *(float*)(g_res[i] + kResBuyMul) = 1.05f; }
    {
        BYTE** vec = (BYTE**)(g_game + kGameResVector);
        vec[0] = g_res[0]; vec[1] = g_res[0] + 4 * 0x340; vec[2] = vec[1];
    }
    *(float*)(g_res[0] + kResClassFactor) = kFactor;
    *(float*)(g_res[1] + kResClassFactor) = kFactor;
    g_resRaw = g_res[0]; g_resDone = g_res[1];
    g_origAddWaste = FOrigWaste;
    g_origCitizenHome = FOrigHome;
    g_clearRoute = FClear; g_addStop = FAdd; g_addResource = FRes; g_startRoute = FStart; g_abortRoute = FAbort;

    //   name                                          houses res  In Out stops factory line  break  noOff waste  fac  min  per
    Run({"A one stop, one hour",                        25, 110, 4, 1, 1, true,  false, false, false, true, 1.0f, 60, 0.30f});
    Run({"B balanced ini: stops by formula",            25, 110, 4, 1, 0, true,  false, false, false, true, 1.0f, 60, 0.30f});
    Run({"C formula, game ends the route at stop 1",    25, 110, 4, 1, 0, true,  false, true,  false, true, 1.0f, 60, 0.30f});
    Run({"D both rows at warehouse, no line",           25, 110, 4, 1, 0, false, false, false, false, true, 1.0f, 60, 0.30f});
    Run({"E warehouse plus the player's truck line",    25, 110, 4, 1, 0, false, true,  false, false, true, 1.0f, 60, 0.30f});
    Run({"F waste management off",                      25, 110, 4, 1, 0, true,  false, false, false, false,1.0f, 60, 0.30f});
    Run({"G no Out trucks",                             25, 110, 4, 0, 0, true,  false, false, false, true, 1.0f, 60, 0.30f});
    Run({"H factory stopped",                           25, 110, 4, 1, 0, true,  false, false, false, true, 0.0f, 60, 0.30f});
    Run({"I no service at all, the mod just installed", 25, 110, 0, 0, 0, true,  false, false, true,  true, 0.0f, 60, 0.30f});
    Run({"J city: 200 houses, 22000 residents",        200, 110, 8, 4, 0, true,  false, false, false, true, 4.0f, 60, 0.30f});
    Run({"K overproduction: mail_per_waste 2",          25, 110, 4, 1, 0, false, true,  false, false, true, 1.0f, 60, 2.0f});
    Run({"L long game: warehouse, 8 hours",             25, 110, 4, 1, 0, false, true,  false, false, true, 1.0f, 480, 2.0f});
    Run({"M balanced, 8 hours, warehouse and line",     25, 110, 4, 1, 0, false, true,  false, false, true, 1.0f, 480, 0.30f});
    Run({"N balanced, 8 hours, rows at the factory",    25, 110, 4, 1, 0, true,  false, false, false, true, 1.0f, 480, 0.30f});
    Run({"O balanced, 8 hours, two Out trucks",         25, 110, 3, 2, 0, false, true,  false, false, true, 1.0f, 480, 0.30f});
    g_unreachableNext = 3;
    Run({"Q three houses without a road, 8 hours",      25, 110, 4, 1, 0, false, true,  false, false, true, 1.0f, 480, 0.30f});
    g_unreachableNext = 0;
    Run({"P city, 8 hours, large office and warehouse", 200, 110, 8, 4, 0, false, true,  false, false, true, 4.0f, 480, 0.30f});

    // The trade margin, on the sim's illustrative prices.
    ApplyPrices(g_game);
    float buy = Rd<float>(g_resRaw, kResBuyRub), sell = Rd<float>(g_resDone, kResSellRub);
    float wage = Rd<float>(g_res[2], kResBuyRub);
    printf("--- trade, per tonne: buy raw %.1f, process with 40 worker days at %.1f = %.1f, sell %.1f\n",
           buy, wage, 40 * wage, sell);
    printf("    gross %.1f, after wages %.1f = %.0f%% of wages; a 20 t plane earns %.0f after wages and takes the factory 20 days\n",
           sell - buy, sell - buy - 40 * wage, (sell - buy - 40 * wage) * 100 / (40 * wage), 20 * (sell - buy - 40 * wage));
    return 0;
}
