// coop/world/dimension_travel.cpp -- see coop/world/dimension_travel.h.

#include "coop/world/dimension_travel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"   // SetPassThrough: solo rules inside a dimension
#include "ue_wrap/core/types.h"
#include "ue_wrap/world/world_singleton.h"  // GameInstance: the travel state the entry writes
#include "coop/player/run_end_travel.h" // LoadLevelWatchTag: the one watch kept there

#include <atomic>
#include <cmath>
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

void LogGi(const char* when) {
    void* gi = ue_wrap::world_singleton::GameInstance();
    if (!gi) { UE_LOGI("[DIMENSION] %s: no game instance", when); return; }
    const GiFields& f = Gi(gi);
    const uint8_t* b = static_cast<const uint8_t*>(gi);
    std::wstring sub = f.subArea >= 0 ? R::ToString(*reinterpret_cast<const R::FName*>(b + f.subArea)) : L"?";
    ue_wrap::FVector d{};
    if (f.locDist >= 0) d = *reinterpret_cast<const ue_wrap::FVector*>(b + f.locDist);
    UE_LOGI("[DIMENSION] %s: gameInstance subArea='%ls' locationDistance=(%.0f,%.0f,%.0f) loadObjects=%d", when,
            sub.c_str(), d.X, d.Y, d.Z, f.loadObjects >= 0 ? b[f.loadObjects] : -1);
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

bool g_lastViaSubArea = false, g_lastClear = false;

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
        if (f.subArea >= 0 && f.locDist >= 0 && f.loadObjects >= 0) {
            p.flags |= F::kHaveGi;
            if (b[f.loadObjects]) p.flags |= F::kLoadObjects;
            p.subAreaLen = PutAscii(p.subArea, R::ToString(*reinterpret_cast<const R::FName*>(b + f.subArea)));
            const auto& d = *reinterpret_cast<const ue_wrap::FVector*>(b + f.locDist);
            p.locDist[0] = d.X; p.locDist[1] = d.Y; p.locDist[2] = d.Z;
        }
    }
    if (s.SendReliable(coop::net::ReliableKind::WorldTravel, &p, sizeof(p))) {
        ++g_sent;
        UE_LOGI("[DIMENSION] HOST travels to '%ls' (flags 0x%02X) -- the clients are sent there too", level.c_str(),
                static_cast<unsigned>(p.flags));
    } else {
        UE_LOGW("[DIMENSION] HOST travels to '%ls' -- the travel could not be sent; the clients stay", level.c_str());
    }
}

// The host's own call, on this peer: lib_C's "Load Level as Sub Area"(level) when the host's travel came
// through it, else lib_C::loadLevel(level, clearSubArea), authored by this peer's own player.
bool CallTravel(const std::wstring& level, bool viaSubArea, bool clearSubArea) {
    void* cdo = R::FindClassDefaultObject(L"lib_C");
    void* local = coop::players::Registry::Get().Local();
    if (!cdo || !local) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(level);
    void* fn = viaSubArea ? R::FindFunction(R::ClassOf(cdo), L"Load Level as Sub Area") : nullptr;
    if (viaSubArea && !fn) UE_LOGW("[DIMENSION] lib_C has no 'Load Level as Sub Area' here -- plain loadLevel");
    if (fn) {
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<R::FName>(L"Level", name) || !f.Set<void*>(L"__WorldContext", local)) return false;
        return ue_wrap::Call(cdo, f);
    }
    fn = R::FindFunction(R::ClassOf(cdo), L"loadLevel");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<R::FName>(L"level", name) || !f.Set<void*>(L"__WorldContext", local)) return false;
    const uint8_t clear = clearSubArea ? 1 : 0;
    f.SetRaw(L"clearSubArea", &clear, 1);
    return ue_wrap::Call(cdo, f);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void OnLocalTravel(const std::wstring& level, bool own, bool viaSubArea, bool clearSubArea) {
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
    LogGi(toDimension ? "travel to a dimension" : "travel to the gameplay map");
    g_lastViaSubArea = viaSubArea;
    g_lastClear = clearSubArea;
    if (s->role() == coop::net::Role::Host && s->connected() && !g_following) SendTravel(*s, level);
}

bool InDimension() { return g_inDimension.load(std::memory_order_relaxed); }

const wchar_t* ReturnLevel() { return kGameplayLevel; }

void OnWorldTravel(const coop::net::WorldTravelPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return;
    if (p.len == 0 || p.len > sizeof(p.name) || p.subAreaLen > sizeof(p.subArea)) return;
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::wstring level;
    for (uint8_t i = 0; i < p.len; ++i) {
        if (!isNameChar(p.name[i])) { UE_LOGW("[DIMENSION] a WorldTravel with a level name that is not a level name -- dropped"); return; }
        level.push_back(static_cast<wchar_t>(p.name[i]));
    }
    std::wstring subArea;
    for (uint8_t i = 0; i < p.subAreaLen; ++i) {
        if (!isNameChar(p.subArea[i])) { subArea.clear(); break; }
        subArea.push_back(static_cast<wchar_t>(p.subArea[i]));
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
            uint8_t* b = static_cast<uint8_t*>(gi);
            if (f.subArea >= 0) *reinterpret_cast<R::FName*>(b + f.subArea) =
                ue_wrap::fname_utils::StringToFName(subArea.empty() ? std::wstring(L"None") : subArea);
            const bool finite = std::isfinite(p.locDist[0]) && std::isfinite(p.locDist[1]) && std::isfinite(p.locDist[2]);
            if (f.locDist >= 0 && finite)
                *reinterpret_cast<ue_wrap::FVector*>(b + f.locDist) = ue_wrap::FVector{p.locDist[0], p.locDist[1], p.locDist[2]};
            if (f.loadObjects >= 0) b[f.loadObjects] = (p.flags & F::kLoadObjects) ? 1 : 0;
        }
    }
    LogGi("before following the host");
    g_following = true;
    const bool ok = CallTravel(level, (p.flags & F::kViaSubArea) != 0, (p.flags & F::kClearSubArea) != 0);
    g_following = false;
    ++g_followed;
    UE_LOGI("[DIMENSION] the host went to '%ls' (flags 0x%02X) -- this client follows: %s", level.c_str(),
            static_cast<unsigned>(p.flags), ok ? "travelling" : "the travel could not be started");
}

void OnSessionStart() {
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

}  // namespace coop::dimension_travel
