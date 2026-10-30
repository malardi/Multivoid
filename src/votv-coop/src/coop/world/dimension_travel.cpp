// coop/world/dimension_travel.cpp -- see coop/world/dimension_travel.h.

#include "coop/world/dimension_travel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"   // Puppet, DestroySlot: no body from another level
#include "coop/player/remote_player.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"   // SetPassThrough: solo rules inside a dimension
#include "ue_wrap/core/script_fingerprint.h"  // the [BP-DUMP] of the gameplay mode
#include "ue_wrap/core/paths.h"
#include "ue_wrap/facts/function_row.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include <cstdio>
#include <windows.h>
#include "ue_wrap/core/types.h"
#include "ue_wrap/world/world_singleton.h"  // GameInstance: the travel state the entry writes
#include "ue_wrap/engine/world_identity.h"   // CurrentWorld, Generation: this peer's arrival
#include "coop/player/run_end_travel.h" // LoadLevelWatchTag: the one watch kept there

#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>

namespace coop::dimension_travel {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kGameplayLevel = L"untitled_1";
constexpr const wchar_t* kMenuLevel     = L"menu";

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_inDimension{false};
std::wstring g_level;          // the dimension this peer is in, for the log
bool g_following = false;      // a client's own loadLevel that answers the host's WorldTravel
uint64_t g_sent = 0, g_followed = 0;

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return i == a.size() && b[i] == 0;
}

// mainGameInstance's travel state: the sub-area the run stands in, the entry's distance, and whether the
// next level applies the save. Resolved by name once per game-instance class.
struct GiFields { int32_t subArea = -2, locDist = -2, loadObjects = -2; };
GiFields& Gi(void* gi) {
    static GiFields f;
    static void* sCls = nullptr;
    void* cls = gi ? R::ClassOf(gi) : nullptr;
    if (cls && cls != sCls) {
        sCls = cls;
        f.subArea = R::FindPropertyOffset(cls, L"subArea");
        f.locDist = R::FindPropertyOffset(cls, L"locationDistance");
        f.loadObjects = R::FindPropertyOffset(cls, L"loadObjects");
    }
    return f;
}

// The carried-items arrays a level travel fills and the next level's gamemode spawns from.
void LogTransports(void* gi, const char* when) {
    void* cls = R::ClassOf(gi);
    const wchar_t* names[] = {L"playerInv_transport", L"playerEquipment_transport", L"playerHold_transport"};
    int32_t nums[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        const int32_t off = R::FindPropertyOffset(cls, names[i]);
        if (off >= 0) nums[i] = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(gi) + off + 8);
    }
    UE_LOGI("[DIMENSION] %s: gameInstance transports inv=%d equipment=%d hold=%d", when, nums[0], nums[1], nums[2]);
}

std::wstring ReadToken(void* gi);

void LogGi(const char* when) {
    void* gi = ue_wrap::world_singleton::GameInstance();
    if (!gi) { UE_LOGI("[DIMENSION] %s: no game instance", when); return; }
    LogTransports(gi, when);
    const GiFields& f = Gi(gi);
    const uint8_t* b = static_cast<const uint8_t*>(gi);
    std::wstring sub = f.subArea >= 0 ? R::ToString(*reinterpret_cast<const R::FName*>(b + f.subArea)) : L"?";
    ue_wrap::FVector d{};
    if (f.locDist >= 0) d = *reinterpret_cast<const ue_wrap::FVector*>(b + f.locDist);
    UE_LOGI("[DIMENSION] %s: gameInstance subArea='%ls' locationDistance=(%.0f,%.0f,%.0f) loadObjects=%d NewVar_1='%ls' gi=%p", when,
            sub.c_str(), d.X, d.Y, d.Z, f.loadObjects >= 0 ? b[f.loadObjects] : -1, ReadToken(gi).c_str(), gi);
}

template <size_t N>
uint8_t PutAscii(char (&dst)[N], const std::wstring& src) {
    uint8_t n = 0;
    for (wchar_t c : src) {
        if (n >= N) break;
        dst[n++] = (c > 0 && c < 0x80) ? static_cast<char>(c) : '_';
    }
    return n;
}

// mainGameInstance.NewVar_1: the pass a legitimate entry writes before its travel. The next level's gamemode
// compares it with its constant and clears it; a level off its own list (untitled_1, the tutorials, the menu)
// reached without it spawns 100000 hidden props, or quits a paused game (b277's census and the gamemode's
// dump). The host's travel carries its value; a client's own travel into a dimension writes it.
constexpr const wchar_t* kEntryPass = L"PQXYyeofZ8cr5rJD4YXLVw";

struct WString { wchar_t* data; int32_t num; int32_t max; };

int32_t TokenOff(void* gi) {
    static void* sCls = nullptr;
    static int32_t sOff = -2;
    void* cls = R::ClassOf(gi);
    if (cls != sCls) { sCls = cls; sOff = R::FindPropertyOffset(cls, L"NewVar_1"); }
    return sOff;
}

std::wstring ReadToken(void* gi) {
    const int32_t off = gi ? TokenOff(gi) : -1;
    if (off < 0) return std::wstring();
    const auto* w = reinterpret_cast<const WString*>(static_cast<const uint8_t*>(gi) + off);
    if (!w->data || w->num <= 1) return std::wstring();
    return std::wstring(w->data, static_cast<size_t>(w->num - 1));
}

void WriteToken(const std::wstring& token) {
    void* gi = ue_wrap::world_singleton::GameInstance();
    const int32_t off = gi ? TokenOff(gi) : -1;
    if (off < 0) { UE_LOGW("[DIMENSION] mainGameInstance.NewVar_1 not found -- the entry pass cannot be written"); return; }
    auto* w = reinterpret_cast<WString*>(static_cast<uint8_t*>(gi) + off);
    const size_t n = token.size() + 1;
    auto* mem = static_cast<wchar_t*>(R::EngineAlloc(n * sizeof(wchar_t)));
    if (!mem) return;
    std::memcpy(mem, token.c_str(), n * sizeof(wchar_t));
    if (w->data && w->max > 0) R::EngineFree(w->data);
    w->data = mem;
    w->num = w->max = static_cast<int32_t>(n);
    UE_LOGI("[DIMENSION] the entry pass is written (%zu chars)", token.size());
}

bool g_lastViaSubArea = false, g_lastClear = false;
std::wstring g_lastOption;  // the host's own loadLevel Option: the entry pass it carries

void SendTravel(coop::net::Session& s, const std::wstring& level) {
    coop::net::WorldTravelPayload p{};
    p.len = PutAscii(p.name, level);
    if (p.len == 0) return;
    namespace F = coop::net::world_travel_flag;
    if (g_lastViaSubArea) p.flags |= F::kViaSubArea;
    if (g_lastClear) p.flags |= F::kClearSubArea;
    if (void* gi = ue_wrap::world_singleton::GameInstance()) {
        const GiFields& f = Gi(gi);
        const uint8_t* b = static_cast<const uint8_t*>(gi);
        if (f.loadObjects >= 0) {
            p.flags |= F::kHaveGi;
            if (b[f.loadObjects]) p.flags |= F::kLoadObjects;
        }
        p.tokenLen = PutAscii(p.token, !g_lastOption.empty() ? g_lastOption : ReadToken(gi));
    }
    if (s.SendReliable(coop::net::ReliableKind::WorldTravel, &p, sizeof(p))) {
        ++g_sent;
        UE_LOGI("[DIMENSION] HOST travels to '%ls' (flags 0x%02X, entry pass %u chars) -- the clients are sent there too",
                level.c_str(), static_cast<unsigned>(p.flags), static_cast<unsigned>(p.tokenLen));
    } else {
        UE_LOGW("[DIMENSION] HOST travels to '%ls' -- the travel could not be sent; the clients stay", level.c_str());
    }
}

// The host's own call, on this peer: lib_C's "Load Level as Sub Area"(level) when the host's travel came
// through it, else lib_C::loadLevel(level, clearSubArea), authored by this peer's own player.
// A UE4 FString parameter, pointing at the caller's own buffer for the call: the script copies it.
struct ParamFString { const wchar_t* data; int32_t num; int32_t max; };

bool CallTravel(const std::wstring& level, bool viaSubArea, bool clearSubArea, const std::wstring& option) {
    const ParamFString opt{option.c_str(), static_cast<int32_t>(option.size() + 1), static_cast<int32_t>(option.size() + 1)};
    void* cdo = R::FindClassDefaultObject(L"lib_C");
    void* local = coop::players::Registry::Get().Local();
    if (!cdo || !local) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(level);
    void* fn = viaSubArea ? R::FindFunction(R::ClassOf(cdo), L"Load Level as Sub Area") : nullptr;
    if (viaSubArea && !fn) UE_LOGW("[DIMENSION] lib_C has no 'Load Level as Sub Area' here -- plain loadLevel");
    if (fn) {
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<R::FName>(L"Level", name) || !f.Set<void*>(L"__WorldContext", local)) return false;
        if (!option.empty()) f.SetRaw(L"Option", &opt, sizeof(opt));
        return ue_wrap::Call(cdo, f);
    }
    fn = R::FindFunction(R::ClassOf(cdo), L"loadLevel");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<R::FName>(L"level", name) || !f.Set<void*>(L"__WorldContext", local)) return false;
    const uint8_t clear = clearSubArea ? 1 : 0;
    f.SetRaw(L"clearSubArea", &clear, 1);
    // loadLevel writes its Option into mainGameInstance.NewVar_1 -- the entry pass the next level's gamemode
    // checks; the game's own travels pass the pass there (b279's dump), an empty Option wipes it.
    if (!option.empty()) f.SetRaw(L"Option", &opt, sizeof(opt));
    return ue_wrap::Call(cdo, f);
}

// ---- the peers' levels -------------------------------------------------------------------------
struct PeerLvl { bool known = false; std::wstring level; };  // level empty: travelling
PeerLvl g_peer[coop::net::kMaxPeers];
std::wstring g_myLevel;          // this peer's world's name; empty while it travels
bool g_inTransit = false;        // this peer's own level travel is under way (set at the travel, cleared on arrival)
uint32_t g_seenGen = 0;
uint64_t g_poseDropLogged = 0;

void SendPeerLevel(const std::wstring& level) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    coop::net::PeerLevelPayload p{};
    p.len = PutAscii(p.name, level);
    s->SendReliable(coop::net::ReliableKind::PeerLevel, &p, sizeof(p));
}

void DropPuppetOf(int slot);  // defined after the anonymous namespace (puppet_drive)

// ---- the carried equipment ---------------------------------------------------------------------
// mainGamemode's BeginPlay spawns one prop per entry of saveSlot.beginEquipment beside the player, then
// clears the array: what a level travel carries. A client's labyrinth found thousands there (b274/b275: the
// flood is that loop, ExecuteUbergraph_mainGamemode's beginEquipment walk), the host's a handful. Read at the
// gamemode's BeginPlay, before the loop; a client's array past any real kit is emptied in a dimension.
UE_FACT_FN(kRowGmBeginPlay, "dimension_travel", L"mainGamemode_C", L"ReceiveBeginPlay", Declared);
constexpr size_t kMaxCarried = 200;
bool g_gmWatch = false, g_gmLive = false;

struct NameArray { R::FName* data; int32_t num; int32_t max; };

uint32_t g_carryCheckedGen = 0;

void CheckCarried(void* gm, const char* where) {
    if (!gm) return;
    const int32_t slotOff = R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot");
    void* slot = slotOff >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(gm) + slotOff) : nullptr;
    if (!slot) {
        UE_LOGI("[DIMENSION] %s: no saveSlot (off=%d)", where, slotOff);
        return;
    }
    const int32_t eqOff = R::FindPropertyOffset(R::ClassOf(slot), L"beginEquipment");
    if (eqOff < 0) return;
    auto* arr = reinterpret_cast<NameArray*>(static_cast<uint8_t*>(slot) + eqOff);
    std::wstring first;
    for (int32_t i = 0; i < arr->num && i < 8 && arr->data; ++i) first += R::ToString(arr->data[i]) + L" ";
    auto* s = g_session.load(std::memory_order_acquire);
    const bool client = s && s->running() && s->role() == coop::net::Role::Client;
    UE_LOGI("[DIMENSION] %s: saveSlot.beginEquipment holds %d (first: %ls)%s", where, arr->num, first.c_str(),
            InDimension() ? " -- in a dimension" : "");
    if (client && InDimension() && arr->num > static_cast<int32_t>(kMaxCarried)) {
        UE_LOGW("[DIMENSION] a client's carried equipment of %d entries is no kit -- emptied before the level spawns it",
                arr->num);
        arr->num = 0;  // the storage stays; the loop re-reads the length every pass and ends
    }
}

ue_wrap::script_gate::Verdict OnGamemodeBeginPlayPre(const ue_wrap::script_gate::Call& call) {
    CheckCarried(call.object, "gamemode BeginPlay");
    return ue_wrap::script_gate::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // The same tag as run_end_travel's travel watch: a dimension's pass-through keeps it.
    if (!g_gmWatch)
        g_gmWatch = ue_wrap::script_gate::WatchName(kRowGmBeginPlay, coop::player::run_end_travel::LoadLevelWatchTag(),
                                                    &OnGamemodeBeginPlayPre, nullptr);
}

void OnLocalTravel(const std::wstring& level, bool own, bool viaSubArea, bool clearSubArea, const std::wstring& option) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !own) return;
    if (EqualsNoCase(level, kMenuLevel)) return;  // the menu is the run's end, judged by run_end_travel
    const bool toDimension = !EqualsNoCase(level, kGameplayLevel);
    const bool was = g_inDimension.exchange(toDimension, std::memory_order_relaxed);
    g_level = toDimension ? level : std::wstring();
    s->SetWorldTrafficPaused(toDimension);
    // Solo rules inside the level: no seam refuses the game's calls there (a client refused the
    // labyrinth's own spawns and its script never returned), but the travel watch stays, the way home.
    ue_wrap::script_gate::SetPassThrough(toDimension, coop::player::run_end_travel::LoadLevelWatchTag());
    UE_LOGI("[DIMENSION] this peer travels to '%ls' -- %s%s", level.c_str(),
            toDimension ? "a dimension: the session goes on there" : "the gameplay map",
            was && !toDimension ? " (back from a dimension)" : "");
    // (b271's loadObjects=0 for a client's dimension is gone: the flood was the gamemode's missing-pass
    // trap, not the save; the host's value travels as it is.)
    LogGi(toDimension ? "travel to a dimension" : "travel to the gameplay map");
    // In transit: the others stop putting this peer's body into their level until it says where it is, and this
    // peer puts no other body into the world it is leaving or the one still loading.
    g_myLevel.clear();
    g_inTransit = true;
    // The bodies of the others go before this world does, as they do when the host leads (it says "travelling"
    // first); each comes back on arrival if its peer stands on the same level.
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) DropPuppetOf(slot);
    SendPeerLevel(std::wstring());
    g_lastViaSubArea = viaSubArea;
    g_lastOption = option;
    g_lastClear = clearSubArea;
    if (s->role() == coop::net::Role::Host && s->connected() && !g_following) SendTravel(*s, level);
}

bool InDimension() { return g_inDimension.load(std::memory_order_relaxed); }

const wchar_t* ReturnLevel() { return kGameplayLevel; }

void OnWorldTravel(const coop::net::WorldTravelPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return;
    if (p.len == 0 || p.len > sizeof(p.name) || p.tokenLen > sizeof(p.token)) return;
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::wstring level;
    for (uint8_t i = 0; i < p.len; ++i) {
        if (!isNameChar(p.name[i])) { UE_LOGW("[DIMENSION] a WorldTravel with a level name that is not a level name -- dropped"); return; }
        level.push_back(static_cast<wchar_t>(p.name[i]));
    }
    std::wstring token;
    for (uint8_t i = 0; i < p.tokenLen && i < sizeof(p.token); ++i) {
        const char c = p.token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { token.clear(); break; }
        token.push_back(static_cast<wchar_t>(c));
    }
    if (EqualsNoCase(level, kMenuLevel)) return;  // never sent; a client is never sent to the menu by the host
    // An event the host fired may already have sent this client there (the labyrinth's trigger reaches both).
    const bool toDimension = !EqualsNoCase(level, kGameplayLevel);
    const bool already = toDimension ? (InDimension() && EqualsNoCase(g_level, level.c_str())) : !InDimension();
    if (already) {
        UE_LOGI("[DIMENSION] the host went to '%ls'; this client is already there", level.c_str());
        return;
    }
    namespace F = coop::net::world_travel_flag;
    // The host's entry wrote its game instance's travel state before its travel; this one gets the same.
    if ((p.flags & F::kHaveGi) != 0) {
        if (void* gi = ue_wrap::world_singleton::GameInstance()) {
            const GiFields& f = Gi(gi);
            if (f.loadObjects >= 0) static_cast<uint8_t*>(gi)[f.loadObjects] = (p.flags & F::kLoadObjects) ? 1 : 0;
        }
    }
    // The entry pass: the host's, or the game's own constant when the host's had been spent already.
    const std::wstring pass = toDimension ? (token.empty() ? std::wstring(kEntryPass) : token) : std::wstring();
    if (toDimension) WriteToken(pass);
    LogGi("before following the host");
    g_following = true;
    const bool ok = CallTravel(level, (p.flags & F::kViaSubArea) != 0, (p.flags & F::kClearSubArea) != 0, pass);
    g_following = false;
    ++g_followed;
    UE_LOGI("[DIMENSION] the host went to '%ls' (flags 0x%02X) -- this client follows: %s", level.c_str(),
            static_cast<unsigned>(p.flags), ok ? "travelling" : "the travel could not be started");
}

void OnSessionStart() {
    for (auto& pl : g_peer) pl = PeerLvl{};
    g_inTransit = false;
    g_seenGen = 0;
    if (auto* s = g_session.load(std::memory_order_acquire)) s->SetWorldTrafficPaused(false);
    ue_wrap::script_gate::SetPassThrough(false, 0);
    g_inDimension.store(false, std::memory_order_relaxed);
    g_level.clear();
}

void OnDisconnect() {
    if (g_sent || g_followed)
        UE_LOGI("[DIMENSION] session end -- travels sent=%llu followed=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_followed));
    g_sent = g_followed = 0;
    ue_wrap::script_gate::SetPassThrough(false, 0);  // no session: the gate's own holder is gone anyway
    // InDimension stays as it is: the world this peer is in does not change when the session ends.
}

namespace {
// [devkeys] The gameplay mode's whole Blueprint, as the mod's decoder reads it, written once per process to
// coop_logs\bp_<function>.txt: the dimension work reads the game's own logic there (b274: a client's labyrinth
// spawns prop_C endlessly from ExecuteUbergraph_mainGamemode@12796, the host's does not).
void DumpGamemodeOnce() {
    static bool sDone = false;  // b279: the dump names more classes; once per process
    if (sDone) return;
    static const bool sOn = coop::config::ResolveFlag(coop::config_registry::rows::devkeys);
    if (!sOn) { sDone = true; return; }
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return;
    sDone = true;
    const std::wstring dir = ue_wrap::paths::ExeDir() + L"\\coop_logs";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    struct Want { const wchar_t* cls; const wchar_t* fn; };
    const Want kFns[] = {{L"mainGamemode_C", L"ExecuteUbergraph_mainGamemode"}, {L"lib_C", L"loadLevel"},
                         {L"lib_C", L"Load Level as Sub Area"}, {L"lib_C", L"transition"},
                         {L"mainPlayer_C", L"ExecuteUbergraph_mainPlayer"}, {L"mainGameInstance_C", L"keepPlayer"}};
    for (const Want& w : kFns) {
        const wchar_t* name = w.fn;
        void* cls = R::FindClass(w.cls);
        void* fn = cls ? R::FindFunction(cls, name) : nullptr;
        if (!fn) { UE_LOGI("[BP-DUMP] %ls: not found", name); continue; }
        const auto r = ue_wrap::script_fingerprint::Of(fn);
        const std::wstring path = dir + L"\\bp_" + name + L".txt";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            std::fwrite(r.text.data(), 1, r.text.size(), f);
            std::fclose(f);
        }
        UE_LOGI("[BP-DUMP] %ls: %zu bytes written (ok=%d%s%s)", name, r.text.size(), r.ok ? 1 : 0,
                r.problem.empty() ? "" : ", problem: ", r.problem.c_str());
    }
}
}  // namespace

void Tick() {
    DumpGamemodeOnce();
    if (g_gmWatch && !g_gmLive) {
        ue_wrap::script_gate::ResolvePendingNames();
        g_gmLive = ue_wrap::script_gate::NameWatchLive(kRowGmBeginPlay, coop::player::run_end_travel::LoadLevelWatchTag());
        if (g_gmLive) UE_LOGI("[DIMENSION] the gamemode's BeginPlay is watched (the carried equipment)");
    }
    void* world = ue_wrap::world_identity::CurrentWorld();
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (!world || gen == g_seenGen) return;
    if (!coop::players::Registry::Get().Local()) return;  // the body first: the level is up when it stands in it
    g_seenGen = gen;
    g_myLevel = R::ToString(R::NameOf(world));
    g_inTransit = false;
    UE_LOGI("[DIMENSION] this peer's world is now '%ls' -- told to the others", g_myLevel.c_str());
    LogGi("arrival");
    SendPeerLevel(g_myLevel);
    // A peer that already said it stands on another level loses its body here.
    for (int slot = 0; slot < coop::net::kMaxPeers; ++slot)
        if (!PoseAccepted(slot)) DropPuppetOf(slot);
}

void OnPeerLevel(int slot, const coop::net::PeerLevelPayload& p) {
    if (slot < 0 || slot >= coop::net::kMaxPeers || p.len > sizeof(p.name)) return;
    std::wstring level;
    for (uint8_t i = 0; i < p.len; ++i) {
        const char c = p.name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return;
        level.push_back(static_cast<wchar_t>(c));
    }
    // Only a change is news: the answer below is itself a PeerLevel, so answering every word made two peers in
    // one level trade it back and forth for as long as they stayed (~55 a second each way, seen in the labyrinth).
    const bool changed = !g_peer[slot].known || !EqualsNoCase(g_peer[slot].level, level.c_str());
    g_peer[slot].known = true;
    g_peer[slot].level = level;
    if (!changed) return;
    UE_LOGI("[DIMENSION] peer slot %d %ls%ls", slot, level.empty() ? L"is travelling" : L"stands in '",
            level.empty() ? L"" : (level + L"'").c_str());
    if (!PoseAccepted(slot)) DropPuppetOf(slot);
    // A peer that just arrived hears where this one is (it may have missed the word while loading).
    if (!level.empty() && !g_myLevel.empty()) SendPeerLevel(g_myLevel);
}

bool PeerLevelKnown(int slot) { return slot >= 0 && slot < coop::net::kMaxPeers && g_peer[slot].known; }

bool PoseAccepted(int slot) {
    if (slot < 0 || slot >= coop::net::kMaxPeers) return true;
    const PeerLvl& pl = g_peer[slot];
    if (!pl.known) return true;            // never said: the join's own world, as before
    if (pl.level.empty()) return false;    // travelling: its poses are of the level it left
    // This peer is between levels: a body spawned now lands in the world being torn down or the one still
    // loading. A client going home alone from the labyrinth (by its death or its own travel) crashed in the
    // load twice of three times while the host's body, on another level, was put back mid-travel.
    if (g_inTransit) return false;
    if (g_myLevel.empty()) return true;    // this peer has not named its own world yet
    return EqualsNoCase(pl.level, g_myLevel.c_str());
}

namespace {
void DropPuppetOf(int slot) {
    if (!coop::puppet_drive::Puppet(slot).valid()) return;
    if (++g_poseDropLogged <= 20)
        UE_LOGI("[DIMENSION] peer slot %d is on another level -- its body leaves this one until it is here", slot);
    coop::puppet_drive::DestroySlot(slot);
}
}  // namespace

void OnGamemodeSpawn(void* gamemode) {
    // The census sees the carried-equipment walk's first spawn: the array is read (and a client's emptied in a
    // dimension) once per world, from inside the walk -- its loop re-reads the length every pass.
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_carryCheckedGen) return;
    g_carryCheckedGen = gen;
    CheckCarried(gamemode, "the gamemode's first spawn of this world");
    LogGi("the gamemode's first spawn of this world (the entry pass is checked one frame later)");
}

bool TravelAsGameEntry(const std::wstring& level) {
    return CallTravel(level, false, true, EqualsNoCase(level, kGameplayLevel) ? std::wstring() : std::wstring(kEntryPass));
}

}  // namespace coop::dimension_travel
