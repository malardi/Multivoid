// coop/props/pry_break_sync.cpp -- see coop/props/pry_break_sync.h.

#include "coop/props/pry_break_sync.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"  // the sender's puppet, the pour's player
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/call.h"            // ParamFrame / Call (the host's pour)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // FunctionParams / ParamOffset
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <string>

namespace coop::pry_break_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;
namespace CB = coop::net::container_break;

// Every class's function of these names fires the watch, so each callback gates on its class first:
// crowbarOpen is also a door's (door_verb_intent owns those), broken() also a bomb barrel's (the
// explosion lane owns those).
UE_FACT_FN(kRowCrowbarOpen, "pry_break_sync", nullptr, L"crowbarOpen", AnyClass);
UE_FACT_FN(kRowBroken, "pry_break_sync", nullptr, L"broken", AnyClass);
UE_FACT_FN(kRowBrokenFire, "pry_break_sync", nullptr, L"broken_fire", AnyClass);
// A gas can pours through its target's verb: fuelUp (the ATV, the portable generator) or gascanFuel (the wood
// chipper, a lamp, a torch, another can). Both hand the can to the target, which calls the can's getFuel on its own
// fuel field.
UE_FACT_FN(kRowFuelUp, "pry_break_sync", nullptr, L"fuelUp", AnyClass);
UE_FACT_FN(kRowGascanFuel, "pry_break_sync", nullptr, L"gascanFuel", AnyClass);

constexpr int kTagPry        = 0x50425059;  // 'PBPY'
constexpr int kTagBroken     = 0x50424252;  // 'PBBR'
constexpr int kTagBrokenFire = 0x50424246;  // 'PBBF'
constexpr int kTagFuelUp     = 0x50424655;  // 'PBFU'
constexpr int kTagGascanFuel = 0x50424746;  // 'PBGF'
// The host pours from its own gas can nearest the target, within this of it: the client's can is in the
// client's hand beside the target, and its copy on the host is driven there.
constexpr float kFuelCanReachUU = 600.0f;

struct Watch { const ue_wrap::facts::Function* row; int tag; sg::PreFn pre; const char* what; };
enum class Reg : uint8_t { Pending, Registered, Refused };

// A plank's pry runs the crowbar's animation before the plank lets go, so its release is looked for over
// a window rather than at the call: every tick until its Static and frozen are both off, or until this.
constexpr uint64_t kPlankWatchMs = 10000;
constexpr int      kMaxPlanks    = 16;

// A container breaks where it is hit, or where it lands when thrown, so its reach is wider than a press:
// the authority adds the container's bounds and the puppet's lag allowance to it.
constexpr float kBreakReachUU = 1500.0f;
// A gas can or a barrel is set off from afar too (a shot, a thrown object), so its reach is wider.
constexpr float kBlastReachUU = 5000.0f;
constexpr coop::net::IntentBudget kBreakBudget{4.0f, 4.0f};
constexpr size_t   kMaxPendingBreaks = 8;
constexpr uint64_t kRefusalSayMs     = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};

void* g_plankClass     = nullptr;  // prop_pryablePlank_C
void* g_containerClass = nullptr;  // prop_container_C
void* g_gascanClass    = nullptr;  // prop_gascan_C: its broken() is a fuel can's explosion
void* g_barrelClass    = nullptr;  // prop_bombBarrel_C: its broken() is the barrel's explosion

struct PendingPlank {
    void*    prop = nullptr;
    int32_t  idx = -1;
    uint64_t untilMs = 0;
};
PendingPlank g_planks[kMaxPlanks];
int g_plankCount = 0;

struct Slot {
    coop::net::IntentQueue<coop::net::ContainerBreakIntentPayload> queue;
    uint64_t nextSayMs = 0;
    bool     waitSaid  = false;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

uint64_t g_pliesSeen = 0, g_planksSent = 0, g_planksApplied = 0, g_plankTimeouts = 0;
uint64_t g_breaksSent = 0, g_breaksRan = 0, g_breaksDenied = 0, g_breaksNative = 0;
uint64_t g_fuelsSent = 0, g_fuelsRan = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsA(void* obj, void*& cls, const wchar_t* name) {
    if (!obj) return false;
    if (!cls) cls = ue_wrap::object_index::ClassByName(name);
    void* oc = R::ClassOf(obj);
    return cls && oc && R::IsDescendantOfAny(oc, &cls, 1);
}
bool IsPlank(void* obj)     { return IsA(obj, g_plankClass, L"prop_pryablePlank_C"); }
bool IsContainer(void* obj) { return IsA(obj, g_containerClass, L"prop_container_C"); }
// What breaks through broken()/broken_fire() and must break once, on the host: a container spills its
// loot, a gas can and a bomb barrel spawn their explosion. A client's own explosion is dissolved by the
// explosion lane (only launcher shots and the ATV are re-asked of the host there), so a client's break
// of a can or a barrel left it broken with no blast anywhere; through the host it explodes for all.
bool IsBreakable(void* obj) {
    return IsContainer(obj) || IsA(obj, g_gascanClass, L"prop_gascan_C") || IsA(obj, g_barrelClass, L"prop_bombBarrel_C");
}

bool Stuck(void* prop) { return PR::IsStatic(prop) || PR::IsFrozen(prop); }

// ---- planks: the prying peer sends the release ------------------------------------------------
sg::Verdict OnPryPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return sg::Verdict::Run;  // SP: nothing to mirror
    if (!call.object || !IsPlank(call.object)) return sg::Verdict::Run;
    // A client's call on an object of a world it has not announced ready is that world's load. The world
    // announce is a client's edge: the host's own world is the session's, and its pries always count.
    if (s->role() == coop::net::Role::Client && !coop::net_pump::IsInAnnouncedWorld(call.object))
        return sg::Verdict::Run;
    if (!Stuck(call.object)) return sg::Verdict::Run;  // already free: nothing to send
    ++g_pliesSeen;
    const uint64_t until = NowMs() + kPlankWatchMs;
    for (int i = 0; i < g_plankCount; ++i) {
        if (g_planks[i].prop == call.object) { g_planks[i].untilMs = until; return sg::Verdict::Run; }
    }
    if (g_plankCount >= kMaxPlanks) {
        UE_LOGW("[PRY] pending planks full (%d) -- this pry runs here alone", kMaxPlanks);
        return sg::Verdict::Run;
    }
    g_planks[g_plankCount++] = PendingPlank{call.object, R::InternalIndexOf(call.object), until};
    UE_LOGI("[PRY] a crowbar pries plank %p key='%ls' -- its release is watched", call.object,
            PR::GetInteractableKeyString(call.object).c_str());
    return sg::Verdict::Run;
}

void SendPlankRelease(coop::net::Session& s, void* prop) {
    coop::net::PropStickStatePayload p{};
    const std::wstring keyW = PR::GetInteractableKeyString(prop);
    if (!keyW.empty() && keyW != L"None") coop::net::WireKeyFromString(keyW, p.key);
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(prop);
    p.elementId = (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
    if (p.key.len == 0 && p.elementId == 0) {
        UE_LOGW("[PRY] plank %p has neither key nor eid -- its release is not sent", prop);
        return;
    }
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(prop, loc) || !E::TryGetActorRotation(prop, rot)) {
        UE_LOGW("[PRY] plank key='%ls' has no readable pose -- its release is not sent", keyW.c_str());
        return;
    }
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.flags = 0;    // the unstick
    p.holdGen = 0;  // an unstick ends no hold
    const PR::VelocityState v = PR::GetPhysicsVelocity(prop);
    if (v.ok) {
        p.linVelX = v.linearCmS.X;   p.linVelY = v.linearCmS.Y;   p.linVelZ = v.linearCmS.Z;
        p.angVelX = v.angularDegS.X; p.angVelY = v.angularDegS.Y; p.angVelZ = v.angularDegS.Z;
    }
    if (!s.SendReliable(coop::net::ReliableKind::PropStickState, &p, sizeof(p))) {
        UE_LOGW("[PRY] the release of plank key='%ls' was not sent (the session refused it)", keyW.c_str());
        return;
    }
    ++g_planksSent;
    UE_LOGI("[PRY] plank key='%ls' eid=%u let go here -- release sent, pose=(%.0f,%.0f,%.0f)", keyW.c_str(),
            p.elementId, p.locX, p.locY, p.locZ);
}

void TickPlanks(coop::net::Session& s) {
    if (g_plankCount == 0) return;
    const uint64_t now = NowMs();
    int w = 0;
    for (int i = 0; i < g_plankCount; ++i) {
        PendingPlank pp = g_planks[i];
        if (!pp.prop || !R::IsLiveByIndex(pp.prop, pp.idx)) continue;  // gone since the pry
        if (!Stuck(pp.prop)) {
            if (s.connected()) SendPlankRelease(s, pp.prop);
            continue;
        }
        if (now >= pp.untilMs) {
            ++g_plankTimeouts;  // the pry did not free it (a pry the game refused, or one still going)
            continue;
        }
        g_planks[w++] = pp;
    }
    g_plankCount = w;
}

// ---- containers: a client asks the host -------------------------------------------------------
sg::Verdict OnBreakPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object || !IsBreakable(call.object)) return sg::Verdict::Run;
    // A call on an object of a world this client has not announced ready is that world's load: native.
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    const std::wstring key = PR::GetInteractableKeyString(call.object);
    if (key.empty() || key == L"None") {
        ++g_breaksNative;
        UE_LOGW("[CRATE] container %p has no key the host could resolve -- its break runs here alone", call.object);
        return sg::Verdict::Run;
    }
    coop::net::ContainerBreakIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = (call.tag == kTagBrokenFire) ? CB::kBrokenFire : CB::kBroken;
    if (!s->SendReliable(coop::net::ReliableKind::ContainerBreakIntent, &p, sizeof(p))) {
        UE_LOGW("[CRATE] the break of key='%ls' was not sent (the session refused it); it stays whole as the "
                "host has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_breaksSent;
    UE_LOGI("[CRATE] CLIENT SENT a %s of key='%ls' (#%llu)", p.verb == CB::kBrokenFire ? "fire break" : "break",
            key.c_str(), static_cast<unsigned long long>(g_breaksSent));
    return sg::Verdict::Cancel;
}

// ---- refuelling: a client's pour runs here and on the host ------------------------------------
// The client's own pour runs (its copy fills at once, as it would alone), and the host pours the same into its
// own copy, the one the ATV's stream and the save carry; before this the host's stream wrote the ATV's fuel back
// to empty a moment later and the host's save never had it.
sg::Verdict OnFuelPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object || !coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    const std::wstring key = PR::GetInteractableKeyString(call.object);
    if (key.empty() || key == L"None") {
        UE_LOGW("[FUEL] %ls %p has no key the host could resolve -- its fuel stays here alone",
                R::ClassNameOf(call.object).c_str(), call.object);
        return sg::Verdict::Run;
    }
    coop::net::ContainerBreakIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = (call.tag == kTagFuelUp) ? CB::kFuelUp : CB::kGascanFuel;
    if (call.locals && call.function) {
        static R::ParamOffset sBack(L"back");
        const int32_t bo = sBack.Of(call.function);
        if (bo >= 0) p._pad[0] = call.locals[bo] ? 1 : 0;
    }
    if (s->SendReliable(coop::net::ReliableKind::ContainerBreakIntent, &p, sizeof(p))) {
        ++g_fuelsSent;
        UE_LOGI("[FUEL] CLIENT SENT a pour%s into %ls key='%ls' (#%llu)", p._pad[0] ? " back" : "",
                R::ClassNameOf(call.object).c_str(), key.c_str(), static_cast<unsigned long long>(g_fuelsSent));
    }
    return sg::Verdict::Run;
}

struct NearestCan { ue_wrap::FVector at; void* best; float bestD2; };

void* NearestGascan(void* target) {
    NearestCan n{};
    if (!E::TryGetActorLocation(target, n.at)) return nullptr;
    n.bestD2 = kFuelCanReachUU * kFuelCanReachUU;
    if (!g_gascanClass) g_gascanClass = ue_wrap::object_index::ClassByName(L"prop_gascan_C");
    if (!g_gascanClass) return nullptr;
    ue_wrap::object_index::ForEachInstance(g_gascanClass, [](void* ctx, void* obj, int32_t) {
        auto* nc = static_cast<NearestCan*>(ctx);
        ue_wrap::FVector a{};
        if (!E::TryGetActorLocation(obj, a)) return;
        const float dx = a.X - nc->at.X, dy = a.Y - nc->at.Y, dz = a.Z - nc->at.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < nc->bestD2) { nc->bestD2 = d2; nc->best = obj; }
    }, &n);
    return n.best;
}

bool ExecuteFuel(coop::net::Session& s, const coop::net::ContainerBreakIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* target = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    const wchar_t* verb = (p.verb == CB::kFuelUp) ? L"fuelUp" : L"gascanFuel";
    void* fn = target ? R::FindDispatchFunctionCached(R::ClassOf(target), verb) : nullptr;
    if (!fn) {
        UE_LOGI("[FUEL] DENY slot=%u key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(),
                target ? "it has no such verb" : "nothing holds that key here");
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kBreakReachUU).Authorize(target);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) return false;
    if (!subject) {
        UE_LOGI("[FUEL] DENY slot=%u key='%ls' -- %s (%.0f uu of %.0f)", static_cast<unsigned>(slot), key.c_str(),
                coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU);
        return true;
    }
    void* can = NearestGascan(target);
    if (!can || can == target) {
        UE_LOGI("[FUEL] DENY slot=%u key='%ls' -- no gas can of the host's within %.0f uu of it",
                static_cast<unsigned>(slot), key.c_str(), kFuelCanReachUU);
        return true;
    }
    ue_wrap::ParamFrame f(fn);
    bool canSet = false;
    for (const auto& prm : R::FunctionParams(fn)) {
        std::wstring n = prm.name;
        for (auto& ch : n) ch = static_cast<wchar_t>(::towlower(ch));
        if (n == L"back") { f.Set<bool>(prm.name.c_str(), p._pad[0] != 0); continue; }
        if (prm.size != static_cast<int32_t>(sizeof(void*))) continue;
        if (n == L"player") {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
            void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
            f.Set<void*>(prm.name.c_str(), puppet);
        } else if (!canSet) {
            f.Set<void*>(prm.name.c_str(), can);
            canSet = true;
        }
    }
    const bool ran = canSet && ue_wrap::Call(target, f);
    if (ran) ++g_fuelsRan;
    UE_LOGI("[FUEL] host ran slot %u's %ls%s on key='%ls' from its can %p: %s", static_cast<unsigned>(slot), verb,
            p._pad[0] ? " (back)" : "", key.c_str(), can, ran ? "ran" : "did not run");
    return true;
}

// False keeps the break at its sender's queue head (no body to measure its reach from yet).
bool ExecuteBreak(coop::net::Session& s, const coop::net::ContainerBreakIntentPayload& p, uint8_t slot) {
    if (p.verb == CB::kFuelUp || p.verb == CB::kGascanFuel) return ExecuteFuel(s, p, slot);
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* box = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!box || !IsBreakable(box)) {
        ++g_breaksDenied;
        UE_LOGI("[CRATE] DENY slot=%u key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(),
                box ? "that prop does not break this way" : "nothing holds that key here (already broken?)");
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, IsContainer(box) ? kBreakReachUU : kBlastReachUU).Authorize(box);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[CRATE] slot %u's breaks wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        ++g_breaksDenied;
        UE_LOGI("[CRATE] DENY slot=%u key='%ls' -- %s (%.0f uu of %.0f)", static_cast<unsigned>(slot), key.c_str(),
                coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU);
        return true;
    }
    const wchar_t* verb = (p.verb == CB::kBrokenFire) ? L"broken_fire" : L"broken";
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(box), verb);
    const bool ran = fn && R::CallFunction(box, fn, nullptr);
    if (ran) ++g_breaksRan; else ++g_breaksDenied;
    UE_LOGI("[CRATE] host ran slot %u's %ls on key='%ls': %s", static_cast<unsigned>(slot), verb, key.c_str(),
            ran ? "ran" : "the verb did not resolve or run");
    return true;
}

// ---- watches ---------------------------------------------------------------------------------
const Watch kWatches[] = {
    { &kRowCrowbarOpen, kTagPry,        &OnPryPre,   "crowbarOpen" },
    { &kRowBroken,      kTagBroken,     &OnBreakPre, "broken" },
    { &kRowBrokenFire,  kTagBrokenFire, &OnBreakPre, "broken_fire" },
    { &kRowFuelUp,      kTagFuelUp,     &OnFuelPre,  "fuelUp" },
    { &kRowGascanFuel,  kTagGascanFuel, &OnFuelPre,  "gascanFuel" },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, kWatches[i].pre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[PRY-BREAK] the script-body gate refused the watch on '%s' -- it runs only where it is used",
                kWatches[i].what);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        int live = 0, pending = 0;
        for (int i = 0; i < kWatchCount; ++i) {
            if (g_reg[i] == Reg::Pending) ++pending;
            else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
        }
        if (live == kWatchCount) {
            g_settled = true;
            UE_LOGI("[PRY-BREAK] the plank pry and container break gates are live");
        } else if (pending == 0 && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[PRY-BREAK] %d of %d gates are dead -- those verbs run only where they are used",
                    kWatchCount - live, kWatchCount);
        }
    }
    if (!session.running()) return;
    TickPlanks(session);
    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBreakBudget, NowMs, [&](const coop::net::ContainerBreakIntentPayload& p) {
            if (!ExecuteBreak(session, p, slot)) return coop::net::Served::Wait;
            sl.waitSaid = false;
            return coop::net::Served::Done;
        });
    }
}

void OnContainerBreakIntent(coop::net::Session& session, const coop::net::ContainerBreakIntentPayload& payload,
                            uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    // Run at once, in the drain: the client's own destroy of the container follows its break on the same
    // lane (session_lanes.h pins both to Bulk), and a break left for the next tick would find the container
    // that destroy already removed. Only a sender with no body here yet, or one with breaks still queued
    // ahead of this one, waits in the queue.
    if (sl.queue.size() == 0 && ExecuteBreak(session, payload, senderSlot)) return;
    auto copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPendingBreaks)) {
        const uint64_t now = NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[CRATE] slot %u queue full (%zu) -- refusing its breaks until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_breaksDenied;
    }
}

bool TryApplyPlankFree(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW,
                       uint8_t senderPeerSlot) {
    if (!IsPlank(prop)) return false;
    const bool wasStuck = Stuck(prop);
    if (wasStuck) {
        // The game's own setter, whose init() derives physics and collision from the flags, as a load does.
        if (!PR::CallSetPropProps(prop, /*isStatic=*/false, /*frozen=*/false, /*sleeping=*/false)) {
            PR::WriteStatic(prop, false);
            PR::WriteFrozen(prop, false);
        }
        E::SetActorSimulatePhysics(prop, true);
    }
    const bool owned = coop::remote_prop::IsOwnedHere(prop);
    if (!owned) {
        E::SetActorLocation(prop, ue_wrap::FVector{p.locX, p.locY, p.locZ});
        E::SetActorRotation(prop, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
        if (void* mesh = PR::GetStaticMesh(prop)) {
            E::SetComponentLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
            E::SetComponentAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
        }
    }
    ++g_planksApplied;
    UE_LOGI("[PRY] plank key='%ls' eid=%u let go here (was %s; %s; slot %u)", keyW.c_str(), p.elementId,
            wasStuck ? "nailed" : "already free", owned ? "another hold moves it" : "from the sender's pose",
            senderPeerSlot);
    return true;
}

void OnDisconnect() {
    if (g_pliesSeen || g_planksSent || g_planksApplied || g_breaksSent || g_breaksRan || g_breaksDenied)
        UE_LOGI("[PRY-BREAK] session end -- pries=%llu planks sent=%llu applied=%llu timed out=%llu; breaks "
                "sent=%llu ran=%llu denied=%llu native=%llu",
                static_cast<unsigned long long>(g_pliesSeen), static_cast<unsigned long long>(g_planksSent),
                static_cast<unsigned long long>(g_planksApplied), static_cast<unsigned long long>(g_plankTimeouts),
                static_cast<unsigned long long>(g_breaksSent), static_cast<unsigned long long>(g_breaksRan),
                static_cast<unsigned long long>(g_breaksDenied), static_cast<unsigned long long>(g_breaksNative));
    g_plankCount = 0;
    g_pliesSeen = g_planksSent = g_planksApplied = g_plankTimeouts = 0;
    g_breaksSent = g_breaksRan = g_breaksDenied = g_breaksNative = 0;
}

}  // namespace coop::pry_break_sync
