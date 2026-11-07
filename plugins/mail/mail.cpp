// WRSR Mail - a postal service for Workers & Resources: Soviet Republic
// Copyright (C) 2026 TheRealMeowMeow00
// SPDX-License-Identifier: GPL-3.0-only
//
// A postal service built on the structure of the waste system. Mail is generated beside
// waste from the same events, carried by trucks a technical office type building owns,
// processed in a factory and used up by the homes. The waste code itself is only ever
// read: the waste hook forwards its arguments unchanged, and everything this plugin writes
// is a mail storage or a resident's happiness, within limits. Every site is found by a
// byte signature; a mismatch turns that part off and logs why.

#include <atomic>
#include <stdint.h>

#include "../../src/tesmio_plugin.h"

#define MAIL_VERSION "1.0.0"

// The prologue up to the null check of the building argument, with the rel32 of
// that branch wildcarded.
static const short kSigAddWaste[] = {
    0x48,0x8B,0xC4,                         // mov  rax, rsp
    0x4C,0x89,0x40,0x18,                    // mov  [rax+18h], r8
    0x48,0x89,0x50,0x10,                    // mov  [rax+10h], rdx
    0x55,0x56,0x57,                         // push rbp, rsi, rdi
    0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,// push r12, r13, r14, r15
    0x48,0x8D,0x68,0x88,                    // lea  rbp, [rax-78h]
    0x48,0x81,0xEC,0x40,0x01,0x00,0x00,     // sub  rsp, 140h
    0x48,0xC7,0x45,0xE8,0xFE,0xFF,0xFF,0xFF,// mov  qword ptr [rbp-18h], -2
    0x48,0x89,0x58,0x08,                    // mov  [rax+8], rbx
    0x0F,0x29,0x70,0xB8,                    // movaps [rax-48h], xmm6
    0x0F,0x29,0x78,0xA8,                    // movaps [rax-58h], xmm7
    0x44,0x0F,0x29,0x40,0x98,               // movaps [rax-68h], xmm8
    0x44,0x0F,0x29,0x48,0x88,               // movaps [rax-78h], xmm9
    0x44,0x0F,0x29,0x90,0x78,0xFF,0xFF,0xFF,// movaps [rax-88h], xmm10
    0x49,0x8B,0xF8,                         // mov  rdi, r8
    0x4C,0x8B,0xE2,                         // mov  r12, rdx
    0x48,0x8B,0xD9,                         // mov  rbx, rcx
    0x48,0x85,0xD2,                         // test rdx, rdx
    0x0F,0x84,-1,-1,-1,-1,                  // je   out
    0x0F,0x57,0xC0,                         // xorps xmm0, xmm0
};

// mov rax,rsp / two movs / three pushes: 14 bytes, ending on a boundary, nothing
// rip-relative.
static const size_t kStolen = 14;

// Game layout, from the decompilation. Read only.
static const size_t kBuildingTypeOff = 0x318;   // Building -> type descriptor
static const size_t kTypeNumberOff   = 0x360;   // type descriptor -> BUILDINGTYPE_*

struct WasteEntry { void* resource; float amount; float pad; };   // stride 0x10
struct WasteList  { WasteEntry* begin; WasteEntry* end; WasteEntry* cap; };

typedef void (*AddWasteFn)(void* game, void* building, WasteList* list, float factor);
static AddWasteFn g_origAddWaste;

static const int kTypeBuckets = 128;        // the last one collects anything larger
static const int kMaxEntries  = 64;         // far above the real waste type count
static const double kFixed    = 1000000.0;  // counters are micro-tonnes

static std::atomic<uint64_t> g_calls[kTypeBuckets];
static std::atomic<uint64_t> g_waste[kTypeBuckets];
static std::atomic<uint64_t> g_rejected;
static std::atomic<uint64_t> g_lastLog;

static double g_mailPerWaste = 0.25;
static DWORD  g_logMs        = 60000;

// The mail resource records, set once they are published. NULL means no mail in this game.
static BYTE* g_resRaw;
static BYTE* g_resDone;
static void MailFromWaste(BYTE* building, double waste);
static void MailGameTick(void* game);

static int TypeBucket(const void* building)
{
    if (!building) return kTypeBuckets - 1;
    const BYTE* type = *(const BYTE* const*)((const BYTE*)building + kBuildingTypeOff);
    if (!type) return kTypeBuckets - 1;
    int n = *(const int*)(type + kTypeNumberOff);
    return (n >= 0 && n < kTypeBuckets - 1) ? n : kTypeBuckets - 1;
}

// The list holds what this event produced, before the game applies `factor`.
// Mirrors the game's scaling for the normal path: factor applies only when > 0.
static bool MeasureList(const WasteList* list, float factor, double* out)
{
    if (!list || !list->begin || list->end < list->begin) return false;
    ptrdiff_t n = list->end - list->begin;
    if (n > kMaxEntries) return false;

    double sum = 0.0;
    for (ptrdiff_t i = 0; i < n; ++i)
    {
        float a = list->begin[i].amount;
        if (a > 0.0f) sum += a;             // false for NaN as well
    }
    *out = (factor > 0.0f) ? sum * factor : sum;
    return true;
}

static void LogIfDue(void)
{
    if (!g_logMs) return;
    uint64_t now  = GetTickCount64();
    uint64_t last = g_lastLog.load(std::memory_order_relaxed);
    if (now - last < g_logMs) return;
    if (!g_lastLog.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;

    uint64_t calls = 0, waste = 0;
    for (int i = 0; i < kTypeBuckets; ++i)
    {
        calls += g_calls[i].load(std::memory_order_relaxed);
        waste += g_waste[i].load(std::memory_order_relaxed);
    }
    Logf("mail  total: %llu events, waste %.3f t, would-be mail %.3f t, rejected lists %llu",
         (unsigned long long)calls, waste / kFixed, waste / kFixed * g_mailPerWaste,
         (unsigned long long)g_rejected.load(std::memory_order_relaxed));

    for (int i = 0; i < kTypeBuckets; ++i)
    {
        uint64_t c = g_calls[i].load(std::memory_order_relaxed);
        if (!c) continue;
        uint64_t w = g_waste[i].load(std::memory_order_relaxed);
        Logf("mail    type %3d: %llu events, waste %.3f t",
             i, (unsigned long long)c, w / kFixed);
    }
}

// Runs inside the game's waste path, potentially often: no locks, no allocation,
// and nothing but reads of game memory. The original always runs, with the same
// arguments, whatever happened above it.
static void AddWasteDetour(void* game, void* building, WasteList* list, float factor)
{
    double amount = 0.0;
    if (MeasureList(list, factor, &amount))
    {
        int b = TypeBucket(building);
        g_calls[b].fetch_add(1, std::memory_order_relaxed);
        g_waste[b].fetch_add((uint64_t)(amount * kFixed), std::memory_order_relaxed);
    }
    else
    {
        g_rejected.fetch_add(1, std::memory_order_relaxed);
    }

    g_origAddWaste(game, building, list, factor);

    // After the game's own work, on the mail storages only.
    if (amount > 0.0 && building && g_resRaw) MailFromWaste((BYTE*)building, amount);

    LogIfDue();
}

// ---- stage 1b: dispatch prototype ---------------------------------------------
//
// Off unless mail.ini says dispatch = 1. Proves the question the whole design
// hangs on: can a vehicle that is not water, sewage or waste, parked at a
// technical office, be sent on the same kind of one-shot route the game builds
// for its water tankers (FUN_14017c000), with a resource we choose?
//
// The game's own assigners only look at classes 15, 16 and 17, so a vehicle of
// another class sits idle at the office and nothing competes for it. Only
// vehicles whose type name contains vehicle_name are touched. The waste code,
// waste storage and waste vehicles are never read or written here.


// Building
static const size_t kBldStorageBegin = 0x970, kBldStorageEnd = 0x978;
// Technical office
static const size_t kOffVehBegin = 0xC70, kOffVehEnd = 0xC78;
static const size_t kOffTgtBegin = 0xF90, kOffTgtEnd = 0xF98;
// Vehicle
static const size_t kVehRecord = 0x1708, kVehOwner = 0x4F8, kVehParkedAt = 0x4F0;
static const size_t kVehBusyA = 0x528, kVehBusyB = 0xC80, kVehCooldown = 0x508;
static const size_t kVehStopBegin = 0x638, kVehStopEnd = 0x640;
static const size_t kVehStopIndex = 0x698, kVehRouteState = 0x594;
// Vehicle type record
static const size_t kVtName = 0x200, kVtClass = 0x8600, kVtCapacity = 0x8604;
// Storage record, stride 0xE0
static const size_t kStorStride = 0xE0, kStorCapacity = 0x8C, kStorClass = 0x90;
static const size_t kStorImport = 0x84, kStorExport = 0x85;
// Resource record
static const size_t kResClassFactor = 0xCC, kResClassStride = 0x20;
// Route stop, stride 0x198
static const size_t kStopStride = 0x198, kStopLoad = 0x00, kStopWeight = 0x04;
static const size_t kStopResources = 0x08, kStopUnload = 0xC8;

struct StoredRes { BYTE* resource; float amount; float pad; };   // stride 0x10
struct ResPair   { void* resource; uint64_t zero; };

typedef void (*OfficeTickFn)(void* game, BYTE* office);
typedef void (*ClearRouteFn)(BYTE* vehicle, char keep);
typedef char (*AddStopFn)(BYTE* vehicle, BYTE* building, int position, char flag);
typedef void (*AddResourceFn)(void* vectorInStop, ResPair* pair);
typedef char (*StartRouteFn)(BYTE* vehicle, char a, char b, char c);
typedef void (*AbortRouteFn)(BYTE* vehicle);

static OfficeTickFn  g_origOfficeTick;
static ClearRouteFn  g_clearRoute;
static AddStopFn     g_addStop;
static AddResourceFn g_addResource;
static StartRouteFn  g_startRoute;
static AbortRouteFn  g_abortRoute;

static char  g_vehicleName[64] = "covered_b1000";
static char  g_resourceName[64] = "eletronics";
static int   g_maxOrders = 3;
static int   g_maxStops = 0;   // 0 = cargo capacity / the largest mail storage
static int   g_ordersIssued;
static const float kNoWorkCooldown = 30.0f;

template <class T> static T Rd(const void* base, size_t off)
{
    T v;
    memcpy(&v, (const BYTE*)base + off, sizeof(T));
    return v;
}

static int BuildingType(const BYTE* b)
{
    const BYTE* t = Rd<const BYTE*>(b, kBuildingTypeOff);
    return t ? Rd<int>(t, kTypeNumberOff) : -1;
}

// A storage of the wanted class holding this resource, by resource name. The
// name lives inline at the start of the resource record.
static const StoredRes* FindStored(const BYTE* storage, int cls)
{
    if (Rd<int>(storage, kStorClass) != cls) return NULL;
    const StoredRes* it  = Rd<const StoredRes*>(storage, 0);
    const StoredRes* end = Rd<const StoredRes*>(storage, 8);
    if (!it || end < it || end - it > kMaxEntries) return NULL;
    for (; it != end; ++it)
        if (it->resource && strncmp((const char*)it->resource, g_resourceName, 63) == 0)
            return it;
    return NULL;
}

// fill = amount / capacity for this class, as the game computes it.
static double StoredFill(const BYTE* storage, const StoredRes* r, int cls)
{
    double cap = (double)Rd<float>(storage, kStorCapacity) *
                 Rd<float>(r->resource, kResClassFactor + (size_t)cls * kResClassStride);
    return cap > 0.0 ? r->amount / cap : 2.0;
}

// Picks a building to load from and one to unload at, from the office's own
// target list, so the prototype stays inside the office's service area.
static bool PickRoute(const BYTE* office, int cls, BYTE** from, BYTE** to, void** res)
{
    BYTE* const* it  = Rd<BYTE* const*>(office, kOffTgtBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffTgtEnd);
    *from = *to = NULL; *res = NULL;
    double bestFull = 0.0, bestEmpty = 2.0;
    if (!it || end < it) return false;
    for (; it != end; ++it)
    {
        BYTE* b = *it;
        if (!b) continue;
        const BYTE* s  = Rd<const BYTE*>(b, kBldStorageBegin);
        const BYTE* se = Rd<const BYTE*>(b, kBldStorageEnd);
        if (!s || se < s) continue;
        for (; s != se; s += kStorStride)
        {
            const StoredRes* r = FindStored(s, cls);
            if (!r) continue;
            double fill = StoredFill(s, r, cls);
            bool canGive = !(Rd<char>(s, kStorImport) && !Rd<char>(s, kStorExport));
            bool canTake = !Rd<char>(s, kStorExport);
            if (canGive && r->amount > 0.0f && fill > bestFull)
            {
                bestFull = fill; *from = b; *res = r->resource;
            }
            if (canTake && fill < 0.5 && fill < bestEmpty)
            {
                bestEmpty = fill; *to = b;
            }
        }
    }
    return *from && *to && *from != *to;
}

static BYTE* LastStop(BYTE* vehicle)
{
    BYTE* end = Rd<BYTE*>(vehicle, kVehStopEnd);
    return end - kStopStride;
}

// The same sequence as FUN_14017c000, with our resource instead of water.
static bool IssueRoute(BYTE* vehicle, BYTE* from, BYTE* to, BYTE* base, void* res)
{
    g_clearRoute(vehicle, 1);

    bool ok = g_addStop(vehicle, from, -1, 0) != 0;
    if (ok)
    {
        BYTE* s = LastStop(vehicle);
        s[kStopLoad] = 1; s[kStopUnload] = 0;
        *(float*)(s + kStopWeight) = 1.0f;
        ResPair pair = { res, 0 };
        g_addResource(s + kStopResources, &pair);

        ok = g_addStop(vehicle, to, -1, 0) != 0;
    }
    if (ok)
    {
        BYTE* s = LastStop(vehicle);
        s[kStopLoad] = 0; s[kStopUnload] = 1;
        *(float*)(s + kStopWeight) = 1.0f;

        ok = g_addStop(vehicle, base, -1, 0) != 0;
    }
    if (ok)
    {
        BYTE* s = LastStop(vehicle);
        s[kStopLoad] = 0; s[kStopUnload] = 0;
        ok = g_startRoute(vehicle, 0, 0, 0) != 0;
    }
    if (!ok)
    {
        g_abortRoute(vehicle);
        g_clearRoute(vehicle, 1);
        return false;
    }
    *(float*)(vehicle + kVehCooldown) = 0.0f;
    return true;
}

static bool VehicleSelected(const BYTE* vehicle)
{
    const BYTE* vt = Rd<const BYTE*>(vehicle, kVehRecord);
    return vt && strstr((const char*)vt + kVtName, g_vehicleName) != NULL;
}

// A vehicle with stops in its route is working, even between two stops, when the
// busy pointers are null. Without this the dispatcher clears a live route, the start
// then fails and the loop repeats every tick (seen in the 20:06 run).
static int VehicleStops(const BYTE* vehicle)
{
    const BYTE* begin = Rd<const BYTE*>(vehicle, kVehStopBegin);
    const BYTE* end   = Rd<const BYTE*>(vehicle, kVehStopEnd);
    if (!begin || !end || end < begin) return 0;
    ptrdiff_t n = (end - begin) / (ptrdiff_t)kStopStride;
    return (n > 0 && n < 64) ? (int)n : 0;
}

static bool VehicleOnRoute(const BYTE* vehicle)
{
    int n = VehicleStops(vehicle);
    if (n <= 0) return false;
    int idx = Rd<int>(vehicle, kVehStopIndex);
    // The last stop of every route this plugin builds is the office base. A truck
    // sitting there with the route still in its list is done, not busy. Treating it
    // as busy left the In trucks frozen on routes loaded from a save (seen at 20:43,
    // three In with route 3 stops idx 2 state 2 and no new orders).
    return idx >= 0 && idx < n - 1;
}

// Our own cooldown. vehicle+0x508 belongs to the game and the game resets it, so a
// refusal written there does not hold. This table is ours alone.
struct TruckWait { const void* truck; uint64_t until; int lastStops; int stopLimit; };
static TruckWait g_wait[64];

static void WaitTruck(const BYTE* truck, DWORD ms)
{
    uint64_t until = GetTickCount64() + ms;
    for (int i = 0; i < 64; ++i)
    {
        if (g_wait[i].truck == truck) { g_wait[i].until = until; return; }
        if (!g_wait[i].truck) { g_wait[i].truck = truck; g_wait[i].until = until; return; }
    }
    g_wait[0].truck = truck;
    g_wait[0].until = until;
    g_wait[0].lastStops = 0;
}

static TruckWait* TruckRecord(const BYTE* truck)
{
    for (int i = 0; i < 64; ++i)
    {
        if (g_wait[i].truck == truck) return &g_wait[i];
        if (!g_wait[i].truck) { g_wait[i].truck = truck; return &g_wait[i]; }
    }
    return NULL;
}

static bool TruckWaiting(const BYTE* truck)
{
    for (int i = 0; i < 64; ++i)
    {
        if (g_wait[i].truck == truck) return GetTickCount64() < g_wait[i].until;
        if (!g_wait[i].truck) return false;
    }
    return false;
}

// Parked at its own office, on no route, no wait of ours, cooldown run out.
static bool VehicleFree(const BYTE* vehicle, const BYTE* office)
{
    return Rd<const BYTE*>(vehicle, kVehOwner) == office &&
           Rd<const BYTE*>(vehicle, kVehParkedAt) == office &&
           Rd<const void*>(vehicle, kVehBusyA) == NULL &&
           Rd<const void*>(vehicle, kVehBusyB) == NULL &&
           !VehicleOnRoute(vehicle) &&
           !TruckWaiting(vehicle) &&
           Rd<float>(vehicle, kVehCooldown) <= 0.0f;
}

static const int kProbeOffices = 32;
static const BYTE* g_probeOffice[kProbeOffices];
static int         g_probeVehicles[kProbeOffices];

static void ProbeOffice(const BYTE* office)
{
    BYTE* const* it  = Rd<BYTE* const*>(office, kOffVehBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffVehEnd);
    int n = (it && end >= it) ? (int)(end - it) : 0;
    int slot = -1;
    for (int i = 0; i < kProbeOffices; ++i)
    {
        if (g_probeOffice[i] == office) { slot = i; break; }
        if (!g_probeOffice[i]) { g_probeOffice[i] = office; g_probeVehicles[i] = -1; slot = i; break; }
    }
    if (slot < 0 || g_probeVehicles[slot] == n) return;
    g_probeVehicles[slot] = n;

    BYTE* const* tb = Rd<BYTE* const*>(office, kOffTgtBegin);
    BYTE* const* te = Rd<BYTE* const*>(office, kOffTgtEnd);
    Logf("mail  office %p: %d vehicles, %d targets", office, n, (tb && te >= tb) ? (int)(te - tb) : 0);
    for (int i = 0; i < n && i < 16; ++i)
    {
        const BYTE* v  = it[i];
        const BYTE* vt = v ? Rd<const BYTE*>(v, kVehRecord) : NULL;
        if (!vt) continue;
        Logf("mail    vehicle %p: '%.60s' class %d, selected %d, free %d, route %d stops idx %d state %d",
             v, (const char*)vt + kVtName, Rd<int>(vt, kVtClass),
             VehicleSelected(v) ? 1 : 0, VehicleFree(v, office) ? 1 : 0,
             VehicleStops(v), Rd<int>(v, kVehStopIndex), Rd<int>(v, kVehRouteState));
    }
}

// Runs once for each office, after the game's own assigners.
static void RunDispatch(BYTE* office)
{
    ProbeOffice(office);
    if (g_ordersIssued >= g_maxOrders) return;

    BYTE* const* it  = Rd<BYTE* const*>(office, kOffVehBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffVehEnd);
    if (!it || end < it) return;
    for (; it != end; ++it)
    {
        BYTE* v = *it;
        if (!v || !VehicleSelected(v) || !VehicleFree(v, office)) continue;

        int cls = Rd<int>(Rd<const BYTE*>(v, kVehRecord), kVtClass);
        BYTE* from; BYTE* to; void* res;
        if (!PickRoute(office, cls, &from, &to, &res))
        {
            *(float*)(v + kVehCooldown) = kNoWorkCooldown;
            Logf("mail  vehicle %p: no source and target with '%s' class %d in this office's area",
                 v, g_resourceName, cls);
            continue;
        }
        bool ok = IssueRoute(v, from, to, office, res);
        if (ok) ++g_ordersIssued;
        else *(float*)(v + kVehCooldown) = kNoWorkCooldown;
        Logf("mail  order %s: vehicle %p, from %p (type %d) to %p (type %d), '%s', issued %d/%d",
             ok ? "started" : "REFUSED", v, from, BuildingType(from), to, BuildingType(to),
             g_resourceName, g_ordersIssued, g_maxOrders);
        if (g_ordersIssued >= g_maxOrders) return;
    }
}

static void RunMailDispatch(BYTE* office);

static void OfficeTickDetour(void* game, BYTE* office)
{
    g_origOfficeTick(game, office);
    if (g_resRaw) MailGameTick(game);
    if (g_resRaw) RunMailDispatch(office);
    else RunDispatch(office);
}

// FUN_1403e2900 decides if a vehicle type fits a building: the purchase window lists
// only what passes. The selected vehicle fits a technical office as well, everything
// else gets the game's own answer.
static const int kTypeTechOffice = 0x31;

// A mail service is a technical office whose type names exactly two sources. The game's own
// offices list water, sewage and every waste, so they never match.
static const size_t kOffRowBegin = 0xF78, kOffRowEnd = 0xF80, kOffRowStride = 0x10;
static bool IsMailOffice(const BYTE* b)
{
    if (!b || BuildingType(b) != kTypeTechOffice) return false;
    const BYTE* it  = Rd<const BYTE*>(b, kOffRowBegin);
    const BYTE* end = Rd<const BYTE*>(b, kOffRowEnd);
    return it && end > it && (size_t)(end - it) == 2 * kOffRowStride;
}

// Any covered road truck serves mail, a modder's or the game's. It carries what its route
// asks for, so there is no truck for each kind of mail. FUN_1403E2900 tests the vehicle's
// kind at +0x294 against 1 for the office's own road vehicles (read in code).
static const size_t kVtKind = 0x294;
static const int kVtKindRoad = 1, kVtClassCovered = 0;
static bool CoveredTruckType(const BYTE* vt)
{
    return vt && Rd<int>(vt, kVtKind) == kVtKindRoad && Rd<int>(vt, kVtClass) == kVtClassCovered;
}

// A virtual vehicle class. The game has no room for a new class (docs/vehicle-classes.md), so a
// mail truck stays a covered truck to it, and mail_vehicles says which ones a mail service may
// buy: names or parts of names, split by spaces or commas. Empty means every covered truck. It
// only limits buying: a truck already in a service keeps working when the list changes.
static char g_mailVehicles[2048];

static bool ListedMailVehicle(const char* name)
{
    if (!g_mailVehicles[0]) return true;
    const char* p = g_mailVehicles;
    while (*p)
    {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* e = p;
        while (*e && *e != ' ' && *e != ',' && *e != '\t') ++e;
        size_t len = (size_t)(e - p);
        if (len)
            for (const char* n = name; *n; ++n)
                if (strncmp(n, p, len) == 0) return true;
        p = e;
    }
    return false;
}

typedef unsigned long long (*VehicleFitsFn)(void* game, BYTE* building, BYTE* vehicleType,
                                            char a, void* b);
static VehicleFitsFn g_origVehicleFits;
static bool g_fitLogged;

static bool SelectedVehicleType(const BYTE* vt)
{
    return vt && strstr((const char*)vt + kVtName, g_vehicleName) != NULL;
}

static unsigned long long VehicleFitsDetour(void* game, BYTE* building, BYTE* vehicleType,
                                            char a, void* b)
{
    // With the resources on, a mail service takes covered trucks and nothing else, and an
    // ordinary office keeps the game's own answer, so it is offered no covered trucks.
    if (g_resRaw && building && vehicleType && IsMailOffice(building))
    {
        if (!CoveredTruckType(vehicleType)) return 0;
        if (!ListedMailVehicle((const char*)vehicleType + kVtName)) return 0;
        if (!g_fitLogged)
        {
            g_fitLogged = true;
            Logf("mail  buy: covered trucks accepted for mail service %p, first '%.60s'", building,
                 (const char*)vehicleType + kVtName);
        }
        return 1;
    }
    if (!g_resRaw && building && SelectedVehicleType(vehicleType) && BuildingType(building) == kTypeTechOffice)
    {
        if (!g_fitLogged)
        {
            g_fitLogged = true;
            Logf("mail  buy: '%.60s' accepted for technical office %p", (const char*)vehicleType + kVtName, building);
        }
        return 1;
    }
    return g_origVehicleFits(game, building, vehicleType, a, b);
}

// ---- choosing the mail warehouse in the service's window -----------------------
//
// The rows of "Utilities management" are waste rows: FUN_1401d51a0, which runs while the
// player picks a building, accepts only what FUN_14019cbf0 says has a waste storage. A
// covered mail warehouse is refused. So while that picker runs, on its own thread, a
// building with a storage dedicated to mail is accepted as well. The one other caller
// of FUN_14019cbf0, building creation, and every other building keep the game's answer,
// so waste is not affected.

typedef void (*AssignPickFn)(void* game);
typedef unsigned long long (*WasteAcceptFn)(void* game, BYTE* building, void* res, void* d, char e);
static AssignPickFn  g_origAssignPick;
static WasteAcceptFn g_origWasteAccept;
static DWORD g_pickThread;
static bool  g_pickLogged;

// Only a dedicated storage has mail as its single entry. A plain covered storage has the game's
// own list of goods, without mail (seen on customs houses). Every waste producing building has
// small dedicated ones of its own, so a destination also has to be large.
static const float kMailBuildingMinCapacity = 5.0f;
static bool IsMailBuilding(const BYTE* b)
{
    const BYTE* s  = Rd<const BYTE*>(b, kBldStorageBegin);
    const BYTE* se = Rd<const BYTE*>(b, kBldStorageEnd);
    if (!s || se < s) return false;
    for (; s != se; s += kStorStride)
    {
        const StoredRes* it  = Rd<const StoredRes*>(s, 0);
        const StoredRes* end = Rd<const StoredRes*>(s, 8);
        if (!it || end - it != 1 || !it->resource) continue;
        if (Rd<float>(s, kStorCapacity) < kMailBuildingMinCapacity) continue;
        const char* name = (const char*)it->resource;
        if (strcmp(name, "mail_raw") == 0 || strcmp(name, "mail_done") == 0) return true;
    }
    return false;
}

static void AssignPickDetour(void* game)
{
    g_pickThread = GetCurrentThreadId();
    g_origAssignPick(game);
    g_pickThread = 0;
}

static unsigned long long WasteAcceptDetour(void* game, BYTE* building, void* res, void* d, char e)
{
    if (building && g_pickThread == GetCurrentThreadId() && IsMailBuilding(building))
    {
        if (!g_pickLogged)
        {
            g_pickLogged = true;
            Logf("mail  pick: building %p accepted as a mail destination", building);
        }
        return 1;
    }
    return g_origWasteAccept(game, building, res, d, e);
}

// ---- mail in every building ------------------------------------------------------
//
// The end of the building parser pushes the default waste bin onto the type's storage
// list with FUN_1400cc2f0, which deep copies the record. Two more pushes of the same
// record, its fields changed for the call and restored after, give the type a mail_raw
// storage (export) and a mail_done storage (import). The bin itself is pushed first and
// unchanged, and these are covered class with the default kind, so the waste code, which
// looks for class 17 and kinds 5 and 6, skips them.

typedef void* (*PushStorageFn)(void* vec, BYTE* rec);
static PushStorageFn g_origPushStorage;
static const BYTE*   g_binPushReturn;
static int           g_mailTypes;
static float         g_mailCapacity  = 0.2f;
static float         g_mailThreshold = 0.25f;

static void PushOne(void* vec, BYTE* rec, BYTE* res, bool exportIt)
{
    StoredRes entry = { res, 0.0f, 0.0f };
    StoredRes zero  = { res, 0.0f, 0.0f };
    StoredRes* v[6] = { &entry, &entry + 1, &entry + 1, &zero, &zero + 1, &zero + 1 };
    memcpy(rec, v, sizeof(v));
    rec[kStorImport] = exportIt ? 0 : 1;
    rec[kStorExport] = exportIt ? 1 : 0;
    *(int*)(rec + 0x88) = -1;
    *(float*)(rec + kStorCapacity) = g_mailCapacity;
    *(int*)(rec + kStorClass) = 0;
    g_origPushStorage(vec, rec);
}

// A post office or a warehouse declares big mail storages of its own, and those are already
// in the list when the bin is pushed; a small pair on top made it ask for mail for itself.
// The parser's type is a local of its frame, so the type number is not at hand here, and the
// mail service is dealt with where it runs: see IsMailOffice in MailFromWaste, StopAllowed
// and DrawWindowDetour.
static bool DeclaresMail(const void* vec)
{
    const BYTE* s  = Rd<const BYTE*>(vec, 0);
    const BYTE* se = Rd<const BYTE*>(vec, 8);
    for (; s && s < se; s += kStorStride)
    {
        const StoredRes* it  = Rd<const StoredRes*>(s, 0);
        const StoredRes* end = Rd<const StoredRes*>(s, 8);
        if (it && end - it == 1 && (it->resource == g_resRaw || it->resource == g_resDone)) return true;
    }
    return false;
}

static void* PushStorageDetour(void* vec, BYTE* rec)
{
    // Only the default bin. Buildings that declare their own bin in the ini, container stands
    // above all, stay without mail: with mail on them the stands broke the game (2026-11-07).
    bool atBin = (const BYTE*)__builtin_return_address(0) == g_binPushReturn;
    void* r = g_origPushStorage(vec, rec);
    if (atBin && g_resRaw && g_resDone && !DeclaresMail(vec))
    {
        const BYTE* oldBegin = Rd<const BYTE*>(vec, 0);
        bool intoList = (const BYTE*)r >= oldBegin && (const BYTE*)r < Rd<const BYTE*>(vec, 8);
        size_t offset = intoList ? (size_t)((const BYTE*)r - oldBegin) : 0;
        BYTE saved[0xE0];
        memcpy(saved, rec, sizeof(saved));
        PushOne(vec, rec, g_resRaw, true);
        PushOne(vec, rec, g_resDone, false);
        memcpy(rec, saved, sizeof(saved));
        // The list may have moved, and a caller may hold on to what it was given.
        if (intoList) r = Rd<BYTE*>(vec, 0) + offset;
        if (g_mailTypes++ == 0) Logf("mail  storages: first building type got its mail storages");
    }
    return r;
}

static StoredRes* OwnMail(BYTE* b, BYTE* res, BYTE** storage = NULL)
{
    BYTE* s  = Rd<BYTE*>(b, kBldStorageBegin);
    BYTE* se = Rd<BYTE*>(b, kBldStorageEnd);
    if (!s || se < s) return NULL;
    for (; s != se; s += kStorStride)
    {
        StoredRes* it  = Rd<StoredRes*>(s, 0);
        StoredRes* end = Rd<StoredRes*>(s, 8);
        if (it && end - it == 1 && it->resource == res)
        {
            if (storage) *storage = s;
            return it;
        }
    }
    return NULL;
}

static uint64_t g_mailMade, g_mailUsed, g_mailDecayed;   // micro-tonnes
static float    g_mailPerPerson = 0.0003f;   // tonnes per resident per day
static float    g_mailRawFactor = 1.0f;      // raw mail follows use, against the reference year
static bool IsHome(const BYTE* b);

// Raw mail comes from every building that makes waste, so it follows the life of the
// building and with waste management off the chain simply rests. Processed mail is used up
// per resident, see HomeNeedTick.
static void MailFromWaste(BYTE* b, double waste)
{
    if (IsMailOffice(b)) return;
    BYTE* s = NULL;
    StoredRes* raw = OwnMail(b, g_resRaw, &s);
    if (raw && Rd<float>(s, kStorCapacity) < kMailBuildingMinCapacity)
    {
        float add = (float)(waste * g_mailPerWaste * g_mailRawFactor);
        float cap = Rd<float>(s, kStorCapacity) * Rd<float>(raw->resource, kResClassFactor);
        if (raw->amount + add > cap) add = cap - raw->amount;
        if (add > 0.0f) { raw->amount += add; g_mailMade += (uint64_t)(add * kFixed); }
    }
}

// ---- what mail does for the people at home -------------------------------------------
//
// The hook is FUN_14083a6d0, the demand processing of a person, whose second argument is
// the person and whose person+0x20 is the building. The game raises happiness (the first
// status float, person+0xD8) only up to a limit: building+0xEB4 times 1.5, 1.2 or 1.0 by
// difficulty, FUN_140834490. That function cannot be hooked at its entry (its first
// instruction is rip-relative) and the game recomputes, saves and displays the field, so
// neither is touched. Once per game day each resident moves:
//
//   * processed mail in the house: the resident uses mail_use_per_person_day of it and goes
//     up by mail_happy_bonus, to the limit + mail_happy_range;
//   * none, and a mail service covers the house: down by mail_happy_debuff, to the limit -
//     mail_happy_range;
//   * none, and no service covers it, or no mail exists in this game at all: nothing.
//     Nobody is punished for a service that does not exist, or for mail that cannot exist
//     because waste management is off.
//
// When the game itself raises a person who is above the limit it puts the person back on
// the limit, so the top of the range is the part most likely to differ in the real game.

static const size_t kPersonBuilding  = 0x20;
static const size_t kPersonHappiness = 0xD8;
static const size_t kPersonNoDemands = 0xC8;    // foreign workers, tourists
static const size_t kPersonKind      = 0x71C;   // 0 citizen, 1 and 2 tourists
static const size_t kBldQuality      = 0xEB4;
static const size_t kGameDifficulty  = 0x5C8;
static const int    kTypeNoQuality   = 0x32;    // FUN_140834490 ignores the field here

static float    g_happyBonus  = 0.02f;
static float    g_happyDebuff = 0.03f;
static float    g_happyRange  = 0.25f;
static uint64_t g_homeFed, g_homeUnhappy, g_homeNeutral;

// A mail day is a calendar day: game+0x590 is the day of the year, +0x594 the year, advanced
// by the game when its timer at +0x59C reaches 60 (read in code). A mod that stretches the
// date stretches this too. Reading the engine timer instead ran 1.7 to 2.6 times fast in
// the runs of 2026-11-06, and nobody needs it with the date this close at hand.
static uint32_t g_gameDay;
static int      g_calendarDay = -1, g_calendarYear = -1;

// People write letters in 1920 and order parcels in 2020. mail_use_by_year gives the grams a
// resident uses per day at some years, straight lines between them, flat outside. Raw mail
// grows along, or the late game has nothing to process. mail_per_waste is meant for the use
// of mail_use_reference grams. Empty table: the amounts stay as they are.
struct EraPoint { int year; float grams; };
static const int kMaxEras = 16;
static EraPoint g_era[kMaxEras];
static int   g_eraCount;
static float g_eraReference = 300.0f;

static float GramsInYear(int year, int day)
{
    if (g_eraCount == 0) return 0.0f;
    float t = (float)year + (float)day / 365.0f;
    if (t <= (float)g_era[0].year) return g_era[0].grams;
    for (int i = 1; i < g_eraCount; ++i)
    {
        if (t > (float)g_era[i].year) continue;
        float span = (float)(g_era[i].year - g_era[i - 1].year);
        float k = (t - (float)g_era[i - 1].year) / span;
        return g_era[i - 1].grams + (g_era[i].grams - g_era[i - 1].grams) * k;
    }
    return g_era[g_eraCount - 1].grams;
}

static void ApplyEra(int year, int day)
{
    float grams = GramsInYear(year, day);
    if (grams <= 0.0f) return;
    g_mailPerPerson = grams / 1000000.0f;
    g_mailRawFactor = g_eraReference > 0.0f ? grams / g_eraReference : 1.0f;
}

// "1920:100 1950:300": rising years only, anything else ends the table where it is.
static int ParseEras(const char* text)
{
    g_eraCount = 0;
    while (text && *text && g_eraCount < kMaxEras)
    {
        int year = 0, used = 0;
        float grams = 0.0f;
        if (sscanf(text, " %d:%f%n", &year, &grams, &used) != 2 || grams <= 0.0f) break;
        if (g_eraCount && year <= g_era[g_eraCount - 1].year) break;
        g_era[g_eraCount].year = year;
        g_era[g_eraCount].grams = grams;
        ++g_eraCount;
        text += used;
    }
    return g_eraCount;
}

static void TickClock(const void* game)
{
    int day  = Rd<int>(game, 0x590);
    int year = Rd<int>(game, 0x594);
    if (day == g_calendarDay && year == g_calendarYear) return;
    bool first = g_calendarDay < 0;
    g_calendarDay = day;
    g_calendarYear = year;
    ApplyEra(year, day);
    if (!first) ++g_gameDay;
}

// Sets of pointers, compared and never followed, so a demolished building costs nothing.
// Open addressing with a short probe: a full neighbourhood means "not recorded", never
// another entry's answer. The old single slot table made two people who shared a slot
// reset each other's timer and pay on every tick.
static const int kProbe = 16;
struct PersonSeen { const void* person; uint32_t day; };
static PersonSeen g_personSeen[1 << 18];
static const BYTE* g_homes[1 << 15];
static int g_homeCount;
struct Served { const BYTE* b; uint64_t at; };
static Served g_served[1 << 15];
static const DWORD kServedMs = 120000;

static uint32_t PtrHash(const void* p)
{
    uint64_t v = (uint64_t)(uintptr_t)p >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(v >> 32);
}

static bool IsHome(const BYTE* b)
{
    uint32_t h = PtrHash(b);
    for (int i = 0; i < kProbe; ++i)
    {
        const BYTE* e = g_homes[(h + i) & 0x7FFF];
        if (e == b) return true;
        if (!e) return false;
    }
    return false;
}

static void MarkHome(const BYTE* b)
{
    uint32_t h = PtrHash(b);
    for (int i = 0; i < kProbe; ++i)
    {
        const BYTE*& e = g_homes[(h + i) & 0x7FFF];
        if (e == b) return;
        if (!e) { e = b; ++g_homeCount; return; }
    }
}

static void MarkServed(const BYTE* b, uint64_t now)
{
    uint32_t h = PtrHash(b);
    Served* free_ = NULL;
    for (int i = 0; i < kProbe; ++i)
    {
        Served& e = g_served[(h + i) & 0x7FFF];
        if (e.b == b) { e.at = now; return; }
        if (!e.b || now - e.at >= kServedMs) { if (!free_) free_ = &e; }
    }
    if (free_) { free_->b = b; free_->at = now; }
}

static bool IsServed(const BYTE* b, uint64_t now)
{
    uint32_t h = PtrHash(b);
    for (int i = 0; i < kProbe; ++i)
    {
        const Served& e = g_served[(h + i) & 0x7FFF];
        if (e.b == b) return now - e.at < kServedMs;
    }
    return false;
}

// True once per game day for each person. A person who finds no free slot waits.
static bool FirstTodayFor(const void* person)
{
    uint32_t h = PtrHash(person);
    PersonSeen* free_ = NULL;
    for (int i = 0; i < kProbe; ++i)
    {
        PersonSeen& e = g_personSeen[(h + i) & 0x3FFFF];
        if (e.person == person)
        {
            if (e.day == g_gameDay) return false;
            e.day = g_gameDay;
            return true;
        }
        if (!e.person || e.day + 2 < g_gameDay) { if (!free_) free_ = &e; }
    }
    if (!free_) return false;
    free_->person = person;
    free_->day = g_gameDay;
    return true;
}

typedef void (*CitizenHomeFn)(void* a, BYTE* person, void* c, void* d);
static CitizenHomeFn g_origCitizenHome;

static void HomeNeedTick(const void* game, BYTE* person)
{
    if (!person || !game || !g_resDone) return;
    const BYTE* home = Rd<const BYTE*>(person, kPersonBuilding);
    if (!home || Rd<int>(person, kPersonKind) != 0 || Rd<int>(person, kPersonNoDemands) != 0) return;
    if (BuildingType(home) == kTypeNoQuality) return;

    BYTE* storage = NULL;
    StoredRes* done = OwnMail((BYTE*)home, g_resDone, &storage);
    if (!done || !storage) return;
    // The small per-building storage. The warehouse and the factory have big ones.
    if (Rd<float>(storage, kStorCapacity) >= kMailBuildingMinCapacity) return;

    MarkHome(home);
    if (!FirstTodayFor(person)) return;

    int difficulty = Rd<int>(game, kGameDifficulty);
    float limit = Rd<float>(home, kBldQuality) * (difficulty == 0 ? 1.5f : difficulty == 1 ? 1.2f : 1.0f);
    if (limit <= 0.0f) return;

    float h = Rd<float>(person, kPersonHappiness);
    if (done->amount > 0.0f)
    {
        float top = limit * (1.0f + g_happyRange);
        if (h < top) { h += g_happyBonus; if (h > top) h = top; }
        ++g_homeFed;
        float use = g_mailPerPerson < done->amount ? g_mailPerPerson : done->amount;
        done->amount -= use;
        g_mailUsed += (uint64_t)(use * kFixed);
    }
    else if (g_mailMade > 0 && IsServed(home, GetTickCount64()))
    {
        float bottom = limit * (1.0f - g_happyRange);
        if (h > bottom) { h -= g_happyDebuff; if (h < bottom) h = bottom; }
        ++g_homeUnhappy;
    }
    else
    {
        ++g_homeNeutral;
        return;
    }
    if (h < 0.0f) h = 0.0f;
    *(float*)(person + kPersonHappiness) = h;
}

// What the citizen's window says about mail: 100 mail in the house, 0 none and a service
// covers it, -1 mail is not this person's concern. The same three cases as above.
static int MailNeedPercent(const BYTE* person)
{
    if (!person || !g_resDone) return -1;
    const BYTE* home = Rd<const BYTE*>(person, kPersonBuilding);
    if (!home || Rd<int>(person, kPersonKind) != 0 || Rd<int>(person, kPersonNoDemands) != 0) return -1;
    if (BuildingType(home) == kTypeNoQuality) return -1;
    BYTE* storage = NULL;
    StoredRes* done = OwnMail((BYTE*)home, g_resDone, &storage);
    if (!done || !storage || Rd<float>(storage, kStorCapacity) >= kMailBuildingMinCapacity) return -1;
    if (done->amount > 0.0f) return 100;
    return g_mailMade > 0 && IsServed(home, GetTickCount64()) ? 0 : -1;
}

static void CitizenHomeDetour(void* a, BYTE* person, void* c, void* d)
{
    g_origCitizenHome(a, person, c, d);
    if (a && g_resRaw) MailGameTick(a);
    HomeNeedTick(a, person);
}

// ---- the dispatcher ------------------------------------------------------------------
//
// The first row of the service's window is where collected mail goes, the second is where
// processed mail comes from. Trucks named _in collect, the others deliver. Each trip is
// one cargo: a round of stops, the warehouse, back to base.

static const int kSlotDest = 0, kSlotSource = 1;   // first and second row
static const int kMaxStops = 24;   // a truck's capacity over the smallest building
static const float kIdleCooldown = 15.0f;
static const DWORD kRevisitMs = 60000;

struct Visit { const BYTE* b; uint64_t at; };
static Visit g_visits[1024];

static Visit* VisitSlot(const BYTE* b)
{
    return &g_visits[(((uintptr_t)b) >> 4) % 1024];
}
static bool VisitedRecently(const BYTE* b)
{
    const Visit* v = VisitSlot(b);
    return v->b == b && GetTickCount64() - v->at < kRevisitMs;
}

static BYTE* SlotBuilding(const BYTE* office, int id)
{
    const BYTE* it  = Rd<const BYTE*>(office, 0xF78);
    const BYTE* end = Rd<const BYTE*>(office, 0xF80);
    if (!it || end < it) return NULL;
    // By position, in the order of the window: the row ids depend on the ini tokens.
    if ((end - it) / 0x10 <= id) return NULL;
    return Rd<BYTE*>(it + (size_t)id * 0x10, 0);
}

// Up to the number of buildings whose mail storage fills the truck, best first.
// Collect: fullest raw. Deliver: emptiest done. The limit is cargo capacity over the
// largest mail storage among the candidates; the class factor is applied to both, so
// a 4.5 t truck with 0.6 t storages asks for 7 stops, with 0.35 t ones for 13.
// max_stops > 0 in the ini overrides the formula with a fixed number.
// Delivery goes where people live: nobody at a school uses the mail it would hold. The
// homes are learned from the people, so until one is known every building qualifies.
// The game finds a path for the whole route when it starts, and ends a route on a stop it
// cannot reach. Stops go fullest first and nobody ever empties a building no truck gets to,
// so one of those heads every route and stops all the trucks: seen 2026-11-07, 20 of 22
// routes refused. Such a building is left out for a while, roads get built. Each try has a
// price: a route that ends on the way sends its truck straight back to base, which the player
// sees as a teleport (2026-11-07). So the pause grows, 5, 15, then 30 minutes, and a building
// a truck did reach starts over.
static const DWORD kBadStopMs[3] = { 300000, 900000, 1800000 };
struct BadStop { const BYTE* b; uint64_t until; int strikes; };
static BadStop g_badStops[64];

static bool IsBadStop(const BYTE* b)
{
    uint64_t now = GetTickCount64();
    for (auto& e : g_badStops)
        if (e.b == b) return now < e.until;
    return false;
}

static void MarkBadStop(const BYTE* b, const char* why)
{
    BadStop* slot = &g_badStops[0];
    for (auto& e : g_badStops)
    {
        if (e.b == b) { slot = &e; break; }
        if (e.until < slot->until) slot = &e;
    }
    int strikes = slot->b == b ? slot->strikes : 0;
    DWORD pause = kBadStopMs[strikes < 3 ? strikes : 2];
    slot->b = b;
    slot->until = GetTickCount64() + pause;
    slot->strikes = strikes + 1;
    Logf("mail  stops: building %p type %d left out for %u minutes, %s", (const void*)b, BuildingType(b),
         (unsigned)(pause / 60000), why);
}

static void ClearBadStop(const BYTE* b)
{
    for (auto& e : g_badStops)
        if (e.b == b) { e.b = NULL; e.until = 0; e.strikes = 0; return; }
}

static bool StopAllowed(const BYTE* b, bool collect)
{
    if (!b || IsMailBuilding(b) || IsMailOffice(b) || VisitedRecently(b) || IsBadStop(b)) return false;
    if (!collect && g_homeCount > 0 && !IsHome(b)) return false;
    return true;
}

static float g_mailMinBatch = 0.25f;    // share of a truck worth a trip

// Set when a multi stop route keeps ending with the cargo still on board.
static int g_stopForced;
static int g_multiAborts;

static const int kAutoStopsMax = 12;

static int PickStops(const BYTE* office, BYTE* res, bool collect, BYTE** out, float cargoCap)
{
    BYTE* const* it  = Rd<BYTE* const*>(office, kOffTgtBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffTgtEnd);
    if (!it || end < it) return 0;

    // First pass: the largest mail storage among the candidates.
    float maxCap = 0.0f;
    for (BYTE* const* p = it; p != end; ++p)
    {
        BYTE* b = *p;
        if (!StopAllowed(b, collect)) continue;
        BYTE* s = NULL;
        StoredRes* r = OwnMail(b, res, &s);
        if (!r) continue;
        float cap = Rd<float>(s, kStorCapacity) * Rd<float>(r->resource, kResClassFactor);
        if (cap <= 0.0f) continue;
        float fill = r->amount / cap;
        if (collect) { if (fill < g_mailThreshold) continue; }
        else         { if (fill > 1.0f - g_mailThreshold) continue; }
        if (cap > maxCap) maxCap = cap;
    }
    if (maxCap <= 0.0f) return 0;

    // Houses differ in size, so without a fixed number the stops are taken as long as they add
    // to the load, see the end. This is only the most a route can have.
    int limit = g_maxStops > 0 ? g_maxStops : kAutoStopsMax;
    if (limit > kMaxStops) limit = kMaxStops;
    if (g_stopForced > 0 && limit > g_stopForced) limit = g_stopForced;

    float score[kMaxStops];
    int n = 0;
    for (BYTE* const* p = it; p != end; ++p)
    {
        BYTE* b = *p;
        if (!StopAllowed(b, collect)) continue;
        BYTE* s = NULL;
        StoredRes* r = OwnMail(b, res, &s);
        if (!r) continue;
        float cap = Rd<float>(s, kStorCapacity) * Rd<float>(r->resource, kResClassFactor);
        if (cap <= 0.0f) continue;
        float fill = r->amount / cap;
        float sc;
        if (collect) { if (fill < g_mailThreshold) continue; sc = fill; }
        else         { if (fill > 1.0f - g_mailThreshold) continue; sc = 1.0f - fill; }
        int at;
        if (n < limit) at = n++;
        else if (score[limit - 1] < sc) at = limit - 1;
        else continue;
        while (at > 0 && score[at - 1] < sc) { score[at] = score[at - 1]; out[at] = out[at - 1]; --at; }
        score[at] = sc; out[at] = b;
    }
    if (g_maxStops <= 0)
    {
        float load = 0.0f;
        int keep = 0;
        while (keep < n && load < cargoCap)
        {
            BYTE* st = NULL;
            StoredRes* r = OwnMail(out[keep], res, &st);
            ++keep;
            if (!r) continue;
            float cap = Rd<float>(st, kStorCapacity) * Rd<float>(r->resource, kResClassFactor);
            load += collect ? r->amount : cap - r->amount;
        }
        n = keep;
    }
    return n;
}

static bool AddPlainStop(BYTE* v, BYTE* b, bool load, bool unload, void* res)
{
    if (!g_addStop(v, b, -1, 0)) return false;
    BYTE* s = LastStop(v);
    s[kStopLoad] = load ? 1 : 0;
    s[kStopUnload] = unload ? 1 : 0;
    *(float*)(s + kStopWeight) = 1.0f;
    if (load && res)
    {
        ResPair pair = { res, 0 };
        g_addResource(s + kStopResources, &pair);
    }
    return true;
}

// ---- what the truck actually does ------------------------------------------------
//
// The log used to say only that a route had started, so "never arrived" and "arrived
// and loaded nothing" read the same. A route is watched until the truck is free again:
// the mail amount at each stop and the truck's cargo, both at issue and at the end.
// Storage records are dumped once, so a mail storage this plugin pushed onto a type can
// be compared field by field with the warehouse's own $STORAGE_SPECIAL one. Read only.

static const size_t kVehCargoBegin = 0x3B8, kVehCargoEnd = 0x3C0;   // {resource, amount}
static const size_t kStorSetBegin  = 0x18;                         // per-resource settings
static const size_t kStorKind      = 0x88;

static const char* ResName(const void* res)
{
    static char buf[40];
    if (!res) return "-";
    const char* c = (const char*)res;
    int i = 0;
    for (; i < (int)sizeof(buf) - 1; ++i)
    {
        if (!c[i]) break;
        if (c[i] < 0x20 || c[i] > 0x7E) return "?";
        buf[i] = c[i];
    }
    buf[i] = 0;
    return i ? buf : "?";
}

static int   VectorCount(const void* v)
{
    const StoredRes* it  = Rd<const StoredRes*>(v, 0);
    const StoredRes* end = Rd<const StoredRes*>(v, 8);
    return (it && end >= it && end - it <= kMaxEntries) ? (int)(end - it) : -1;
}

static float StorageAmount(const void* s)
{
    if (!s) return -1.0f;
    const StoredRes* it  = Rd<const StoredRes*>(s, 0);
    const StoredRes* end = Rd<const StoredRes*>(s, 8);
    if (!it || end < it || end - it > kMaxEntries) return -1.0f;
    float sum = 0.0f;
    for (; it != end; ++it) sum += it->amount;
    return sum;
}

static float CargoAmount(const void* v)
{
    if (!v) return -1.0f;
    const StoredRes* it  = Rd<const StoredRes*>(v, kVehCargoBegin);
    const StoredRes* end = Rd<const StoredRes*>(v, kVehCargoEnd);
    if (!it || end < it || end - it > kMaxEntries) return -1.0f;
    float sum = 0.0f;
    for (; it != end; ++it) sum += it->amount;
    return sum;
}

static const BYTE* g_dumped[12];
static int         g_dumpedN;

static void DumpStorages(const BYTE* b, const char* why)
{
    if (!b || !g_resRaw) return;
    for (int i = 0; i < g_dumpedN; ++i) if (g_dumped[i] == b) return;
    if (g_dumpedN >= (int)(sizeof(g_dumped) / sizeof(g_dumped[0]))) return;
    g_dumped[g_dumpedN++] = b;

    const BYTE* s  = Rd<const BYTE*>(b, kBldStorageBegin);
    const BYTE* se = Rd<const BYTE*>(b, kBldStorageEnd);
    int n = (s && se >= s) ? (int)((se - s) / (ptrdiff_t)kStorStride) : -1;
    Logf("mail  storage dump (%s): building %p type %d, %d storages, mail_raw class factor %.3f",
         why, b, BuildingType(b), n, (double)Rd<float>(g_resRaw, kResClassFactor));
    if (n <= 0 || n > 32) return;
    for (int i = 0; i < n; ++i, s += kStorStride)
    {
        int en = VectorCount(s);
        Logf("mail    [%d] class %d kind %d capacity %.3f import %d export %d entries %d",
             i, Rd<int>(s, kStorClass), Rd<int>(s, kStorKind), (double)Rd<float>(s, kStorCapacity),
             Rd<char>(s, kStorImport) ? 1 : 0, Rd<char>(s, kStorExport) ? 1 : 0, en);
        const StoredRes* it = Rd<const StoredRes*>(s, 0);
        for (int k = 0; k < en && k < 8; ++k)
            Logf("mail      content '%s': amount %.4f t, other %.4f",
                 ResName(it[k].resource), (double)it[k].amount, (double)Rd<float>(&it[k], 0xC));
        const StoredRes* set  = Rd<const StoredRes*>(s, kStorSetBegin);
        const StoredRes* sete = Rd<const StoredRes*>(s, kStorSetBegin + 8);
        int sn = (set && sete >= set && sete - set <= kMaxEntries) ? (int)(sete - set) : -1;
        for (int k = 0; k < sn && k < 8; ++k)
            Logf("mail      setting '%s': share %.4f enabled %d",
                 ResName(set[k].resource), (double)set[k].amount, (int)Rd<char>(&set[k], 0xC));
    }
}

struct WatchStop { BYTE* building; BYTE* storage; float atIssue; float last; };
struct Watch
{
    const BYTE* office;
    BYTE*    truck;
    BYTE*    depot;
    const BYTE* depotBuilding;
    uint64_t issued, lastLine;
    int      n;
    bool     collect;
    WatchStop stop[kMaxStops];
    // Where the truck last did anything, for TruckStuck.
    bool     seen;
    int      stillStop;
    float    stillCargo;
    float    stillPos[3];
    uint64_t lastTick, stillMs;
};
static Watch g_watch[32];

static void WatchFree(int i)
{
    memset(&g_watch[i], 0, sizeof(g_watch[i]));
}

// Whether a raw pointer is still in a list the game owns right now. Only an entry found
// there is ever followed, so a sold truck or a demolished building is simply dropped.
static bool InList(const BYTE* office, size_t begin, size_t end, const void* p)
{
    BYTE* const* it = Rd<BYTE* const*>(office, begin);
    BYTE* const* e  = Rd<BYTE* const*>(office, end);
    for (; it && it < e; ++it) if (*it == p) return true;
    return false;
}

// A truck that gains far more at one stop than the stops hold: what is it carrying, and what
// does that building have? Seen 2026-11-06 at a shop, 4.2 t on board and no mail gone.
static float g_watchCargo[sizeof(g_watch) / sizeof(g_watch[0])];

static void LogCargoJump(const BYTE* v, int slot, float cargo, const BYTE* building)
{
    float was = g_watchCargo[slot];
    g_watchCargo[slot] = cargo;
    if (cargo - was < 0.5f) return;
    Logf("mail  cargo: truck %p %.3f -> %.3f t at stop %d", v, (double)was, (double)cargo, Rd<int>(v, kVehStopIndex));
    const StoredRes* it  = Rd<const StoredRes*>(v, kVehCargoBegin);
    const StoredRes* end = Rd<const StoredRes*>(v, kVehCargoEnd);
    if (it && end >= it && end - it <= kMaxEntries)
        for (; it != end; ++it)
            Logf("mail    on board '%s': %.3f t", ResName(it->resource), (double)it->amount);
    if (building) DumpStorages(building, "cargo jump");
}

// A truck can stop for good in the middle of a route: 2026-11-07, 05:43 and 05:59, every truck
// of two offices, the stop number frozen, the cargo gone to 0, the route still in its list, so
// to the dispatcher it stays busy. The player's way out was the depot button of each truck.
// Why it happens is not known yet: the line below logs what is needed to find out.
//
// A truck counts as stuck when for mail_stuck_seconds of running game it did not move, did not
// change its stop and did not change its cargo. Only ticks count, so a paused game adds nothing.
typedef float* (*NodePositionFn)(void* node, float* out);       // C3D_NODE::GetPosition, by value
static NodePositionFn g_nodePosition;
static const size_t kVehNode = 0xD60;
static uint64_t g_stuckMs = 90000;

static bool TruckStuck(Watch& w, BYTE* v, int here, float cargo, uint64_t now)
{
    if (!g_nodePosition || !g_stuckMs) return false;
    float p[3];
    g_nodePosition(v + kVehNode, p);
    float dx = p[0] - w.stillPos[0], dy = p[1] - w.stillPos[1], dz = p[2] - w.stillPos[2];
    float dc = cargo - w.stillCargo;
    uint64_t step = w.lastTick && now > w.lastTick ? now - w.lastTick : 0;
    w.lastTick = now;
    if (step > 2000) step = 2000;
    if (!w.seen || here != w.stillStop || dc < -0.0005f || dc > 0.0005f || dx * dx + dy * dy + dz * dz > 0.25f)
    {
        w.seen = true;
        w.stillStop = here;
        w.stillCargo = cargo;
        memcpy(w.stillPos, p, sizeof(p));
        w.stillMs = 0;
        return false;
    }
    w.stillMs += step;
    return w.stillMs >= g_stuckMs;
}

// The game's own depot button, FUN_1403fbdc0, with the arguments the button passes. It clears
// the route, puts the truck back at its office and, for a service of this type, keeps the cargo.
typedef void (*SendHomeFn)(void* game, BYTE* v, char, char, char, char);
static SendHomeFn g_sendHome;
static void* g_gameNow;

// Called on the owner's tick: follows the stops of every route this plugin started.
static void WatchTrucks(const BYTE* office)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < (int)(sizeof(g_watch) / sizeof(g_watch[0])); ++i)
    {
        Watch& w = g_watch[i];
        if (!w.truck || w.office != office) continue;
        if (!InList(office, kOffVehBegin, kOffVehEnd, w.truck)) { WatchFree(i); continue; }
        if (w.depot && SlotBuilding(office, kSlotDest) != w.depotBuilding) { w.depot = NULL; }
        for (int j = 0; j < w.n; ++j)
            if (w.stop[j].storage && !InList(office, kOffTgtBegin, kOffTgtEnd, w.stop[j].building))
                w.stop[j].storage = NULL;
        BYTE* v = w.truck;
        float cargo = CargoAmount(v);
        int here = Rd<int>(v, kVehStopIndex);
        LogCargoJump(v, i, cargo, here >= 0 && here < w.n && w.stop[here].storage ? w.stop[here].building : NULL);
        for (int j = 0; j < w.n; ++j)
        {
            float a = StorageAmount(w.stop[j].storage);
            float d = a - w.stop[j].last;
            if (d < -0.0005f || d > 0.0005f)
            {
                Logf("mail  watch: truck %p stop %d (type %d): mail %.3f -> %.3f t, cargo %.3f t, stop %d",
                     w.truck, j, w.stop[j].storage ? BuildingType(w.stop[j].building) : -1, (double)w.stop[j].last,
                     (double)a, (double)cargo, Rd<int>(v, kVehStopIndex));
                w.stop[j].last = a;
            }
        }
        bool busy = Rd<const void*>(v, kVehBusyA) != NULL || Rd<const void*>(v, kVehBusyB) != NULL ||
                    VehicleOnRoute(v);
        if (!busy)
        {
            Logf("mail  watch: truck %p free after %.1f s, cargo %.3f t, depot mail %.3f t",
                 w.truck, (now - w.issued) / 1000.0, (double)cargo,
                 (double)StorageAmount(w.depot));
            for (int j = 0; j < w.n; ++j)
                Logf("mail    stop %d (type %d): mail %.3f t now, %.3f t at start",
                     j, w.stop[j].storage ? BuildingType(w.stop[j].building) : -1,
                     (double)StorageAmount(w.stop[j].storage), (double)w.stop[j].atIssue);
            for (int j = 0; j < w.n; ++j)
                if (w.stop[j].storage && StorageAmount(w.stop[j].storage) < w.stop[j].atIssue - 0.0005f)
                    ClearBadStop(w.stop[j].building);
            // Mail still on board: the route ended early, and on the first stop not reached.
            if (w.collect && cargo > 0.001f)
                for (int j = 0; j < w.n; ++j)
                {
                    if (!w.stop[j].storage || StorageAmount(w.stop[j].storage) < w.stop[j].atIssue - 0.0005f) continue;
                    MarkBadStop(w.stop[j].building, "the route ended before it");
                    break;
                }
            WatchFree(i);
            continue;
        }
        if (now - w.lastLine >= 15000)
        {
            w.lastLine = now;
            Logf("mail  watch: truck %p still busy %.0f s, stop %d/%d, cargo %.3f t",
                 w.truck, (now - w.issued) / 1000.0, Rd<int>(v, kVehStopIndex), w.n, (double)cargo);
        }
        if (g_sendHome && g_gameNow && TruckStuck(w, v, here, cargo, now))
        {
            const BYTE* at = here >= 0 && here < w.n ? w.stop[here].building : NULL;
            Logf("mail  stuck: truck %p, stop %d/%d of a route of %d, building %p type %d, cargo %.3f t, %.0f s on the way; "
                 "parked %d, at %p, busy %p %p %p, state %d %d, flags %d %d %d %d, recalled",
                 v, here, w.n, VehicleStops(v), at, at ? BuildingType(at) : -1, (double)cargo, (now - w.issued) / 1000.0,
                 Rd<const BYTE*>(v, kVehParkedAt) == office ? 1 : 0, Rd<const void*>(v, kVehParkedAt),
                 Rd<const void*>(v, kVehBusyA), Rd<const void*>(v, 0x530), Rd<const void*>(v, kVehBusyB),
                 Rd<int>(v, kVehRouteState), Rd<int>(v, 0x50C),
                 Rd<char>(v, 0xD39), Rd<char>(v, 0xD3A), Rd<char>(v, 0xD3D), Rd<char>(v, 0xD3F));
            if (at) MarkBadStop(at, "a truck got stuck on the way to it");
            g_sendHome(g_gameNow, v, 0, 0, 1, 1);
            WaitTruck(v, 15000);
            WatchFree(i);
            continue;
        }
        // Without the two calls above nothing can be done about it, and the slot is wanted back.
        if ((!g_sendHome || !g_nodePosition || !g_stuckMs) && now - w.issued > 240000)
        {
            Logf("mail  watch: truck %p did not finish within 240 s, giving up on it", w.truck);
            WatchFree(i);
        }
    }
}

static void WatchRoute(const BYTE* office, BYTE* v, BYTE* depot, BYTE* const* stops, int n, BYTE* res)
{
    Watch* w = NULL;
    for (int i = 0; i < (int)(sizeof(g_watch) / sizeof(g_watch[0])); ++i)
        if (g_watch[i].truck == v) { w = &g_watch[i]; break; }
        else if (!g_watch[i].truck && !w) w = &g_watch[i];
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->truck = v;
    w->collect = res == g_resRaw;
    w->office = office;
    w->depotBuilding = depot;
    BYTE* depotStorage = NULL;
    if (depot) OwnMail(depot, res, &depotStorage);
    w->depot = depotStorage;          // the storage, so the free line shows real tonnes
    w->issued = w->lastLine = GetTickCount64();
    w->n = n > kMaxStops ? kMaxStops : n;
    for (int i = 0; i < w->n; ++i)
    {
        BYTE* s = NULL;
        w->stop[i].building = stops[i];
        OwnMail(stops[i], res, &s);
        w->stop[i].storage = s;
        w->stop[i].atIssue = StorageAmount(s);
        w->stop[i].last = w->stop[i].atIssue;
    }
}

// What is on board decides the trip: a truck never mixes the two kinds of mail. 0 nothing,
// 1 raw, 2 processed, 3 something else, which this plugin has no route for.
static int CargoKind(const BYTE* v)
{
    const StoredRes* it  = Rd<const StoredRes*>(v, kVehCargoBegin);
    const StoredRes* end = Rd<const StoredRes*>(v, kVehCargoEnd);
    if (!it || end < it || end - it > kMaxEntries) return 0;
    for (; it != end; ++it)
    {
        if (it->amount <= 0.001f) continue;
        if (it->resource == g_resRaw) return 1;
        if (it->resource == g_resDone) return 2;
        return 3;
    }
    return 0;
}

// How much a trip of this kind is worth to an empty truck, in tonnes. Below zero: no such
// trip can start now. A building about to run full or dry counts as a whole load.
static float TripWorth(BYTE* office, BYTE* res, bool collect, float cargoCap)
{
    BYTE* depot = SlotBuilding(office, collect ? kSlotDest : kSlotSource);
    if (!depot) return -1.0f;
    BYTE* depotStorage = NULL;
    StoredRes* stock = OwnMail(depot, res, &depotStorage);
    if (!stock) return -1.0f;
    float factor = Rd<float>(res, kResClassFactor);
    if (collect && Rd<float>(depotStorage, kStorCapacity) * factor - stock->amount < 0.05f) return -1.0f;
    if (!collect && stock->amount <= 0.0f) return -1.0f;
    BYTE* stops[kMaxStops];
    int n = PickStops(office, res, collect, stops, cargoCap);
    float worth = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        BYTE* st = NULL;
        StoredRes* r = OwnMail(stops[i], res, &st);
        if (!r || !st) continue;
        float c = Rd<float>(st, kStorCapacity) * factor;
        worth += collect ? r->amount : c - r->amount;
        if (collect ? r->amount >= 0.75f * c : r->amount <= 0.0f) worth += cargoCap;
    }
    return n > 0 ? worth : -1.0f;
}

// An empty truck takes whichever trip is worth making, and when both are it takes turns.
// Going by worth alone starved delivery: many buildings make raw mail and few houses take the
// processed kind, so collecting was always worth more while raw mail kept coming.
static uint64_t g_roleTrips[2];   // collected, delivered

static bool ChooseCollect(float rawWorth, float doneWorth, float need)
{
    bool canRaw  = rawWorth >= need;
    bool canDone = doneWorth >= need;
    if (canRaw && canDone) return g_roleTrips[0] <= g_roleTrips[1];
    if (canRaw) return true;
    if (canDone) return false;
    return rawWorth >= doneWorth;
}

static void RunTruck(BYTE* v, BYTE* office)
{
    const BYTE* vt = Rd<const BYTE*>(v, kVehRecord);
    int onBoard = CargoKind(v);
    if (onBoard == 3) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 30000); return; }
    bool collect = onBoard == 1;
    if (onBoard == 0)
    {
        float capacity = vt ? Rd<float>(vt, kVtCapacity) : 0.0f;
        float rawWorth  = TripWorth(office, g_resRaw, true, capacity * Rd<float>(g_resRaw, kResClassFactor));
        float doneWorth = TripWorth(office, g_resDone, false, capacity * Rd<float>(g_resDone, kResClassFactor));
        collect = ChooseCollect(rawWorth, doneWorth, g_mailMinBatch * capacity * Rd<float>(g_resRaw, kResClassFactor));
    }
    BYTE* depot = SlotBuilding(office, collect ? kSlotDest : kSlotSource);
    BYTE* res = collect ? g_resRaw : g_resDone;
    if (!depot) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 15000); return; }

    float cargoCap = vt ? Rd<float>(vt, kVtCapacity) * Rd<float>(res, kResClassFactor) : 0.0f;
    float cargo = CargoAmount(v);
    if (cargo < 0.0f) cargo = 0.0f;

    BYTE* depotStorage = NULL;
    StoredRes* stock = OwnMail(depot, res, &depotStorage);
    if (!stock) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 15000); return; }

    // A collecting truck that still has most of a load goes to unload first. Collecting
    // more would only drive it around full, which is what a route ended early leaves.
    bool unloadOnly = collect && cargoCap > 0.0f && cargo >= 0.5f * cargoCap;
    if (collect)
    {
        // Nowhere to put it: wait, instead of collecting for a full depot.
        float room = Rd<float>(depotStorage, kStorCapacity) * Rd<float>(res, kResClassFactor) - stock->amount;
        if (room < 0.05f) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 30000); return; }
    }
    TruckWait* rec = TruckRecord(v);
    if (unloadOnly && rec && rec->lastStops > 1 && ++g_multiAborts >= 3 && g_stopForced == 0)
    {
        g_stopForced = 1;
        Logf("mail  stops: multi stop routes keep ending with the cargo on board, one stop per route from now on");
    }

    // A delivering truck plans for what it can actually bring.
    bool loadFirst = !collect && !(cargoCap > 0.0f && cargo >= 0.5f * cargoCap);
    float planCap = cargoCap;
    if (!collect)
    {
        float have = cargo + (loadFirst ? stock->amount : 0.0f);
        if (have <= 0.0f) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 15000); return; }
        if (have < planCap) planCap = have;
    }

    BYTE* stops[kMaxStops];
    int n = unloadOnly ? 0 : PickStops(office, res, collect, stops, planCap);
    if (rec && rec->stopLimit > 0 && n > rec->stopLimit) n = rec->stopLimit;
    if (n == 0 && !unloadOnly) { *(float*)(v + kVehCooldown) = kIdleCooldown; WaitTruck(v, 15000); return; }

    // No trips for a handful of letters: a truck leaves for a quarter of a load, or when a
    // building is nearly full, or empty when delivering. That last part keeps a small town,
    // which never adds up to a quarter of a truck, served all the same.
    if (!unloadOnly)
    {
        float batch = 0.0f;
        bool urgent = false;
        for (int i = 0; i < n; ++i)
        {
            BYTE* st = NULL;
            StoredRes* r = OwnMail(stops[i], res, &st);
            if (!r || !st) continue;
            float c = Rd<float>(st, kStorCapacity) * Rd<float>(res, kResClassFactor);
            batch += collect ? r->amount : c - r->amount;
            if (collect ? r->amount >= 0.75f * c : r->amount <= 0.0f) urgent = true;
        }
        if (!urgent && batch < g_mailMinBatch * cargoCap)
        {
            *(float*)(v + kVehCooldown) = kIdleCooldown;
            WaitTruck(v, 15000);
            return;
        }
    }

    g_clearRoute(v, 1);
    bool ok = true;
    const char* why = NULL;
    BYTE* addedStops[kMaxStops];
    int added = 0;
    if (loadFirst && !AddPlainStop(v, depot, true, false, res)) { ok = false; why = "warehouse stop"; }
    for (int i = 0; ok && i < n; ++i)
    {
        // A building the game refuses as a stop is skipped, and the log says so.
        if (AddPlainStop(v, stops[i], collect, !collect, res)) addedStops[added++] = stops[i];
        else Logf("mail  route: building %p (type %d) refused as a stop", stops[i], BuildingType(stops[i]));
    }
    if (ok && added == 0 && !unloadOnly) { ok = false; why = "no stop accepted"; }
    if (ok && collect && !AddPlainStop(v, depot, false, true, NULL)) { ok = false; why = "warehouse stop"; }
    if (ok && !AddPlainStop(v, office, false, false, NULL)) { ok = false; why = "base stop"; }
    if (ok && !g_startRoute(v, 0, 0, 0)) { ok = false; why = "start"; }
    if (!ok)
    {
        int noPath = Rd<char>(v, 0xD3F), noStop = Rd<char>(v, 0xD3D), other = Rd<char>(v, 0xD39);
        g_abortRoute(v);
        g_clearRoute(v, 1);
        Logf("mail  route REFUSED: truck %p, %s, %d stops, at %s, flags %d %d %d", v,
             collect ? "collect" : "deliver", n, why ? why : "?", noPath, noStop, other);
        // Halve the route until it starts. A single stop that does not is the bad one.
        bool started = why && strcmp(why, "start") == 0;
        if (started && rec && added > 1)
        {
            rec->stopLimit = (added + 1) / 2;
            *(float*)(v + kVehCooldown) = 2.0f;
            WaitTruck(v, 2000);
            return;
        }
        if (started && added == 1)
        {
            MarkBadStop(addedStops[0], "no path for a truck");
            if (rec) rec->stopLimit = 0;
            *(float*)(v + kVehCooldown) = 2.0f;
            WaitTruck(v, 2000);
            return;
        }
        *(float*)(v + kVehCooldown) = kNoWorkCooldown;
        WaitTruck(v, 30000);
        return;
    }
    uint64_t now = GetTickCount64();
    for (int i = 0; i < added; ++i) { Visit* w = VisitSlot(addedStops[i]); w->b = addedStops[i]; w->at = now; }
    if (rec) { rec->lastStops = added; rec->stopLimit = 0; }
    ++g_roleTrips[collect ? 0 : 1];
    *(float*)(v + kVehCooldown) = 0.0f;
    Logf("mail  route: truck %p %s%s, %d stops, cargo %.3f t; mail made %.3f t, used %.3f t",
         v, unloadOnly ? "unloads first, " : "", collect ? "collects" : "delivers", added, (double)CargoAmount(v),
         g_mailMade / kFixed, g_mailUsed / kFixed);
    for (int i = 0; i < added; ++i)
    {
        BYTE* s = NULL;
        StoredRes* r = OwnMail(addedStops[i], res, &s);
        Logf("mail    stop %d: building %p type %d, %s %.3f t in a storage of %.3f t",
             i, (void*)addedStops[i], BuildingType(addedStops[i]), ResName(res),
             r ? (double)r->amount : -1.0, s ? (double)Rd<float>(s, kStorCapacity) : -1.0);
    }
    if (collect)
    {
        DumpStorages(depot, "warehouse");
        for (int i = 0; i < added; ++i) DumpStorages(addedStops[i], "collect stop");
    }
    WatchRoute(office, v, depot, addedStops, added, res);
}

// Mail is urgent: left in storage it goes out of date, on a vehicle it does not. Once per
// game day every storage that merely keeps mail loses mail_decay_per_day of it, whether or
// not a mail service uses that warehouse, and whether it is a mail warehouse or a plain
// covered one holding other goods too. Factories are exempt: their storages only pass mail
// through and carry the import or export flag, a warehouse's carry neither. So are the small
// stores of houses, customs and terminals, which hold too little to hoard in, by size as
// well, so a misread flag cannot age a whole town's mail. Stock therefore has a natural
// ceiling, surplus flow / rate, and hoarding does not pay.
//
// The list is the one the game's building tick FUN_140139A70 walks. Both callers of this
// run on that thread from FUN_14030C790, the office tick inside that walk and the citizen
// tick after it, so nothing changes the list under this one. Decompiled, not run.
static const size_t kGameBuildings = 0x11B08;   // begin, end
static const ptrdiff_t kMaxBuildings = 1 << 20; // past this the field is not what it seems
static float    g_decayPerDay = 0.0025f;
static float    g_decayTopAmount;           // the fullest store since the last log line
static int      g_decayTopType = -1;
static uint32_t g_decayDay;

static void DecayBuilding(const BYTE* b)
{
    BYTE* s  = Rd<BYTE*>(b, kBldStorageBegin);
    BYTE* se = Rd<BYTE*>(b, kBldStorageEnd);
    for (; s && s < se; s += kStorStride)
    {
        if (Rd<char>(s, kStorImport) || Rd<char>(s, kStorExport)) continue;
        if (Rd<float>(s, kStorCapacity) < kMailBuildingMinCapacity) continue;
        StoredRes* it  = Rd<StoredRes*>(s, 0);
        StoredRes* end = Rd<StoredRes*>(s, 8);
        for (; it && it < end; ++it)
        {
            if (it->resource != g_resRaw && it->resource != g_resDone) continue;
            if (it->amount > g_decayTopAmount)
            {
                g_decayTopAmount = it->amount;
                g_decayTopType   = BuildingType(b);
            }
            float loss = it->amount * g_decayPerDay;
            it->amount -= loss;
            g_mailDecayed += (uint64_t)(loss * kFixed);
        }
    }
}

// A house's mail storages follow its size, so that any house keeps about the same days of
// mail. The size is the capacity of its passenger storage, $STORAGE RESOURCE_TRANSPORT_PASSANGER
// or $STORAGE_LIVING_AUTO. Each building has its own storage records, so this touches one house.
// A new house gets its size on the next day. Mail above a lowered capacity just stays.
static const int   kBldTypeLiving = 2, kStorClassPassengers = 7;
static const float kHomeMailMinTonnes = 0.05f;
static float g_homeKgPerResident = 1.5f;
static int   g_homesSized;

static float HomeMailCapacity(float residents, float factor)
{
    float tonnes = residents * g_homeKgPerResident / 1000.0f;
    if (tonnes < kHomeMailMinTonnes) tonnes = kHomeMailMinTonnes;
    float cap = tonnes / factor;
    // From kMailBuildingMinCapacity on a store counts as a depot and ages.
    return cap < kMailBuildingMinCapacity - 0.1f ? cap : kMailBuildingMinCapacity - 0.1f;
}

static void SizeHomeMail(BYTE* b)
{
    if (g_homeKgPerResident <= 0.0f || !g_resRaw || !g_resDone || BuildingType(b) != kBldTypeLiving) return;
    float residents = 0.0f;
    BYTE* s  = Rd<BYTE*>(b, kBldStorageBegin);
    BYTE* se = Rd<BYTE*>(b, kBldStorageEnd);
    for (; s && s < se; s += kStorStride)
        if (Rd<int>(s, kStorClass) == kStorClassPassengers) { residents = Rd<float>(s, kStorCapacity); break; }
    if (residents <= 0.0f) return;
    BYTE* const res[2] = { g_resRaw, g_resDone };
    bool sized = false;
    for (int i = 0; i < 2; ++i)
    {
        BYTE* st = NULL;
        float factor = Rd<float>(res[i], kResClassFactor);
        if (factor <= 0.0f || !OwnMail(b, res[i], &st) || !st) continue;
        *(float*)(st + kStorCapacity) = HomeMailCapacity(residents, factor);
        sized = true;
    }
    if (sized) ++g_homesSized;
}

// Planes and ships trade through customs houses too, type 20: the road ones and two hidden
// ones, zoll_air_west and zoll_air_soviet. Type 31 is the power connection, one class 9 store.
static const int kBldTypeCustoms = 0x14;

// A customs house takes a good only if a store of it lists that good. The generic stores of
// $STORAGE have a fixed list from the game, 11 goods for covered, and the put in FUN_140420350
// finds nothing else, so a vehicle keeps its mail (seen 2026-11-07: "the cargo stays in the
// vehicle", no mail in any store of zoll_air_*). This adds both mails to the covered store of each
// customs house, in the two lists every store has, as empty entries like the others. It is done
// on the instance, so it also works on a map made earlier, and it does nothing the second time.
static bool ListsResource(const BYTE* store, const BYTE* res)
{
    const StoredRes* it  = Rd<const StoredRes*>(store, 0);
    const StoredRes* end = Rd<const StoredRes*>(store, 8);
    for (; it && it < end; ++it)
        if (it->resource == res) return true;
    return false;
}

static int AddMailToCustoms(const BYTE* b)
{
    if (!g_addResource || !g_resRaw || !g_resDone) return 0;
    int added = 0;
    BYTE* s  = Rd<BYTE*>(b, kBldStorageBegin);
    BYTE* se = Rd<BYTE*>(b, kBldStorageEnd);
    for (; s && s < se; s += kStorStride)
    {
        // The generic store: neither flag, and a list of goods, unlike a store for one good.
        if (Rd<int>(s, kStorClass) != 0 || Rd<char>(s, kStorImport) || Rd<char>(s, kStorExport)) continue;
        if (VectorCount(s) < 2 && !ListsResource(s, g_resRaw)) continue;
        BYTE* const res[2] = { g_resRaw, g_resDone };
        for (int i = 0; i < 2; ++i)
        {
            if (ListsResource(s, res[i])) continue;
            ResPair pair = { res[i], 0 };
            g_addResource(s, &pair);
            g_addResource(s + kStorSetBegin, &pair);
            ++added;
        }
        break;
    }
    return added;
}

static void DecayAllStorage(const void* game)
{
    if (g_decayDay == g_gameDay) return;
    g_decayDay = g_gameDay;
    BYTE* const* it  = Rd<BYTE* const*>(game, kGameBuildings);
    BYTE* const* end = Rd<BYTE* const*>(game, kGameBuildings + 8);
    if (!it || end < it || end - it > kMaxBuildings) return;
    static int loggedHomes = -1;
    g_homesSized = 0;
    int customs = 0;
    for (; it != end; ++it)
    {
        if (!*it) continue;
        SizeHomeMail(*it);
        if (g_resRaw && BuildingType(*it) == kBldTypeCustoms && AddMailToCustoms(*it)) ++customs;
        if (g_decayPerDay > 0.0f) DecayBuilding(*it);
    }
    if (customs) Logf("mail  customs: %d customs houses now list mail", customs);
    if (g_homesSized != loggedHomes)
    {
        loggedHomes = g_homesSized;
        Logf("mail  homes: %d houses have mail storage by size, %.1f kg per resident", g_homesSized, (double)g_homeKgPerResident);
    }
}

// One line a minute: enough to tell an old map, no mail yet and refused stops apart.
static void LogMailState(const BYTE* office)
{
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 60000) return;
    last = now;
    BYTE* const* it  = Rd<BYTE* const*>(office, kOffTgtBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffTgtEnd);
    int total = 0, withStorage = 0, ready = 0;
    static int perType[kTypeBuckets], perTypeWith[kTypeBuckets];
    memset(perType, 0, sizeof(perType));
    memset(perTypeWith, 0, sizeof(perTypeWith));
    float best = 0.0f;
    BYTE* bestB = NULL;
    for (; it && it < end; ++it)
    {
        if (!*it) continue;
        ++total;
        int tb = TypeBucket(*it);
        ++perType[tb];
        BYTE* s = NULL;
        StoredRes* r = OwnMail(*it, g_resRaw, &s);
        if (!r || IsMailBuilding(*it)) continue;
        ++withStorage;
        ++perTypeWith[tb];
        float cap = Rd<float>(s, kStorCapacity) * Rd<float>(r->resource, kResClassFactor);
        float fill = cap > 0.0f ? r->amount / cap : 0.0f;
        if (fill > best) { best = fill; bestB = *it; }
        if (fill >= g_mailThreshold) ++ready;
    }
    Logf("mail  state: %d targets, %d with mail storage, %d ready, fullest %.0f%%; dest %p, source %p; made %.3f t, used %.3f t",
         total, withStorage, ready, best * 100.0f, SlotBuilding(office, kSlotDest), SlotBuilding(office, kSlotSource),
         g_mailMade / kFixed, g_mailUsed / kFixed);
    Logf("mail  home: day %u, %d homes known; resident days with mail %llu, without %llu, not served %llu; %.3f t used; %.3f t lost in storage",
         g_gameDay, g_homeCount, (unsigned long long)g_homeFed, (unsigned long long)g_homeUnhappy,
         (unsigned long long)g_homeNeutral, g_mailUsed / kFixed, g_mailDecayed / kFixed);
    Logf("mail  clock: calendar day %d of year %d; %.0f g per resident, raw x%.2f; fullest ageing store %.3f t in a building of type %d",
         g_calendarDay, g_calendarYear, (double)g_mailPerPerson * 1000000.0, (double)g_mailRawFactor,
         (double)g_decayTopAmount, g_decayTopType);
    g_decayTopAmount = 0.0f;
    g_decayTopType = -1;
    {
        const BYTE* in  = SlotBuilding(office, kSlotDest);
        const BYTE* out = SlotBuilding(office, kSlotSource);
        StoredRes* raw  = in  ? OwnMail((BYTE*)in, g_resRaw) : NULL;
        StoredRes* done = out ? OwnMail((BYTE*)out, g_resDone) : NULL;
        Logf("mail  depot: first row (type %d) raw %.3f t; second row (type %d) processed %.3f t; trips collected %llu, delivered %llu",
             in ? BuildingType(in) : -1, raw ? (double)raw->amount : -1.0,
             out ? BuildingType(out) : -1, done ? (double)done->amount : -1.0,
             (unsigned long long)g_roleTrips[0], (unsigned long long)g_roleTrips[1]);
    }
    for (int i = 0; i < kTypeBuckets; ++i)
        if (perType[i]) Logf("mail    targets of type %3d: %d, with mail storage %d", i, perType[i], perTypeWith[i]);
    DumpStorages(SlotBuilding(office, kSlotDest), "warehouse");
    DumpStorages(bestB, "fullest target");
}

// An office with a mail truck covers its targets. The list is walked at most every 30 s
// per office, so the entries stay fresh within kServedMs.
struct OfficeMark { const BYTE* office; uint64_t at; };
static OfficeMark g_officeMark[32];

static void MarkOfficeTargets(const BYTE* office)
{
    uint64_t now = GetTickCount64();
    OfficeMark* slot = NULL;
    for (auto& m : g_officeMark)
    {
        if (m.office == office) { slot = &m; break; }
        if (!m.office && !slot) slot = &m;
    }
    if (!slot) return;
    if (slot->office == office && now - slot->at < 30000) return;
    slot->office = office;
    slot->at = now;
    BYTE* const* v  = Rd<BYTE* const*>(office, kOffVehBegin);
    BYTE* const* ve = Rd<BYTE* const*>(office, kOffVehEnd);
    bool hasTruck = false;
    for (; v && v < ve; ++v)
        if (*v && CoveredTruckType(Rd<const BYTE*>(*v, kVehRecord))) { hasTruck = true; break; }
    if (!hasTruck) return;
    BYTE* const* t  = Rd<BYTE* const*>(office, kOffTgtBegin);
    BYTE* const* te = Rd<BYTE* const*>(office, kOffTgtEnd);
    for (; t && t < te; ++t) if (*t) MarkServed(*t, now);
}

static void RunMailDispatch(BYTE* office)
{
    if (!IsMailOffice(office)) return;
    WatchTrucks(office);
    ProbeOffice(office);
    LogMailState(office);
    MarkOfficeTargets(office);
    BYTE* const* it  = Rd<BYTE* const*>(office, kOffVehBegin);
    BYTE* const* end = Rd<BYTE* const*>(office, kOffVehEnd);
    if (!it || end < it) return;
    for (; it != end; ++it)
    {
        BYTE* v = *it;
        if (v && CoveredTruckType(Rd<const BYTE*>(v, kVehRecord)) && VehicleFree(v, office)) RunTruck(v, office);
    }
}

// ---- locating the site ------------------------------------------------------

static BYTE*  g_textStart;
static SIZE_T g_textSize;

static bool LocateText(void)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_exeBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, ".text", 5) == 0)
        {
            g_textStart = (BYTE*)g_exeBase + sec[i].VirtualAddress;
            g_textSize  = sec[i].Misc.VirtualSize;
            return true;
        }
    return false;
}

// Unique match or nothing. Ambiguity is failure on purpose.
static BYTE* ScanUnique(const short* sig, int len)
{
    BYTE* found = NULL;
    int   count = 0;
    if (len <= 0 || (SIZE_T)len > g_textSize) return NULL;
    const SIZE_T last = g_textSize - (SIZE_T)len;
    for (SIZE_T i = 0; i <= last; ++i)
    {
        BYTE* p = g_textStart + i;
        int   k = 0;
        for (; k < len; ++k)
        {
            const short want = sig[k];
            if (want < 0) continue;
            if (p[k] != (BYTE)want) break;
        }
        if (k != len) continue;
        if (++count > 1) return NULL;
        found = p;
    }
    return (count == 1) ? found : NULL;
}

// Direct calls to the site. A sanity figure for the log, and zero means the
// signature found something that is not the function we think it is.
static int CountCallers(const BYTE* target)
{
    int n = 0;
    if (g_textSize < 5) return 0;
    for (SIZE_T i = 0; i <= g_textSize - 5; ++i)
    {
        const BYTE* p = g_textStart + i;
        if (*p != 0xE8) continue;
        int rel;
        memcpy(&rel, p + 1, 4);
        if (p + 5 + rel == target) ++n;
    }
    return n;
}

// ---- locating the dispatch prototype -------------------------------------------

// The office tick prologue, up to the saves of xmm6-xmm10.
static const short kSigOfficeTick[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x48,0x08,
    0x55,0x53,0x56,0x57, 0x41,0x54,0x41,0x55, 0x41,0x56,0x41,0x57,
    0x48,0x8D,0xA8,0x38,0xFF,0xFF,0xFF,
    0x48,0x81,0xEC,0x88,0x01,0x00,0x00,
    0x48,0xC7,0x45,0xD8,0xFE,0xFF,0xFF,0xFF,
    0x0F,0x29,0x70,0xA8, 0x0F,0x29,0x78,0x98,
    0x44,0x0F,0x29,0x40,0x88,
    0x44,0x0F,0x29,0x88,0x78,0xFF,0xFF,0xFF,
    0x44,0x0F,0x29,0x90,0x68,0xFF,0xFF,0xFF,
};
// mov rax,rsp / mov / rbp rbx rsi rdi / r12 / r13: 15 bytes, nothing rip-relative.
static const size_t kStolenOffice = 15;

// FUN_14017c000, the game's water tanker route builder. Its calls give us the
// route functions, so none of those addresses is hardcoded.
static const short kSigRouteBuilder[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x6C,0x24,0x18,
    0x56,0x57,0x41,0x56, 0x48,0x83,0xEC,0x50,
    0x48,0x8B,0x9C,0x24,0x90,0x00,0x00,0x00,
    0x4D,0x8B,0xF1, 0x49,0x8B,0xF8, 0x48,0x8B,0xF2, 0x48,0x8B,0xE9,
    0x48,0x85,0xDB, 0x0F,0x84,-1,-1,-1,-1,
    0x41,0xB0,0x01,
};

// The vehicle check, through its load of the building's type number and the first test.
static const short kSigVehicleFits[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x6C,0x24,0x18, 0x44,0x88,0x4C,0x24,0x20,
    0x56,0x57, 0x41,0x54,0x41,0x55,0x41,0x56,
    0x48,0x83,0xEC,0x50,
    0x48,0x8B,0xAA,0x18,0x03,0x00,0x00,
    0x45,0x0F,0xB6,0xF1,
    0x49,0x8B,0xD8, 0x48,0x8B,0xF2, 0x4C,0x8B,0xE9,
    0x8B,0xBD,0x60,0x03,0x00,0x00,
    0x83,0xFF,0x6D,
};
// Three register saves, 15 bytes, nothing rip-relative.
static const size_t kStolenFits = 15;

static BYTE* CallTarget(BYTE* base, size_t off)
{
    BYTE* p = base + off;
    if (*p != 0xE8) return NULL;
    int rel;
    memcpy(&rel, p + 1, 4);
    return p + 5 + rel;
}

static bool ResolveRouteFunctions(void)
{
    BYTE* b = ScanUnique(kSigRouteBuilder, (int)(sizeof(kSigRouteBuilder) / sizeof(kSigRouteBuilder[0])));
    if (!b) { Logf("mail  dispatch: route builder not located by signature"); return false; }

    BYTE* clear  = CallTarget(b, 0x58);
    BYTE* add    = CallTarget(b, 0x7D);
    BYTE* res    = CallTarget(b, 0xEF);
    BYTE* add2   = CallTarget(b, 0x101);
    BYTE* add3   = CallTarget(b, 0x165);
    BYTE* start  = CallTarget(b, 0x1BC);
    BYTE* abortf = CallTarget(b, 0x1CD);
    BYTE* clear2 = CallTarget(b, 0x1DC);
    if (!clear || !add || !res || !start || !abortf || add != add2 || add != add3 || clear != clear2)
    {
        Logf("mail  dispatch: route builder calls do not match, not using them");
        return false;
    }
    g_clearRoute  = (ClearRouteFn)clear;
    g_addStop     = (AddStopFn)add;
    g_addResource = (AddResourceFn)res;
    g_startRoute  = (StartRouteFn)start;
    g_abortRoute  = (AbortRouteFn)abortf;
    return true;
}

static bool InstallDispatch(void)
{
    if (!ResolveRouteFunctions()) return false;
    BYTE* site = ScanUnique(kSigOfficeTick, (int)(sizeof(kSigOfficeTick) / sizeof(kSigOfficeTick[0])));
    if (!site) { Logf("mail  dispatch: office tick not located by signature"); return false; }

    BYTE expect[kStolenOffice];
    memcpy(expect, site, kStolenOffice);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)OfficeTickDetour, &tramp, expect, kStolenOffice, "mail/office dispatch"))
    {
        Logf("mail  dispatch: hook refused");
        return false;
    }
    g_origOfficeTick = (OfficeTickFn)tramp;
    Logf("mail  dispatch prototype on: office tick at rva 0x%llX, vehicle '%s', resource '%s', at most %d orders",
         (unsigned long long)(site - g_exeBase), g_vehicleName, g_resourceName, g_maxOrders);

    BYTE* fits = ScanUnique(kSigVehicleFits, (int)(sizeof(kSigVehicleFits) / sizeof(kSigVehicleFits[0])));
    if (!fits) { Logf("mail  buy: vehicle check not located by signature, the truck cannot be bought"); return true; }
    BYTE expectFits[kStolenFits];
    memcpy(expectFits, fits, kStolenFits);
    tramp = NULL;
    if (InstallInlineHook(fits, (void*)VehicleFitsDetour, &tramp, expectFits, kStolenFits, "mail/vehicle check"))
    {
        g_origVehicleFits = (VehicleFitsFn)tramp;
        Logf("mail  buy: vehicle check at rva 0x%llX hooked", (unsigned long long)(fits - g_exeBase));
    }
    else Logf("mail  buy: hook refused");
    return true;
}

// The building picker, through its frame setup and the first reads of the game.
static const short kSigAssignPick[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x48,0x08,
    0x55,0x53,0x56,0x57, 0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x8D,0x6C,0x24,0xB8,
    0x48,0x81,0xEC,0x48,0x01,0x00,0x00,
    0x48,0xC7,0x45,0xF8,0xFE,0xFF,0xFF,0xFF,
    0x0F,0x29,0x70,0xA8, 0x0F,0x29,0x78,0x98, 0x44,0x0F,0x29,0x40,0x88,
    0x4C,0x8B,0xE9, 0x80,0xB9,0xBC,0x0F,0x00,0x00,0x00,
};
// mov rax,rsp / mov / rbp rbx rsi rdi / r12 / r13: 15 bytes, nothing rip-relative.
static const size_t kStolenPick = 15;

// The waste storage test, through its storage count.
static const short kSigWasteAccept[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18,
    0x57, 0x41,0x56, 0x41,0x57, 0x48,0x83,0xEC,0x20,
    0x49,0x8B,0xE8, 0x4C,0x8B,0xDA, 0x4C,0x8B,0xF1,
    0x48,0x85,0xD2, 0x0F,0x84,-1,-1,-1,-1,
    0x4C,0x8B,0x8A,0x78,0x09,0x00,0x00,
    0x49,0xBF,0x25,0x49,0x92,0x24,0x49,0x92,0x24,0x49,
    0x4C,0x2B,0x8A,0x70,0x09,0x00,0x00,
};
// Three register saves, 15 bytes, nothing rip-relative.
static const size_t kStolenAccept = 15;

static void InstallPicker(void)
{
    BYTE* pick = ScanUnique(kSigAssignPick, (int)(sizeof(kSigAssignPick) / sizeof(kSigAssignPick[0])));
    BYTE* acc  = ScanUnique(kSigWasteAccept, (int)(sizeof(kSigWasteAccept) / sizeof(kSigWasteAccept[0])));
    if (!pick || !acc)
    {
        Logf("mail  pick: picker %d, test %d not located by signature, the warehouse cannot be chosen",
             pick ? 1 : 0, acc ? 1 : 0);
        return;
    }
    // The test first: a picker hook alone changes nothing.
    BYTE expect[kStolenAccept];
    memcpy(expect, acc, kStolenAccept);
    void* tramp = NULL;
    if (!InstallInlineHook(acc, (void*)WasteAcceptDetour, &tramp, expect, kStolenAccept, "mail/waste accept"))
    {
        Logf("mail  pick: test hook refused");
        return;
    }
    g_origWasteAccept = (WasteAcceptFn)tramp;

    BYTE expectPick[kStolenPick];
    memcpy(expectPick, pick, kStolenPick);
    tramp = NULL;
    if (!InstallInlineHook(pick, (void*)AssignPickDetour, &tramp, expectPick, kStolenPick, "mail/picker"))
    {
        Logf("mail  pick: picker hook refused");
        return;
    }
    g_origAssignPick = (AssignPickFn)tramp;
    Logf("mail  pick: picker at rva 0x%llX, test at rva 0x%llX",
         (unsigned long long)(pick - g_exeBase), (unsigned long long)(acc - g_exeBase));
}

// The call that pushes the default bin, in the parser, and what follows it.
static const short kSigBinPush[] = {
    0x48,0x8D,0x95,0x90,0x0E,0x00,0x00, 0x48,0x8D,0x8D,0x50,0x1E,0x00,0x00,
    0xE8,-1,-1,-1,-1, 0x90, 0x48,0x8D,0x8D,0x90,0x0E,0x00,0x00, 0xE8,
};
static const BYTE kPushProlog[15] = {
    0x40,0x57, 0x48,0x83,0xEC,0x30, 0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,
};

static void InstallMailStorages(void)
{
    BYTE* site = ScanUnique(kSigBinPush, (int)(sizeof(kSigBinPush) / sizeof(kSigBinPush[0])));
    BYTE* push = site ? CallTarget(site, 14) : NULL;
    if (!push || memcmp(push, kPushProlog, sizeof(kPushProlog)) != 0)
    {
        Logf("mail  storages: bin push not located, buildings get no mail storage");
        return;
    }
    g_binPushReturn = site + 19;
    void* tramp = NULL;
    if (!InstallInlineHook(push, (void*)PushStorageDetour, &tramp, kPushProlog, sizeof(kPushProlog), "mail/storages"))
    {
        Logf("mail  storages: hook refused");
        return;
    }
    g_origPushStorage = (PushStorageFn)tramp;
    Logf("mail  storages: bin push at rva 0x%llX", (unsigned long long)(site + 14 - g_exeBase));
}

// ---- mail rows in the living building window -----------------------------------------
//
// Generic building windows list the mail storages on their own. The living window draws
// only classes 15 and 16 itself, so there the rows go right after the waste section
// FUN_140775000, drawn by the function that section uses for the bin.

typedef void (*SectionFn)(void* game, float* x, float* y, BYTE* window);
typedef void* (*LivingWindowFn)(void* game, BYTE* window, void* c, void* d);
typedef void (*DrawStorageFn)(void* game, BYTE* window, BYTE* storage, float* x, float* y,
                              uint64_t a, int b, int c, int d, int owner, int index,
                              int e, int f, int g, int h, int i);
static SectionFn      g_origSection;
static LivingWindowFn g_origLiving;
static DrawStorageFn  g_drawStorage;
static bool           g_inLiving;

static void* LivingWindowDetour(void* game, BYTE* window, void* c, void* d)
{
    g_inLiving = true;
    void* r = g_origLiving(game, window, c, d);
    g_inLiving = false;
    return r;
}

static void SectionDetour(void* game, float* x, float* y, BYTE* window)
{
    g_origSection(game, x, y, window);
    if (!g_inLiving || !g_drawStorage || !g_resRaw) return;
    BYTE* b  = Rd<BYTE*>(window, 0x240);
    BYTE* s  = b ? Rd<BYTE*>(b, kBldStorageBegin) : NULL;
    BYTE* se = b ? Rd<BYTE*>(b, kBldStorageEnd) : NULL;
    if (!s || se < s) return;
    int index = 0;
    for (; s != se; s += kStorStride, ++index)
    {
        StoredRes* it  = Rd<StoredRes*>(s, 0);
        StoredRes* end = Rd<StoredRes*>(s, 8);
        if (it && end - it == 1 && (it->resource == g_resRaw || it->resource == g_resDone))
            g_drawStorage(game, window, s, x, y, 0, 1, 0, -1, Rd<int>(b, 0x9E8), index, 0, 0, 0, 1, 0);
    }
}

static const short kSigSection[] = {
    0x48,0x8B,0xC4, 0x4C,0x89,0x40,0x18, 0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,
    0x48,0x81,0xEC,0xD0,0x00,0x00,0x00, 0x49,0x8B,0xB9,0x40,0x02,0x00,0x00,
    0x4D,0x8B,0xF9, 0xF3,0x0F,0x10,0x02, 0x4C,0x8B,0xF2, 0xF3,0x41,0x0F,0x10,0x08,
};
static const size_t kStolenSection = 15;   // through push r15, nothing rip-relative
static const size_t kSectionDrawCall = 0x1ee;
static const BYTE kDrawProlog[16] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 };

static const short kSigLiving[] = {
    0x40,0x55,0x56,0x57,0x41,0x56, 0x48,0x8D,0xAC,0x24,0xA8,0xDB,0xFF,0xFF,
    0xB8,0x68,0x25,0x00,0x00, 0xE8,-1,-1,-1,-1, 0x48,0x2B,0xE0, 0x48,0x8B,0x05,-1,-1,-1,-1,
    0x48,0x33,0xC4, 0x48,0x89,0x85,0x90,0x23,0x00,0x00, 0x80,0x7A,0x01,0x00,
    0x48,0x8B,0xF9, 0x4C,0x8B,0xB2,0x40,0x02,0x00,0x00,
};
static const size_t kStolenLiving = 14;    // through the lea, nothing rip-relative

static void InstallLivingRows(void)
{
    BYTE* sec = ScanUnique(kSigSection, (int)(sizeof(kSigSection) / sizeof(kSigSection[0])));
    BYTE* liv = ScanUnique(kSigLiving, (int)(sizeof(kSigLiving) / sizeof(kSigLiving[0])));
    BYTE* draw = sec ? CallTarget(sec, kSectionDrawCall) : NULL;
    if (!sec || !liv || !draw || memcmp(draw, kDrawProlog, sizeof(kDrawProlog)) != 0)
    {
        Logf("mail  window: not located, houses show no mail rows (the mail itself still works)");
        return;
    }
    g_drawStorage = (DrawStorageFn)draw;
    BYTE e1[kStolenSection], e2[kStolenLiving];
    memcpy(e1, sec, kStolenSection);
    memcpy(e2, liv, kStolenLiving);
    void* t1 = NULL;
    void* t2 = NULL;
    if (!InstallInlineHook(sec, (void*)SectionDetour, &t1, e1, kStolenSection, "mail/window section"))
    {
        Logf("mail  window: section hook refused");
        return;
    }
    g_origSection = (SectionFn)t1;
    if (!InstallInlineHook(liv, (void*)LivingWindowDetour, &t2, e2, kStolenLiving, "mail/living window"))
    {
        Logf("mail  window: living hook refused");
        return;
    }
    g_origLiving = (LivingWindowFn)t2;
    Logf("mail  window: mail rows on at section rva 0x%llX", (unsigned long long)(sec - g_exeBase));
}

// ---- the home need hook ----------------------------------------------------------
//
// The prologue of FUN_14083a6d0: mov rax,rsp, the six pushes, then sub rsp,0E0h. The
// stolen part is the mov and the pushes, 14 bytes; nothing rip-relative.
static const short kSigCitizenHome[] = {
    0x48,0x8B,0xC4,
    0x55, 0x56, 0x57,
    0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xEC,0xE0,0x00,0x00,0x00,
    0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF,
};
static const size_t kStolenHome = 14;

static bool InstallHomeNeed(void)
{
    BYTE* site = ScanUnique(kSigCitizenHome, (int)(sizeof(kSigCitizenHome) / sizeof(kSigCitizenHome[0])));
    if (!site)
    {
        Logf("mail  home need: citizen-at-home not located by signature, the need is off");
        return false;
    }
    BYTE expect[kStolenHome];
    memcpy(expect, site, kStolenHome);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)CitizenHomeDetour, &tramp, expect, kStolenHome, "mail/home need"))
    {
        Logf("mail  home need: hook refused, the need is off");
        return false;
    }
    g_origCitizenHome = (CitizenHomeFn)tramp;
    Logf("mail  home: on at rva 0x%llX, +%.3f or -%.3f happiness per day, within %.0f%% of the limit",
         (unsigned long long)(site - g_exeBase), (double)g_happyBonus, (double)g_happyDebuff,
         (double)g_happyRange * 100.0);
    return true;
}

// ---- stage 2: the mail resources -------------------------------------------------
//
// mail_raw and mail_done, added to the engine's resource vector by this plugin, so
// the mod works under both loaders. The approach comes from TesmioLoader's
// plugins/resources/resources.cpp (MaxLegend, GPL-3.0), reduced to what mail needs: a
// hook on the resource lookup claims free slots after the base records are published.
//
// The resource count is part of the save format: a game with these resources needs
// them to load, and an existing save will not load with them.
//
// No relocation here. Only free capacity is used, and if there is none the resources
// are not added and the log says so. Another plugin may own the array.

static const size_t kGameResVector = 0xC2B0;   // begin, end, cap
static const size_t kResStride     = 0x340;
static const size_t kResName       = 0x00;
static const size_t kResNameMax    = 32;
static const size_t kResCaption    = 0x40;
static const ptrdiff_t kBaseRecords = 57;
static const int   kMailTextBase   = 1700000;  // above the game's ids and resources.cpp's

struct MailRes { const char* name; int textId; wchar_t caption[64]; };
static MailRes g_mailRes[2] = {
    { "mail_raw",  kMailTextBase + 0, L"Unprocessed mail" },
    { "mail_done", kMailTextBase + 1, L"Processed mail" },
};
static const char kResTemplate[] = "eletronics";   // covered class, no cargo mesh

typedef BYTE* (*ResourceGetFn)(void* game, const char* name, void* a, void* b);
static ResourceGetFn g_origResourceGet;
static BYTE* g_lastResEnd;
static bool  g_resArmed;
static bool  g_resRefusedLogged;

static BYTE* FindResource_(BYTE* begin, BYTE* end, const char* name)
{
    for (BYTE* r = begin; r < end; r += kResStride)
        if (strncmp((const char*)r + kResName, name, kResNameMax) == 0) return r;
    return NULL;
}

// Idempotent, and cheap while the vector does not change. A map load clears the
// vector with end = begin, so a changed end is the signal to look again.
// Records the prices are worked out from. Going back to the menu and opening another map
// rebuilds the list, so these die with it and are looked up again.
static BYTE* g_resWorkers;

static void ArmMailResources(BYTE* game)
{
    BYTE** vec  = (BYTE**)(game + kGameResVector);
    BYTE* begin = vec[0];
    BYTE* end   = vec[1];
    BYTE* cap   = vec[2];
    if (end == g_lastResEnd && g_resArmed) return;
    g_lastResEnd = end;
    g_resWorkers = NULL;
    if (!begin || end < begin || cap < end) return;
    if ((end - begin) / (ptrdiff_t)kResStride < kBaseRecords) { g_resArmed = false; return; }

    if (FindResource_(begin, end, g_mailRes[0].name) && FindResource_(begin, end, g_mailRes[1].name))
    {
        g_resRaw  = FindResource_(begin, end, g_mailRes[0].name);
        g_resDone = FindResource_(begin, end, g_mailRes[1].name);
        g_resArmed = true;
        return;
    }
    BYTE* tmpl = FindResource_(begin, end, kResTemplate);
    if (!tmpl) return;

    ptrdiff_t room = (cap - end) / (ptrdiff_t)kResStride;
    if (room < 2)
    {
        if (!g_resRefusedLogged)
        {
            g_resRefusedLogged = true;
            Logf("mail  resources: only %d free slots, 2 needed. mail_raw and mail_done NOT added; "
                 "remove another mod's resource or raise its capacity", (int)room);
        }
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        BYTE* rec = end + (size_t)i * kResStride;
        memcpy(rec, tmpl, kResStride);
        memset(rec + kResName, 0, kResNameMax);
        strncpy((char*)rec + kResName, g_mailRes[i].name, kResNameMax - 1);
        *(int*)(rec + kResCaption) = g_mailRes[i].textId;
    }
    // The records are complete before end moves.
    vec[1] = end + 2 * kResStride;
    g_lastResEnd = vec[1];
    g_resRaw  = end;
    g_resDone = end + kResStride;
    g_resArmed = true;
    Logf("mail  resources: mail_raw and mail_done published at slots %d and %d, the list had %d records and room for %d",
         (int)((end - begin) / (ptrdiff_t)kResStride), (int)((end - begin) / (ptrdiff_t)kResStride) + 1,
         (int)((end - begin) / (ptrdiff_t)kResStride), (int)room);
}

// The vehicle parser resolves $RESOURCE_ALLOW_ONLY with this same lookup. If the vehicle
// scripts are read before the base resource list exists, the name resolves to nothing and
// the token is dropped without a word. These lines date the lookups for the mail names and
// show whether the record was there at that moment.
static bool g_lookupNotReadyLogged;

static BYTE* ResourceGetDetour(void* game, const char* name, void* a, void* b)
{
    bool mailName = name && (strcmp(name, g_mailRes[0].name) == 0 || strcmp(name, g_mailRes[1].name) == 0);
    if (mailName && !g_resArmed && !g_lookupNotReadyLogged)
    {
        g_lookupNotReadyLogged = true;
        BYTE** vec = game ? (BYTE**)((BYTE*)game + kGameResVector) : NULL;
        int records = vec ? (int)((vec[1] - vec[0]) / (ptrdiff_t)kResStride) : 0;
        Logf("mail  reslookup: '%s' asked with only %d resource records, the mail ones are not there yet",
             name, records);
    }
    if (game) ArmMailResources((BYTE*)game);
    if (mailName && !g_resArmed)
        Logf("mail  reslookup: '%s' could not be answered: the base resource list is not ready", name);
    BYTE* r = g_origResourceGet(game, name, a, b);
    if (mailName && g_resArmed && r != g_resRaw && r != g_resDone)
        Logf("mail  reslookup: '%s' -> %p, not a mail record", name, (void*)r);
    return r;
}

// ---- trade across the border ---------------------------------------------------------
//
// Buy raw mail abroad, process it at home, sell the result abroad. A resource nothing
// produces prices at zero, so raw mail needs a price, and the price solver would make the
// margin whatever its formula likes, so both prices are set here instead. The solver only
// recomputes on some days, so writing them back once a second is enough.
//
// A resource has four prices, read in code. The value of a cargo in a vehicle's window is the
// sell pair. A customs house pays the same pair with nothing on top: seen in the game on 2026-11-07,
// 271.89 t of processed mail sold to Soviet countries for 309,990 RUB, 1140.1 a tonne against a
// price of 1140.39 in the log. The code that pays it, in the vehicle tick, was not read to the end. The market table names the currencies: its columns read the
// snapshot lists below, which FUN_1402fc5c0 fills from these fields.
//
// The margin is tied to wages and not to the goods. What the factory earns on one tonne, sell
// price less buy price, is its labour (workers * days per tonne * the wage) times
// 1 + mail_trade_margin. So processing foreign mail pays like a decent job for those
// workers and the profit per day is bounded by the factory's own throughput. Nothing else
// in this scales with volume: a full plane is a good trip because it is many tonnes, and
// all of it still has to go through the factory, by truck, with transport paid separately.
//
// Everything is counted in the game's own wage, in both currencies. The market inflates over
// the years, so a fixed number means nothing, and the wage inflates with it (1.47 in 1920, 12
// in 1980, in logged runs). Nothing the player trades moves it, unlike the price of a good.
static const size_t kResPriceKind = 0x44;
static const size_t kResBuyUsd = 0x58, kResBuyRub = 0x5C, kResSellUsd = 0x60, kResSellRub = 0x64;
// The gap between the two prices, about 0.95 and 1.05. Other goods in the table keep that ratio.
static const size_t kResSellMul = 0x88, kResBuyMul = 0x8C;
// What the market table shows: {resource, price, other} per resource, recomputed by the game's
// price solver, which has no way to price raw mail. Same order as the fields above.
static const size_t kGameMarketBuyUsd = 0x114C0, kGameMarketBuyRub = 0x114D8,
                    kGameMarketSellUsd = 0x114F0, kGameMarketSellRub = 0x11508;
static float g_tradeRawDays  = 40.0f;   // worker days a tonne of raw mail costs abroad
static float g_tradeMargin   = 1.0f;
static float g_tradeLabour   = 40.0f;   // worker days per tonne: 40 workers, 1 t a day
static uint64_t g_lastPriceMs;
static uint64_t g_lastPriceLogMs;

static void WritePrices(size_t buyOff, size_t sellOff)
{
    float w = Rd<float>(g_resWorkers, buyOff);
    if (w <= 0.0f) return;
    float sellMul = Rd<float>(g_resDone, kResSellMul);
    float buyMul  = Rd<float>(g_resRaw, kResBuyMul);
    if (sellMul <= 0.0f || buyMul <= 0.0f) return;
    float gap     = sellMul / buyMul;
    float rawBuy  = w * g_tradeRawDays;
    float doneSell = rawBuy + w * g_tradeLabour * (1.0f + g_tradeMargin);
    *(float*)(g_resRaw + buyOff)   = rawBuy;
    *(float*)(g_resRaw + sellOff)  = rawBuy * gap;
    *(float*)(g_resDone + sellOff) = doneSell;
    *(float*)(g_resDone + buyOff)  = doneSell / gap;
}

// Only ever compares the entry's pointer, so a list from another moment cannot hurt.
static void WriteMarketEntry(void* game, size_t listOff, const BYTE* res, float price)
{
    BYTE** list = (BYTE**)((BYTE*)game + listOff);
    BYTE* it = list[0];
    BYTE* end = list[1];
    if (!it || end <= it || end - it > 0x10 * 1024 || ((end - it) & 0xF)) return;
    for (; it != end; it += 0x10)
        if (*(const BYTE**)it == res) { *(float*)(it + 8) = price; return; }
}

static void WriteMarketTable(void* game)
{
    const BYTE* const res[2] = { g_resRaw, g_resDone };
    for (int i = 0; i < 2; ++i)
    {
        WriteMarketEntry(game, kGameMarketBuyUsd,  res[i], Rd<float>(res[i], kResBuyUsd));
        WriteMarketEntry(game, kGameMarketBuyRub,  res[i], Rd<float>(res[i], kResBuyRub));
        WriteMarketEntry(game, kGameMarketSellUsd, res[i], Rd<float>(res[i], kResSellUsd));
        WriteMarketEntry(game, kGameMarketSellRub, res[i], Rd<float>(res[i], kResSellRub));
    }
}

static void ApplyPrices(void* game)
{
    BYTE** vec = (BYTE**)((BYTE*)game + kGameResVector);
    if (!g_resWorkers)
    {
        for (BYTE* r = vec[0]; r && r < vec[1]; r += kResStride)
        {
            if (Rd<int>(r, kResPriceKind) == -1) { g_resWorkers = r; break; }
        }
        if (!g_resWorkers) return;
    }
    WritePrices(kResBuyRub, kResSellRub);
    WritePrices(kResBuyUsd, kResSellUsd);
    WriteMarketTable(game);
    // Every few minutes: prices wander, and what a window shows has to be compared to them.
    uint64_t now = GetTickCount64();
    if ((!g_lastPriceLogMs || now - g_lastPriceLogMs >= 300000) && Rd<float>(g_resRaw, kResBuyRub) > 0.0f)
    {
        g_lastPriceLogMs = now;
        Logf("mail  trade: raw buy %.2f RUB, %.2f USD; processed sell %.2f RUB, %.2f USD; wage %.2f RUB, %.2f USD",
             (double)Rd<float>(g_resRaw, kResBuyRub), (double)Rd<float>(g_resRaw, kResBuyUsd),
             (double)Rd<float>(g_resDone, kResSellRub), (double)Rd<float>(g_resDone, kResSellUsd),
             (double)Rd<float>(g_resWorkers, kResBuyRub), (double)Rd<float>(g_resWorkers, kResBuyUsd));
    }
}

static void MailGameTick(void* game)
{
    g_gameNow = game;
    TickClock(game);
    DecayAllStorage(game);
    uint64_t now = GetTickCount64();
    if (now - g_lastPriceMs < 1000) return;
    g_lastPriceMs = now;
    if (g_tradeMargin >= 0.0f) ApplyPrices(game);
}

typedef wchar_t* (*GetStringFn)(void* self, int id);
static GetStringFn g_origGetString;

// The rows of a mail service are waste rows to the game, FUN_140770A30 prints a name and a
// tag for each. While it draws a mail service, and only then, those four texts are mail.
static const int kRowNamePlastic = 0x22C, kRowTagPlastic = 0x619;
static const int kRowNameToxic   = 0x22E, kRowTagToxic   = 0x1009;
// "Warehouse/post office", where the game says "Open storage/factory".
static wchar_t g_rowTag[64] = L"Depot/office";
static DWORD g_rowsThread;
// Shorter than the resources' own names: the name column is narrow and they ran into the tag.
static wchar_t g_rowRaw[64]  = L"Raw mail";
static wchar_t g_rowDone[64] = L"Processed mail";

typedef void (*DrawRowsFn)(void* game, float* x, float* y, BYTE* window, void* e, int f, char g);
static DrawRowsFn g_origDrawRows;

// The winter priorities come right after the rows in the office window, one long block with
// no condition of its own. A mail service clears no snow, so there the rows return past the
// block, to where the game goes on with the rest of the window. The block changes none of
// the registers used after it except xmm2 (the y position, read again by the fuel row that
// follows), xmm3, the x position and r13d, and the thunk sets those as the block's end would.
// A window of another shape keeps the section.
static const size_t kWinterCallReturn = 0x21D3;   // after the call of FUN_140770A30
static const size_t kWinterTail       = 0x3218;   // movss xmm3,[scale], before the resume
static const size_t kWinterResume     = 0x3245;   // cmp dword [rdi+5B0h],2
static const BYTE*  g_winterCallReturn;

extern "C" {
const float* g_mailUiScale;
const BYTE*  g_mailWinterResume;
BYTE*        g_mailRowsRsp;
void MailSkipWinter(void);
void MailDrawRowsStub(void);
void MailDrawRowsEntry(void* game, float* x, float* y, BYTE* window, void* e, int f, char g);
}

__asm__(
    ".text\n"
    ".globl MailSkipWinter\n"
    "MailSkipWinter:\n"
    "    movq g_mailUiScale(%rip), %rax\n"
    "    movss (%rax), %xmm3\n"
    "    mulss %xmm10, %xmm3\n"
    "    movss -0x80(%rbp), %xmm2\n"
    "    addss %xmm3, %xmm2\n"
    "    movss %xmm2, -0x80(%rbp)\n"
    "    movss %xmm14, -0x7c(%rbp)\n"
    "    xorl %r13d, %r13d\n"
    "    jmpq *g_mailWinterResume(%rip)\n"
    ".globl MailDrawRowsStub\n"
    "MailDrawRowsStub:\n"
    "    movq %rsp, g_mailRowsRsp(%rip)\n"
    "    jmp MailDrawRowsEntry\n");

extern "C" void MailDrawRowsEntry(void* game, float* x, float* y, BYTE* window, void* e, int f, char g)
{
    void** ret = (void**)g_mailRowsRsp;
    const BYTE* office = window ? Rd<const BYTE*>(window, 0x240) : NULL;
    bool mail = g_resRaw && IsMailOffice(office);
    if (mail) g_rowsThread = GetCurrentThreadId();
    g_origDrawRows(game, x, y, window, e, f, g);
    if (mail) g_rowsThread = 0;
    if (mail && g_mailWinterResume && *ret == (const void*)g_winterCallReturn)
        *ret = (void*)MailSkipWinter;
}

// The citizen's window prints its needs under one heading, which is where mail goes: an extra
// line would mean moving everything below it in a function that lays itself out as it draws.
static const int kTextCurrentDemands = 0x9D3;
static const BYTE* g_needPerson;
static DWORD g_needThread;
static wchar_t g_needText[128];

typedef void (*DrawPersonFn)(void* game, float* x, float* y, BYTE* person, char e, void* f, void* g);
static DrawPersonFn g_origDrawPerson;

static void DrawPersonDetour(void* game, float* x, float* y, BYTE* person, char e, void* f, void* g)
{
    const BYTE* was = g_needPerson;
    DWORD wasThread = g_needThread;
    if (g_resRaw && person) { g_needPerson = person; g_needThread = GetCurrentThreadId(); }
    g_origDrawPerson(game, x, y, person, e, f, g);
    g_needPerson = was;
    g_needThread = wasThread;
}

// English unless the game speaks Russian, told by what its own text looks like. The first call
// may come before the language is loaded, so an empty answer leaves the question open. A caption
// set in the ini stays whatever the language.
static const wchar_t* g_needFormat = L"Current needs (mail: %d g, %d%%):";
static bool g_captionFromIni[2];
static bool g_languageKnown;

static bool HasCyrillic(const wchar_t* t)
{
    for (; t && *t; ++t)
        if (*t >= 0x400 && *t <= 0x4FF) return true;
    return false;
}

static void UseRussian()
{
    wcscpy(g_rowTag, L"Склад/почтамт");
    wcscpy(g_rowRaw, L"Сырая почта");
    wcscpy(g_rowDone, L"Готовая почта");
    g_needFormat = L"Текущие потребности (почта: %d г, %d%%):";
    if (!g_captionFromIni[0]) wcscpy(g_mailRes[0].caption, L"Необработанная почта");
    if (!g_captionFromIni[1]) wcscpy(g_mailRes[1].caption, L"Обработанная почта");
}

static const wchar_t* MailNeedHeading(const BYTE* person)
{
    int percent = MailNeedPercent(person);
    if (percent < 0) return NULL;
    _snwprintf(g_needText, 127, g_needFormat, (int)(g_mailPerPerson * 1000000.0f + 0.5f), percent);
    g_needText[127] = 0;
    return g_needText;
}

static wchar_t* GetStringDetour(void* self, int id)
{
    if (!g_languageKnown)
    {
        const wchar_t* sample = g_origGetString(self, kRowNamePlastic);
        if (sample && sample[0])
        {
            g_languageKnown = true;
            if (HasCyrillic(sample)) UseRussian();
        }
    }
    if (id == kTextCurrentDemands && g_needThread && g_needThread == GetCurrentThreadId())
    {
        const wchar_t* heading = MailNeedHeading(g_needPerson);
        if (heading) return (wchar_t*)heading;
    }
    if (g_rowsThread && g_rowsThread == GetCurrentThreadId())
    {
        if (id == kRowNamePlastic) return g_rowRaw;
        if (id == kRowNameToxic)   return g_rowDone;
        if (id == kRowTagPlastic || id == kRowTagToxic) return g_rowTag;
    }
    for (auto& r : g_mailRes)
        if (id == r.textId) return r.caption;
    return g_origGetString(self, id);
}

// The office window FUN_14074C500 collects its warnings in one text: for each it appends "-",
// the text from GetString, then a line break. A mail service has no garbage trucks and stores
// no waste, so those lines are false there. Conditions and text are the game's, on its stack,
// so the line is taken out when it is asked for: the dash goes, the text comes back empty.
// One empty line can stay behind. A frame of another shape, or another caller, is left alone.
static const size_t kWinCodeSize = 0x1800;          // the warnings are all inside this
static const size_t kWinTextFromCallerRsp = 0x360;  // rbp = rsp + 0x110, text at rbp + 0x250
static const int    kWinTextMax = 0x400;
static const BYTE* g_winCode;
static DWORD g_winThread;
static wchar_t g_noText[1] = { 0 };

static bool IsWasteWarning(int id)
{
    return id == 0x10D6 || id == 0x10D7 || (id >= 0x10DD && id <= 0x10E3);
}

static bool DropWarningDash(wchar_t* text)
{
    int n = 0;
    while (n < kWinTextMax && text[n]) ++n;
    if (n == 0 || n >= kWinTextMax || text[n - 1] != L'-') return false;
    text[n - 1] = 0;
    return true;
}

typedef void (*DrawWindowFn)(void* game, BYTE* window);
static DrawWindowFn g_origDrawWindow;

static bool EndsWithMail(const BYTE* b);

static void DrawWindowDetour(void* game, BYTE* window)
{
    BYTE* office = window ? Rd<BYTE*>(window, 0x240) : NULL;
    bool mail = g_resRaw && IsMailOffice(office);
    // The service has the small pair from its type too. It never fills, and it is not shown.
    bool hide = mail && EndsWithMail(office);
    if (mail) g_winThread = GetCurrentThreadId();
    if (hide) *(BYTE**)(office + kBldStorageEnd) -= 2 * kStorStride;
    g_origDrawWindow(game, window);
    if (hide) *(BYTE**)(office + kBldStorageEnd) += 2 * kStorStride;
    if (mail) g_winThread = 0;
}

// GetString's import points at the stub: the same call, with the stack pointer as a third
// argument.
extern "C" wchar_t* MailGetStringEntry(void* self, int id, BYTE* rsp)
{
    if (g_winThread && IsWasteWarning(id) && g_winThread == GetCurrentThreadId())
    {
        const BYTE* ret = *(const BYTE**)rsp;
        if (g_winCode && ret >= g_winCode && ret < g_winCode + kWinCodeSize &&
            DropWarningDash((wchar_t*)(rsp + 8 + kWinTextFromCallerRsp)))
            return g_noText;
    }
    return GetStringDetour(self, id);
}
__asm__(
    ".text\n"
    ".globl MailGetStringStub\n"
    "MailGetStringStub:\n"
    "    movq %rsp, %r8\n"
    "    jmp MailGetStringEntry\n");
extern "C" wchar_t* MailGetStringStub(void* self, int id);

static const short kSigResourceGet[] = {
    0x40,0x55, 0x57, 0x41,0x56, 0x48,0x8B,0xEC, 0x48,0x83,0xEC,0x40,
    0x48,0xC7,0x45,0xE0,0xFE,0xFF,0xFF,0xFF,
    0x48,0x89,0x5C,0x24,0x68, 0x48,0x89,0x74,0x24,0x70,
    0x4C,0x8B,0xF2, 0x48,0x8B,0xF1, 0x0F,0x57,0xC0,
};
// Through the frame setup: 20 bytes, nothing rip-relative.
static const size_t kStolenResGet = 20;

static const char kGetStringImport[] = "?GetString@C3D_LANGUAGE@@QEAAPEA_WH@Z";

// mov rax,rsp; mov [rax+18h],r8; mov [rax+10h],rdx; push rbp, rbx, rsi, rdi, r12...r15;
// lea rbp,[rax-0C8h]; sub rsp,188h; mov rbx,[r9+240h]. Stolen: up to the fourth push, 15
// bytes, nothing rip-relative.
static const short kSigDrawRows[] = {
    0x48,0x8B,0xC4, 0x4C,0x89,0x40,0x18, 0x48,0x89,0x50,0x10,
    0x55, 0x53, 0x56, 0x57,
    0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8D,0xA8,0x38,0xFF,0xFF,0xFF,
    0x48,0x81,0xEC,0x88,0x01,0x00,0x00,
    0x49,0x8B,0x99,0x40,0x02,0x00,0x00,
};
static const size_t kStolenDrawRows = 15;

// mov rax,rsp; the seven pushes; lea rbp,[rax-0B38h]; sub rsp,0C10h. Stolen: mov and pushes,
// 14 bytes, nothing rip-relative. The frame sizes in the signature are the ones the text
// offset above is worked out from, so a different frame does not match.
static const short kSigDrawWindow[] = {
    0x48,0x8B,0xC4, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8D,0xA8,0xC8,0xF4,0xFF,0xFF,
    0x48,0x81,0xEC,0x10,0x0C,0x00,0x00,
    0x48,0xC7,0x45,0x08,0xFE,0xFF,0xFF,0xFF,
};
static const size_t kStolenDrawWindow = 14;

static void InstallWindowWarnings(void)
{
    BYTE* site = ScanUnique(kSigDrawWindow, (int)(sizeof(kSigDrawWindow) / sizeof(kSigDrawWindow[0])));
    if (!site) { Logf("mail  warnings: office window not located, the service keeps its waste warnings"); return; }
    BYTE expect[kStolenDrawWindow];
    memcpy(expect, site, kStolenDrawWindow);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)DrawWindowDetour, &tramp, expect, kStolenDrawWindow, "mail/window warnings"))
    {
        Logf("mail  warnings: hook refused, the service keeps its waste warnings");
        return;
    }
    g_origDrawWindow = (DrawWindowFn)tramp;
    g_winCode = site;

    // The winter block is skipped only if the code around it is what this was written for.
    static const BYTE kCallEnd[3]  = { 0xF3, 0x0F, 0x10 };                    // movss after the call
    static const BYTE kTail[4]     = { 0xF3, 0x0F, 0x10, 0x1D };              // movss xmm3,[rip+x]
    static const BYTE kResume[7]   = { 0x83, 0xBF, 0xB0, 0x05, 0x00, 0x00, 0x02 };
    if (memcmp(site + kWinterCallReturn, kCallEnd, sizeof(kCallEnd)) == 0 &&
        memcmp(site + kWinterTail, kTail, sizeof(kTail)) == 0 &&
        memcmp(site + kWinterResume, kResume, sizeof(kResume)) == 0 &&
        g_origDrawRows)
    {
        g_mailUiScale = (const float*)(site + kWinterTail + 8 + *(const int*)(site + kWinterTail + 4));
        g_winterCallReturn = site + kWinterCallReturn;
        g_mailWinterResume = site + kWinterResume;
        Logf("mail  winter: priorities hidden for mail services");
    }
    else
        Logf("mail  winter: the window is not what was expected, the section stays");
    Logf("mail  warnings: waste warnings off for mail services, window at rva 0x%llX", (unsigned long long)(site - g_exeBase));
}

// mov rax,rsp; push rbp, rbx, rsi, rdi, r12...r15; lea rbp,[rax-538h]; sub rsp,5F8h.
// Stolen: mov and pushes, 15 bytes, nothing rip-relative.
static const short kSigDrawPerson[] = {
    0x48,0x8B,0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8D,0xA8,0xC8,0xFA,0xFF,0xFF,
    0x48,0x81,0xEC,0xF8,0x05,0x00,0x00,
    0x0F,0x29,0x70,0xA8, 0x0F,0x29,0x78,0x98,
};
static const size_t kStolenDrawPerson = 15;

static void InstallPersonWindow(void)
{
    BYTE* site = ScanUnique(kSigDrawPerson, (int)(sizeof(kSigDrawPerson) / sizeof(kSigDrawPerson[0])));
    if (!site) { Logf("mail  person: citizen window not located, it does not mention mail"); return; }
    BYTE expect[kStolenDrawPerson];
    memcpy(expect, site, kStolenDrawPerson);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)DrawPersonDetour, &tramp, expect, kStolenDrawPerson, "mail/person window"))
    {
        Logf("mail  person: hook refused, the citizen window does not mention mail");
        return;
    }
    g_origDrawPerson = (DrawPersonFn)tramp;
    Logf("mail  person: mail in the citizen window at rva 0x%llX, %.0f g per resident per day",
         (unsigned long long)(site - g_exeBase), (double)g_mailPerPerson * 1000000.0);
}

// Shops, pubs and the like restock what runs low themselves, straight from whatever store
// nearby has it, in FUN_140171E10 and three twins. Every storage of the building takes part,
// so they pulled in both kinds of mail: raw mail bought back from a warehouse, and processed
// mail no truck brought. The mail storages are the last two, pushed after the waste bin, so
// during those ticks the building's list simply ends before them.
typedef void (*BuildingTickFn)(void* game, BYTE* building);
static BuildingTickFn g_origRestock[4];

static bool EndsWithMail(const BYTE* b)
{
    const BYTE* begin = Rd<const BYTE*>(b, kBldStorageBegin);
    const BYTE* end   = Rd<const BYTE*>(b, kBldStorageEnd);
    if (!begin || end < begin || (size_t)(end - begin) < 2 * kStorStride) return false;
    for (int i = 0; i < 2; ++i)
    {
        const BYTE* s = end - (size_t)(2 - i) * kStorStride;
        const StoredRes* it = Rd<const StoredRes*>(s, 0);
        const StoredRes* e  = Rd<const StoredRes*>(s, 8);
        if (!it || e - it != 1 || it->resource != (i == 0 ? g_resRaw : g_resDone)) return false;
        if (Rd<float>(s, kStorCapacity) >= kMailBuildingMinCapacity) return false;
    }
    return true;
}

template <int I> static void RestockDetour(void* game, BYTE* b)
{
    bool hide = g_resRaw && b && EndsWithMail(b);
    if (hide) *(BYTE**)(b + kBldStorageEnd) -= 2 * kStorStride;
    g_origRestock[I](game, b);
    if (hide) *(BYTE**)(b + kBldStorageEnd) += 2 * kStorStride;
}

static const short kSigRestock0[] = { 0x48,0x8B,0xC4, 0x55, 0x53, 0x57, 0x41,0x55, 0x41,0x56, 0x48,0x8D,0x68,0x88,
    0x48,0x81,0xEC,0x50,0x01,0x00,0x00, 0x0F,0x29,0x70,0xA8, 0x4C,0x8B,0xE9 };
static const short kSigRestock1[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x48,0x08, 0x55, 0x56, 0x48,0x8D,0x68,0x88,
    0x48,0x81,0xEC,0x68,0x01,0x00,0x00, 0x0F,0x29,0x70,0xA8, 0x45,0x33,0xC9, 0x0F,0x29,0x78,0x98 };
static const short kSigRestock2[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x48,0x08, 0x55, 0x56, 0x48,0x8D,0x68,0x88,
    0x48,0x81,0xEC,0x68,0x01,0x00,0x00, 0x0F,0x29,0x70,0xA8, 0x45,0x33,0xC9, 0xF3,0x0F,0x10,0x35 };
static const short kSigRestock3[] = { 0x48,0x8B,0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,0x55, 0x41,0x57, 0x48,0x8D,0x68,0xB8,
    0x48,0x81,0xEC,0x18,0x01,0x00,0x00, 0x0F,0x29,0x70,0xB8, 0x4C,0x8B,0xF9 };

static void InstallOneRestock(int i, const short* sig, int n, size_t stolen, void* detour)
{
    BYTE* site = ScanUnique(sig, n);
    if (!site) { Logf("mail  restock %d: not located, this kind of building can still pull in mail", i); return; }
    BYTE expect[32];
    memcpy(expect, site, stolen);
    void* tramp = NULL;
    if (!InstallInlineHook(site, detour, &tramp, expect, stolen, "mail/restock"))
    {
        Logf("mail  restock %d: hook refused, this kind of building can still pull in mail", i);
        return;
    }
    g_origRestock[i] = (BuildingTickFn)tramp;
    Logf("mail  restock %d: mail kept out at rva 0x%llX", i, (unsigned long long)(site - g_exeBase));
}

static void InstallRestock(void)
{
#define MAIL_N(a) (int)(sizeof(a) / sizeof(a[0]))
    InstallOneRestock(0, kSigRestock0, MAIL_N(kSigRestock0), 14, (void*)RestockDetour<0>);
    InstallOneRestock(1, kSigRestock1, MAIL_N(kSigRestock1), 20, (void*)RestockDetour<1>);
    InstallOneRestock(2, kSigRestock2, MAIL_N(kSigRestock2), 20, (void*)RestockDetour<2>);
    InstallOneRestock(3, kSigRestock3, MAIL_N(kSigRestock3), 15, (void*)RestockDetour<3>);
#undef MAIL_N
}

// mov rax,rsp; mov [rax+20h],r9b; mov [rax+10h],rdx; mov [rax+8],rcx; push rbp, rsi, rdi;
// mov rbp,rsp; sub rsp,80h; mov qword [rbp-30h],-2; mov [rax+18h],rbx; movzx esi,r8b.
static const short kSigSendHome[] = {
    0x48,0x8B,0xC4, 0x44,0x88,0x48,0x20, 0x48,0x89,0x50,0x10, 0x48,0x89,0x48,0x08,
    0x55, 0x56, 0x57, 0x48,0x8B,0xEC, 0x48,0x81,0xEC,0x80,0x00,0x00,0x00,
    0x48,0xC7,0x45,0xD0,0xFE,0xFF,0xFF,0xFF, 0x48,0x89,0x58,0x18, 0x41,0x0F,0xB6,0xF0,
};

// A house with only a pedestrian way in. The road search FUN_14067E8F0 lets a vehicle go for a
// building without road bays only if it counts as a service: ambulances and fire engines by
// their owner's type, and at a waste office (0x31, which a mail service is) the classes 1, 2 and
// 15 to 17, the garbage and tank trucks. A covered truck is class 0 and is left out, so it gets
// "no path". This adds class 0 to that list. No other class 0 vehicle can be at such an office:
// VehicleFitsDetour lets covered trucks only into a mail service. The same flag also makes the
// search ignore road signs, as it does for garbage trucks.
//
//   cmp ecx,31h; jne; test r10,r10; je; mov ecx,[r10+8600h]; lea eax,[rcx-0Fh]; cmp eax,2; jbe;
//   lea eax,[rcx-1]; cmp eax,1; ja; mov byte [rsp+32h],1
// becomes, in the last line, mov eax,ecx; nop; cmp eax,2; ja: classes 0, 1 and 2.
static const short kSigServiceClass[] = {
    0x83,0xF9,0x31, 0x75,0x23, 0x4D,0x85,0xD2, 0x74,0x1E,
    0x41,0x8B,0x8A,0x00,0x86,0x00,0x00, 0x8D,0x41,0xF1, 0x83,0xF8,0x02, 0x76,0x08,
    0x8D,0x41,0xFF, 0x83,0xF8,0x01, 0x77,0x07, 0xC6,0x44,0x24,0x32,0x01,
};
static const size_t kServiceClassLea = 25, kServiceClassLimit = 30;

static bool WriteServiceClass(BYTE* site)
{
    DWORD old;
    if (!VirtualProtect(site + kServiceClassLea, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[kServiceClassLea] = 0x8B;
    site[kServiceClassLea + 1] = 0xC1;
    site[kServiceClassLea + 2] = 0x90;
    site[kServiceClassLimit] = 0x02;
    VirtualProtect(site + kServiceClassLea, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site + kServiceClassLea, 6);
    return true;
}

static void InstallFootPaths(void)
{
    BYTE* site = ScanUnique(kSigServiceClass, (int)(sizeof(kSigServiceClass) / sizeof(kSigServiceClass[0])));
    if (!site || !WriteServiceClass(site))
    {
        Logf("mail  foot paths: road search not located, houses without a road stay without mail");
        return;
    }
    Logf("mail  foot paths: covered trucks of a mail service count as a service, rva 0x%llX",
         (unsigned long long)(site - g_exeBase));
}

static void InstallStuckWatch(void)
{
    g_sendHome = (SendHomeFn)ScanUnique(kSigSendHome, (int)(sizeof(kSigSendHome) / sizeof(kSigSendHome[0])));
    HMODULE engine = GetModuleHandleA("C3DDLL64.dll");
    g_nodePosition = engine ? (NodePositionFn)GetProcAddress(engine, "?GetPosition@C3D_NODE@@QEAA?AVC3DVECTOR3@@XZ") : NULL;
    if (!g_sendHome || !g_nodePosition)
    {
        Logf("mail  stuck: depot call %p, position call %p, a stuck truck stays stuck", (void*)g_sendHome, (void*)g_nodePosition);
        g_sendHome = NULL;
        g_nodePosition = NULL;
        return;
    }
    Logf("mail  stuck: recalling a truck that does nothing for %d s", (int)(g_stuckMs / 1000));
}

static void InstallRowCaptions(void)
{
    BYTE* site = ScanUnique(kSigDrawRows, (int)(sizeof(kSigDrawRows) / sizeof(kSigDrawRows[0])));
    if (!site) { Logf("mail  rows: not located by signature, the service's rows keep their waste captions"); return; }
    BYTE expect[kStolenDrawRows];
    memcpy(expect, site, kStolenDrawRows);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)MailDrawRowsStub, &tramp, expect, kStolenDrawRows, "mail/row captions"))
    {
        Logf("mail  rows: hook refused, the service's rows keep their waste captions");
        return;
    }
    g_origDrawRows = (DrawRowsFn)tramp;
    Logf("mail  rows: captions on at rva 0x%llX", (unsigned long long)(site - g_exeBase));
}

static bool InstallResources(void)
{
    BYTE* site = ScanUnique(kSigResourceGet, (int)(sizeof(kSigResourceGet) / sizeof(kSigResourceGet[0])));
    if (!site) { Logf("mail  resources: lookup not located by signature, not adding resources"); return false; }

    BYTE expect[kStolenResGet];
    memcpy(expect, site, kStolenResGet);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)ResourceGetDetour, &tramp, expect, kStolenResGet, "mail/resources"))
    {
        Logf("mail  resources: hook refused");
        return false;
    }
    g_origResourceGet = (ResourceGetFn)tramp;

    void* orig = NULL;
    if (PatchIat(g_exe, DLL_ENGINE, kGetStringImport, (void*)MailGetStringStub, &orig, "mail/captions"))
        g_origGetString = (GetStringFn)orig;
    else
        Logf("mail  resources: caption hook refused, the resources will have the template's name");

    Logf("mail  resources: lookup hooked at rva 0x%llX", (unsigned long long)(site - g_exeBase));
    return true;
}

// The game finds an icon by name in media_soviet\resources, so the icons have to be
// there. A file that already exists is left alone.
static void InstallResourceIcons(void)
{
    char self[MAX_PATH], game[MAX_PATH];
    HMODULE me = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&InstallResourceIcons, &me);
    if (!GetModuleFileNameA(me, self, sizeof(self)) || !GetModuleFileNameA(g_exe, game, sizeof(game))) return;
    char* s = strrchr(self, '\\'); if (s) *s = 0;          // ...\plugins
    s = strrchr(self, '\\');       if (s) *s = 0;          // the item folder
    s = strrchr(game, '\\');       if (s) *s = 0;
    for (auto& r : g_mailRes)
    {
        char src[MAX_PATH], dst[MAX_PATH];
        _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\vfs\\media_soviet\\resources\\%s.png", self, r.name);
        _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\media_soviet\\resources\\%s.png", game, r.name);
        if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) continue;
        if (CopyFileA(src, dst, TRUE)) Logf("mail  resources: icon installed: %s.png", r.name);
        else Logf("mail  resources: no icon for %s (looked in %s)", r.name, src);
    }
}

// ---- settings: the ini beside the DLL, so both loaders behave the same ---------

static char g_ini[MAX_PATH];

static void FindIniBesideDll(void)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&FindIniBesideDll, &self);
    DWORD n = GetModuleFileNameA(self, g_ini, sizeof(g_ini));
    if (n < 4 || n >= sizeof(g_ini)) { g_ini[0] = 0; return; }
    memcpy(g_ini + n - 3, "ini", 3);
}

static int CfgInt(const char* key, int fallback)
{
    return g_ini[0] ? (int)GetPrivateProfileIntA("mail", key, fallback, g_ini) : fallback;
}

static double CfgFloat(const char* key, double fallback)
{
    char buf[64];
    if (!g_ini[0] || !GetPrivateProfileStringA("mail", key, "", buf, sizeof(buf), g_ini) || !buf[0])
        return fallback;
    return atof(buf);
}

// ---- plugin entry points --------------------------------------------------------

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "mail";
    info->version = MAIL_VERSION;

    FindIniBesideDll();
    if (!CfgInt("enabled", 1))
    {
        Logf("mail  enabled = 0, nothing to do");
        return 1;
    }

    g_mailPerWaste = CfgFloat("mail_per_waste", 0.25);
    if (!(g_mailPerWaste >= 0.0 && g_mailPerWaste <= 10.0)) g_mailPerWaste = 0.25;
    int logSeconds = CfgInt("log_seconds", 60);
    g_logMs = logSeconds > 0 ? (DWORD)logSeconds * 1000u : 0u;

    if (!LocateText())
    {
        Logf("mail  .text not found, not hooking");
        return 1;
    }

    BYTE* site = ScanUnique(kSigAddWaste, (int)(sizeof(kSigAddWaste) / sizeof(kSigAddWaste[0])));
    if (!site)
    {
        Logf("mail  waste function not located by signature, not hooking (different game version?)");
        return 1;
    }
    int callers = CountCallers(site);
    if (callers == 0)
    {
        Logf("mail  signature matched at rva 0x%llX but nothing calls it, not hooking",
             (unsigned long long)(site - g_exeBase));
        return 1;
    }

    BYTE expect[kStolen];
    memcpy(expect, site, kStolen);
    void* tramp = NULL;
    if (!InstallInlineHook(site, (void*)AddWasteDetour, &tramp, expect, kStolen, "mail/waste observer"))
    {
        Logf("mail  hook refused, game left unpatched");
        return 1;
    }
    g_origAddWaste = (AddWasteFn)tramp;
    g_lastLog.store(GetTickCount64(), std::memory_order_relaxed);

    Logf("mail  observer on: waste function at rva 0x%llX, %d direct callers, mail_per_waste %.3f. "
         "Waste is forwarded untouched.",
         (unsigned long long)(site - g_exeBase), callers, g_mailPerWaste);

    if (CfgInt("resources", 1))
    {
        // UTF-8 ini, wide captions.
        const char* keys[2] = { "caption_raw", "caption_done" };
        for (int i = 0; i < 2 && g_ini[0]; ++i)
        {
            char buf[128];
            if (GetPrivateProfileStringA("mail", keys[i], "", buf, sizeof(buf), g_ini) && buf[0])
            {
                MultiByteToWideChar(CP_UTF8, 0, buf, -1, g_mailRes[i].caption, 64);
                g_captionFromIni[i] = true;
            }
        }
        g_mailCapacity    = (float)CfgFloat("mail_capacity", 0.2);
        g_stuckMs = (uint64_t)(CfgFloat("mail_stuck_seconds", 90.0) * 1000.0);
        g_homeKgPerResident = (float)CfgFloat("mail_home_kg_per_resident", 1.5);
        if (g_ini[0]) GetPrivateProfileStringA("mail", "mail_vehicles", "", g_mailVehicles, sizeof(g_mailVehicles), g_ini);
        if (g_mailVehicles[0]) Logf("mail  buy: a mail service buys only: %s", g_mailVehicles);
        g_mailThreshold   = (float)CfgFloat("mail_threshold", 0.25);
        {
            char eras[256] = "";
            GetPrivateProfileStringA("mail", "mail_use_by_year", "1920:100 1950:300 1980:500 2000:800 2020:1200",
                                     eras, sizeof(eras), g_ini);
            g_eraReference = (float)CfgFloat("mail_use_reference", 300.0);
            int n = ParseEras(eras);
            Logf("mail  era: %d points in mail_use_by_year, raw mail measured against %.0f g", n, (double)g_eraReference);
        }
        g_happyBonus      = (float)CfgFloat("mail_happy_bonus", 0.02);
        g_happyDebuff     = (float)CfgFloat("mail_happy_debuff", 0.03);
        g_happyRange      = (float)CfgFloat("mail_happy_range", 0.25);
        g_decayPerDay     = (float)CfgFloat("mail_decay_per_day", 0.0025);
        g_mailMinBatch    = (float)CfgFloat("mail_min_batch", 0.25);
        g_tradeMargin     = (float)CfgFloat("mail_trade_margin", 1.0);
        g_tradeRawDays    = (float)CfgFloat("mail_trade_raw_days", 40.0);
        InstallResourceIcons();
        InstallResources();     // its failure leaves the rest running
        InstallPicker();        // needs the resources, so only with them
        InstallMailStorages();
        InstallLivingRows();
        InstallHomeNeed();
        InstallStuckWatch();
        if (CfgInt("mail_foot_paths", 1)) InstallFootPaths();
        InstallRestock();
        InstallPersonWindow();
        InstallRowCaptions();
        InstallWindowWarnings();
    }

    if (CfgInt("dispatch", 0))
    {
        if (g_ini[0])
        {
            char buf[64];
            if (GetPrivateProfileStringA("mail", "vehicle_name", "", buf, sizeof(buf), g_ini) && buf[0])
                strncpy_s(g_vehicleName, sizeof(g_vehicleName), buf, _TRUNCATE);
            if (GetPrivateProfileStringA("mail", "resource_name", "", buf, sizeof(buf), g_ini) && buf[0])
                strncpy_s(g_resourceName, sizeof(g_resourceName), buf, _TRUNCATE);
        }
        g_maxOrders = CfgInt("max_orders", 3);
        g_maxStops  = CfgInt("max_stops", 1);
        InstallDispatch();      // its failure leaves the observer running
    }

    // Zero keeps the plugin loaded. Every non-zero return above means "nothing
    // hooked, unload me", which is only true before the hook goes in.
    return 0;
}
