// coop/player/hand_extinguisher_sync.cpp -- see coop/player/hand_extinguisher_sync.h. Ported from the
// WatchersOfTheVoid fork (MIT): the extinguisher and the washer only (the buckets and the trowel are the
// record lane's), and the washer's spray is a hand pulse, since the fork's water projectile lane is not here.

#include "coop/player/hand_extinguisher_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/actors/pressure_washer.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::hand_extinguisher_sync {
namespace {

namespace FX = ue_wrap::fire_extinguisher;
namespace Water = ue_wrap::pressure_washer;
using net::HandExtinguisherPayload;
using ue_wrap::CachedObjRef;

constexpr uint8_t kToolExtinguisher = 0;
constexpr uint8_t kToolWasher = 1;
constexpr uint64_t kSprayStaleMs = 1500;  // a spray not refreshed for this long is shown stopped
constexpr uint64_t kWasherPulseMs = 200;  // the washer's burst rate on a mirror

float MaxCharge(uint8_t tool) { return tool == kToolWasher ? 10.f : 1.5f; }

// One slot's hand: the newest sample and when it was taken or arrived; on the host, the peers it is still
// owed to; on a peer showing it, the mirror it was written to and whether that mirror shows the spray.
struct Row {
    HandExtinguisherPayload state{};
    uint64_t sentAt = 0;
    uint32_t pending = 0;
    bool have = false;
    bool applied = false;
    bool sprayVisible = false;
    CachedObjRef appliedActor;
    uint8_t appliedTool = 0;
    uint64_t pulseAt = 0;
};

std::atomic<net::Session*> g_session{nullptr};
std::array<Row, net::kMaxPeers> g_rows{};
uint32_t g_world = 0;
uint32_t g_localSequence = 0;
uint64_t g_epoch = 0;
uint16_t g_people[net::kMaxPeers]{};
bool g_ticking = false;
uint64_t g_nextPoll = 0;
uint64_t g_rateAt[net::kMaxPeers]{};
uint8_t g_rateCount[net::kMaxPeers]{};

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
net::Session* Session() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}
bool After(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
bool Current(net::Session* session, uint64_t epoch) {
    return session && Session() == session && g_epoch == epoch && g_world == ue_wrap::world_identity::Generation();
}

// Take a mirror's spray down: the actor this row lit, if it is still a live display mirror nobody else's row
// names, has its particle and sound stopped.
void StopRow(Row& row) {
    const CachedObjRef actor = row.appliedActor;
    const uint8_t tool = row.appliedTool;
    const bool visible = row.sprayVisible;
    row.appliedActor.Reset();
    row.applied = false;
    row.sprayVisible = false;
    void* a = actor.Get();
    if (!a || !visible || tool != kToolExtinguisher || !hand_item::IsMirrorActor(a)) return;
    for (const Row& other : g_rows)
        if (other.appliedActor.Is(a)) return;
    FX::SetRemoteSpray(a, false);
}

void ResetRows() {
    auto previous = g_rows;
    ++g_epoch;
    g_rows = {};
    g_localSequence = 0;
    g_nextPoll = 0;
    std::fill(std::begin(g_rateAt), std::end(g_rateAt), 0);
    std::fill(std::begin(g_rateCount), std::end(g_rateCount), 0);
    for (auto& person : g_people) person = 0;
    for (auto& row : previous) StopRow(row);
}

void World() {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world == g_world) return;
    ResetRows();
    g_world = world;
}

// A slot whose occupant changed is a peer gone: its row and what it was owed go with it.
void ReconcileOwners() {
    for (int slot = 0; slot < net::kMaxPeers; ++slot) {
        const uint16_t person = roster_ledger::Get(slot).playerNo;
        if (person == g_people[slot]) continue;
        if (g_people[slot]) OnPeerLeft(slot);
        g_people[slot] = person;
    }
}

uint32_t ReadyMask(net::Session& s, uint8_t owner) {
    uint32_t mask = 0;
    for (int slot = 1; slot < net::kMaxPeers; ++slot)
        if (slot != owner && s.IsSlotWorldReady(slot)) mask |= uint32_t(1u << slot);
    return mask;
}

// The tool the hand actor is, or false for neither.
bool ToolOf(void* actor, uint8_t& tool) {
    if (!actor) return false;
    if (Water::IsWasher(actor)) { tool = kToolWasher; return true; }
    if (FX::IsExtinguisher(actor)) { tool = kToolExtinguisher; return true; }
    return false;
}

bool ReadState(void* actor, uint8_t tool, bool& spraying, float& charge) {
    if (tool == kToolWasher) {
        if (!Water::Read(actor, spraying, charge)) return false;
    } else if (!FX::ReadSpraying(actor, spraying) || !FX::ReadCharge(actor, charge)) {
        return false;
    }
    if (!std::isfinite(charge) || charge > MaxCharge(tool)) return false;
    // The game's drain can cross zero by one frame before the spray stops.
    charge = (std::max)(0.f, charge);
    if (charge == 0.f) spraying = false;
    return true;
}

// The owner: sample the hand and queue a change, or a refresh while it sprays.
void SampleLocal(net::Session& s, uint64_t now) {
    const uint8_t slot = players::Registry::Get().LocalPeerId();
    if (slot >= net::kMaxPeers) return;
    void* actor = hand_item::LocalHandActor();
    const uint32_t generation = hand_item::Generation(slot);
    const uint16_t person = roster_ledger::Get(slot).playerNo;
    uint8_t tool = 0;
    if (!generation || !person || !ToolOf(actor, tool)) {
        g_rows[slot] = {};
        return;
    }
    bool spraying = false;
    float charge = 0.f;
    if (!ReadState(actor, tool, spraying, charge)) return;
    Row& row = g_rows[slot];
    const bool same = row.have && row.state.generation == generation && row.state.tool == tool &&
                      row.state.spraying == uint8_t(spraying) && std::fabs(row.state.charge - charge) < .01f;
    if (same && now - row.sentAt < (spraying ? 500u : 1000u)) return;
    HandExtinguisherPayload p{};
    p.slot = slot;
    p.spraying = uint8_t(spraying);
    p.tool = tool;
    p.reserved = 0;
    p.generation = generation;
    if (!++g_localSequence) ++g_localSequence;
    p.sequence = g_localSequence;
    p.charge = charge;
    p.playerNo = person;
    if (s.role() == net::Role::Client) {
        if (!s.SendReliableToSlot(0, net::ReliableKind::HandExtinguisher, &p, sizeof(p))) return;  // next poll
        row.state = p;
        row.have = true;
        row.sentAt = now;
        return;
    }
    row.state = p;
    row.have = true;
    row.sentAt = now;
    row.pending = ReadyMask(s, slot);
}

// Each remote slot's newest sample onto the mirror of its generation.
void ApplyToMirrors(net::Session& s, uint64_t epoch, uint64_t now) {
    const uint8_t local = players::Registry::Get().LocalPeerId();
    for (uint8_t slot = 0; slot < net::kMaxPeers; ++slot) {
        if (slot == local) continue;  // the owner's own hand is the game's
        Row& row = g_rows[slot];
        if (!row.have) continue;
        const HandExtinguisherPayload state = row.state;
        const bool spraying = state.spraying && state.charge > 0.f && now - row.sentAt <= kSprayStaleMs;
        void* actor = hand_item::MirrorGeneration(slot) == state.generation ? hand_item::MirrorActorForSlot(slot) : nullptr;
        // A mirror replaced, or a spray gone stale: the old display is taken down first.
        if (row.appliedActor.Raw() &&
            (row.appliedActor.Get() != actor || row.appliedTool != state.tool || (row.sprayVisible && !spraying))) {
            StopRow(row);
            if (!Current(&s, epoch)) return;
        }
        if (!actor) continue;  // HandItem and this lane cross at a switch or a join: wait for the mirror
        if (!row.applied) {
            bool ok;
            if (state.tool == kToolWasher) {
                ok = Water::SetRemoteLevel(actor, state.charge);
            } else {
                ok = FX::WriteCharge(actor, state.charge);
                // The particle and sound are switched only on a change: a Play every refresh restarts the sound.
                if (ok && (!row.appliedActor.Is(actor) || row.sprayVisible != spraying)) {
                    ok = FX::SetRemoteSpray(actor, spraying);
                    if (ok) row.sprayVisible = spraying;
                }
            }
            if (!Current(&s, epoch)) return;
            if (!ok) continue;
            row.applied = true;
            row.appliedActor.Set(actor);
            row.appliedTool = state.tool;
            if (state.tool == kToolWasher) row.sprayVisible = spraying;
        }
        if (!spraying) continue;
        coop::RemotePlayer* puppet = players::Registry::Get().Puppet(slot);
        if (!puppet || !puppet->valid() || !puppet->HasPose()) continue;
        const ue_wrap::FRotator aim = puppet->GetSyncedAimRotation();
        if (state.tool == kToolExtinguisher) {
            FX::AimRemoteSpray(actor, aim);
        } else if (now >= row.pulseAt) {
            row.pulseAt = now + kWasherPulseMs;
            ue_wrap::FVector origin{};
            if (ue_wrap::engine::TryGetActorLocation(actor, origin)) Water::EmitRemote(actor, origin, aim);
        }
        if (!Current(&s, epoch)) return;
    }
}

// Host: every kept sample to the peers it is owed to; a spray gone stale is sent stopped.
void Flush(net::Session& s, uint64_t now) {
    for (Row& row : g_rows) {
        if (!row.have || !row.pending) continue;
        HandExtinguisherPayload state = row.state;
        if (now - row.sentAt > kSprayStaleMs) state.spraying = 0;
        for (int slot = 1; slot < net::kMaxPeers; ++slot) {
            const uint32_t bit = uint32_t(1u << slot);
            if (!(row.pending & bit) || !s.IsSlotWorldReady(slot)) continue;
            if (s.SendReliableToSlot(slot, net::ReliableKind::HandExtinguisher, &state, sizeof(state)))
                row.pending &= uint32_t(~bit);
        }
    }
}

}  // namespace

void Install(net::Session* session) {
    if (g_session.load() != session) OnDisconnect();
    g_session.store(session, std::memory_order_release);
    FX::ResolveNames();
    World();
}

void Tick() {
    net::Session* s = Session();
    if (!s || g_ticking) return;
    struct Guard { Guard() { g_ticking = true; } ~Guard() { g_ticking = false; } } guard;
    World();
    ReconcileOwners();
    const uint64_t epoch = g_epoch;
    const uint64_t now = NowMs();
    if (!FX::ResolveNames()) return;
    if (now >= g_nextPoll) {
        g_nextPoll = now + 100;
        SampleLocal(*s, now);
        if (!Current(s, epoch)) return;
    }
    ApplyToMirrors(*s, epoch, now);
    if (Current(s, epoch) && s->role() == net::Role::Host) Flush(*s, now);
}

void Receive(const HandExtinguisherPayload& p, uint8_t senderSlot) {
    net::Session* s = Session();
    if (!s || p.slot >= net::kMaxPeers || !p.generation || !p.sequence || !p.playerNo || p.spraying > 1 ||
        p.tool > kToolWasher || p.reserved || !std::isfinite(p.charge) || p.charge < 0.f ||
        p.charge > MaxCharge(p.tool))
        return;
    World();
    ReconcileOwners();
    if (roster_ledger::Get(p.slot).playerNo != p.playerNo) return;
    const uint64_t now = NowMs();
    if (s->role() == net::Role::Host) {
        // A client speaks for its own hand alone, at the owner's pace (ten a second at most).
        if (senderSlot == 0 || senderSlot != p.slot || senderSlot >= net::kMaxPeers || !s->IsSlotWorldReady(senderSlot))
            return;
        if (now - g_rateAt[senderSlot] >= 1000) { g_rateAt[senderSlot] = now; g_rateCount[senderSlot] = 0; }
        if (g_rateCount[senderSlot] >= 20) return;
        ++g_rateCount[senderSlot];
    } else if (senderSlot != 0 || p.slot == players::Registry::Get().LocalPeerId()) {
        return;
    }
    Row& row = g_rows[p.slot];
    if (row.have && !After(p.sequence, row.state.sequence)) return;
    row.state = p;
    row.have = true;
    row.applied = false;
    row.sentAt = now;
    if (s->role() == net::Role::Host) row.pending = ReadyMask(*s, p.slot);
}

void QueueConnect(int slot) {
    if (slot <= 0 || slot >= net::kMaxPeers) return;
    net::Session* s = Session();
    if (!s || s->role() != net::Role::Host) return;
    World();
    for (Row& row : g_rows)
        if (row.have && row.state.slot != slot) row.pending |= uint32_t(1u << slot);
}

void OnPeerLeft(int slot) {
    if (slot < 0 || slot >= net::kMaxPeers) return;
    Row previous = g_rows[slot];
    g_rows[slot] = {};
    g_people[slot] = 0;
    g_rateAt[slot] = 0;
    g_rateCount[slot] = 0;
    for (Row& row : g_rows) row.pending &= uint32_t(~(1u << slot));
    StopRow(previous);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    ResetRows();
    g_world = 0;
}

}  // namespace coop::hand_extinguisher_sync
