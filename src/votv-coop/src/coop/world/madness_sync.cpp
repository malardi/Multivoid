// coop/world/madness_sync.cpp -- see coop/world/madness_sync.h.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/world/madness_sync.cpp (MIT).

#include "coop/world/madness_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/madness.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::madness_sync {
namespace {

namespace M = ue_wrap::madness;
namespace R = ue_wrap::reflection;

constexpr uint64_t kHostPollMs = 250;
constexpr uint64_t kHeartbeatMs = 1000;
constexpr uint64_t kClientApplyMs = 100;
constexpr float kMaxElapsed = 86400.f;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host: the master last seen (with its slot index), the event and send state.
void* g_master = nullptr;
int32_t g_masterIdx = -1;
uint32_t g_eventId = 0;
uint32_t g_sequence = 0;
coop::net::MadnessStatePayload g_sent{};
bool g_everSent = false;
uint64_t g_lastPollMs = 0, g_lastSendMs = 0;

// Client: the newest state the host sent, when it came, and what this peer shows of it.
coop::net::MadnessStatePayload g_state{};
bool g_have = false;
uint64_t g_receivedMs = 0, g_lastApplyMs = 0;
uint32_t g_appliedEvent = 0;
M::View g_view;
bool g_applyFailSaid = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Newer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

void HostTick(coop::net::Session& s, uint64_t now) {
    if (now - g_lastPollMs < kHostPollMs) return;
    g_lastPollMs = now;
    void* master = (g_master && g_masterIdx >= 0 && R::IsLiveByIndex(g_master, g_masterIdx)) ? g_master : nullptr;
    if (!master) {
        // The one held is gone (or none was): a master found now is a new event.
        master = M::FindMaster();
        if (master) {
            if (++g_eventId == 0) ++g_eventId;
            UE_LOGI("[MADNESS] host: the event's master %p stands (event %u)", master, g_eventId);
        }
        g_master = master;
        g_masterIdx = master ? R::InternalIndexOf(master) : -1;
    }
    M::State st{};
    if (master && !M::Read(master, st)) return;  // a read that faulted: asked again at the next poll
    if (!master && !g_everSent) return;          // no event this session yet: nothing to say
    coop::net::MadnessStatePayload p{};
    p.eventId = st.active ? g_eventId : 0;
    p.kills = st.kills < 0 ? 0 : st.kills;
    p.elapsed = (std::isfinite(st.elapsed) && st.elapsed >= 0.f && st.elapsed <= kMaxElapsed) ? st.elapsed : 0.f;
    p.active = st.active ? 1 : 0;
    p.hud = st.active && st.hud ? 1 : 0;
    p.playing = st.active && st.playing ? 1 : 0;
    const bool changed = !g_everSent || p.active != g_sent.active || p.hud != g_sent.hud ||
                         p.playing != g_sent.playing || p.kills != g_sent.kills || p.eventId != g_sent.eventId;
    const bool heartbeat = p.active && now - g_lastSendMs >= kHeartbeatMs;
    if (!changed && !heartbeat) return;
    if (++g_sequence == 0) ++g_sequence;
    p.sequence = g_sequence;
    if (!s.SendReliable(coop::net::ReliableKind::MadnessState, &p, static_cast<int>(sizeof(p)))) return;
    if (changed)
        UE_LOGI("[MADNESS] host: event %u active=%u hud=%u playing=%u kills=%d at %.1fs", p.eventId,
                static_cast<unsigned>(p.active), static_cast<unsigned>(p.hud), static_cast<unsigned>(p.playing),
                p.kills, static_cast<double>(p.elapsed));
    g_sent = p;
    g_everSent = true;
    g_lastSendMs = now;
}

void ClientTick(uint64_t now) {
    if (!g_have || now - g_lastApplyMs < kClientApplyMs) return;
    g_lastApplyMs = now;
    if (g_appliedEvent != g_state.eventId) {
        M::Clear(g_view);  // another event's music and counter go before this one's start
        g_appliedEvent = g_state.eventId;
    }
    M::State st{};
    st.active = g_state.active != 0;
    st.hud = g_state.hud != 0;
    st.playing = g_state.playing != 0;
    st.kills = g_state.kills;
    st.elapsed = g_state.elapsed;
    // The music's position now: the host's age at the send plus the time since it came.
    const float since = static_cast<float>(now - g_receivedMs) / 1000.f;
    float startTime = g_state.elapsed + since;
    if (!std::isfinite(startTime) || startTime < 0.f) startTime = 0.f;
    if (startTime > kMaxElapsed) startTime = kMaxElapsed;
    const bool ok = M::Apply(g_view, st, startTime);
    if (!ok && !g_applyFailSaid) {
        g_applyFailSaid = true;
        UE_LOGW("[MADNESS] client: the event's music or counter could not be shown yet -- retried");
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    const uint64_t now = NowMs();
    if (s->role() == coop::net::Role::Host) HostTick(*s, now);
    else ClientTick(now);
}

void OnMadnessState(const coop::net::MadnessStatePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    if (p.sequence == 0 || p.active > 1 || p.hud > 1 || p.playing > 1 || p.kills < 0 ||
        !std::isfinite(p.elapsed) || p.elapsed < 0.f || p.elapsed > kMaxElapsed ||
        (p.active && p.eventId == 0) || (!p.active && (p.hud || p.playing))) {
        UE_LOGW("[MADNESS] client: a malformed state dropped (seq %u)", p.sequence);
        return;
    }
    if (g_have && !Newer(p.sequence, g_state.sequence)) return;
    if (!g_have || p.active != g_state.active || p.eventId != g_state.eventId)
        UE_LOGI("[MADNESS] client: event %u active=%u hud=%u playing=%u kills=%d", p.eventId,
                static_cast<unsigned>(p.active), static_cast<unsigned>(p.hud), static_cast<unsigned>(p.playing),
                p.kills);
    g_state = p;
    g_have = true;
    g_receivedMs = NowMs();
    g_lastApplyMs = 0;  // shown at the next tick
}

void OnDisconnect() {
    M::Clear(g_view);
    g_view = M::View{};
    g_master = nullptr;
    g_masterIdx = -1;
    g_eventId = 0;
    g_sequence = 0;
    g_sent = coop::net::MadnessStatePayload{};
    g_everSent = false;
    g_lastPollMs = g_lastSendMs = 0;
    g_state = coop::net::MadnessStatePayload{};
    g_have = false;
    g_receivedMs = g_lastApplyMs = 0;
    g_appliedEvent = 0;
    g_applyFailSaid = false;
}

}  // namespace coop::madness_sync
