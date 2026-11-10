// WRSR Mail - a postal service for Workers & Resources: Soviet Republic
// Copyright (C) 2026 TheRealMeowMeow00
// SPDX-License-Identifier: GPL-3.0-only
//
// Offline test of the observer: no game code runs. SOVIET64.exe is mapped as an
// image only to check the signature against the real bytes.
//
// Usage: test_observer.exe <path to SOVIET64.exe>

#include "../plugins/mail/mail.cpp"

#include <array>
#include <map>
#include <string>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (c) { printf("ok    %s\n", #c); } else { printf("FAIL  %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

static void TestLog(const char* fmt, ...)
{
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n");
}

// A stand-in for the game function: records exactly what it was handed.
static void* g_seenGame; static void* g_seenBuilding; static WasteList* g_seenList; static float g_seenFactor; static int g_seenCalls;
static void FakeOriginal(void* game, void* building, WasteList* list, float factor)
{
    g_seenGame = game; g_seenBuilding = building; g_seenList = list; g_seenFactor = factor; ++g_seenCalls;
}

static int g_hookInstalls; static size_t g_hookStolen;
static int FakeInstallOk(void* target, void* detour, void** trampoline, const unsigned char* expect, size_t stolen, const char*)
{
    if (memcmp(target, expect, stolen) != 0 || detour != (void*)AddWasteDetour) return 0;
    *trampoline = (void*)FakeOriginal;
    // The test exe has no ini, so the default of dispatch = 0 applies here.
    g_hookStolen = stolen;
    ++g_hookInstalls;
    return 1;
}
static int FakeInstallRefuse(void*, void*, void**, const unsigned char*, size_t, const char*) { return 0; }

// ---- fakes for the dispatch prototype ----------------------------------------

static std::vector<BYTE> g_fakeStops;
static std::vector<std::string> g_seq;
static std::vector<void*> g_stopBuildings;
static std::vector<ResPair> g_stopResources;
static bool g_failStart;

static void FakeClear(BYTE* v, char)
{
    g_seq.push_back("clear");
    *(BYTE**)(v + kVehStopBegin) = g_fakeStops.data();
    *(BYTE**)(v + kVehStopEnd)   = g_fakeStops.data();
    g_stopBuildings.clear();
}
static char FakeAdd(BYTE* v, BYTE* b, int pos, char flag)
{
    g_seq.push_back("add");
    if (pos != -1 || flag != 0) return 0;
    *(BYTE**)(v + kVehStopEnd) = *(BYTE**)(v + kVehStopEnd) + kStopStride;
    g_stopBuildings.push_back(b);
    return 1;
}
static void FakeRes(void*, ResPair* p) { g_seq.push_back("res"); g_stopResources.push_back(*p); }
static char FakeStart(BYTE*, char, char, char) { g_seq.push_back("start"); return g_failStart ? 0 : 1; }
static void FakeAbort(BYTE*) { g_seq.push_back("abort"); }
static int  g_tickCalls;
static void FakeTick(void*, BYTE*) { ++g_tickCalls; }

struct FakeBuilding
{
    BYTE b[0x1000] = {};
    BYTE s[kStorStride] = {};
    StoredRes r[1] = {};
    BYTE type[0x400] = {};
    void init(BYTE* res, float amount, float cap, int cls, bool import, bool exportf, int typeNo)
    {
        r[0].resource = res; r[0].amount = amount;
        *(StoredRes**)(s + 0) = r; *(StoredRes**)(s + 8) = r + 1;
        *(float*)(s + kStorCapacity) = cap;
        *(int*)(s + kStorClass) = cls;
        s[kStorImport] = import; s[kStorExport] = exportf;
        *(BYTE**)(b + kBldStorageBegin) = s;
        *(BYTE**)(b + kBldStorageEnd)   = s + kStorStride;
        *(int*)(type + kTypeNumberOff) = typeNo;
        *(BYTE**)(b + kBuildingTypeOff) = type;
    }
};

static int g_fitsOrigCalls;
static unsigned long long FakeFits(void*, BYTE*, BYTE*, char a, void* b)
{
    ++g_fitsOrigCalls;
    return (a == 1 && b == NULL) ? 7 : 0;
}

static uint64_t TotalCalls() { uint64_t n = 0; for (auto& c : g_calls) n += c.load(); return n; }

// ---- a stand-in for the game's build menu: the tab vector and its functions ----------
typedef std::vector<std::array<BYTE, 0x30>> FakeItems;
static std::vector<std::array<BYTE, 0xB0>> g_fakeTabs;
static std::map<std::string, BYTE*> g_fakeTools;
static bool g_fakeColorAligned = true;

static size_t FakeTabCount(void*) { return g_fakeTabs.size(); }
static BYTE*  FakeTabAt(void*, size_t i) { return g_fakeTabs.at(i).data(); }
static void*  FakeCtor(void* rec) { return rec; }
static void   FakeDtor(void*) {}
static void   FakeTabPush(void*, void* rec)
{
    std::array<BYTE, 0xB0> a;
    memcpy(a.data(), rec, a.size());
    g_fakeTabs.push_back(a);
}
static void FakeTabInit(BYTE* tab, const char* name, int textId, int group, const float* color)
{
    if ((ULONG_PTR)color % 16) g_fakeColorAligned = false;   // the game reads it with movaps
    strcpy((char*)tab, name);
    *(int*)(tab + 0x48) = textId;
    *(int*)(tab + kTabGroup) = group;
    memcpy(tab + kTabColor, color, 16);
}
static size_t FakeItemCount(void* vec) { FakeItems* p = *(FakeItems**)vec; return p ? p->size() : 0; }
static BYTE*  FakeItemAt(void* vec, size_t i) { return (*(FakeItems**)vec)->at(i).data(); }
static void   FakeItemPush(void* vec, void* rec)
{
    FakeItems*& p = *(FakeItems**)vec;
    if (!p) p = new FakeItems;
    std::array<BYTE, 0x30> a;
    memcpy(a.data(), rec, a.size());
    p->push_back(a);
}
static void FakeItemInit(BYTE* item, void* tool, int textId, int)
{
    *(void**)item = tool;
    *(int*)(item + 0x20) = textId;
}
static void* FakeFindTool(void*, const char* name)
{
    auto it = g_fakeTools.find(name);
    return it == g_fakeTools.end() ? NULL : it->second;
}
static void FakeIntPush(void* vec, const int* value)
{
    std::vector<int>*& p = *(std::vector<int>**)vec;
    if (!p) p = new std::vector<int>;
    p->push_back(*value);
}
static void FakeMenu(void)
{
    static const int groups[26] = { 0,0,0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4,4,4, 5,5 };
    g_fakeTabs.clear();
    for (int i = 0; i < 26; ++i)
    {
        std::array<BYTE, 0xB0> a = {};
        sprintf((char*)a.data(), "t%d", i);
        *(int*)(a.data() + kTabGroup) = groups[i];
        float color[4] = { 1.0f, 1.0f, 1.0f, 0.5f + i };
        memcpy(a.data() + kTabColor, color, 16);
        g_fakeTabs.push_back(a);
    }
    g_tabCount = FakeTabCount;  g_tabAt = FakeTabAt;  g_tabCtor = FakeCtor;  g_tabDtor = FakeDtor;
    g_tabPush = FakeTabPush;    g_tabInit = FakeTabInit;
    g_itemCount = FakeItemCount; g_itemAt = FakeItemAt; g_itemCtor = FakeCtor; g_itemDtor = FakeDtor;
    g_itemPush = FakeItemPush;  g_itemInit = FakeItemInit;
    g_findTool = FakeFindTool;  g_intPush = FakeIntPush;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s SOVIET64.exe\n", argv[0]); return 2; }

    static TsmHost host;
    host.log = TestLog;
    H = &host;
    g_logMs = 0;
    g_origAddWaste = FakeOriginal;

    // A fake building and type descriptor at the real offsets.
    static BYTE fakeType[0x400];  *(int*)(fakeType + kTypeNumberOff) = 2;
    static BYTE fakeBuilding[0x400];  *(void**)(fakeBuilding + kBuildingTypeOff) = fakeType;
    int gameTag = 0;

    // 1. The detour forwards everything unchanged and leaves the list intact.
    std::vector<WasteEntry> items = { {(void*)0x10, 0.5f, 0.0f}, {(void*)0x20, 1.5f, 0.0f}, {(void*)0x30, -1.0f, 0.0f} };
    WasteList list = { items.data(), items.data() + items.size(), items.data() + items.size() };
    std::vector<WasteEntry> before = items;
    WasteList listBefore = list;

    AddWasteDetour(&gameTag, fakeBuilding, &list, 2.0f);

    CHECK(g_seenCalls == 1);
    CHECK(g_seenGame == &gameTag);
    CHECK(g_seenBuilding == fakeBuilding);
    CHECK(g_seenList == &list);
    CHECK(g_seenFactor == 2.0f);
    CHECK(memcmp(&list, &listBefore, sizeof(list)) == 0);
    CHECK(memcmp(items.data(), before.data(), items.size() * sizeof(WasteEntry)) == 0);

    // 2. It measures positive amounts times the factor, in the living bucket.
    CHECK(g_calls[2].load() == 1);
    CHECK(g_waste[2].load() == (uint64_t)(4.0 * kFixed));

    // 3. factor <= 0 is not applied, as in the game's normal path.
    AddWasteDetour(&gameTag, fakeBuilding, &list, 0.0f);
    CHECK(g_waste[2].load() == (uint64_t)(6.0 * kFixed));
    CHECK(g_seenFactor == 0.0f);

    // 4. Odd input is rejected, and the original still runs with it.
    uint64_t callsBefore = TotalCalls();
    int seenBefore = g_seenCalls;
    AddWasteDetour(&gameTag, fakeBuilding, NULL, 1.0f);
    CHECK(g_seenCalls == seenBefore + 1);
    CHECK(g_seenList == NULL);
    WasteList bad = { items.data() + 1, items.data(), items.data() };
    AddWasteDetour(&gameTag, fakeBuilding, &bad, 1.0f);
    CHECK(g_seenCalls == seenBefore + 2);
    WasteList huge = { items.data(), items.data() + kMaxEntries + 1, items.data() };
    AddWasteDetour(&gameTag, fakeBuilding, &huge, 1.0f);
    CHECK(g_seenCalls == seenBefore + 3);
    CHECK(g_rejected.load() == 3);
    CHECK(TotalCalls() == callsBefore);

    // 5. An empty list counts as an event with no waste, a null building and a
    //    large type number land in the overflow bucket.
    WasteList empty = { items.data(), items.data(), items.data() };
    AddWasteDetour(&gameTag, NULL, &empty, 1.0f);
    CHECK(g_calls[kTypeBuckets - 1].load() == 1);
    *(int*)(fakeType + kTypeNumberOff) = 9999;
    AddWasteDetour(&gameTag, fakeBuilding, &empty, 1.0f);
    CHECK(g_calls[kTypeBuckets - 1].load() == 2);

    // 6. NaN is not counted.
    *(int*)(fakeType + kTypeNumberOff) = 6;
    union { uint32_t u; float f; } nan; nan.u = 0x7FC00000;
    std::vector<WasteEntry> nanItems = { {(void*)0x10, nan.f, 0.0f} };
    WasteList nanList = { nanItems.data(), nanItems.data() + 1, nanItems.data() + 1 };
    AddWasteDetour(&gameTag, fakeBuilding, &nanList, 1.0f);
    CHECK(g_calls[6].load() == 1 && g_waste[6].load() == 0);

    // 6b. The dispatch prototype, on fake game structures.
    {
        static BYTE resCovered[0x400];
        strcpy((char*)resCovered, "eletronics");
        *(float*)(resCovered + kResClassFactor) = 1.0f;      // class 0
        static BYTE resOther[0x400];
        strcpy((char*)resOther, "food");
        *(float*)(resOther + kResClassFactor) = 1.0f;

        static FakeBuilding src, dst, wasteBld;
        src.init(resCovered, 8.0f, 10.0f, 0, false, true, 5);   // export, 80% full
        dst.init(resCovered, 1.0f, 10.0f, 0, true, false, 3);   // import, 10% full
        wasteBld.init(resCovered, 9.0f, 10.0f, 17, false, false, 2);  // wrong class

        static BYTE office[0x2000], vt[0x9000], vtOther[0x9000], veh[0x2000], vehOther[0x2000];
        strcpy((char*)vt + kVtName, "covered_b1000");
        *(int*)(vt + kVtClass) = 0;
        strcpy((char*)vtOther + kVtName, "garbage_gz53m");
        *(int*)(vtOther + kVtClass) = 17;
        *(BYTE**)(veh + kVehRecord) = vt;
        *(BYTE**)(veh + kVehOwner) = office;
        *(BYTE**)(veh + kVehParkedAt) = office;
        *(BYTE**)(vehOther + kVehRecord) = vtOther;
        *(BYTE**)(vehOther + kVehOwner) = office;
        *(BYTE**)(vehOther + kVehParkedAt) = office;
        BYTE* wasteBefore = (BYTE*)malloc(sizeof(vehOther)); memcpy(wasteBefore, vehOther, sizeof(vehOther));

        BYTE* vehicles[2] = { vehOther, veh };
        *(BYTE***)(office + kOffVehBegin) = vehicles;
        *(BYTE***)(office + kOffVehEnd)   = vehicles + 2;
        BYTE* targets[3] = { wasteBld.b, src.b, dst.b };
        *(BYTE***)(office + kOffTgtBegin) = targets;
        *(BYTE***)(office + kOffTgtEnd)   = targets + 3;

        g_fakeStops.assign(kStopStride * 8, 0);
        g_clearRoute = FakeClear; g_addStop = FakeAdd; g_addResource = FakeRes;
        g_startRoute = FakeStart; g_abortRoute = FakeAbort;
        g_origOfficeTick = FakeTick;
        g_maxOrders = 1;

        BYTE* from; BYTE* to; void* res;
        CHECK(PickRoute(office, 0, &from, &to, &res));
        CHECK(from == src.b && to == dst.b && res == resCovered);
        CHECK(!PickRoute(office, 17, &from, &to, &res));   // nothing to carry for that class

        OfficeTickDetour(NULL, office);
        CHECK(g_tickCalls == 1);        // the game's own tick always runs, first
        CHECK(g_ordersIssued == 1);
        std::string seq; for (auto& c : g_seq) seq += c + " ";
        printf("sequence: %s\n", seq.c_str());
        CHECK(seq == "clear add res add add start ");
        CHECK(g_stopBuildings.size() == 3 && g_stopBuildings[0] == src.b &&
              g_stopBuildings[1] == dst.b && g_stopBuildings[2] == office);
        CHECK(g_stopResources.size() == 1 && g_stopResources[0].resource == resCovered);
        BYTE* s0 = g_fakeStops.data();
        CHECK(s0[kStopLoad] == 1 && s0[kStopUnload] == 0 && *(float*)(s0 + kStopWeight) == 1.0f);
        BYTE* s1 = s0 + kStopStride;
        CHECK(s1[kStopLoad] == 0 && s1[kStopUnload] == 1);
        BYTE* s2 = s1 + kStopStride;
        CHECK(s2[kStopLoad] == 0 && s2[kStopUnload] == 0);

        // The fake route has ended: no stops left, so the truck counts as free again.
        *(BYTE**)(veh + kVehStopEnd) = *(BYTE**)(veh + kVehStopBegin);

        // The garbage truck and the waste-class storage were left alone.
        CHECK(memcmp(wasteBefore, vehOther, sizeof(vehOther)) == 0);
        CHECK(wasteBld.r[0].amount == 9.0f);

        // The limit holds.
        g_seq.clear();
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.empty() && g_tickCalls == 2);

        // A busy vehicle is skipped.
        g_ordersIssued = 0; g_maxOrders = 5; g_seq.clear();
        *(float*)(veh + kVehCooldown) = 3.0f;
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.empty());
        *(float*)(veh + kVehCooldown) = 0.0f;
        *(void**)(veh + kVehBusyA) = office;
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.empty());
        *(void**)(veh + kVehBusyA) = NULL;
        *(void**)(veh + kVehParkedAt) = NULL;
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.empty());
        *(BYTE**)(veh + kVehParkedAt) = office;

        // A refused start clears the route and counts nothing.
        g_failStart = true;
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.back() == "clear" && g_seq[g_seq.size() - 2] == "abort");
        CHECK(g_ordersIssued == 0 && *(float*)(veh + kVehCooldown) == kNoWorkCooldown);
        g_failStart = false;
        *(float*)(veh + kVehCooldown) = 0.0f;

        // No source or target: a cooldown, not an order.
        g_seq.clear();
        *(BYTE***)(office + kOffTgtEnd) = targets + 1;
        OfficeTickDetour(NULL, office);
        CHECK(g_seq.empty() && *(float*)(veh + kVehCooldown) == kNoWorkCooldown);
    }

    // 6c. The vehicle check: only the selected vehicle at a technical office changes.
    {
        static FakeBuilding office, factory;
        static BYTE dummyRes[0x400];
        office.init(dummyRes, 0, 1, 0, false, false, kTypeTechOffice);
        factory.init(dummyRes, 0, 1, 0, false, false, 6);
        static BYTE zil[0x9000], garbage[0x9000];
        strcpy(g_vehicleName, "zil_130_mail_service");
        strcpy((char*)zil + kVtName, "x/zil_130_mail_service");
        strcpy((char*)garbage + kVtName, "garbage_gz53m");
        g_origVehicleFits = FakeFits;
        g_fitsOrigCalls = 0;

        CHECK(VehicleFitsDetour(NULL, office.b, zil, 1, NULL) == 1);
        CHECK(g_fitsOrigCalls == 0);
        CHECK(VehicleFitsDetour(NULL, office.b, garbage, 1, NULL) == 7);    // the game decides
        CHECK(VehicleFitsDetour(NULL, factory.b, zil, 1, NULL) == 7);       // the game decides
        CHECK(VehicleFitsDetour(NULL, NULL, zil, 1, NULL) == 7);            // never dereferenced
        CHECK(g_fitsOrigCalls == 3);
    }

    // 6d. The resources: published once, after the base records, only with room.
    {
        const int kCap = 63;
        std::vector<BYTE> arr(kCap * kResStride, 0);
        std::vector<BYTE> game(kGameResVector + 64, 0);
        BYTE** vec = (BYTE**)(game.data() + kGameResVector);
        BYTE* begin = arr.data();
        vec[0] = begin; vec[1] = begin; vec[2] = begin + kCap * kResStride;
        strcpy((char*)begin + 7 * kResStride, "x");       // not yet published
        g_lastResEnd = NULL; g_resArmed = false; g_resRefusedLogged = false;

        ArmMailResources(game.data());
        CHECK(vec[1] == begin);                            // waits for the base records

        // The base records appear, the template among them.
        for (int i = 0; i < 57; ++i) sprintf((char*)begin + i * kResStride, "res%d", i);
        strcpy((char*)begin + 42 * kResStride, "eletronics");
        memset(begin + 42 * kResStride + kResName + 32, 0x5A, 8);    // marks copied content
        *(int*)(begin + 42 * kResStride + kResCaption) = 999;
        *(float*)(begin + 42 * kResStride + kResClassFactor) = 0.25f;
        vec[1] = begin + 57 * kResStride;

        ArmMailResources(game.data());
        CHECK(vec[1] == begin + 59 * kResStride);
        BYTE* rawRec = begin + 57 * kResStride;
        BYTE* doneRec = begin + 58 * kResStride;
        CHECK(strcmp((char*)rawRec, "mail_raw") == 0 && strcmp((char*)doneRec, "mail_done") == 0);
        CHECK(*(int*)(rawRec + kResCaption) == kMailTextBase && *(int*)(doneRec + kResCaption) == kMailTextBase + 1);
        CHECK(*(float*)(rawRec + kResClassFactor) == 0.25f);
        CHECK(*(int*)(begin + 42 * kResStride + kResCaption) == 999);    // the template is untouched

        // Idempotent.
        ArmMailResources(game.data());
        CHECK(vec[1] == begin + 59 * kResStride);

        // A map load clears the vector, and it fills up again.
        vec[1] = begin;
        ArmMailResources(game.data());
        CHECK(vec[1] == begin);
        vec[1] = begin + 57 * kResStride;
        ArmMailResources(game.data());
        CHECK(vec[1] == begin + 59 * kResStride);

        // No room: nothing is written past the capacity.
        vec[1] = begin + 62 * kResStride;
        memset(begin + 57 * kResStride, 0, 5 * kResStride);
        g_resArmed = false; g_lastResEnd = NULL;
        ArmMailResources(game.data());
        CHECK(vec[1] == begin + 62 * kResStride);
        CHECK(g_resRefusedLogged);
    }

    // 6e. Captions: ours are answered, every other id goes to the game.
    {
        g_origGetString = [](void*, int) -> wchar_t* { static wchar_t s[] = L"game"; return s; };
        CHECK(wcscmp(GetStringDetour(NULL, kMailTextBase + 1), L"Processed mail") == 0);
        CHECK(wcscmp(GetStringDetour(NULL, 0x100E), L"game") == 0);
    }

    // 6f. What mail does at home: a move once per game day, within a band around the game's
    //     limit, and nothing at all where no service covers the house.
    {
        static BYTE doneRes[0x400];
        strcpy((char*)doneRes, "mail_done");
        static FakeBuilding home;
        home.init(doneRes, 0.01f, 0.6f, 0, true, false, 2);
        *(float*)(home.b + kBldQuality) = 0.8f;
        static BYTE person[0x750] = {};
        *(BYTE**)(person + kPersonBuilding) = home.b;
        *(float*)(person + kPersonHappiness) = 0.7f;
        static BYTE game[0x1000] = {};
        *(int*)(game + kGameDifficulty) = 2;
        g_resDone = doneRes;
        g_happyBonus = 0.02f; g_happyDebuff = 0.03f; g_happyRange = 0.25f;
        memset(g_personSeen, 0, sizeof(g_personSeen));
        g_gameDay = 1;

        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) > 0.719f && *(float*)(person + kPersonHappiness) < 0.721f);
        CHECK(IsHome(home.b));
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) < 0.721f);      // once per day

        // No mail and no service: nothing.
        home.r[0].amount = 0.0f;
        g_gameDay = 2;
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) > 0.719f);
        CHECK(g_homeNeutral == 1);

        // No mail, a service covers it: down, and the band holds.
        g_mailMade = 1;
        MarkServed(home.b, GetTickCount64());
        g_gameDay = 3;
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) < 0.691f);
        *(float*)(person + kPersonHappiness) = 0.605f;
        g_gameDay = 4;
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) > 0.599f);      // limit 0.8 - 25%
        CHECK(g_homeUnhappy == 2);

        // Top of the band.
        home.r[0].amount = 0.1f;
        *(float*)(person + kPersonHappiness) = 0.995f;
        g_gameDay = 5;
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) < 1.001f);      // limit 0.8 + 25%

        // A tourist is left alone.
        *(int*)(person + kPersonKind) = 1;
        *(float*)(person + kPersonHappiness) = 0.5f;
        g_gameDay = 6;
        HomeNeedTick(game, person);
        CHECK(*(float*)(person + kPersonHappiness) == 0.5f);
        *(int*)(person + kPersonKind) = 0;

        // Thousands of people keep their own record: nobody pays twice in a day.
        static BYTE crowd[20000][16];
        memset(g_personSeen, 0, sizeof(g_personSeen));
        g_gameDay = 7;
        int first = 0, again = 0;
        for (auto& c : crowd) if (FirstTodayFor(c)) ++first;
        for (auto& c : crowd) if (FirstTodayFor(c)) ++again;
        CHECK(first > 19900);
        CHECK(again == 0);

        memset(g_personSeen, 0, sizeof(g_personSeen));
        g_resDone = NULL;
        g_gameDay = 0;
        g_mailMade = 0;
    }

    // 6g. The trade margin is what the factory's labour earns, plus the configured share.
    {
        static BYTE recs[3][kResStride];
        BYTE* raw = recs[0]; BYTE* done = recs[1];
        static BYTE worker[kResStride], other[kResStride];
        *(float*)(worker + kResBuyRub) = 5.0f;
        *(float*)(done + kResSellMul) = 0.95f;
        *(float*)(raw + kResBuyMul) = 1.05f;
        g_resRaw = raw; g_resDone = done; g_resWorkers = worker;
        g_tradeRawDays = 40.0f; g_tradeMargin = 0.25f; g_tradeLabour = 40.0f;   // explicit: the sum is the point
        WritePrices(kResBuyRub, kResSellRub);
        // What processing earns: customs pays these as they are, seen in the game (see mail.cpp).
        float earned = *(float*)(done + kResSellRub) - *(float*)(raw + kResBuyRub);
        CHECK(earned > 249.0f && earned < 251.0f);                  // 200 wages + 25%
        CHECK(*(float*)(raw + kResBuyRub) > 199.0f && *(float*)(raw + kResBuyRub) < 201.0f);
        // Neither can be turned around at customs: buying and selling back loses the gap.
        CHECK(*(float*)(raw + kResSellRub) < *(float*)(raw + kResBuyRub));
        CHECK(*(float*)(done + kResBuyRub) > *(float*)(done + kResSellRub));
        // The other currency stays alone.
        CHECK(*(float*)(raw + kResBuyUsd) == 0.0f && *(float*)(done + kResSellUsd) == 0.0f);

        // The market table shows the same prices, and only its own entry changes.
        static BYTE game[0x12000];
        struct Entry { BYTE* res; float price; float other; };
        static Entry sell[3] = { { other, 7.0f, 0.5f }, { raw, 0.0f, 0.5f }, { done, 0.0f, 0.5f } };
        *(Entry**)(game + kGameMarketSellRub) = sell;
        *(Entry**)(game + kGameMarketSellRub + 8) = sell + 3;
        WriteMarketTable(game);                                     // the other three lists are empty
        CHECK(sell[0].price == 7.0f && sell[0].other == 0.5f);
        CHECK(sell[1].price == *(float*)(raw + kResSellRub) && sell[1].other == 0.5f);
        CHECK(sell[2].price == *(float*)(done + kResSellRub) && sell[2].other == 0.5f);
        // The prices follow the wage: double it and both double, the margin too.
        float rawWas = *(float*)(raw + kResBuyRub), doneWas = *(float*)(done + kResSellRub);
        *(float*)(worker + kResBuyRub) = 10.0f;
        WritePrices(kResBuyRub, kResSellRub);
        CHECK(*(float*)(raw + kResBuyRub) == 2.0f * rawWas && *(float*)(done + kResSellRub) == 2.0f * doneWas);
        *(float*)(worker + kResBuyRub) = 5.0f;
        WritePrices(kResBuyRub, kResSellRub);
        // A list that does not look like one is left alone.
        sell[2].price = 1.0f;
        *(BYTE**)(game + kGameMarketSellRub + 8) = (BYTE*)sell + 0x15;
        WriteMarketTable(game);
        CHECK(sell[2].price == 1.0f);
        g_resRaw = NULL; g_resDone = NULL; g_resWorkers = NULL;
    }

    // 6h. Mail in storage ages everywhere, service or no service; pass-through stores do not.
    {
        static BYTE rawRes[0x400], doneRes[0x400], otherRes[0x400];
        g_resRaw = rawRes; g_resDone = doneRes; g_decayPerDay = 0.01f;
        StoredRes plain[2]  = { { otherRes, 10.0f, 0.0f }, { rawRes, 10.0f, 0.0f } };
        StoredRes own[1]    = { { doneRes, 100.0f, 0.0f } };
        StoredRes passed[1] = { { rawRes, 5.0f, 0.0f } };
        static BYTE stores[3 * kStorStride];
        BYTE* s0 = stores, *s1 = stores + kStorStride, *s2 = stores + 2 * kStorStride;
        *(StoredRes**)s0 = plain;  *(StoredRes**)(s0 + 8) = plain + 2;
        *(StoredRes**)s1 = own;    *(StoredRes**)(s1 + 8) = own + 1;
        *(StoredRes**)s2 = passed; *(StoredRes**)(s2 + 8) = passed + 1;
        s2[kStorImport] = 1;
        *(float*)(s0 + kStorCapacity) = 300.0f;
        *(float*)(s1 + kStorCapacity) = 300.0f;
        *(float*)(s2 + kStorCapacity) = 20.0f;
        static BYTE store[0x1000];
        *(BYTE**)(store + kBldStorageBegin) = stores;
        *(BYTE**)(store + kBldStorageEnd)   = stores + sizeof(stores);
        BYTE* list[2] = { NULL, store };
        static BYTE game[0x13000];
        *(BYTE***)(game + kGameBuildings)     = list;
        *(BYTE***)(game + kGameBuildings + 8) = list + 2;
        g_gameDay = 1; g_decayDay = 0; g_mailDecayed = 0;

        DecayAllStorage(game);
        CHECK(plain[0].amount == 10.0f);                            // not mail
        CHECK(plain[1].amount > 9.89f && plain[1].amount < 9.91f);  // beside other goods
        CHECK(own[0].amount > 98.9f && own[0].amount < 99.1f);      // nobody's depot
        CHECK(passed[0].amount == 5.0f);                            // a factory's input
        DecayAllStorage(game);
        CHECK(plain[1].amount > 9.89f);                             // once per day
        g_gameDay = 2;
        DecayAllStorage(game);
        CHECK(plain[1].amount < 9.81f);

        // A house's small store with its flag misread still keeps its mail.
        float before = own[0].amount;
        *(float*)(s1 + kStorCapacity) = 0.6f;
        g_gameDay = 3;
        DecayAllStorage(game);
        CHECK(own[0].amount == before);

        // A list that makes no sense is left alone.
        before = plain[1].amount;
        *(BYTE***)(game + kGameBuildings + 8) = list - 1;
        g_gameDay = 4;
        DecayAllStorage(game);
        CHECK(plain[1].amount == before);

        g_resRaw = NULL; g_resDone = NULL; g_gameDay = 0; g_decayDay = 0;
    }

    // 6i. A mail day is a change of date, and the first look is not a day.
    {
        static BYTE game[0x1000];
        g_gameDay = 0; g_calendarDay = -1; g_calendarYear = -1;
        *(int*)(game + 0x590) = 100; *(int*)(game + 0x594) = 1950;
        TickClock(game);
        CHECK(g_gameDay == 0);
        TickClock(game);
        CHECK(g_gameDay == 0);
        *(int*)(game + 0x590) = 101;
        TickClock(game);
        CHECK(g_gameDay == 1);
        *(int*)(game + 0x590) = 0; *(int*)(game + 0x594) = 1951;
        TickClock(game);
        CHECK(g_gameDay == 2);
        g_gameDay = 0; g_calendarDay = -1; g_calendarYear = -1;
    }

    // 6j. Which offices are mail services, which vehicles serve them, what they carry.
    {
        static BYTE type[0x400], office[0x2000], rows[3 * kOffRowStride];
        *(int*)(type + kTypeNumberOff) = kTypeTechOffice;
        *(BYTE**)(office + kBuildingTypeOff) = type;
        *(BYTE**)(office + kOffRowBegin) = rows;
        *(BYTE**)(office + kOffRowEnd) = rows + 2 * kOffRowStride;
        CHECK(IsMailOffice(office));
        *(BYTE**)(office + kOffRowEnd) = rows + 3 * kOffRowStride;
        CHECK(!IsMailOffice(office));                               // the game's own offices
        *(BYTE**)(office + kOffRowEnd) = rows + 2 * kOffRowStride;
        *(int*)(type + kTypeNumberOff) = 7;
        CHECK(!IsMailOffice(office));                               // two rows, no office

        static BYTE vt[0x9000];
        *(int*)(vt + kVtKind) = kVtKindRoad;
        *(int*)(vt + kVtClass) = kVtClassCovered;
        CHECK(CoveredTruckType(vt));
        *(int*)(vt + kVtClass) = 17;
        CHECK(!CoveredTruckType(vt));                               // a garbage truck
        *(int*)(vt + kVtClass) = kVtClassCovered;
        *(int*)(vt + kVtKind) = 2;
        CHECK(!CoveredTruckType(vt));                               // a covered wagon

        static BYTE raw[0x400], done[0x400], other[0x400], v[0x1000];
        g_resRaw = raw; g_resDone = done;
        StoredRes cargo[2] = { { raw, 0.0f, 0.0f }, { done, 0.0f, 0.0f } };
        *(StoredRes**)(v + kVehCargoBegin) = cargo;
        *(StoredRes**)(v + kVehCargoEnd) = cargo + 2;
        CHECK(CargoKind(v) == 0);
        cargo[1].amount = 1.0f;
        CHECK(CargoKind(v) == 2);
        cargo[1].amount = 0.0f; cargo[0].amount = 1.0f;
        CHECK(CargoKind(v) == 1);
        cargo[0].resource = other;
        CHECK(CargoKind(v) == 3);
        g_resRaw = NULL; g_resDone = NULL;
    }

    // 6k. A waste warning loses its dash, and nothing else is touched.
    {
        wchar_t t1[16] = L"Power\r\n-";
        CHECK(DropWarningDash(t1));
        CHECK(wcscmp(t1, L"Power\r\n") == 0);
        wchar_t t2[16] = L"-";
        CHECK(DropWarningDash(t2));
        CHECK(t2[0] == 0);
        wchar_t t3[16] = L"Power";
        CHECK(!DropWarningDash(t3));
        CHECK(wcscmp(t3, L"Power") == 0);
        wchar_t t4[4] = { 0 };
        CHECK(!DropWarningDash(t4));
        CHECK(IsWasteWarning(0x10D6) && IsWasteWarning(0x10E3));
        CHECK(!IsWasteWarning(0x5B0) && !IsWasteWarning(0x95A) && !IsWasteWarning(0x597));
    }

    // 6l. A resident uses a fixed amount a day, and the window tells the three cases apart.
    {
        static BYTE doneRes[0x400];
        strcpy((char*)doneRes, "mail_done");
        static FakeBuilding home;
        home.init(doneRes, 0.1f, 0.6f, 0, true, false, 2);
        *(float*)(home.b + kBldQuality) = 0.8f;
        static BYTE p1[0x750] = {}, p2[0x750] = {};
        *(BYTE**)(p1 + kPersonBuilding) = home.b;
        *(BYTE**)(p2 + kPersonBuilding) = home.b;
        static BYTE game[0x1000] = {};
        *(int*)(game + kGameDifficulty) = 2;
        g_resDone = doneRes;
        g_mailPerPerson = 0.01f;
        memset(g_personSeen, 0, sizeof(g_personSeen));
        memset(g_served, 0, sizeof(g_served));
        g_gameDay = 1; g_mailMade = 0;
        float before = home.r[0].amount;
        CHECK(MailNeedPercent(p1) == 100);
        HomeNeedTick(game, p1);
        CHECK(__builtin_fabsf(before - home.r[0].amount - 0.01f) < 0.0001f);
        HomeNeedTick(game, p1);                                     // once a day
        CHECK(__builtin_fabsf(before - home.r[0].amount - 0.01f) < 0.0001f);
        HomeNeedTick(game, p2);                                     // two people, twice the use
        CHECK(__builtin_fabsf(before - home.r[0].amount - 0.02f) < 0.0001f);
        CHECK(wcsstr(MailNeedHeading(p1), L"100%") != NULL);

        home.r[0].amount = 0.005f;                                  // never below nothing
        g_gameDay = 2;
        HomeNeedTick(game, p1);
        CHECK(home.r[0].amount == 0.0f);

        CHECK(MailNeedPercent(p1) == -1);                           // no service, no mail made
        CHECK(MailNeedHeading(p1) == NULL);
        g_mailMade = 1;
        MarkServed(home.b, GetTickCount64());
        CHECK(MailNeedPercent(p1) == 0);
        CHECK(wcsstr(MailNeedHeading(p1), L", 0%") != NULL);
        *(int*)(p1 + kPersonKind) = 1;
        CHECK(MailNeedPercent(p1) == -1);                           // a tourist

        memset(g_personSeen, 0, sizeof(g_personSeen));
        memset(g_served, 0, sizeof(g_served));
        g_resDone = NULL; g_gameDay = 0; g_mailMade = 0;
    }

    // 6m. Use and raw mail follow the year, and a bad table does no harm.
    {
        CHECK(ParseEras("1920:100 1950:300 2020:1200") == 3);
        CHECK(GramsInYear(1900, 0) == 100.0f);                      // flat before
        CHECK(GramsInYear(1920, 0) == 100.0f);
        CHECK(GramsInYear(1935, 0) > 199.0f && GramsInYear(1935, 0) < 201.0f);
        CHECK(GramsInYear(1950, 0) > 299.0f && GramsInYear(1950, 0) < 301.0f);
        CHECK(GramsInYear(2100, 0) == 1200.0f);                     // flat after
        CHECK(GramsInYear(1935, 182) > GramsInYear(1935, 0));       // no steps on New Year's day

        g_eraReference = 300.0f;
        ApplyEra(2020, 0);
        CHECK(g_mailPerPerson > 0.00119f && g_mailPerPerson < 0.00121f);
        CHECK(g_mailRawFactor > 3.99f && g_mailRawFactor < 4.01f);
        ApplyEra(1920, 0);
        CHECK(g_mailRawFactor > 0.33f && g_mailRawFactor < 0.34f);

        CHECK(ParseEras("1950:300 1920:100") == 1);                 // years going back end it
        CHECK(ParseEras("rubbish") == 0);
        CHECK(ParseEras("") == 0);
        g_mailPerPerson = 0.0003f; g_mailRawFactor = 1.0f;
        ApplyEra(2020, 0);                                          // empty table: untouched
        CHECK(g_mailPerPerson == 0.0003f && g_mailRawFactor == 1.0f);
        ParseEras("");
    }

    // 6n. A building no truck reaches is left out, for a while.
    {
        static BYTE type[0x400], b1[0x1000], b2[0x1000];
        *(BYTE**)(b1 + kBuildingTypeOff) = type;
        *(BYTE**)(b2 + kBuildingTypeOff) = type;
        memset(g_badStops, 0, sizeof(g_badStops));
        CHECK(!IsBadStop(b1));
        MarkBadStop(b1, "test");
        CHECK(IsBadStop(b1));
        CHECK(!IsBadStop(b2));
        CHECK(!StopAllowed(b1, true));
        uint64_t first = 0, second = 0;
        for (auto& e : g_badStops) if (e.b == b1) first = e.until;
        MarkBadStop(b1, "test");
        for (auto& e : g_badStops) if (e.b == b1) second = e.until;
        CHECK(second > first + 300000);                             // every try costs a teleport
        for (auto& e : g_badStops) if (e.b == b1) e.until = GetTickCount64() - 1;
        CHECK(!IsBadStop(b1));                                      // roads get built
        MarkBadStop(b1, "test");
        ClearBadStop(b1);
        CHECK(!IsBadStop(b1));                                      // a truck got there
        memset(g_badStops, 0, sizeof(g_badStops));
    }

    // 6o. Only the two mail storages at the end of a building are hidden from restocking.
    {
        static BYTE raw[0x400], done[0x400], other[0x400];
        g_resRaw = raw; g_resDone = done;
        StoredRes food[1] = { { other, 1.0f, 0.0f } };
        StoredRes r[1] = { { raw, 0.0f, 0.0f } };
        StoredRes d[1] = { { done, 0.0f, 0.0f } };
        static BYTE st[3 * kStorStride];
        BYTE* s0 = st, *s1 = st + kStorStride, *s2 = st + 2 * kStorStride;
        *(StoredRes**)s0 = food; *(StoredRes**)(s0 + 8) = food + 1;
        *(StoredRes**)s1 = r;    *(StoredRes**)(s1 + 8) = r + 1;
        *(StoredRes**)s2 = d;    *(StoredRes**)(s2 + 8) = d + 1;
        *(float*)(s0 + kStorCapacity) = 70.0f;
        *(float*)(s1 + kStorCapacity) = 0.6f;
        *(float*)(s2 + kStorCapacity) = 0.6f;
        static BYTE b[0x1000];
        *(BYTE**)(b + kBldStorageBegin) = st;
        *(BYTE**)(b + kBldStorageEnd) = st + sizeof(st);
        CHECK(EndsWithMail(b));
        *(float*)(s2 + kStorCapacity) = 300.0f;                     // a warehouse's own
        CHECK(!EndsWithMail(b));
        *(float*)(s2 + kStorCapacity) = 0.6f;
        *(BYTE**)(b + kBldStorageEnd) = st + 2 * kStorStride;       // something else is last
        CHECK(!EndsWithMail(b));
        g_resRaw = NULL; g_resDone = NULL;
    }

    // 6p. Who gets the small mail storages: not a type that declares mail itself.
    {
        static BYTE raw[0x400], done[0x400], other[0x400];
        g_resRaw = raw; g_resDone = done;
        static BYTE vec[0x20], list[2 * kStorStride];
        StoredRes food[1] = { { other, 0.0f, 0.0f } };
        StoredRes mail[1] = { { done, 0.0f, 0.0f } };
        *(StoredRes**)list = food; *(StoredRes**)(list + 8) = food + 1;
        *(BYTE**)vec = list;
        *(BYTE**)(vec + 8) = list + kStorStride;
        CHECK(!DeclaresMail(vec));                                  // a house
        *(StoredRes**)(list + kStorStride) = mail; *(StoredRes**)(list + kStorStride + 8) = mail + 1;
        *(BYTE**)(vec + 8) = list + 2 * kStorStride;
        CHECK(DeclaresMail(vec));                                   // a post office
        g_resRaw = NULL; g_resDone = NULL;
    }

    // 6q. Both trips possible: turns, however much more one is worth.
    {
        g_roleTrips[0] = 5; g_roleTrips[1] = 5;
        CHECK(ChooseCollect(9.0f, 1.0f, 0.5f));                     // level: collect
        g_roleTrips[0] = 6;
        CHECK(!ChooseCollect(9.0f, 1.0f, 0.5f));                    // deliver's turn
        CHECK(ChooseCollect(9.0f, -1.0f, 0.5f));                    // nothing to deliver
        CHECK(!ChooseCollect(-1.0f, 1.0f, 0.5f));                   // nothing to collect
        g_roleTrips[0] = 0; g_roleTrips[1] = 0;
    }

    // 6s. A truck is stuck only after standing still, on one stop, with one cargo, for long enough.
    {
        struct Fake
        {
            static float* Pos(void* node, float* out)
            {
                memcpy(out, node, 3 * sizeof(float));
                return out;
            }
        };
        static BYTE truck[0x2000];
        float* pos = (float*)(truck + kVehNode);
        g_nodePosition = Fake::Pos;
        g_stuckMs = 90000;
        Watch w;
        memset(&w, 0, sizeof(w));
        pos[0] = 10.0f;
        uint64_t t = 1000;
        float clock = 0.0f;
        CHECK(!TruckStuck(w, truck, 3, 0.5f, clock += 0.1f, t));                   // first sight
        for (int i = 0; i < 89; ++i) { t += 1000; CHECK(!TruckStuck(w, truck, 3, 0.5f, clock += 0.1f, t)); }
        t += 1000;
        CHECK(TruckStuck(w, truck, 3, 0.5f, clock += 0.1f, t));                    // 90 s
        // Moving, a new stop or a change of cargo start the count again.
        pos[0] = 12.0f; t += 1000; CHECK(!TruckStuck(w, truck, 3, 0.5f, clock += 0.1f, t));
        CHECK(w.stillMs == 0);
        t += 1000; CHECK(!TruckStuck(w, truck, 3, 0.5f, clock += 0.1f, t)); CHECK(w.stillMs == 1000);
        t += 1000; CHECK(!TruckStuck(w, truck, 4, 0.5f, clock += 0.1f, t)); CHECK(w.stillMs == 0);
        t += 1000; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock += 0.1f, t)); CHECK(w.stillMs == 0);
        // A long gap between two ticks counts for little.
        t += 600000; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock += 0.1f, t)); CHECK(w.stillMs == 2000);
        // A paused game ticks on, with its timer standing: none of that counts, however long.
        {
            uint64_t before = w.stillMs;
            for (int i = 0; i < 300; ++i) { t += 1000; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock, t)); }
            CHECK(w.stillMs == before);
            // Back in play, the pause adds no more than one long gap does.
            t += 1000; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock += 0.1f, t)); CHECK(w.stillMs == before + 2000);
        }
        // Turned off, or no way to read the position.
        g_stuckMs = 0; t += 200000; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock += 0.1f, t));
        g_stuckMs = 90000; g_nodePosition = NULL; CHECK(!TruckStuck(w, truck, 4, 0.6f, clock += 0.1f, t));
    }

    // 6t. A house's mail storages follow its size; only a house, only its own.
    {
        static BYTE rawRes[0x400], doneRes[0x400], otherRes[0x400];
        *(float*)(rawRes + kResClassFactor) = 0.35f;
        *(float*)(doneRes + kResClassFactor) = 0.35f;
        g_resRaw = rawRes; g_resDone = doneRes;
        g_homeKgPerResident = 1.5f;
        float t250 = HomeMailCapacity(250.0f, 0.35f) * 0.35f;
        float t20  = HomeMailCapacity(20.0f, 0.35f) * 0.35f;
        CHECK(t250 > 0.374f && t250 < 0.376f);
        CHECK(t20 > 0.049f && t20 < 0.051f);                        // the floor
        CHECK(HomeMailCapacity(5000.0f, 0.35f) < kMailBuildingMinCapacity);  // never a depot

        static BYTE type[0x400], house[0x2000], stores[3 * kStorStride];
        StoredRes people[1] = { { otherRes, 0.0f, 0.0f } };
        StoredRes r[1] = { { rawRes, 0.0f, 0.0f } };
        StoredRes d[1] = { { doneRes, 0.0f, 0.0f } };
        BYTE* s0 = stores, *s1 = stores + kStorStride, *s2 = stores + 2 * kStorStride;
        *(StoredRes**)s0 = people; *(StoredRes**)(s0 + 8) = people + 1;
        *(StoredRes**)s1 = r;      *(StoredRes**)(s1 + 8) = r + 1;
        *(StoredRes**)s2 = d;      *(StoredRes**)(s2 + 8) = d + 1;
        *(int*)(s0 + kStorClass) = kStorClassPassengers;
        *(float*)(s0 + kStorCapacity) = 250.0f;
        *(float*)(s1 + kStorCapacity) = 0.6f;
        *(float*)(s2 + kStorCapacity) = 0.6f;
        *(BYTE**)(house + kBldStorageBegin) = stores;
        *(BYTE**)(house + kBldStorageEnd) = stores + sizeof(stores);
        *(BYTE**)(house + 0x318) = type;
        *(int*)(type + 0x360) = kBldTypeLiving;
        CHECK(BuildingType(house) == kBldTypeLiving);
        SizeHomeMail(house);
        float sized = *(float*)(s1 + kStorCapacity) * 0.35f;
        CHECK(sized > 0.374f && sized < 0.376f);
        CHECK(*(float*)(s2 + kStorCapacity) == *(float*)(s1 + kStorCapacity));
        CHECK(*(float*)(s0 + kStorCapacity) == 250.0f);             // the residents stay
        // Anything else, a house without the number, or the feature off: left alone.
        *(float*)(s1 + kStorCapacity) = 0.6f;
        *(int*)(type + 0x360) = 3; SizeHomeMail(house);
        CHECK(*(float*)(s1 + kStorCapacity) == 0.6f);
        *(int*)(type + 0x360) = kBldTypeLiving;
        *(float*)(s0 + kStorCapacity) = 0.0f; SizeHomeMail(house);
        CHECK(*(float*)(s1 + kStorCapacity) == 0.6f);
        *(float*)(s0 + kStorCapacity) = 250.0f;
        g_homeKgPerResident = 0.0f; SizeHomeMail(house);
        CHECK(*(float*)(s1 + kStorCapacity) == 0.6f);
        g_homeKgPerResident = 1.5f;
        g_resRaw = NULL; g_resDone = NULL;
    }

    // 6w. The language follows the game's own text, a caption from the ini stays.
    {
        CHECK(!HasCyrillic(L"Plastic waste"));
        CHECK(HasCyrillic(L"Пластиковые отходы"));
        CHECK(!HasCyrillic(L""));
        CHECK(!HasCyrillic(NULL));
        wchar_t raw[64], done[64], tag[64];
        const wchar_t* format = g_needFormat;
        bool from0 = g_captionFromIni[0], from1 = g_captionFromIni[1];
        wcscpy(raw, g_mailRes[0].caption); wcscpy(done, g_mailRes[1].caption); wcscpy(tag, g_rowTag);
        CHECK(wcscmp(g_rowTag, L"Depot/office") == 0);
        wcscpy(g_mailRes[0].caption, L"mine");
        g_captionFromIni[0] = true; g_captionFromIni[1] = false;
        UseRussian();
        CHECK(wcscmp(g_mailRes[0].caption, L"mine") == 0);
        CHECK(HasCyrillic(g_mailRes[1].caption));
        CHECK(HasCyrillic(g_rowRaw) && HasCyrillic(g_rowTag) && HasCyrillic(g_needFormat));
        wcscpy(g_mailRes[0].caption, raw); wcscpy(g_mailRes[1].caption, done); wcscpy(g_rowTag, tag);
        wcscpy(g_rowRaw, L"Raw mail"); wcscpy(g_rowDone, L"Processed mail");
        g_needFormat = format;
        g_captionFromIni[0] = from0; g_captionFromIni[1] = from1;
    }

    // 6v. Customs lists mail in its covered store, once, and touches no other store.
    {
        static BYTE rawRes[0x400], doneRes[0x400], otherRes[0x400];
        g_resRaw = rawRes; g_resDone = doneRes;
        static StoredRes pool[64];
        static int poolN;
        poolN = 0;
        struct Fake
        {
            static void Push(void* vec, ResPair* pair)
            {
                StoredRes** v = (StoredRes**)vec;
                int n = (int)(v[1] - v[0]);
                StoredRes* fresh = pool + poolN;
                for (int i = 0; i < n; ++i) fresh[i] = v[0][i];
                fresh[n].resource = (BYTE*)pair->resource;
                fresh[n].amount = 0.0f;
                fresh[n].pad = 0.0f;
                poolN += n + 1;
                v[0] = fresh; v[1] = fresh + n + 1; v[2] = fresh + n + 1;
            }
        };
        AddResourceFn saved = g_addResource;
        g_addResource = Fake::Push;
        static BYTE building[0x2000], stores[3 * kStorStride];
        memset(stores, 0, sizeof(stores));
        StoredRes gravel[2] = { { otherRes, 0.0f, 0.0f }, { otherRes, 0.0f, 0.0f } };
        StoredRes covered[2] = { { otherRes, 0.0f, 0.0f }, { otherRes, 1.5f, 0.0f } };
        StoredRes coveredSet[2] = { { otherRes, 0.0f, 0.0f }, { otherRes, 0.0f, 0.0f } };
        BYTE* s0 = stores, *s1 = stores + kStorStride, *s2 = stores + 2 * kStorStride;
        *(StoredRes**)s0 = gravel;  *(StoredRes**)(s0 + 8) = gravel + 2;
        *(int*)(s0 + kStorClass) = 2;
        *(StoredRes**)s1 = covered; *(StoredRes**)(s1 + 8) = covered + 2;
        *(StoredRes**)(s1 + kStorSetBegin) = coveredSet; *(StoredRes**)(s1 + kStorSetBegin + 8) = coveredSet + 2;
        *(StoredRes**)s2 = covered; *(StoredRes**)(s2 + 8) = covered + 2;   // a second covered one, flagged
        *(BYTE**)(building + kBldStorageBegin) = stores;
        *(BYTE**)(building + kBldStorageEnd) = stores + sizeof(stores);
        s2[kStorExport] = 1;
        CHECK(AddMailToCustoms(building) == 2);
        CHECK(ListsResource(s1, rawRes) && ListsResource(s1, doneRes));
        CHECK(!ListsResource(s0, rawRes));
        CHECK(!ListsResource(s2, rawRes));
        CHECK((*(StoredRes**)(s1 + 8) - *(StoredRes**)s1) == 4);
        CHECK((*(StoredRes**)(s1 + kStorSetBegin + 8) - *(StoredRes**)(s1 + kStorSetBegin)) == 4);
        CHECK((*(StoredRes**)s1)[1].amount == 1.5f);                 // what was there stays
        CHECK(AddMailToCustoms(building) == 0);                      // once
        // A store for one good is not the generic one.
        BYTE* single = stores;
        *(int*)(s0 + kStorClass) = 0;
        StoredRes one[1] = { { otherRes, 0.0f, 0.0f } };
        *(StoredRes**)single = one; *(StoredRes**)(single + 8) = one + 1;
        *(BYTE**)(building + kBldStorageEnd) = stores + kStorStride;
        CHECK(AddMailToCustoms(building) == 0);
        g_addResource = saved;
    }

    // 6u. The virtual class: an empty list takes all, a list takes what it names, parts included.
    {
        g_mailVehicles[0] = 0;
        CHECK(ListedMailVehicle("covered_rnt_agk"));
        strcpy(g_mailVehicles, "zis10covered, covered_rnt");
        CHECK(ListedMailVehicle("2493938698/zis10covered"));
        CHECK(ListedMailVehicle("covered_rnt_agk"));
        CHECK(!ListedMailVehicle("covered_b1000"));
        CHECK(!ListedMailVehicle(""));
        strcpy(g_mailVehicles, " , ");                              // nothing named, nothing fits
        CHECK(!ListedMailVehicle("covered_b1000"));
        g_mailVehicles[0] = 0;
    }

    // 7. The real game bytes: the signature is unique and the callers are there.
    HMODULE exe = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(exe != NULL);
    if (exe)
    {
        g_exeBase = (BYTE*)((ULONG_PTR)exe & ~(ULONG_PTR)3);
        CHECK(LocateText());
        BYTE* site = ScanUnique(kSigAddWaste, (int)(sizeof(kSigAddWaste) / sizeof(kSigAddWaste[0])));
        CHECK(site != NULL);
        if (site)
        {
            unsigned long long rva = (unsigned long long)(site - g_exeBase);
            printf("site rva 0x%llX, callers %d\n", rva, CountCallers(site));
            CHECK(rva == 0x1990C0);
            CHECK(CountCallers(site) == 7);
            // The stolen bytes end on the boundary after `push rdi`.
            CHECK(kStolen == 14 && site[11] == 0x55 && site[12] == 0x56 && site[13] == 0x57 && site[14] == 0x41);
        }

        // The dispatch prototype's sites.
        // The road search: found, and what the write leaves is the intended instruction.
        BYTE* serviceClass = ScanUnique(kSigServiceClass, (int)(sizeof(kSigServiceClass) / sizeof(kSigServiceClass[0])));
        CHECK(serviceClass != NULL && (unsigned long long)(serviceClass - g_exeBase) == 0x67EB43);
        if (serviceClass)
        {
            static BYTE copy[64];
            memcpy(copy, serviceClass, sizeof(copy));
            CHECK(copy[kServiceClassLea] == 0x8D && copy[kServiceClassLimit] == 0x01);
            CHECK(WriteServiceClass(copy));
            CHECK(copy[kServiceClassLea] == 0x8B && copy[kServiceClassLea + 1] == 0xC1 && copy[kServiceClassLea + 2] == 0x90);
            CHECK(copy[kServiceClassLea + 3] == 0x83 && copy[kServiceClassLea + 4] == 0xF8 && copy[kServiceClassLimit] == 0x02);
            CHECK(copy[kServiceClassLimit + 1] == 0x77 && copy[kServiceClassLimit + 2] == 0x07);
            CHECK(memcmp(copy, serviceClass, kServiceClassLea) == 0);
            CHECK(memcmp(copy + kServiceClassLimit + 1, serviceClass + kServiceClassLimit + 1, 10) == 0);
        }
        BYTE* sendHome = ScanUnique(kSigSendHome, (int)(sizeof(kSigSendHome) / sizeof(kSigSendHome[0])));
        CHECK(sendHome != NULL && (unsigned long long)(sendHome - g_exeBase) == 0x3FBDC0);
        BYTE* tick = ScanUnique(kSigOfficeTick, (int)(sizeof(kSigOfficeTick) / sizeof(kSigOfficeTick[0])));
        CHECK(tick != NULL);
        if (tick)
        {
            CHECK((unsigned long long)(tick - g_exeBase) == 0x1C3030);
            // rbp rbx rsi rdi r12 r13 end at 15, and r14 starts the next instruction.
            CHECK(tick[7] == 0x55 && tick[11] == 0x41 && tick[12] == 0x54 && tick[13] == 0x41 && tick[14] == 0x55 && tick[15] == 0x41 && tick[16] == 0x56);
        }
        BYTE* fits = ScanUnique(kSigVehicleFits, (int)(sizeof(kSigVehicleFits) / sizeof(kSigVehicleFits[0])));
        CHECK(fits != NULL);
        if (fits)
        {
            CHECK((unsigned long long)(fits - g_exeBase) == 0x3E2900);
            CHECK(CountCallers(fits) == 13);
            CHECK(kStolenFits == 15 && fits[15] == 0x56);    // the next instruction starts there
        }
        BYTE* pick = ScanUnique(kSigAssignPick, (int)(sizeof(kSigAssignPick) / sizeof(kSigAssignPick[0])));
        CHECK(pick != NULL);
        if (pick) CHECK((unsigned long long)(pick - g_exeBase) == 0x1D51A0);
        BYTE* accept = ScanUnique(kSigWasteAccept, (int)(sizeof(kSigWasteAccept) / sizeof(kSigWasteAccept[0])));
        CHECK(accept != NULL);
        if (accept) CHECK((unsigned long long)(accept - g_exeBase) == 0x19CBF0);

        {
            // A dedicated mail storage counts, a storage listing many resources does not.
            static BYTE bld[0x1000], stor[kStorStride * 2];
            static char mailRes[64] = "mail_raw", otherRes[64] = "eletronics";
            StoredRes one[1]  = { { (BYTE*)mailRes, 0.0f, 0.0f } };
            StoredRes many[2] = { { (BYTE*)otherRes, 0.0f, 0.0f }, { (BYTE*)mailRes, 0.0f, 0.0f } };
            *(float*)(stor + kStorCapacity) = 300.0f;
            *(BYTE**)(bld + kBldStorageBegin) = stor;
            *(BYTE**)(bld + kBldStorageEnd)   = stor + kStorStride;
            *(StoredRes**)(stor + 0) = many; *(StoredRes**)(stor + 8) = many + 2;
            CHECK(!IsMailBuilding(bld));
            *(StoredRes**)(stor + 0) = one; *(StoredRes**)(stor + 8) = one + 1;
            CHECK(IsMailBuilding(bld));
            *(float*)(stor + kStorCapacity) = 0.2f;
            CHECK(!IsMailBuilding(bld));      // a house's own small storage
            *(float*)(stor + kStorCapacity) = 300.0f;
            one[0].resource = (BYTE*)otherRes;
            CHECK(!IsMailBuilding(bld));
            *(BYTE**)(bld + kBldStorageEnd) = stor;
            CHECK(!IsMailBuilding(bld));
        }

        BYTE* binPush = ScanUnique(kSigBinPush, (int)(sizeof(kSigBinPush) / sizeof(kSigBinPush[0])));
        CHECK(binPush != NULL);
        if (binPush)
        {
            CHECK((unsigned long long)(binPush + 14 - g_exeBase) == 0x11B05D);
            CHECK(memcmp(CallTarget(binPush, 14), kPushProlog, sizeof(kPushProlog)) == 0);
        }

        BYTE* secSite = ScanUnique(kSigSection, (int)(sizeof(kSigSection) / sizeof(kSigSection[0])));
        CHECK(secSite != NULL);
        if (secSite)
        {
            CHECK((unsigned long long)(secSite - g_exeBase) == 0x775000);
            CHECK((unsigned long long)(CallTarget(secSite, kSectionDrawCall) - g_exeBase) == 0x706C00);
        }
        BYTE* livSite = ScanUnique(kSigLiving, (int)(sizeof(kSigLiving) / sizeof(kSigLiving[0])));
        CHECK(livSite != NULL);
        if (livSite) CHECK((unsigned long long)(livSite - g_exeBase) == 0x761300);

        BYTE* resGet = ScanUnique(kSigResourceGet, (int)(sizeof(kSigResourceGet) / sizeof(kSigResourceGet[0])));
        CHECK(resGet != NULL);
        if (resGet)
        {
            CHECK((unsigned long long)(resGet - g_exeBase) == 0x2AA830);
            CHECK(kStolenResGet == 20 && resGet[20] == 0x48 && resGet[21] == 0x89);   // the next mov
        }
        BYTE* citizenHome = ScanUnique(kSigCitizenHome,
                                       (int)(sizeof(kSigCitizenHome) / sizeof(kSigCitizenHome[0])));
        CHECK(citizenHome != NULL);
        if (citizenHome)
            CHECK((unsigned long long)(citizenHome - g_exeBase) == 0x83A6D0);
        BYTE* menu = ScanUnique(kSigMenuBuilder, (int)(sizeof(kSigMenuBuilder) / sizeof(kSigMenuBuilder[0])));
        CHECK(menu != NULL);
        if (menu)
        {
            CHECK((unsigned long long)(menu - g_exeBase) == 0x51E30);
            CHECK(kStolenMenu == 18 && menu[18] == 0xB8);      // the next instruction, mov eax
            CHECK(ResolveMenuTab(menu));
            CHECK((BYTE*)g_tabVec    - g_exeBase == 0x9E17D8);
            CHECK((BYTE*)g_toolGame  - g_exeBase == 0x9D4F10);
            CHECK((BYTE*)g_tabCount  - g_exeBase == 0x08E390);
            CHECK((BYTE*)g_tabAt     - g_exeBase == 0x08E380);
            CHECK((BYTE*)g_tabCtor   - g_exeBase == 0x079430);
            CHECK((BYTE*)g_tabDtor   - g_exeBase == 0x079470);
            CHECK((BYTE*)g_tabPush   - g_exeBase == 0x08E3C0);
            CHECK((BYTE*)g_tabInit   - g_exeBase == 0x051CA0);
            CHECK((BYTE*)g_itemCount - g_exeBase == 0x08E470);
            CHECK((BYTE*)g_itemAt    - g_exeBase == 0x08E460);
            CHECK((BYTE*)g_itemCtor  - g_exeBase == 0x079500);
            CHECK((BYTE*)g_itemDtor  - g_exeBase == 0x079520);
            CHECK((BYTE*)g_itemPush  - g_exeBase == 0x08E500);
            CHECK((BYTE*)g_itemInit  - g_exeBase == 0x051B80);
            CHECK((BYTE*)g_findTool  - g_exeBase == 0x03AAA0);
            CHECK((BYTE*)g_intPush   - g_exeBase == 0x01DF90);
        }
        CHECK(ResolveRouteFunctions());
        CHECK((BYTE*)g_clearRoute  - g_exeBase == 0x67CB30);
        CHECK((BYTE*)g_addStop     - g_exeBase == 0x67D5D0);
        CHECK((BYTE*)g_addResource - g_exeBase == 0x0B14E0);
        CHECK((BYTE*)g_startRoute  - g_exeBase == 0x6B8EB0);
        CHECK((BYTE*)g_abortRoute  - g_exeBase == 0x6B8C80);
    }

    // The build menu tab, against a stand-in menu of the game's shape: 26 tabs in six groups.
    {
        static BYTE service[0x60], sorting[0x60], other[0x60], notBuilding[0x60];
        static int someType;
        *(void**)(service + kToolType) = &someType;
        *(void**)(sorting + kToolType) = &someType;
        *(void**)(other + kToolType)   = &someType;
        strcpy(g_tabItemId, "ITEM");
        strcpy(g_tabExtra, "999/Other, 999/Missing");

        FakeMenu();
        g_fakeTools.clear();
        AddMailTab();
        CHECK(g_fakeTabs.size() == 26);                        // no buildings, no tab

        g_fakeTools["ITEM/MailService"]   = service;
        g_fakeTools["ITEM/SortingOffice"] = sorting;
        g_fakeTools["ITEM/MailWarehouse"] = notBuilding;       // a tool without a building type
        g_fakeTools["999/Other"]          = other;
        AddMailTab();
        CHECK(g_fakeTabs.size() == 27);
        if (g_fakeTabs.size() == 27)
        {
            BYTE* tab = g_fakeTabs[24].data();
            CHECK(strcmp((char*)tab, "mail") == 0);            // after the industry tabs
            CHECK(strcmp((char*)g_fakeTabs[23].data(), "t23") == 0);
            CHECK(strcmp((char*)g_fakeTabs[25].data(), "t24") == 0);
            CHECK(strcmp((char*)g_fakeTabs[26].data(), "t25") == 0);   // the hidden last tab stays last
            CHECK(*(int*)(tab + 0x48) == kTabTextBase && *(int*)(tab + kTabGroup) == 4);
            CHECK(*(float*)(tab + kTabColor + 12) == 25.5f && g_fakeColorAligned);
            FakeItems* items = *(FakeItems**)(tab + kTabItems);
            CHECK(items && items->size() == 3);
            if (items && items->size() == 3)
            {
                CHECK(*(void**)(*items)[0].data() == service && *(void**)(*items)[1].data() == sorting &&
                      *(void**)(*items)[2].data() == other);
                CHECK(*(int*)((*items)[0].data() + 0x20) == 0x947);
            }
            std::vector<int>* heads = *(std::vector<int>**)(tab + kTabHeadings);
            std::vector<int>* ends  = *(std::vector<int>**)(tab + kTabEnds);
            CHECK(heads && *heads == std::vector<int>({ kTabTextBase + 1, kTabTextBase + 2, kTabTextBase + 3 }));
            CHECK(ends && *ends == std::vector<int>({ 1, 2, 3 }));
        }
        AddMailTab();
        CHECK(g_fakeTabs.size() == 27);                        // not twice
    }

    // 8. The load contract: zero only when the hook is in, non-zero (unload me)
    //    whenever it is not. A plugin with a live hook must never be unloaded.
    if (exe)
    {
        host.exeBase = g_exeBase;
        host.installInlineHook = FakeInstallOk;
        TsmPluginInfo info = {};
        int ret = TsmPluginInit(&host, &info);
        CHECK(ret == 0);
        CHECK(g_hookInstalls == 1);
        CHECK(g_hookStolen == kStolen);
        CHECK(g_origAddWaste == FakeOriginal);
        CHECK(info.name != NULL && strcmp(info.name, "mail") == 0);

        host.installInlineHook = FakeInstallRefuse;
        CHECK(TsmPluginInit(&host, &info) != 0);
    }

    printf(g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
