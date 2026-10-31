// coop/world/balance_sync.cpp -- see coop/world/balance_sync.h.

#include "coop/world/balance_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace coop::balance_sync {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::economy;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host delivery, one row per peer slot -- game-thread only (Tick host branch and the connect edge /
// order refusal that call SendCurrentToSlot, all on the net-pump game thread). A row is marked sent
// ONLY when SendReliableToSlot accepted the payload: the old shape stored the dedup value before a
// broadcast whose result it ignored, so a refused send was never retried until the balance moved,
// and the joiner's seed was one-shot. Now each recipient keeps its own delivery debt: an unsent or
// stale row, or an explicit request (seed / refusal) not yet delivered, is retried every tick. The
// row resets when the slot's occupancy generation changes (a replacement in a reused slot) or the
// world changes. Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/world/balance_sync.cpp
// (MIT).
struct Delivery {
    uint32_t occupant  = 0;      // Session::peerGenerationForSlot when the row was opened
    int32_t  total     = 0;      // the value last ACCEPTED for this slot
    bool     sent      = false;  // total is meaningful
    bool     loggedFail = false; // one refusal line per failing streak, not one per tick
    uint64_t requested = 0;      // SendCurrentToSlot calls for this occupant
    uint64_t delivered = 0;      // the request count covered by the last accepted send
};
std::array<Delivery, coop::net::kMaxPeers> g_delivery{};
uint32_t g_world = 0;            // world_identity::Generation the rows belong to

void Deliver(coop::net::Session* s, int slot, int32_t points) {
    auto& row = g_delivery[slot];
    const uint32_t occupant = s->peerGenerationForSlot(slot);
    if (row.occupant != occupant) { row = {}; row.occupant = occupant; }
    if (!occupant || !s->IsSlotReady(slot)) return;  // empty / not ready: the debt waits
    if (row.sent && row.total == points && row.requested == row.delivered) return;
    const uint64_t request = row.requested;
    const coop::net::BalancePayload p{points};
    if (!s->SendReliableToSlot(slot, coop::net::ReliableKind::BalanceSync, &p, sizeof(p))) {
        if (!row.loggedFail) {
            row.loggedFail = true;
            UE_LOGW("balance_sync: send Points=%d to slot %d refused -- retrying each tick",
                    points, slot);
        }
        return;  // not marked sent: the next tick retries
    }
    row.sent = true;
    row.total = points;
    row.delivered = request;
    row.loggedFail = false;
    UE_LOGI("balance_sync: sent Points=%d to slot %d", points, slot);
}

// Client mirror: the latest host balance to apply, RETRIED each tick until the saveSlot
// resolves -- a connect-edge BalanceSync can arrive before the client's gamemode/saveSlot
// is loaded (the WritePoints would otherwise silently fail + the mirror never lands).
// Atomic because ApplyFromHost runs off the event_feed dispatch while Tick applies on the
// game thread. Latest-wins. Packed as (nonzero sequence << 32 | value) so Tick clears exactly the
// value it applied: a newer BalanceSync landing during the write/repaint stays pending.
std::atomic<uint64_t> g_pending{0};
std::atomic<uint32_t> g_pendingSeq{0};

}  // namespace

void SetSession(coop::net::Session* session) {
    g_delivery = {};
    g_world = ue_wrap::world_identity::Generation();
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        const uint32_t world = ue_wrap::world_identity::Generation();
        if (world != g_world) { g_delivery = {}; g_world = world; }
        // HOST: poll the canonical balance; deliver to every slot that has not accepted it yet.
        int32_t points = 0;
        if (!E::ReadPoints(&points)) return;  // not resolved yet (booting / at menu)
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) Deliver(s, slot, points);
    } else {
        // CLIENT: retry the latest pending mirror until WritePoints succeeds (the
        // saveSlot may not have resolved when the connect-edge BalanceSync arrived).
        uint64_t pending = g_pending.load(std::memory_order_acquire);
        if (!pending) return;
        const int32_t total = static_cast<int32_t>(static_cast<uint32_t>(pending));
        if (E::WritePoints(total)) {
            // WritePoints is side-effect-free, so the on-screen credit number would stay frozen:
            // text_points is push-updated only by the blueprint's addPoints -> SetText. Re-run that
            // repaint with a zero add so the mirrored value actually shows.
            // A false repaint (gamemode unresolved) keeps the value pending: the write is
            // idempotent, so the next tick simply retries both.
            if (!E::RefreshPointsHud()) return;
            // Clear only the value just applied; a newer one published meanwhile stays pending.
            if (g_pending.compare_exchange_strong(pending, 0, std::memory_order_acq_rel))
                UE_LOGI("balance_sync: mirrored host balance -> Points=%d (HUD repainted)", total);
        }
        // else: saveSlot still resolving -- silently retry next tick.
    }
}

void SendCurrentToSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (slot <= 0 || slot >= coop::net::kMaxPeers) return;
    // Record the request first, so a refused or premature send (Points unresolved, slot not ready,
    // send refused) is retried by Tick rather than lost: the joiner seed and the order-refusal
    // resend both need this peer to converge while the host's value is static.
    auto& row = g_delivery[slot];
    const uint32_t occupant = s->peerGenerationForSlot(slot);
    if (row.occupant != occupant) { row = {}; row.occupant = occupant; }
    ++row.requested;
    int32_t points = 0;
    if (!E::ReadPoints(&points)) {
        UE_LOGW("balance_sync: direct send to slot %d -- Points not resolved yet (Tick retries)", slot);
        return;
    }
    Deliver(s, slot, points);
}

void ApplyFromHost(int32_t total) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    // Stash the latest; Tick (game thread) writes it + RETRIES until the saveSlot
    // resolves (a connect-edge BalanceSync can arrive before the gamemode is ready).
    uint32_t seq = g_pendingSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seq == 0) seq = g_pendingSeq.fetch_add(1, std::memory_order_relaxed) + 1;  // 0 means "none"
    g_pending.store((static_cast<uint64_t>(seq) << 32) | static_cast<uint32_t>(total),
                    std::memory_order_release);
}

void CreditLocal(int32_t amount) {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool host      = s && s->role() == coop::net::Role::Host;
    const bool connected = s && s->connected();
    if (host || !connected) {
        // Host OR solo (no live session): apply locally. If hosting, the next TickHost
        // poll sees the new Points and broadcasts it to all clients.
        GT::Post([amount] { E::AddPoints(amount); });
        UE_LOGI("balance_sync: local credit %+d (%s)", amount,
                host ? "host -> will broadcast" : "solo");
        return;
    }
    // A connected CLIENT cannot AUTHOR the shared balance, and that half is deliberate: the lane
    // that let it -- a delta the host applied through AddPoints with no value bound -- was retired
    // whole rather than clamped, because the balance is host-authoritative everywhere. Client-side
    // earning still reaches it, but only where an intent lane forwards the action and the host
    // re-runs the game's own verb on the authoritative instance, as coingun_collect does for a coin
    // pickup. A direct credit from here is not that, so it is refused: defence in depth, since
    // coop::dev_gate already refuses the only caller on a client.
    UE_LOGW("balance_sync: local credit %+d REFUSED -- a client cannot author the shared "
            "balance; earning reaches it through an intent lane the host re-runs", amount);
}

void OnDisconnect() {
    g_delivery = {};
    g_pending.store(0, std::memory_order_release);
}

}  // namespace coop::balance_sync
