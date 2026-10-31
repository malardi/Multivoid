// coop/props/prop_record_refresh.cpp -- see coop/props/prop_record_refresh.h.

#include "coop/props/prop_record_refresh.h"

#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"
#include "coop/player/hand_item.h"
#include "coop/player/local_streams.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::props::prop_record_refresh {
namespace {

namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// The coalescing window. One player action can end in more than one upd() -- the open path runs
// it and the equip behind it can run it again -- and the record is the whole state either way,
// so the second publish would carry the same bytes. Game-thread serial, no lock.
constexpr auto kCoalesce = std::chrono::milliseconds(300);
std::unordered_map<void*, Clock::time_point> g_lastPublish;
// A change that fell inside the window still has to travel: the window drops the repeat of one action, but a
// second action in it (a second GPU, a claim right after an upgrade) changed the state again, and that state
// went nowhere until some later verb -- "an install sometimes does not show". The window's end publishes the
// prop once more, whatever it holds by then.
struct Trailing { int32_t idx; Clock::time_point due; };
std::unordered_map<void*, Trailing> g_trailing;

unsigned long long g_cPublished = 0;
unsigned long long g_cCoalesced = 0;
unsigned long long g_cRelooked  = 0;

// `opened` is read to drive the look below; `name` and init() are what re-derive it.
int32_t g_openedOff = -1;
int32_t g_nameOff   = -1;

// The box's two names, exactly as its own Blueprint spells them: the open action sets
// 'drivebox_sb', putLidOn sets 'drivebox_s' back.
constexpr const wchar_t* kNameOpen   = L"drivebox_sb";
constexpr const wchar_t* kNameClosed = L"drivebox_s";

bool ReadOpened(void* box, bool& out) {
    if (!box) return false;
    if (g_openedOff < 0) {
        void* cls = R::ClassOf(box);
        if (!cls) return false;
        g_openedOff = R::FindPropertyOffset(cls, L"opened");
        if (g_openedOff < 0) return false;
    }
    out = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(box) + g_openedOff);
    return true;
}

// A record has just landed on `box`. Its LOOK follows `name`, NOT `opened`: Aprop_C::init picks
// the mesh from that field and the box's own init() chains to it. And `name` is the one thing the
// record deliberately does not carry -- save_record's splice keeps element 0 of every group, which
// is where Aprop_C writes name, for the RECEIVER, so a sender's name is dropped by design. So the
// look is re-derived here from the state that did travel, with the pair the Blueprint itself uses,
// and init() re-runs the mesh pick. Idempotent: a name already right returns before the write, so
// an init() that loops back through upd() stops on the second pass.
void RefreshLook(void* box, bool opened) {
    void* cls = R::ClassOf(box);
    if (!cls) return;
    if (g_nameOff < 0) g_nameOff = R::FindPropertyOffset(cls, L"name");
    // Looked up on the box's own class at each use, through the lookup memoised by the class's slot
    // and serial: two hash lookups and two slot-and-serial checks, and never a function of a class that
    // is gone.
    void* const initFn = R::FindDispatchFunctionCached(cls, L"init");
    if (g_nameOff < 0 || !initFn) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            UE_LOGW("prop_record_refresh: no `name` offset or no init() on the box -- a mirrored open "
                    "will set the state without the open mesh (it behaves open and looks shut)"); }
        return;
    }
    const wchar_t* want = opened ? kNameOpen : kNameClosed;
    if (ue_wrap::prop::GetPropNameString(box) == want) return;  // already the right look
    R::FName n = ue_wrap::fname_utils::StringToFName(want);
    std::memcpy(reinterpret_cast<uint8_t*>(box) + g_nameOff, &n, sizeof(n));
    ue_wrap::ParamFrame f(initFn);
    if (f.valid()) ue_wrap::Call(box, f);
    ++g_cRelooked;
    UE_LOGI("prop_record_refresh: a record landed on a drive box -> opened=%d, look re-derived to '%ls'",
            opened ? 1 : 0, want);
}

// The reel case's record fields, for the logs: `lid`, and each slot's reel progress (-1 = empty).
int32_t g_lidOff = -1, g_topOff = -1, g_bottomOff = -1;

std::string DescribeMiner(void* m) {
    if (!m) return "-";
    void* cls = R::ClassOf(m);
    const int32_t pts = R::FindPropertyOffset(cls, L"accumulatedPoints");
    const int32_t pow = R::FindPropertyOffset(cls, L"powered");
    char b[96];
    std::snprintf(b, sizeof(b), "points=%d powered=%d",
                  pts >= 0 ? *reinterpret_cast<const int32_t*>(static_cast<const char*>(m) + pts) : -1,
                  pow >= 0 ? (*reinterpret_cast<const uint8_t*>(static_cast<const char*>(m) + pow) ? 1 : 0) : -1);
    return b;
}

std::string DescribeBox(void* box) {
    bool opened = false;
    return ReadOpened(box, opened) ? (opened ? "opened=1" : "opened=0") : "opened=?";
}

std::string DescribeReelCase(void* rc) {
    if (g_lidOff < 0) {
        void* cls = R::ClassOf(rc);
        if (!cls) return "?";
        g_lidOff    = R::FindPropertyOffset(cls, L"lid");
        g_topOff    = R::FindPropertyOffset(cls, L"reeltop");
        g_bottomOff = R::FindPropertyOffset(cls, L"reelBottom");
    }
    if (g_lidOff < 0 || g_topOff < 0 || g_bottomOff < 0) return "?";
    const char* b = reinterpret_cast<const char*>(rc);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "lid=%d reeltop=%.2f reelBottom=%.2f",
                  *reinterpret_cast<const bool*>(b + g_lidOff) ? 1 : 0,
                  *reinterpret_cast<const float*>(b + g_topOff),
                  *reinterpret_cast<const float*>(b + g_bottomOff));
    return buf;
}

// The watched classes. `relook`: the drive box's look does not follow its record and is
// re-derived on receipt (RefreshLook); the reel case's upd() reads `lid` itself.
UE_FACT_FN(kRowUpdBox,     "prop_record_refresh", L"prop_box_C",     L"upd", Dispatch);
UE_FACT_FN(kRowUpdReelbox, "prop_record_refresh", L"prop_reelbox_C", L"upd", Dispatch);
// The crypto miner: its state (power, GPUs and fans, the points it holds, its heat) is its save record, and
// none of it travelled, so a client's upgrade, claim or plug changed only its own copy and the host's kept
// mining into points the client had already claimed. Its three verbs that change it publish the record; the
// host also republishes every live miner on a slow beat (below), since the points grow on each peer's tick.
UE_FACT_FN(kRowMinerUpgrade, "prop_record_refresh", L"prop_miner_C", L"upgrade", Dispatch);
UE_FACT_FN(kRowMinerAction,  "prop_record_refresh", L"prop_miner_C", L"actionOptionIndex", Dispatch);
UE_FACT_FN(kRowMinerPlugged, "prop_record_refresh", L"prop_miner_C", L"plugged", Dispatch);

struct Watched {
    const ue_wrap::facts::Function* row;
    int tag;
    bool relook;
    std::string (*describe)(void*);
};
constexpr Watched kWatched[] = {
    { &kRowUpdBox,     0x44424F58, true,  &DescribeBox },       // 'DBOX'
    { &kRowUpdReelbox, 0x5242584F, false, &DescribeReelCase },  // 'RBXO'
    { &kRowMinerUpgrade, 0x4D4E5550, false, &DescribeMiner },   // 'MNUP'
    { &kRowMinerAction,  0x4D4E4143, false, &DescribeMiner },   // 'MNAC'
    { &kRowMinerPlugged, 0x4D4E504C, false, &DescribeMiner },   // 'MNPL'
};
// The UFunction each watch was armed on, so a re-armed one can be recognised. A world change can
// re-create a Blueprint class and its functions (127 were in a same-process rejoin; these two kept
// theirs): at a new address the old watch would never fire again, and at a recycled one it would
// fire for whatever lives there. An exact watch is re-validated; the neighbouring lanes use name
// watches, which resolve themselves.
ue_wrap::facts::Resolved g_watchedFn[std::size(kWatched)] = {};  // game thread only

void HostMinerBeat();  // below: the host's slow republish of every live miner
void BucketBeat();     // below: a bucket's water, the host's and a holder's
void ArmBucketTick(); // below: a client's copy of a bucket spills nothing on its own
void ArmMaterialWatches(); // below: a client copy of a concrete bucket does not tick; a player's verb marks a prop
void MaterialBeat();       // below: a concrete bucket's and a cement bag's stock, the host's and a toucher's

// The trailing publishes whose window has closed. Game thread, from the per-tick pump.
void FlushTrailing() {
    if (g_trailing.empty()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const auto now = Clock::now();
    for (auto e = g_trailing.begin(); e != g_trailing.end();) {
        if (now < e->second.due) { ++e; continue; }
        void* actor = e->first;
        const int32_t idx = e->second.idx;
        e = g_trailing.erase(e);
        if (!s || !s->connected() || !R::IsLiveByIndex(actor, idx)) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
        if (key.empty() || key == L"None") continue;
        g_lastPublish[actor] = now;
        if (coop::prop_save_data::Publish(s, actor, key)) {
            ++g_cPublished;
            UE_LOGI("prop_record_refresh: published key='%ls' at its window's end (a change inside the window)",
                    key.c_str());
        }
    }
}

const Watched* ByTag(int tag) {
    for (const Watched& w : kWatched)
        if (w.tag == tag) return &w;
    return nullptr;
}

void OnUpdPost(const sg::Call& call) {
    const Watched* w = ByTag(call.tag);
    if (!w || !call.object) return;
    // Our own apply is the one caller to ignore: prop_save_data puts a record in through the
    // prop's loadData, whose tail calls upd(), so without this an applied record would publish
    // itself straight back and the two peers would trade one record forever. The neighbouring
    // lanes gate on the same flag for the same reason. Logged rather than dropped silently: this
    // line is the far side's receipt, and it says which state actually landed.
    if (call.fromOurCode) {
        bool opened = false;
        if (w->relook) {
            if (ReadOpened(call.object, opened)) RefreshLook(call.object, opened);
        } else {
            UE_LOGI("prop_record_refresh: a record landed on %ls -> %s", w->row->Class(),
                    w->describe(call.object).c_str());
        }
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    void* actor = call.object;
    // The load churn is not a change. A joining client runs every save prop's loadData, which
    // ends in upd(), so without this the join would send the host an intent per prop describing
    // the state the host has just sent us. The host's world is the reconciled authority, so the
    // gate never holds there.
    if (s->role() == coop::net::Role::Client &&
        !coop::join_membership_sweep::HasLoadTailQuiesced()) {
        return;
    }
    const auto now = Clock::now();
    auto it = g_lastPublish.find(actor);
    if (it != g_lastPublish.end() && now - it->second < kCoalesce) {
        ++g_cCoalesced;
        g_trailing[actor] = Trailing{R::InternalIndexOf(actor), it->second + kCoalesce};
        return;
    }
    // Evict what the window has passed. The map is keyed on the actor's address, so without this it
    // both grows for the life of the session and can hold a dead box's entry against a new one the
    // allocator puts at the same address -- which would swallow that box's first publish.
    for (auto e = g_lastPublish.begin(); e != g_lastPublish.end();)
        e = (now - e->second >= kCoalesce) ? g_lastPublish.erase(e) : std::next(e);

    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None") {
        // A prop mid-construction: the key is minted inside the construction script, which calls
        // upd() itself. Nothing to address a record to, and the birth paths publish it anyway.
        return;
    }
    g_lastPublish[actor] = now;
    // Host: the record to every peer. Client: an intent the host validates and re-publishes, which
    // is also the acknowledgement. Either way the far side's loadData applies it and calls upd(),
    // so the lid, the contents and the repaint all land together.
    if (coop::prop_save_data::Publish(s, actor, key)) {
        ++g_cPublished;
        UE_LOGI("prop_record_refresh: published %ls key='%ls' %s (%s)", w->row->Class(), key.c_str(),
                w->describe(actor).c_str(),
                s->role() == coop::net::Role::Client ? "client intent" : "host broadcast");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!GT::IsGameThread()) return;
    HostMinerBeat();
    ArmBucketTick();
    BucketBeat();
    ArmMaterialWatches();
    MaterialBeat();
    FlushTrailing();
    // Throttle the class walks while a class is unresolved, the shape prop_spawn_authoring and
    // prop_drop_intent use: this Install is the per-tick retry pump, a FindClass MISS is not cached
    // (a class can load later), and a walk renders a name per object, so an unresolved class would
    // cost one full object-array walk per frame for as long as the world goes without one. A class
    // that IS resolved costs a memoised lookup, so the pass below still runs every frame for it and
    // re-arms the watch when a world load hands back a new function.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }
    for (size_t i = 0; i < std::size(kWatched); ++i) {
        const Watched& w = kWatched[i];
        void* cls = R::FindClass(w.row->Class());
        if (!cls) { s_retry = 60; continue; }  // not loaded yet -- a second of frames from now
        // The memoised lookup holds its answer by slot and serial, so a class re-created by a world
        // load answers with the NEW function and the comparison below re-arms on it.
        void* fn = R::FindDispatchFunctionCached(cls, w.row->Name());
        if (fn && fn == g_watchedFn[i].Fn()) continue;  // armed, on the function that is here now
        if (!fn) {
            static bool s_saidMissing[std::size(kWatched)] = {};
            if (!s_saidMissing[i]) {
                s_saidMissing[i] = true;
                UE_LOGW("prop_record_refresh: %ls::upd UFunction not found -- its record will only "
                        "travel on its birth paths (an in-place change stays local)", w.row->Class());
            }
            s_retry = 60;
            continue;
        }
        // The script gate, not a ProcessEvent observer: the prop calls its own upd() from inside
        // its Blueprint, a route that never reaches ProcessEvent, and an observer there saw
        // nothing. The watch is on the exact UFunction rather than on the name `upd`, which half
        // the props in the game declare.
        if (g_watchedFn[i] && g_watchedFn[i].Fn() != fn)
            sg::Unwatch(g_watchedFn[i], w.tag, nullptr, &OnUpdPost);  // the world that owned it is gone
        const ue_wrap::facts::Resolved armed = ue_wrap::facts::Resolve(*w.row);
        if (!armed || !sg::Watch(armed, w.tag, nullptr, &OnUpdPost)) {
            // Back off with the same sixty frames the unresolved-class branch takes, and say so
            // once: a refusal persists (the gate did not install, or its table is full), so an
            // unthrottled retry would warn and re-resolve every frame for the life of the process.
            s_retry = 60;
            static bool s_saidRefused = false;
            if (!s_saidRefused) {
                s_saidRefused = true;
                UE_LOGW("prop_record_refresh: script-gate watch on %ls::upd refused -- retrying", w.row->Class());
            }
            continue;
        }
        const bool rearm = static_cast<bool>(g_watchedFn[i]);
        g_watchedFn[i] = armed;
        UE_LOGI("prop_record_refresh: %hs %ls::upd -- it republishes its own save record on "
                "every in-place change", rearm ? "re-armed on" : "watching", w.row->Class());
    }
}

namespace {
// HOST: every live miner's record on a slow beat. Each peer's own tick grows the points and the heat, so
// the copies drift between verbs; the host's is the one that counts.
void HostMinerBeat() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    static Clock::time_point sNext{};
    const auto now = Clock::now();
    if (now < sNext) return;
    sNext = now + std::chrono::seconds(4);
    void* cls = ue_wrap::object_index::ClassByName(L"prop_miner_C");
    if (!cls) return;
    struct Ctx { coop::net::Session* s; int n; } ctx{s, 0};
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") return;
        if (coop::prop_save_data::Publish(x->s, obj, key)) ++x->n;
    }, &ctx);
    static int sSaid = 0;
    if (ctx.n > 0 && sSaid++ < 3)
        UE_LOGI("prop_record_refresh: host beat published %d miner record(s)", ctx.n);
}
}  // namespace

bool HoldsLocalChange(void* actor) {
    if (!actor) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return false;
    if (g_trailing.count(actor)) return true;
    const auto it = g_lastPublish.find(actor);
    return it != g_lastPublish.end() && Clock::now() - it->second < std::chrono::milliseconds(800);
}

void ClearMaterialState();  // below

void OnDisconnect() {
    ClearMaterialState();
    if (g_cPublished || g_cCoalesced || g_cRelooked)
        UE_LOGI("prop_record_refresh: session end -- published=%llu coalesced=%llu relooked=%llu",
                g_cPublished, g_cCoalesced, g_cRelooked);
    g_lastPublish.clear();
    g_trailing.clear();
    g_cPublished = 0;
    g_cCoalesced = 0;
    g_cRelooked  = 0;
    g_session.store(nullptr, std::memory_order_release);
}


namespace {
// ---- the bucket's water -----------------------------------------------------------------------------
// A bucket's water (height, its soap and alcohol) is its save record, and nothing carried it: each peer's
// copy filled and spilled by its own tick (which empties it down to its rim whenever its copy tilts, and
// fills it while it floats), so a bucket read 44 on the host and 40 on a client ("133%" against "123%").
// The host's water is the one that counts: a client's copy runs its tick but keeps the water it had, unless
// its own player holds it, and the record travels when the water changes -- from the host, or from the
// client whose hand changed it (an intent the host re-publishes).
UE_FACT_FN(kRowBucketTick, "prop_record_refresh", L"prop_bucket_C", L"ReceiveTick", Declared);
constexpr int kBucketTickTag = 0x424B5454;  // 'BKTT'
bool g_bucketTickArmed = false;

struct BucketOffs { int32_t height = -1, interp = -1, soap = -1, alcOff = -1, emptyOff = -1; uint8_t alcMask = 0, emptyMask = 0; bool ok = false; };
BucketOffs OffsFor(void* b) {
    BucketOffs o;
    void* cls = b ? R::ClassOf(b) : nullptr;
    if (!cls) return o;
    o.height = R::FindPropertyOffset(cls, L"height");
    o.interp = R::FindPropertyOffset(cls, L"height_interp");
    o.soap   = R::FindPropertyOffset(cls, L"soap");
    R::FindBoolProperty(cls, L"alcohol", o.alcOff, o.alcMask);
    R::FindBoolProperty(cls, L"empty", o.emptyOff, o.emptyMask);
    o.ok = o.height >= 0 && o.soap >= 0;
    return o;
}
float ReadF(void* b, int32_t off) { float v = 0.f; std::memcpy(&v, static_cast<const char*>(b) + off, sizeof v); return v; }

bool IsMine(void* b) {
    return b == coop::local_streams::LastHeldActor() || coop::hand_item::IsHandAxisActor(b);
}

// A display mirror of another player's hand item: no world prop, and its state is that player's.
bool IsRemoteMirror(void* a) {
    return a && coop::hand_item::IsHandAxisActor(a) && a != coop::hand_item::LocalHandActor();
}

// The tick's height before the body, per object (a tick does not nest on one object).
std::unordered_map<void*, float> g_tickHeight;

sg::Verdict OnBucketTickPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host || !c.object) return sg::Verdict::Run;
    if (IsMine(c.object) || !coop::net_pump::IsInAnnouncedWorld(c.object)) return sg::Verdict::Run;
    const BucketOffs o = OffsFor(c.object);
    if (o.ok) g_tickHeight[c.object] = ReadF(c.object, o.height);
    return sg::Verdict::Run;
}

void OnBucketTickPost(const sg::Call& c) {
    if (!c.object) return;
    auto it = g_tickHeight.find(c.object);
    if (it == g_tickHeight.end()) return;
    const float before = it->second;
    g_tickHeight.erase(it);
    const BucketOffs o = OffsFor(c.object);
    if (!o.ok) return;
    const float now = ReadF(c.object, o.height);
    if (std::fabs(now - before) < 0.0001f) return;
    // The copy spilled or filled on its own: the host's water stays, and the look follows it.
    std::memcpy(static_cast<char*>(c.object) + o.height, &before, sizeof before);
    if (o.interp >= 0) std::memcpy(static_cast<char*>(c.object) + o.interp, &before, sizeof before);
    static int sSaid = 0;
    if (sSaid++ < 5)
        UE_LOGI("prop_record_refresh: a client copy of bucket %p kept its water %.1f (its own tick made it %.1f)",
                c.object, before, now);
}

void ArmBucketTick() {
    if (g_bucketTickArmed) return;
    g_bucketTickArmed = sg::WatchName(kRowBucketTick, kBucketTickTag, &OnBucketTickPre, &OnBucketTickPost);
    if (g_bucketTickArmed) UE_LOGI("prop_record_refresh: a client's copy of a bucket keeps the host's water");
}

struct BucketSeen { float height = 0.f; int32_t soap = 0; bool alcohol = false; Clock::time_point published{}; bool have = false; };
std::unordered_map<void*, BucketSeen> g_buckets;

void BucketBeat() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) { g_buckets.clear(); return; }
    static Clock::time_point sNext{};
    const auto now = Clock::now();
    if (now < sNext) return;
    sNext = now + std::chrono::milliseconds(500);
    const bool host = s->role() == coop::net::Role::Host;
    struct Ctx { coop::net::Session* s; bool host; Clock::time_point now; std::vector<void*> live; } ctx{s, host, now, {}};
    // The water bucket and the mop's: exact classes, so each is walked.
    for (const wchar_t* name : {L"prop_bucket_C", L"prop_bucket_mop_C"}) {
        void* cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            static_cast<Ctx*>(c)->live.push_back(obj);
        }, &ctx);
    }
    for (auto it = g_buckets.begin(); it != g_buckets.end();) {
        bool still = false;
        for (void* b : ctx.live) if (b == it->first) { still = true; break; }
        it = still ? std::next(it) : g_buckets.erase(it);
    }
    for (void* b : ctx.live) {
        const BucketOffs o = OffsFor(b);
        if (!o.ok) continue;
        const float h = ReadF(b, o.height);
        int32_t soap = 0;
        std::memcpy(&soap, static_cast<const char*>(b) + o.soap, sizeof soap);
        const bool alc = o.alcOff >= 0 && (static_cast<const uint8_t*>(b)[o.alcOff] & o.alcMask);
        // A landed record writes the height and leaves `empty` as it was, which hides the water until a
        // tick of a simulating copy recomputes it: set from the water here, and repainted.
        if (!host && o.emptyOff >= 0 && o.emptyMask && !IsMine(b)) {
            uint8_t& flags = static_cast<uint8_t*>(b)[o.emptyOff];
            const bool empty = (flags & o.emptyMask) != 0;
            if (empty != (h <= 0.f)) {
                flags = h <= 0.f ? static_cast<uint8_t>(flags | o.emptyMask) : static_cast<uint8_t>(flags & ~o.emptyMask);
                if (o.interp >= 0) std::memcpy(static_cast<char*>(b) + o.interp, &h, sizeof h);
                if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(b), L"upd")) {
                    ue_wrap::ParamFrame f(fn);
                    if (f.valid()) ue_wrap::Call(b, f);
                }
            }
        }
        BucketSeen& seen = g_buckets[b];
        const bool changed = !seen.have || std::fabs(seen.height - h) > 0.05f || seen.soap != soap || seen.alcohol != alc;
        const bool first = !seen.have;
        seen.height = h; seen.soap = soap; seen.alcohol = alc; seen.have = true;
        if (first || !changed) continue;   // a first sighting is the save both peers loaded
        // Who speaks for the water: the host, and a client for the bucket in its own hand. Never a mirror of
        // another player's hand, whose water is a display of that player's and no world prop's.
        if (IsRemoteMirror(b)) continue;
        if (!host && !IsMine(b)) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(b);
        if (key.empty() || key == L"None") continue;
        if (coop::prop_save_data::Publish(s, b, key)) {
            seen.published = now;
            g_lastPublish[b] = now;   // a host echo landing inside the window does not undo the hand's change
            ++g_cPublished;
            UE_LOGI("prop_record_refresh: bucket key='%ls' water %.1f soap %d alcohol %d -- published (%s)",
                    key.c_str(), h, soap, alc ? 1 : 0, host ? "host" : "client hand");
        }
    }
}

// ---- building material: the concrete bucket and the cement bag ----------------------------------------
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/props/bucket_sync.cpp (MIT): the stock a
// prop keeps in its save record travels on that record, from the peer that changed it.
//
// A concrete bucket's portions and its dry flag, and a cement bag's units, are their save records, and
// nothing carried them. A trowel's scoop or a bag's pour into a world bucket changed the copy of the peer
// whose player did it, and nobody else's, so the friend's trowel scooped from a bucket the other player
// saw empty, and a bag stayed full on the host however much a client poured from it. And every peer's
// copy of a concrete bucket ran its own tick, which ONLY spills, spawns, replaces and dries (its cooked
// ReceiveTick has no parent call): each copy dried on its own clock and spilled against the pose it was
// shown, so a client's copy set and spawned spill and replacement props of its own.
//
// So: a client's copy of a concrete bucket does not tick (the host's does, and its stock travels on the
// record); a remote hand mirror's never ticks, on any peer; the local hand's runs as the game's. The host
// publishes a world copy's record when its stock changes. A client publishes it (an intent the host takes
// and re-publishes) for a prop its own player's verb just changed: takeConcrete and updStage/updDry on the
// bucket, transferUnit on the bag, each read in its post, outside our own record applies.
UE_FACT_FN(kRowConcreteTick,     "prop_record_refresh", L"prop_concreteBucket_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowConcreteStage,    "prop_record_refresh", L"prop_concreteBucket_C", L"updStage", Declared);
UE_FACT_FN(kRowConcreteDry,      "prop_record_refresh", L"prop_concreteBucket_C", L"updDry", Declared);
UE_FACT_FN(kRowConcreteTake,     "prop_record_refresh", L"prop_concreteBucket_C", L"takeConcrete", Declared);
UE_FACT_FN(kRowBagTransfer,      "prop_record_refresh", L"prop_cementBag_C", L"transferUnit", Declared);
constexpr int kMaterialTag = 0x4D41544C;  // 'MATL'
bool g_concreteTickArmed = false, g_concreteStageArmed = false, g_concreteDryArmed = false;
bool g_concreteTakeArmed = false, g_bagTransferArmed = false;

// CLIENT: props this player's own verb changed a moment ago, and until when their change is this peer's
// to publish. Keyed by address with the slot index beside it, so a recycled address is not taken for it.
struct Touch { int32_t idx; Clock::time_point until; };
std::unordered_map<void*, Touch> g_touched;
constexpr auto kTouchWindow = std::chrono::milliseconds(1500);

void MarkTouched(void* a) {
    if (!a || !R::IsLive(a) || coop::hand_item::IsHandAxisActor(a)) return;  // a hand item is no world prop
    g_touched[a] = Touch{R::InternalIndexOf(a), Clock::now() + kTouchWindow};
}

bool Touched(void* a, Clock::time_point now) {
    const auto it = g_touched.find(a);
    return it != g_touched.end() && now < it->second.until && R::IsLiveByIndex(a, it->second.idx);
}

sg::Verdict OnConcreteTickPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !c.object || c.fromOurCode) return sg::Verdict::Run;
    // A display mirror of another player's hand: spilling, drying or replacing it is that player's game's.
    if (IsRemoteMirror(c.object)) return sg::Verdict::Cancel;
    if (s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    if (c.object == coop::hand_item::LocalHandActor()) return sg::Verdict::Run;  // the hand item is this player's
    if (!coop::net_pump::IsInAnnouncedWorld(c.object) || !coop::join_membership_sweep::HasLoadTailQuiesced())
        return sg::Verdict::Run;
    static int sSaid = 0;
    if (sSaid++ < 3) UE_LOGI("prop_record_refresh: a client copy of concrete bucket %p does not tick (the host's does)", c.object);
    return sg::Verdict::Cancel;
}

// CLIENT: a player's verb changed this prop's stock, so its record is this peer's to send.
void OnMaterialVerbPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || c.fromOurCode || !c.object) return;
    MarkTouched(c.object);
}

// CLIENT: a bag's transferUnit moves units between its `from` and `to`, either of which may be a world
// prop that is not the one the call ran on.
void OnBagTransferPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || c.fromOurCode || !c.object) return;
    MarkTouched(c.object);
    if (!c.function || !c.locals) return;
    const int32_t size = R::FunctionFrameSize(c.function);
    for (const wchar_t* name : {L"from", L"to"}) {
        const int32_t off = R::FindParamOffset(c.function, name);
        if (off < 0 || off + static_cast<int32_t>(sizeof(void*)) > size) continue;
        void* end = nullptr;
        std::memcpy(&end, c.locals + off, sizeof end);
        if (end && R::IsLive(end)) MarkTouched(end);
    }
}

void ArmMaterialWatches() {
    if (!g_concreteTickArmed)
        g_concreteTickArmed = sg::WatchName(kRowConcreteTick, kMaterialTag, &OnConcreteTickPre, nullptr);
    if (!g_concreteStageArmed)
        g_concreteStageArmed = sg::WatchName(kRowConcreteStage, kMaterialTag, nullptr, &OnMaterialVerbPost);
    if (!g_concreteDryArmed)
        g_concreteDryArmed = sg::WatchName(kRowConcreteDry, kMaterialTag, nullptr, &OnMaterialVerbPost);
    if (!g_concreteTakeArmed)
        g_concreteTakeArmed = sg::WatchName(kRowConcreteTake, kMaterialTag, nullptr, &OnMaterialVerbPost);
    if (!g_bagTransferArmed) {
        g_bagTransferArmed = sg::WatchName(kRowBagTransfer, kMaterialTag, nullptr, &OnBagTransferPost);
        if (g_bagTransferArmed)
            UE_LOGI("prop_record_refresh: concrete buckets and cement bags carry their stock on their record");
    }
}

enum class MaterialKind : uint8_t { Concrete, Bag };
struct MaterialSeen { int32_t units = 0; bool dry = false; bool have = false; };
std::unordered_map<void*, MaterialSeen> g_materials;

// The stock as the record carries it: the portions (`units`) and, for the bucket, `dry`.
bool ReadMaterial(void* a, MaterialKind kind, int32_t& units, bool& dry) {
    void* cls = R::ClassOf(a);
    if (!cls) return false;
    const int32_t uo = R::FindPropertyOffset(cls, L"units");
    if (uo < 0) return false;
    std::memcpy(&units, static_cast<const char*>(a) + uo, sizeof units);
    dry = false;
    if (kind == MaterialKind::Concrete) {
        int32_t dOff = -1;
        uint8_t dMask = 0;
        if (!R::FindBoolProperty(cls, L"dry", dOff, dMask) || dOff < 0 || !dMask) return false;
        dry = (static_cast<const uint8_t*>(a)[dOff] & dMask) != 0;
    }
    return true;
}

void MaterialBeat() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) { g_materials.clear(); g_touched.clear(); return; }
    static Clock::time_point sNext{};
    const auto now = Clock::now();
    if (now < sNext) return;
    sNext = now + std::chrono::milliseconds(250);
    const bool host = s->role() == coop::net::Role::Host;
    for (auto it = g_touched.begin(); it != g_touched.end();)
        it = now >= it->second.until ? g_touched.erase(it) : std::next(it);
    struct Found { void* obj; MaterialKind kind; };
    std::vector<Found> live;
    struct Ctx { std::vector<Found>* out; MaterialKind kind; };
    const struct { const wchar_t* name; MaterialKind kind; } kClasses[] = {
        {L"prop_concreteBucket_C", MaterialKind::Concrete},
        {L"prop_cementBag_C",      MaterialKind::Bag},
    };
    for (const auto& k : kClasses) {
        void* cls = ue_wrap::object_index::ClassByName(k.name);
        if (!cls) continue;
        Ctx ctx{&live, k.kind};
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            auto* x = static_cast<Ctx*>(c);
            x->out->push_back(Found{obj, x->kind});
        }, &ctx);
    }
    for (auto it = g_materials.begin(); it != g_materials.end();) {
        bool still = false;
        for (const Found& f : live) if (f.obj == it->first) { still = true; break; }
        it = still ? std::next(it) : g_materials.erase(it);
    }
    for (const Found& f : live) {
        // A hand item is the player's, not the world's: its stock rides the profile and its drop's record.
        if (coop::hand_item::IsHandAxisActor(f.obj)) continue;
        int32_t units = 0;
        bool dry = false;
        if (!ReadMaterial(f.obj, f.kind, units, dry)) continue;
        MaterialSeen& seen = g_materials[f.obj];
        const bool first = !seen.have;
        const bool changed = !first && (seen.units != units || seen.dry != dry);
        seen.units = units; seen.dry = dry; seen.have = true;
        if (first || !changed) continue;   // a first sighting is the save both peers loaded
        // The host speaks for its world copies (a bag a client carries is the client's while it carries it;
        // a bucket's spill and drying are the host's tick even then); a client for what its verb just changed.
        const bool speaks = host ? (f.kind == MaterialKind::Concrete || !coop::remote_prop::IsActorUnderAnyDrive(f.obj))
                                 : Touched(f.obj, now);
        if (!speaks) {
            // A record landed on this copy: its look follows the stock through updStage (which runs updDry).
            if (!host && f.kind == MaterialKind::Concrete) {
                if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(f.obj), L"updStage")) {
                    ue_wrap::ParamFrame fr(fn);
                    if (fr.valid()) ue_wrap::Call(f.obj, fr);
                }
            }
            continue;
        }
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(f.obj);
        if (key.empty() || key == L"None") continue;
        if (coop::prop_save_data::Publish(s, f.obj, key)) {
            g_lastPublish[f.obj] = now;   // a host echo landing inside the window does not undo the change
            ++g_cPublished;
            UE_LOGI("prop_record_refresh: %ls key='%ls' units %d dry %d -- published (%s)",
                    f.kind == MaterialKind::Concrete ? L"concrete bucket" : L"cement bag", key.c_str(), units,
                    dry ? 1 : 0, host ? "host" : "client verb");
        }
    }
}
}  // namespace

void ClearMaterialState() {
    g_materials.clear();
    g_touched.clear();
}

}  // namespace coop::props::prop_record_refresh
