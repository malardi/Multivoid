// coop/world/dimension_travel.cpp -- see coop/world/dimension_travel.h.

#include "coop/world/dimension_travel.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
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

void SendTravel(coop::net::Session& s, const std::wstring& level) {
    coop::net::WorldTravelPayload p{};
    for (wchar_t c : level) {
        if (p.len >= sizeof(p.name)) break;
        p.name[p.len++] = (c > 0 && c < 0x80) ? static_cast<char>(c) : '_';
    }
    if (p.len == 0) return;
    if (s.SendReliable(coop::net::ReliableKind::WorldTravel, &p, sizeof(p))) {
        ++g_sent;
        UE_LOGI("[DIMENSION] HOST travels to '%ls' -- the clients are sent there too", level.c_str());
    } else {
        UE_LOGW("[DIMENSION] HOST travels to '%ls' -- the travel could not be sent; the clients stay", level.c_str());
    }
}

// lib_C::loadLevel(level) on this peer, authored by its own player, through the game's own travel.
bool CallLoadLevel(const std::wstring& level) {
    void* cdo = R::FindClassDefaultObject(L"lib_C");
    void* fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"loadLevel") : nullptr;
    void* local = coop::players::Registry::Get().Local();
    if (!fn || !local) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(level);
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<R::FName>(L"level", name) || !f.Set<void*>(L"__WorldContext", local)) return false;
    return ue_wrap::Call(cdo, f);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void OnLocalTravel(const std::wstring& level, bool own) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !own) return;
    if (EqualsNoCase(level, kMenuLevel)) return;  // the menu is the run's end, judged by run_end_travel
    const bool toDimension = !EqualsNoCase(level, kGameplayLevel);
    const bool was = g_inDimension.exchange(toDimension, std::memory_order_relaxed);
    g_level = toDimension ? level : std::wstring();
    s->SetWorldTrafficPaused(toDimension);
    UE_LOGI("[DIMENSION] this peer travels to '%ls' -- %s%s", level.c_str(),
            toDimension ? "a dimension: the session goes on there" : "the gameplay map",
            was && !toDimension ? " (back from a dimension)" : "");
    if (s->role() == coop::net::Role::Host && s->connected() && !g_following) SendTravel(*s, level);
}

bool InDimension() { return g_inDimension.load(std::memory_order_relaxed); }

const wchar_t* ReturnLevel() { return kGameplayLevel; }

void OnWorldTravel(const coop::net::WorldTravelPayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return;
    if (p.len == 0 || p.len > sizeof(p.name)) return;
    std::wstring level;
    for (uint8_t i = 0; i < p.len; ++i) {
        const char c = p.name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) { UE_LOGW("[DIMENSION] a WorldTravel with a level name that is not a level name -- dropped"); return; }
        level.push_back(static_cast<wchar_t>(c));
    }
    if (EqualsNoCase(level, kMenuLevel)) return;  // never sent; a client is never sent to the menu by the host
    // An event the host fired may already have sent this client there (the labyrinth's trigger reaches both).
    const bool toDimension = !EqualsNoCase(level, kGameplayLevel);
    const bool already = toDimension ? (InDimension() && EqualsNoCase(g_level, level.c_str())) : !InDimension();
    if (already) {
        UE_LOGI("[DIMENSION] the host went to '%ls'; this client is already there", level.c_str());
        return;
    }
    g_following = true;
    const bool ok = CallLoadLevel(level);
    g_following = false;
    ++g_followed;
    UE_LOGI("[DIMENSION] the host went to '%ls' -- this client follows: %s", level.c_str(),
            ok ? "travelling" : "the travel could not be started");
}

void OnSessionStart() {
    if (auto* s = g_session.load(std::memory_order_acquire)) s->SetWorldTrafficPaused(false);
    g_inDimension.store(false, std::memory_order_relaxed);
    g_level.clear();
}

void OnDisconnect() {
    if (g_sent || g_followed)
        UE_LOGI("[DIMENSION] session end -- travels sent=%llu followed=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_followed));
    g_sent = g_followed = 0;
    // InDimension stays as it is: the world this peer is in does not change when the session ends.
}

}  // namespace coop::dimension_travel
