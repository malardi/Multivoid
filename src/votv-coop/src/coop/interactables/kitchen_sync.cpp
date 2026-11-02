// coop/interactables/kitchen_sync.cpp -- see coop/interactables/kitchen_sync.h.

#include "coop/interactables/kitchen_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/kitchen_machine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::kitchen_sync {
namespace {

namespace K  = ue_wrap::kitchen_machine;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using ue_wrap::CachedObjRef;

// Each watch names its class: another class's body of the same name is never asked (the fork watched
// actionOptionIndex, Open and Close on every class, which met the doors' and the coffee lanes' own).
// The presses: a client's runs on the host.
UE_FACT_FN(kRowMwPress, "kitchen_sync", L"prop_microwave_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowOvPress, "kitchen_sync", L"prop_minioven_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowGrPress, "kitchen_sync", L"prop_bbq_C", L"actionOptionIndex", Declared);
// What cooks, burns fuel or plays the grill's cooking: the host's alone, refused on a client's shared machine.
UE_FACT_FN(kRowMwWave, "kitchen_sync", L"prop_microwave_C", L"wave", Declared);
UE_FACT_FN(kRowOvTick, "kitchen_sync", L"prop_minioven_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowGrTick, "kitchen_sync", L"prop_bbq_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowGrSound, "kitchen_sync", L"prop_bbq_C", L"playSound", Declared);
UE_FACT_FN(kRowGrTlUpdate, "kitchen_sync", L"prop_bbq_C", L"Timeline_0__UpdateFunc", Declared);
UE_FACT_FN(kRowGrTlFinished, "kitchen_sync", L"prop_bbq_C", L"Timeline_0__FinishedFunc", Declared);
// The grill's stock and fire: a client's ignite is the host's; a coal or lid it put in or took off is written on the
// host after it ran here; the host's own are published at once.
UE_FACT_FN(kRowGrIgnite, "kitchen_sync", L"prop_bbq_C", L"attemptIgnite", Declared);
UE_FACT_FN(kRowGrCoal, "kitchen_sync", L"prop_bbq_C", L"addCoal", Declared);
UE_FACT_FN(kRowGrLid, "kitchen_sync", L"prop_bbq_C", L"setLid", Declared);

enum Tag : int {
    kTagPress    = 0x4B540101,  // 'KT' 01 01
    kTagProducer = 0x4B540102,
    kTagIgnite   = 0x4B540103,
    kTagCoal     = 0x4B540104,
    kTagLid      = 0x4B540105,
    kTagGrPress  = 0x4B540106,
};

enum Op : uint8_t { kOpPress = 1, kOpIgnite = 2, kOpExtinguish = 3, kOpCoal = 4, kOpLid = 5 };

// The grill's actions on actionOptionIndex: 4 puts it out, 7 takes the lid off (the lid is born in the hand).
constexpr uint8_t kActionUse = 4, kActionMwMode = 8, kActionLidTake = 7;

struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
};

sg::Verdict OnPress(const sg::Call& c);
sg::Verdict OnGrillPress(const sg::Call& c);
void AfterGrillPress(const sg::Call& c);
sg::Verdict OnProducer(const sg::Call& c);
sg::Verdict OnIgnite(const sg::Call& c);
void AfterGrillChange(const sg::Call& c);

const Watch kWatches[] = {
    {&kRowMwPress, kTagPress, &OnPress, nullptr},
    {&kRowOvPress, kTagPress, &OnPress, nullptr},
    {&kRowGrPress, kTagGrPress, &OnGrillPress, &AfterGrillPress},
    {&kRowMwWave, kTagProducer, &OnProducer, nullptr},
    {&kRowOvTick, kTagProducer, &OnProducer, nullptr},
    {&kRowGrTick, kTagProducer, &OnProducer, nullptr},
    {&kRowGrSound, kTagProducer, &OnProducer, nullptr},
    {&kRowGrTlUpdate, kTagProducer, &OnProducer, nullptr},
    {&kRowGrTlFinished, kTagProducer, &OnProducer, nullptr},
    {&kRowGrIgnite, kTagIgnite, &OnIgnite, &AfterGrillChange},
    {&kRowGrCoal, kTagCoal, nullptr, &AfterGrillChange},
    {&kRowGrLid, kTagLid, nullptr, &AfterGrillChange},
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// The reach a sender's use is judged against: the look-at trace's default 200, doubled, as the coffee lane.
constexpr float kReachUU = 400.0f;
constexpr coop::net::IntentBudget kBudget{8.0f, 8.0f};
constexpr size_t kMaxPending = 16;

constexpr uint64_t kPollMs = 250;        // the host reads its machines this often
constexpr uint64_t kScanMs = 2000;       // and looks for new ones this often
constexpr uint64_t kHeartbeatMs = 3000;  // a row unchanged this long is sent again
constexpr uint64_t kHoldMs = 750;        // a client's own edit is not painted over by an older row for this long
constexpr size_t kMaxRows = 128;

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

std::atomic<coop::net::Session*> g_session{nullptr};
uint32_t g_world = 0;
uint32_t g_revision = 0, g_sequence = 0;
uint64_t g_nextPoll = 0, g_nextScan = 0, g_animAt = 0;
uint32_t g_seeds = 0;  // slots owed every row
int      g_lidTake = 0;

R::ParamOffset g_actionOff{L"action"}, g_actionOffOv{L"action"}, g_actionOffGr{L"action"};
R::ParamOffset g_playerOff{L"player"};

// The host's view of one machine: what it last sent and when, and who is still owed it.
struct HostRow {
    CachedObjRef actor;
    coop::net::KitchenStatePayload sent{};
    uint64_t sentAt = 0;
    uint32_t owed = 0;
    bool     have = false;
};
std::unordered_map<std::wstring, HostRow> g_host;

// A client's newest row of one machine, painted once onto the actor that holds its key.
struct ClientRow {
    coop::net::KitchenStatePayload row{};
    CachedObjRef painted;   // the actor the newest row was painted onto
    uint64_t holdUntil = 0; // the local player's own edit stands until then
    bool     fresh = false; // not painted yet
};
std::unordered_map<std::wstring, ClientRow> g_client;

struct Slot {
    coop::net::IntentQueue<coop::net::KitchenIntentPayload> queue;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

uint64_t g_sent = 0, g_ran = 0, g_refused = 0, g_painted = 0, g_cancelled = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}
bool IsClient() {
    auto* s = Live();
    return s && s->role() == coop::net::Role::Client;
}
bool IsHost() {
    auto* s = Live();
    return s && s->role() == coop::net::Role::Host;
}

void ResetWorld() {
    const uint32_t w = ue_wrap::world_identity::Generation();
    if (w == g_world) return;
    g_world = w;
    g_host.clear();
    g_client.clear();
    g_nextPoll = g_nextScan = g_animAt = 0;
    g_lidTake = 0;
}

// The machine's key as both peers hold it, or empty: a held machine is its holder's, and a client's machine of a
// world the host has not announced is its own load.
std::wstring KeyOf(void* a) {
    if (!a || K::KindOf(a) == K::Kind::None || coop::hand_item::IsHandAxisActor(a)) return {};
    std::wstring key = ue_wrap::prop::GetKeyString(a);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) return {};
    return key;
}
bool Shared(void* a) {
    if (KeyOf(a).empty()) return false;
    return !IsClient() || coop::net_pump::IsInAnnouncedWorld(a);
}

bool ToState(const coop::net::KitchenStatePayload& p, K::State& s) {
    s.kind = static_cast<K::Kind>(p.kind);
    s.flags = p.flags;
    s.mode = p.mode;
    s.remaining = p.remaining;
    s.timer = p.timer;
    s.power = p.power;
    s.coals = p.coals;
    s.burn = p.burn;
    s.volume = p.volume;
    return K::Valid(s);
}

void FromState(const K::State& s, coop::net::KitchenStatePayload& p) {
    p.kind = static_cast<uint8_t>(s.kind);
    p.flags = s.flags;
    p.mode = s.mode;
    p.remaining = s.remaining;
    p.timer = s.timer;
    p.power = s.power;
    p.coals = s.coals;
    p.burn = s.burn;
    p.volume = s.volume;
}

// Moved enough to send before the heartbeat: any flag, mode or count; the microwave's clock by half a second (it
// counts down every frame), the oven's power, the grill's burn by 2 %.
bool Moved(const coop::net::KitchenStatePayload& a, const coop::net::KitchenStatePayload& b) {
    return a.kind != b.kind || a.flags != b.flags || a.mode != b.mode || a.coals != b.coals ||
           std::fabs(a.remaining - b.remaining) >= 0.5f || std::fabs(a.timer - b.timer) >= 0.5f ||
           std::fabs(a.power - b.power) >= 0.01f || std::fabs(a.burn - b.burn) >= 0.02f ||
           std::fabs(a.volume - b.volume) >= 0.05f;
}

// ---- the host -------------------------------------------------------------------------------
// Send one machine's row: to every ready client when `slot` is negative, else to that slot alone.
void PublishRow(coop::net::Session& s, const std::wstring& key, HostRow& row, const coop::net::KitchenStatePayload& p,
                int slot) {
    coop::net::KitchenStatePayload out = p;
    coop::net::WireKeyFromString(key, out.key);
    if (slot < 0) {
        out.revision = ++g_revision;
        for (int peer = 1; peer < coop::net::kMaxPeers; ++peer) {
            if (!coop::roster_ledger::Get(peer).playerNo || !s.IsSlotWorldReady(peer)) continue;
            if (s.SendReliableToSlot(peer, coop::net::ReliableKind::KitchenState, &out, sizeof(out)))
                row.owed &= ~(1u << peer);
        }
        if (!row.have || row.sent.flags != out.flags || row.sent.mode != out.mode || row.sent.coals != out.coals)
            UE_LOGI("[KITCHEN] host: key='%ls' kind=%u flags=0x%02X mode=%d coals=%d rev=%u", key.c_str(),
                    static_cast<unsigned>(out.kind), static_cast<unsigned>(out.flags), out.mode, out.coals,
                    out.revision);
        row.sent = out;
        row.sentAt = NowMs();
        row.have = true;
        return;
    }
    out.revision = row.have ? row.sent.revision : ++g_revision;
    if (s.SendReliableToSlot(slot, coop::net::ReliableKind::KitchenState, &out, sizeof(out)))
        row.owed &= ~(1u << slot);
}

// Read the machine and send its row now if it moved (or `force`).
void PublishNow(void* a, bool force) {
    auto* s = Live();
    if (!s || s->role() != coop::net::Role::Host || !Shared(a)) return;
    const std::wstring key = KeyOf(a);
    K::State st{};
    if (!K::Read(a, st)) return;
    if (g_host.size() >= kMaxRows && !g_host.count(key)) return;
    HostRow& row = g_host[key];
    if (!row.actor.Is(a)) {
        row.actor.Set(a);
        force = true;
    }
    coop::net::KitchenStatePayload p{};
    FromState(st, p);
    if (force || !row.have || Moved(p, row.sent)) PublishRow(*s, key, row, p, -1);
}

void ScanInstances() {
    for (const wchar_t* name : {L"prop_microwave_C", L"prop_minioven_C", L"prop_bbq_C"}) {
        void* cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void*, void* a, int32_t) {
                if (!a || R::NameStartsWith(R::NameOf(a), L"Default__")) return;
                if (ue_wrap::world_identity::WorldOf(a) != ue_wrap::world_identity::CurrentWorld()) return;
                const std::wstring key = KeyOf(a);
                if (key.empty() || (g_host.size() >= kMaxRows && !g_host.count(key))) return;
                HostRow& row = g_host[key];
                if (!row.actor.Is(a)) {
                    row.actor.Set(a);
                    row.have = false;
                }
            },
            nullptr);
    }
}

void HostTick(coop::net::Session& s) {
    const uint64_t now = NowMs();
    // The queued intents, a sender at a bounded rate.
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const coop::net::KitchenIntentPayload& p) {
            const std::wstring key = coop::net::StringFromWireKey(p.key);
            void* a = key.empty() ? nullptr : coop::prop_key_index::Find(key);
            if (!a || K::KindOf(a) == K::Kind::None || !Shared(a)) {
                ++g_refused;
                return coop::net::Served::Done;
            }
            if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
                ++g_refused;
                return coop::net::Served::Done;
            }
            const auto subject = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(a);
            if (subject.outcome == coop::element::IntentOutcome::NoBody) return coop::net::Served::Wait;
            if (!subject) {
                ++g_refused;
                UE_LOGI("[KITCHEN] host: slot %u op %u on key='%ls' refused -- %s", static_cast<unsigned>(slot),
                        static_cast<unsigned>(p.op), key.c_str(), coop::element::OutcomeName(subject.outcome));
                return coop::net::Served::Done;
            }
            const K::Kind kind = K::KindOf(a);
            bool ok = false;
            switch (p.op) {
            case kOpPress: {
                coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
                void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
                ok = kind != K::Kind::Grill && K::Press(a, body, p.action, p.control);
                break;
            }
            case kOpIgnite:     ok = kind == K::Kind::Grill && K::SetGrillFire(a, true); break;
            case kOpExtinguish: ok = kind == K::Kind::Grill && K::SetGrillFire(a, false); break;
            case kOpCoal:       ok = kind == K::Kind::Grill && K::RaiseGrillCoals(a, p.value); break;
            case kOpLid:        ok = kind == K::Kind::Grill && K::SetGrillLid(a, p.value != 0); break;
            default: break;
            }
            if (ok) ++g_ran; else ++g_refused;
            UE_LOGI("[KITCHEN] host ran slot %u op %u action %u control %u value %d on key='%ls' -> %s",
                    static_cast<unsigned>(slot), static_cast<unsigned>(p.op), static_cast<unsigned>(p.action),
                    static_cast<unsigned>(p.control), p.value, key.c_str(), ok ? "ok" : "not taken");
            // Sent even when not taken: the sender's copy may have run ahead of the host's and is put back.
            PublishNow(a, true);
            return coop::net::Served::Done;
        });
    }
    if (now >= g_nextScan) {
        g_nextScan = now + kScanMs;
        ScanInstances();
    }
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;
    // What a joiner is owed rides with the poll.
    if (g_seeds) {
        for (auto& [key, row] : g_host) row.owed |= g_seeds;
        g_seeds = 0;
    }
    std::vector<std::wstring> gone;
    for (auto& [key, row] : g_host) {
        void* a = row.actor.Get();
        if (!a || KeyOf(a) != key) {
            gone.push_back(key);
            continue;
        }
        K::State st{};
        if (!K::Read(a, st)) continue;
        coop::net::KitchenStatePayload p{};
        FromState(st, p);
        if (!row.have || Moved(p, row.sent) || now - row.sentAt >= kHeartbeatMs) {
            PublishRow(s, key, row, p, -1);
        } else if (row.owed) {
            for (int peer = 1; peer < coop::net::kMaxPeers; ++peer)
                if ((row.owed & (1u << peer)) && s.IsSlotWorldReady(peer)) PublishRow(s, key, row, row.sent, peer);
        }
    }
    for (const auto& key : gone) g_host.erase(key);
}

// ---- the client -----------------------------------------------------------------------------
void SendIntent(void* a, uint8_t op, uint8_t action, uint8_t control, int32_t value) {
    auto* s = Live();
    if (!s || s->role() != coop::net::Role::Client) return;
    const std::wstring key = KeyOf(a);
    if (key.empty()) return;
    coop::net::KitchenIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.sequence = ++g_sequence;
    p.op = op;
    p.action = action;
    p.control = control;
    p.value = value;
    if (!s->SendReliable(coop::net::ReliableKind::KitchenIntent, &p, sizeof(p))) {
        UE_LOGW("[KITCHEN] client: op %u on key='%ls' was not sent; the machine stays as the host has it",
                static_cast<unsigned>(op), key.c_str());
        return;
    }
    ++g_sent;
    UE_LOGI("[KITCHEN] client sent op %u action %u control %u value %d on key='%ls'", static_cast<unsigned>(op),
            static_cast<unsigned>(action), static_cast<unsigned>(control), value, key.c_str());
    if (op == kOpCoal || op == kOpLid) {
        if (g_client.size() < kMaxRows || g_client.count(key)) g_client[key].holdUntil = NowMs() + kHoldMs;
    }
}

bool PaintRow(const std::wstring& key, ClientRow& cr) {
    void* a = coop::prop_key_index::Find(key);
    if (!a || !Shared(a)) return false;
    K::State st{};
    if (!ToState(cr.row, st) || K::KindOf(a) != st.kind) return false;
    CachedObjRef live;
    live.Set(a);
    const uint32_t world = g_world;
    if (!K::Apply(a, st, [&] { return live.Get() && world == ue_wrap::world_identity::Generation() && IsClient(); }))
        return false;
    cr.painted = live;
    cr.fresh = false;
    ++g_painted;
    return true;
}

void ClientTick() {
    const uint64_t now = NowMs();
    const float seconds = g_animAt ? static_cast<float>(now - g_animAt) / 1000.f : 0.f;
    g_animAt = now;
    const bool poll = now >= g_nextPoll;
    if (poll) g_nextPoll = now + kPollMs;
    for (auto& [key, cr] : g_client) {
        if (now < cr.holdUntil) continue;
        void* painted = cr.painted.Get();
        // A row waits for its actor; one painted onto an actor since replaced (a reload, a re-spawn) is painted again.
        if (cr.fresh || (poll && (!painted || coop::prop_key_index::Find(key) != painted))) {
            if (cr.row.kind) PaintRow(key, cr);
            painted = cr.painted.Get();
        }
        if (painted && cr.row.kind == static_cast<uint8_t>(K::Kind::Microwave)) K::Animate(painted, seconds);
    }
}

// ---- the watches ----------------------------------------------------------------------------
uint8_t ActionOf(const sg::Call& c, R::ParamOffset& off) {
    const int32_t o = c.function ? off.Of(c.function) : -1;
    if (!c.locals || o < 0) return 0xFF;
    return c.locals[o];
}

void* PlayerOf(const sg::Call& c) {
    const int32_t o = c.function ? g_playerOff.Of(c.function) : -1;
    if (!c.locals || o < 0) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, c.locals + o, sizeof(p));
    return p;
}

// A client's press of the microwave's or oven's button runs on the host; every other action runs here.
sg::Verdict OnPress(const sg::Call& c) {
    if (!IsClient() || c.fromOurCode || !Shared(c.object)) return sg::Verdict::Run;
    const K::Kind kind = K::KindOf(c.object);
    const uint8_t action = ActionOf(c, kind == K::Kind::MiniOven ? g_actionOffOv : g_actionOff);
    if (action != kActionUse && !(kind == K::Kind::Microwave && action == kActionMwMode)) return sg::Verdict::Run;
    const uint8_t control = K::ReadControl(c.object);
    // An oven press aimed at neither control does nothing on the host either.
    if (kind == K::Kind::MiniOven && !control) return sg::Verdict::Run;
    SendIntent(c.object, kOpPress, action, control, 0);
    ++g_cancelled;
    return sg::Verdict::Cancel;
}

// The grill: a client's extinguish runs on the host; its lid take runs here (the lid is born in its hand) and the
// bit crosses from setLid.
sg::Verdict OnGrillPress(const sg::Call& c) {
    if (!IsClient() || c.fromOurCode || !Shared(c.object)) return sg::Verdict::Run;
    const uint8_t action = ActionOf(c, g_actionOffGr);
    if (action == kActionUse) {
        SendIntent(c.object, kOpExtinguish, action, 0, 0);
        ++g_cancelled;
        return sg::Verdict::Cancel;
    }
    if (action == kActionLidTake && PlayerOf(c) == coop::players::Registry::Get().Local()) ++g_lidTake;
    return sg::Verdict::Run;
}
void AfterGrillPress(const sg::Call& c) {
    if (g_lidTake > 0 && ActionOf(c, g_actionOffGr) == kActionLidTake) --g_lidTake;
}

sg::Verdict OnProducer(const sg::Call& c) {
    return IsClient() && !c.fromOurCode && Shared(c.object) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

// A client's ignite (a lighter on the grill) is the host's: the host's own coals decide it.
sg::Verdict OnIgnite(const sg::Call& c) {
    if (!IsClient() || c.fromOurCode || !Shared(c.object)) return sg::Verdict::Run;
    K::State st{};
    if (K::Read(c.object, st) && (st.flags & K::kActive)) return sg::Verdict::Cancel;
    SendIntent(c.object, kOpIgnite, 0, 0, 0);
    ++g_cancelled;
    return sg::Verdict::Cancel;
}

// After the grill's ignite, coal or lid ran: the host publishes at once; a client's coal or lid (the item already
// spent or born here) is written on the host's copy.
void AfterGrillChange(const sg::Call& c) {
    if (c.fromOurCode || !Shared(c.object)) return;
    if (IsHost()) {
        PublishNow(c.object, false);
        return;
    }
    if (!IsClient()) return;
    K::State st{};
    if (!K::Read(c.object, st)) return;
    if (c.tag == kTagCoal) SendIntent(c.object, kOpCoal, 0, 0, st.coals);
    else if (c.tag == kTagLid) SendIntent(c.object, kOpLid, 0, 0, (st.flags & K::kLid) ? 1 : 0);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, kWatches[i].pre, kWatches[i].post)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[KITCHEN] the script-body gate refused the watch on '%ls' of '%ls'", kWatches[i].row->Name(),
                kWatches[i].row->Class());
    }
}

void Settle() {
    if (g_settled) return;
    int live = 0, pending = 0;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] == Reg::Pending) ++pending;
        else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
    }
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("[KITCHEN] all %d kitchen watches are live (microwave, mini oven, grill)", kWatchCount);
        return;
    }
    if (pending || sg::PendingNameCount() != 0) return;
    for (int i = 0; i < kWatchCount; ++i)
        if (!sg::NameWatchSettled(*kWatches[i].row, kWatches[i].tag)) return;
    g_settled = true;
    for (int i = 0; i < kWatchCount; ++i)
        if (!sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag))
            UE_LOGW("[KITCHEN] the watch on '%ls' of '%ls' is dead -- that verb runs only where it is made",
                    kWatches[i].row->Name(), kWatches[i].row->Class());
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick() {
    sg::ResolvePendingNames();
    Settle();
    // No watched body runs across ticks: a lid take whose post never came (a later pre cancelled it) ends here.
    g_lidTake = 0;
    auto* s = Live();
    if (!s) return;
    ResetWorld();
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else ClientTick();
}

void OnState(const coop::net::KitchenStatePayload& p) {
    if (!IsClient()) return;
    ResetWorld();
    K::State st{};
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || !ToState(p, st)) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty() || (g_client.size() >= kMaxRows && !g_client.count(key))) return;
    ClientRow& cr = g_client[key];
    if (cr.row.kind && static_cast<int32_t>(p.revision - cr.row.revision) <= 0) return;
    cr.row = p;
    cr.fresh = true;
    if (NowMs() >= cr.holdUntil) PaintRow(key, cr);
}

void OnIntent(const coop::net::KitchenIntentPayload& p, uint8_t senderSlot) {
    auto* s = Live();
    if (!s || s->role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || p.op < kOpPress || p.op > kOpLid) return;
    if (p.op == kOpCoal && (p.value < 1 || p.value > 16)) return;
    if (p.op == kOpLid && p.value != 0 && p.value != 1) return;
    Slot& sl = g_slots[senderSlot];
    auto copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) ++g_refused;
}

void QueueConnect(int slot) {
    if (slot > 0 && slot < coop::net::kMaxPeers) {
        g_seeds |= 1u << slot;
        g_nextPoll = 0;
    }
}

void OnPeerLeft(int slot) {
    if (slot <= 0 || slot >= coop::net::kMaxPeers) return;
    g_seeds &= ~(1u << slot);
    for (auto& [key, row] : g_host) row.owed &= ~(1u << slot);
}

void OnDisconnect() {
    if (g_sent || g_ran || g_refused || g_painted || g_cancelled)
        UE_LOGI("[KITCHEN] session end -- client sent=%llu cancelled=%llu painted=%llu; host ran=%llu refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_cancelled),
                static_cast<unsigned long long>(g_painted), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_refused));
    g_sent = g_ran = g_refused = g_painted = g_cancelled = 0;
    g_session.store(nullptr, std::memory_order_release);
    g_host.clear();
    g_client.clear();
    g_seeds = 0;
    g_world = 0;
    g_revision = g_sequence = 0;
    g_nextPoll = g_nextScan = g_animAt = 0;
    g_lidTake = 0;
}

bool LidTakeActive() {
    return g_lidTake > 0 && IsClient();
}

}  // namespace coop::kitchen_sync
