// coop/player/hand_fire_sync.cpp -- see coop/player/hand_fire_sync.h. Ported from the WatchersOfTheVoid
// fork (MIT): its watches are folded into prop_fire_sync's (MirrorVerbRuns), and its samples name our hand
// generations.

#include "coop/player/hand_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_fire_sync.h"  // WatchesLive: the mirror guard's watches
#include "ue_wrap/actors/prop_burn.h"
#include "ue_wrap/actors/torch.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>

namespace coop::hand_fire_sync {
namespace {

namespace F = ue_wrap::prop_fire;
namespace Torch = ue_wrap::torch;
using net::HandFirePayload;

// What burns (HandFirePayload::kind): a burning prop's flame, or a torch's own.
constexpr uint16_t kFlameProp = 0;
constexpr uint16_t kFlameTorch = 1;
constexpr float kTorchFuelMax = 10000.f;  // a bound on a torch's fuel off the wire, far past a full one
using ue_wrap::CachedObjRef;

// One slot's hand flame: the newest sample, when it arrived (the fuel is aged from there), the mirror it
// was applied to, and on the host the peers it is still owed to.
struct Row {
    HandFirePayload state{};
    CachedObjRef actor;
    uint64_t received = 0;
    uint64_t retryAt = 0;
    uint32_t pending = 0;
    bool applied = false;
    bool shownBurning = false, shownOnce = false;  // the flame last logged on the mirror
};
std::array<Row, net::kMaxPeers> g_rows;
std::atomic<net::Session*> g_session{nullptr};
uint64_t g_epoch = 0, g_poll = 0, g_lastSample = 0;
uint32_t g_world = 0, g_sequence = 0;
bool g_ticking = false;
CachedObjRef g_applying;  // the mirror this lane is applying a sample to (MirrorVerbRuns)
// Host: samples taken per sender in the current second. The owner sends at most four a second.
uint64_t g_rateAt[net::kMaxPeers]{};
uint8_t g_rateCount[net::kMaxPeers]{};

uint64_t Now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
net::Session* Session() { auto* s = g_session.load(); return s && s->connected() ? s : nullptr; }
uint8_t Self() { return players::Registry::Get().LocalPeerId(); }
bool After(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
F::State Decode(const HandFirePayload& p) { return {p.burning != 0, p.fuel, p.fuelSpeed}; }

// The sample as it stands now: its fuel burned down at its own speed since it arrived.
F::State Aged(const Row& r, uint64_t now) {
    F::State value = Decode(r.state);
    value.fuel = (std::max)(0.f, value.fuel - value.fuelSpeed * float(now >= r.received ? now - r.received : 0) / 1000.f);
    if (value.fuel <= 0) value.burning = false;
    return value;
}

void Clear() {
    ++g_epoch;
    g_rows = {};
    g_poll = g_lastSample = 0;
    g_sequence = 0;
    std::fill(std::begin(g_rateAt), std::end(g_rateAt), 0);
    std::fill(std::begin(g_rateCount), std::end(g_rateCount), 0);
    // g_applying and g_ticking are stack-owned guards: a native callback may clear the session under them.
}
void World() {
    const uint32_t w = ue_wrap::world_identity::Generation();
    if (w != g_world) { Clear(); g_world = w; }
}
bool Current(uint64_t epoch) { return epoch == g_epoch && Session() && g_world == ue_wrap::world_identity::Generation(); }

// Who a sample of `owner`'s hand goes to: the host from a client; every other world-ready client from the host.
uint32_t Destinations(net::Session& s, uint8_t owner) {
    if (s.role() == net::Role::Client) return 1;
    uint32_t mask = 0;
    for (int slot = 1; slot < net::kMaxPeers; ++slot)
        if (slot != owner && s.IsSlotWorldReady(slot)) mask |= uint32_t(1u << slot);
    return mask;
}

// A sample's fire is one its kind can hold.
bool ValidFlame(uint16_t kind, const F::State& v) {
    if (kind == kFlameTorch)
        return std::isfinite(v.fuel) && v.fuel >= 0.f && v.fuel <= kTorchFuelMax && v.fuelSpeed == 0.f;
    return kind == kFlameProp && F::Valid(v);
}

// The hand actor's flame: a torch's own (its fuel, not aged by the receivers), or a burning prop's.
bool ReadFlame(void* a, uint16_t& kind, F::State& value) {
    if (Torch::IsTorch(a)) {
        bool burning = false;
        float fuel = 0.f;
        if (!Torch::Read(a, burning, fuel)) return false;
        kind = kFlameTorch;
        value = {burning, (std::max)(0.f, fuel), 0.f};
        return ValidFlame(kind, value);
    }
    kind = kFlameProp;
    return F::Read(a, value);
}

// A torch mirror's flame to the sample's, shown as the torch shows its own (ue_wrap/actors/torch): the fuel and the
// flag written, then its upd(). Not its ignite: that passes through a prop flame and puts it out with an
// extinguishFire a tick later, which the mirror guard would refuse outside this apply, leaving the prop flame lit.
bool ApplyTorch(void* a, const F::State& want, const std::function<bool()>& valid) {
    if (!Torch::Show(a, want.burning, want.fuel) || !valid()) return false;
    bool burning = false;
    float fuel = 0.f;
    return Torch::Read(a, burning, fuel) && burning == want.burning;
}

// The owner: sample the hand's flame and queue it when it changed beyond what the receivers predict.
void SampleOwn(net::Session& s, uint8_t self, uint64_t epoch, uint64_t now) {
    Row& own = g_rows[self];
    void* a = hand_item::LocalHandActor();
    const uint32_t generation = hand_item::Generation(self);
    F::State value{};
    uint16_t kind = kFlameProp;
    if (!a || !generation || !ReadFlame(a, kind, value)) {
        if (!generation) own = {};
        return;
    }
    if (!Current(epoch) || hand_item::Generation(self) != generation || hand_item::LocalHandActor() != a) return;
    const F::State predicted = Aged(own, now);
    // A hand that never burned is not news on a new generation: the receivers' mirror starts unlit.
    if (own.state.generation != generation && !value.burning && !own.state.burning) {
        own.state.generation = generation;
        own.state.kind = kind;
        return;
    }
    // While nothing burns only the lighting is news; while it burns, the fuel drifting from what the receivers age it
    // to, and a refresh every 2 s.
    const bool changed = own.state.generation != generation || own.state.kind != kind ||
                         value.burning != (own.state.burning != 0) ||
                         (value.burning && (std::fabs(value.fuelSpeed - own.state.fuelSpeed) > .01f ||
                                            std::fabs(value.fuel - predicted.fuel) > 1.f ||
                                            now - g_lastSample >= 2000));
    if (!changed) return;
    if (!++g_sequence) ++g_sequence;
    own.state = {generation, g_sequence, value.fuel, value.fuelSpeed, self, uint8_t(value.burning), kind};
    own.received = now;
    own.pending = Destinations(s, self);
    g_lastSample = now;
}

void SendOwed(net::Session& s, uint64_t now) {
    for (auto& r : g_rows) {
        if (!r.state.sequence || !r.pending) continue;
        HandFirePayload p = r.state;
        const F::State aged = Aged(r, now);
        p.fuel = aged.fuel;
        p.burning = uint8_t(aged.burning);
        for (int slot = 0; slot < net::kMaxPeers; ++slot) {
            if (!(r.pending & (1u << slot))) continue;
            if (s.role() == net::Role::Host && !s.IsSlotWorldReady(slot)) continue;
            if (s.SendReliableToSlot(slot, net::ReliableKind::HandFire, &p, sizeof(p)))
                r.pending &= uint32_t(~(1u << slot));
        }
    }
}

// Each remote slot's newest sample onto the mirror of its generation, once per mirror and sample. A rejected
// apply (an underwater mirror refuses its ignite) is retried at 4 Hz, never every frame.
void ApplyToMirrors(uint8_t self, uint64_t epoch, uint64_t now) {
    for (uint8_t slot = 0; slot < net::kMaxPeers; ++slot) {
        if (slot == self) continue;
        Row& r = g_rows[slot];
        const HandFirePayload p = r.state;
        void* a = hand_item::MirrorActorForSlot(slot);
        if (!a || !p.sequence || hand_item::MirrorGeneration(slot) != p.generation) continue;
        if (!r.actor.Is(a)) { r.actor.Set(a); r.applied = false; r.retryAt = 0; }
        if (r.applied || now < r.retryAt) continue;
        r.retryAt = now + 250;
        const CachedObjRef identity = r.actor;
        const auto valid = [&] {
            return Current(epoch) && identity.Is(a) && r.state.sequence == p.sequence &&
                   hand_item::MirrorActorForSlot(slot) == a && hand_item::MirrorGeneration(slot) == p.generation;
        };
        struct ApplyGuard {
            CachedObjRef previous = g_applying;
            explicit ApplyGuard(void* actor) { g_applying.Set(actor); }
            ~ApplyGuard() { g_applying = previous; }
        } apply(a);
        const F::State value = Aged(r, now);
        const bool ok = p.kind == kFlameTorch ? ApplyTorch(a, value, valid) : F::Apply(a, value, valid);
        if (!Current(epoch)) return;
        if (ok && valid()) {
            r.applied = true;
            // One line per change of the mirror's flame, for the lab's log checks.
            if (value.burning != r.shownBurning || !r.shownOnce) {
                r.shownBurning = value.burning;
                r.shownOnce = true;
                UE_LOGI("hand_fire: slot %u mirror gen %u %s (fuel %.1f)", static_cast<unsigned>(slot), p.generation,
                        value.burning ? "BURNING" : "out", value.fuel);
            }
        }
    }
}

}  // namespace

void Install(net::Session* s) {
    if (g_session.load() != s) Clear();
    g_session.store(s);
    World();
}

bool MirrorVerbRuns(void* mirror, bool applyVerb) {
    return applyVerb && mirror && g_applying.Is(mirror);
}

void Receive(const HandFirePayload& p, uint8_t sender) {
    net::Session* s = Session();
    if (!s || p.slot >= net::kMaxPeers || !p.generation || !p.sequence || p.burning > 1 ||
        !ValidFlame(p.kind, Decode(p)) || p.slot == Self())
        return;
    if (s->role() == net::Role::Host) {
        // A client speaks for its own hand alone, at the owner's pace.
        if (!sender || sender >= net::kMaxPeers || sender != p.slot || !s->IsSlotWorldReady(sender)) return;
        const uint64_t now = Now();
        if (now - g_rateAt[sender] >= 1000) { g_rateAt[sender] = now; g_rateCount[sender] = 0; }
        if (g_rateCount[sender] >= 8) return;
        ++g_rateCount[sender];
    } else if (sender != 0) {
        return;
    }
    World();
    Row& r = g_rows[p.slot];
    if (r.state.sequence && !After(p.sequence, r.state.sequence)) return;
    // HandItem and HandFire may cross at a spawn or a join: the newest sample is kept, and applied once a
    // mirror of its generation stands.
    r.state = p;
    r.received = Now();
    r.applied = false;
    r.retryAt = 0;
    r.pending = s->role() == net::Role::Host ? Destinations(*s, p.slot) : 0;
}

void QueueConnect(int slot) {
    if (slot <= 0 || slot >= net::kMaxPeers) return;
    World();
    for (auto& r : g_rows)
        if (r.state.sequence && r.state.slot != slot) r.pending |= uint32_t(1u << slot);
    g_poll = 0;
}

void OnPeerLeft(int slot) {
    if (slot <= 0 || slot >= net::kMaxPeers) return;
    ++g_epoch;
    g_rows[slot] = {};
    g_rateAt[slot] = 0;
    g_rateCount[slot] = 0;
    for (auto& r : g_rows) r.pending &= uint32_t(~(1u << slot));
}

void OnDisconnect() {
    g_session.store(nullptr);
    Clear();
    g_world = 0;
}

void Tick() {
    net::Session* s = Session();
    if (!s || g_ticking) return;
    struct Guard { Guard() { g_ticking = true; } ~Guard() { g_ticking = false; } } guard;
    World();
    const uint64_t epoch = g_epoch, now = Now();
    // Without the mirror guard live, the mirror's own fire verbs would run unchecked.
    if (!Current(epoch) || !coop::prop_fire_sync::WatchesLive()) return;
    const uint8_t self = Self();
    if (self >= net::kMaxPeers) return;
    if (now >= g_poll) {
        g_poll = now + 250;
        SampleOwn(*s, self, epoch, now);
        if (!Current(epoch)) return;
        SendOwed(*s, now);
        if (!Current(epoch)) return;
    }
    ApplyToMirrors(self, epoch, now);
}

}  // namespace coop::hand_fire_sync
