// harness/pump.cpp -- see harness/pump.h.

#include "harness/pump.h"
#include "ue_wrap/core/game_thread.h"  // TasksRun: the stall reporter's liveness count
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/walk_timer.h"   // the walk the game thread is inside

#include "coop/comms/chat_feed.h"
#include "coop/props/prop_key_index.h"
#include "coop/dev/class_lifetime_probe.h"
#include "coop/dev/init_seam_probe.h"
#include "coop/dev/key_parity_probe.h"
#include "coop/dev/mannequin_drill.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/dev/function_lookup_parity.h"
#include "coop/dev/join_abort_drill.h"
#include "coop/dev/world_singleton_parity.h"
#include "coop/player/death_revive.h"
#include "coop/player/nameplate.h"
#include "coop/player/revive_protection.h"
#include "coop/player/run_end_travel.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/shutdown.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace harness::pump {
namespace {

namespace GT = ue_wrap::game_thread;

std::atomic<bool> g_queued{false};
uint64_t g_lastDrain = 0;   // game thread

// The session this pump ticks, handed over at boot rather than reached for: the pump is a leaf,
// and a module that calls up into the lifecycle driver that calls it is a cycle nothing needs.
std::atomic<coop::net::Session*> g_session{nullptr};

// The shutdown hooks, run every tick regardless of possession and idempotent: the HWND subclass
// and the window title must work before the local player exists (a close on the splash). The
// run-ending seam is registered here unconditionally rather than lazily from the pump: registered
// only while a session runs, the single-player guarantee would rest on the watch's absence rather
// than on the seam's own session test, and a negative-control run would grade a watch that was
// never there.
void TickShutdownHooks() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    coop::shutdown::Install(s);
    coop::shutdown::UpdateWindowTitle();
    coop::death_revive::Install(s);
    coop::revive_protection::Install();
    coop::player::run_end_travel::Install(s);
}

}  // namespace

bool BeginComposite() {
    bool idle = false;
    return g_queued.compare_exchange_strong(idle, true, std::memory_order_acq_rel);
}

void EndComposite() { g_queued.store(false, std::memory_order_release); }

bool CompositeDrainIsNew() {
    const uint64_t drain = GT::DrainSerial();
    if (drain == g_lastDrain) return false;
    g_lastDrain = drain;
    return true;
}

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void PostMenuTick() {
    PostComposite([] {
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        if (!s) return;
        coop::net_pump::Tick(*s);
        TickFrameTail();
    });
}

void TickFrameTail() {
    // Self-clearing and cheap when idle: an empty nameplate snapshot hides the HUD at the menu.
    coop::nameplate::Update();
    coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
    coop::chat_feed::Tick();
    // The prop key index's watches' names, driven to resolve (the install itself is harness::Start's first
    // game-thread task); a latched read after that.
    coop::prop_key_index::Install();
    // [dev] The lookup parity probes, once per world, the menu's included: the singleton probe, since a
    // lookup that outlived the world it came from would show there, and the function-list probe, since
    // a class the menu loads is judged too; each a latched read when off.
    coop::dev::world_singleton_parity::Tick();
    coop::dev::function_lookup_parity::Tick();
    // [dev] Whether a class and its functions outlive the world that loaded them; a latched read when off.
    coop::dev::class_lifetime_probe::Tick();
    // [dev] Every keyed Init body the script gate sees; a latched read when off.
    coop::dev::init_seam_probe::Tick();
    // [dev] A key index fed only at the key writes, and the one we keep, against the world; a latched read
    // when off.
    coop::dev::key_parity_probe::Tick();
    // [dev] The walking mannequin's spawn sequence, in session or not; a single atomic read when idle.
    coop::dev::mannequin_drill::TickFrame();
    // [dev] The join-abort proof presses Cancel mid-join here (this tail runs in the transfer wait and
    // the play loop alike, so it catches either abort window) and notes the rejoin's world-ready; a
    // single enum read when off.
    coop::dev::join_abort_drill::OnFrameTail();
    TickShutdownHooks();
}

namespace {
// The game-thread stall reporter: the posted-task counter stands still while a task never returns. Past
// kStallMs it names the walk the game thread is inside (or left last), once and then every 30 s, so a
// frozen peer's log says where it froze. A level load legitimately holds the thread for seconds.
constexpr int64_t kStallMs = 8000;
unsigned long long g_stallSeenTasks = 0;
int64_t g_stallSinceMs = 0;
int64_t g_stallNextSayMs = 0;

void TickStallReport() {
    const unsigned long long tasks = ue_wrap::game_thread::TasksRun();
    const int64_t now = ue_wrap::WalkNowMs();
    if (tasks != g_stallSeenTasks || g_stallSinceMs == 0) {
        g_stallSeenTasks = tasks;
        g_stallSinceMs = now;
        g_stallNextSayMs = now + kStallMs;
        return;
    }
    if (now < g_stallNextSayMs) return;
    g_stallNextSayMs = now + 30000;
    const char* cur = ue_wrap::g_walkCurrent.load(std::memory_order_acquire);
    const char* last = ue_wrap::g_walkLast.load(std::memory_order_relaxed);
    const int64_t inCur = now - ue_wrap::g_walkSinceMs.load(std::memory_order_relaxed);
    UE_LOGW("[GT-STALL] no game-thread task has finished for %lld ms -- inside walk '%s' for %lld ms; "
            "the last walk finished: '%s'",
            static_cast<long long>(now - g_stallSinceMs), cur ? cur : "(none)",
            static_cast<long long>(cur ? inCur : 0), last ? last : "(none)");
    // Where the thread stands, twice per stall at most: at the first report and the next one 30 s on
    // (a loop moves between the two, a wait does not).
    static unsigned long long sStackedFor = ~0ull;
    static int sStacks = 0;
    if (sStackedFor != g_stallSeenTasks) { sStackedFor = g_stallSeenTasks; sStacks = 0; }
    if (sStacks++ < 2) ue_wrap::game_thread::LogStacks("[GT-STALL]");
}
}  // namespace

void TickWatchdogs() {
    TickStallReport();
    // The join watchdog lives HERE, not on the render thread. Every phase budget in join_progress
    // used to be reachable only through ui::loading_screen::Render, i.e. through the ImGui
    // overlay's Present hook -- and harness.cpp treats a failed overlay install as non-fatal and
    // boots on, because the multiplayer entry point is a native UMG button, not ours. In a session
    // like that every join budget was dead. This runs on the thread that drives the join.
    coop::join_progress::MaybeTimeout();
    coop::death_revive::Watchdog();
}

}  // namespace harness::pump
