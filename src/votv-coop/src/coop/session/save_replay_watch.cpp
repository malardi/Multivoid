// coop/session/save_replay_watch.cpp -- see coop/session/save_replay_watch.h.
// The by-name watch on the gamemode's loadObjects (PRE and POST), its installer, the replay record,
// and the in-body mark the destroy seam reads.

#include "coop/session/save_replay_watch.h"

#include "coop/config/config.h"  // ResolveFlag, for the red arms
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/text/utf8_codec.h"  // ToUtf8: the watch row's names in the failure text

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace coop::save_replay_watch {

namespace SG = ue_wrap::script_gate;
namespace WI = ue_wrap::world_identity;

namespace {

UE_FACT_FN(kRowLoadObjects, "join_world_ready", L"mainGamemode_C", L"loadObjects", Declared);
UE_FACT_FN(kRowLoadObjectsRed, "dev.replay_watch_break", L"mainGamemode_C", L"loadObjectsJwRed", Declared);
constexpr int kTagReplay = 0x4A575250;  // 'JWRP'

// The save replay's record: the gamemode that ran loadObjects, and when its body returned.
ue_wrap::CachedObjRef g_replayGm;
std::chrono::steady_clock::time_point g_replayPostAt{};

// The replay watch's installer (game thread).
bool g_watchRegistered = false;
bool g_watchSettled = false;
bool g_watchDead = false;
uint64_t g_nextWatchMs = 0;
const ue_wrap::facts::Function* g_watchRow = nullptr;
bool g_postDrop = false;
bool g_gateBreak = false;
std::atomic<bool> g_watchRefused{false};

// The replay body in progress: the PRE raises it, the POST lowers it. Game thread; the stamp bounds a
// body whose POST never came (a mark stuck up would mute every destroy this client authors).
int g_bodyDepth = 0;
std::chrono::steady_clock::time_point g_bodyEnteredAt{};
constexpr long long kBodyMarkBoundMs = 180000;  // a 19 MB save's replay held one frame for 28 s in the lab

// The save replay's PRE: the gamemode's loadObjects body starts. Its pre-delete destroys every save
// object this world holds and respawns them from the save; none of that is the player's.
SG::Verdict OnLoadObjectsPre(const SG::Call& c) {
    if (g_bodyDepth++ == 0) {
        g_bodyEnteredAt = std::chrono::steady_clock::now();
        UE_LOGI("save_replay_watch: save replay entered (gamemode %p) -- this client's destroys stay local "
                "until it returns", c.object);
    }
    return SG::Verdict::Run;
}

// The save replay's POST: the gamemode's loadObjects body returned. Records which gamemode and when;
// the consume reads it on a later tick.
void OnLoadObjectsPost(const SG::Call& c) {
    if (g_bodyDepth > 0) --g_bodyDepth;
    if (g_postDrop) return;
    const bool fresh = !g_replayGm.Is(c.object);
    g_replayGm.Set(c.object);
    g_replayPostAt = std::chrono::steady_clock::now();
    if (fresh) UE_LOGI("save_replay_watch: save replay returned (gamemode %p)", c.object);
}

}  // namespace

void Drive(coop::net::Session& session, bool requestPending) {
    if (session.role() == coop::net::Role::Host) return;
    if (g_watchSettled && !requestPending) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextWatchMs) return;
    g_nextWatchMs = now + 1000;
    if (!g_watchRow) {
        g_watchRow = coop::config::ResolveFlag(::coop::config_registry::rows::replay_watch_break)
                         ? &kRowLoadObjectsRed : &kRowLoadObjects;
        g_postDrop = coop::config::ResolveFlag(::coop::config_registry::rows::replay_post_drop);
        g_gateBreak = coop::config::ResolveFlag(::coop::config_registry::rows::replay_gate_break);
    }
    if (!g_watchRegistered && !g_watchRefused.load(std::memory_order_relaxed)) {
        g_watchRegistered = SG::WatchName(*g_watchRow, kTagReplay, &OnLoadObjectsPre, &OnLoadObjectsPost);
        if (!g_watchRegistered) g_watchRefused.store(true, std::memory_order_relaxed);  // the consume owns the ERROR
    }
    if (!g_watchRegistered) return;
    SG::ResolvePendingNames();
    if (SG::NameWatchLive(*g_watchRow, kTagReplay)) {
        if (!g_watchSettled) UE_LOGI("save_replay_watch: the save-replay watch is live");
        g_watchSettled = true;
        g_watchDead = false;
    } else if (SG::NameWatchSettled(*g_watchRow, kTagReplay)) {
        g_watchSettled = true;
        g_watchDead = true;
    }
}

// Did the replay run in the world the player is in now (a replay of a world already left does not
// count). The world tested is returned through `world`, read once, so the caller's baseline is the
// world this answer judged.
bool ReturnedInThisWorld(void** world) {
    void* gm = g_replayGm.Get();
    void* cw = WI::CurrentWorld();
    *world = cw;
    return gm && cw && WI::WorldOf(gm) == cw;
}

bool InReplayBody() {
    if (g_bodyDepth <= 0) return false;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              g_bodyEnteredAt).count() < kBodyMarkBoundMs)
        return true;
    UE_LOGW("save_replay_watch: the save replay's body mark is %lld s old with no return -- dropped",
            kBodyMarkBoundMs / 1000);
    g_bodyDepth = 0;
    return false;
}

bool Refused() { return g_watchRefused.load(std::memory_order_relaxed); }

bool Dead() { return g_watchDead; }

bool GateBreak() { return g_gateBreak; }

bool HasRecord() { return g_replayPostAt.time_since_epoch().count() != 0; }

long long MsFromPostTo(std::chrono::steady_clock::time_point t) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      t - g_replayPostAt).count());
}

std::string RowText() {
    return coop::text::ToUtf8(std::wstring(g_watchRow->Class())) + "::" +
           coop::text::ToUtf8(std::wstring(g_watchRow->Name()));
}

void Reset() {
    g_replayGm.Reset();
    g_replayPostAt = {};
    g_bodyDepth = 0;
    // The watch registration and a refusal last for the process; its liveness is read again.
    g_watchSettled = false;
    g_watchDead = false;
}

}  // namespace coop::save_replay_watch
