// coop/dev/director/background_walk.cpp -- a walk-to run on a worker thread; see director.h.

#include "coop/dev/director/director.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_props.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>

namespace coop::director {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace DP = coop::dev::drill_props;
using ue_wrap::FVector;

struct Start {
    std::shared_ptr<BackgroundWalk> walk;
    int deadlineS;
};

// ---- the lab's fallback teleport (director_glide) -----------------------------------------------------------
// A plain walk-to that ended without arriving -- a NavMesh that does not reach the point, a route that sticks --
// ends in one teleport to a stand beside the point instead of the walk's failure. Never for a carrying walk: what a
// carry drags is what its drill measures. The stand has a floor under it (the shared floor trace), the player stands
// at the height above its floor it stood at before, and the stand's eye sees the point: a stand computed into geometry
// is what the game's anti-noclip takes a player out of the world for.

constexpr float kFloorDownCm  = 600.f;   // how far under a stand its floor is looked for
constexpr float kMinColumnCm  = 20.f;    // a column shorter than this began inside geometry
constexpr float kEyeAboveCm   = 40.f;    // the stand's eye, above its origin
constexpr float kPi           = 3.14159265f;

float Flat(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// How high the player's origin stands above the floor under it, or 90 when that floor does not read.
float StandHeight(void* player, const FVector& me) {
    float floorZ = 0.f;
    if (DP::FloorAt(player, me.X, me.Y, me.Z, kFloorDownCm, 0.f, &floorZ) && me.Z - floorZ > 20.f &&
        me.Z - floorZ < 200.f)
        return me.Z - floorZ;
    return 90.f;
}

// A stand `radius` from `to`, tried round it from the side the player is on, with a clear line from the stand's eye to
// the point (a hit within 60 cm of it is the target itself). True once a teleport left the player within `reachCm` of
// `to`, flat. Game thread.
bool TeleportBeside(void* player, const FVector& to, float reachCm, std::vector<FVector>* route) {
    FVector me{};
    if (!E::TryGetActorLocation(player, me)) return false;
    const float height = StandHeight(player, me);
    const float radius = std::clamp(reachCm * 0.5f, 40.f, 150.f);
    const float base = std::atan2(me.Y - to.Y, me.X - to.X);
    for (int i = 0; i < 12; ++i) {
        const int k = (i + 1) / 2 * ((i % 2) ? 1 : -1);   // 0, +1, -1, +2, -2, ... steps of 30 degrees
        const float a = base + static_cast<float>(k) * kPi / 6.f;
        const float x = to.X + radius * std::cos(a), y = to.Y + radius * std::sin(a);
        float floorZ = 0.f;
        if (!DP::FloorAt(player, x, y, to.Z + 150.f, kFloorDownCm, kMinColumnCm, &floorZ)) continue;
        const FVector stand{x, y, floorZ + height};
        ue_wrap::trace::Hit hit;
        if (!ue_wrap::trace::LineHitStatDyn(player, FVector{x, y, stand.Z + kEyeAboveCm}, to, &hit)) continue;
        if (hit.blocked && std::hypot(hit.point.X - to.X, hit.point.Y - to.Y) + std::fabs(hit.point.Z - to.Z) > 60.f)
            continue;
        const ue_wrap::FRotator face{0.f, std::atan2(to.Y - y, to.X - x) * 180.f / kPi, 0.f};
        if (!E::TeleportTo(player, stand, face)) continue;
        FVector now{};
        if (!E::TryGetActorLocation(player, now) || Flat(now, to) > reachCm) continue;
        UE_LOGW("director/glide: the walk did not arrive -- TELEPORT to (%.0f,%.0f,%.0f), %.0f cm from the point "
                "(%.0f,%.0f,%.0f), from (%.0f,%.0f,%.0f)", now.X, now.Y, now.Z, Flat(now, to), to.X, to.Y, to.Z, me.X,
                me.Y, me.Z);
        if (route) route->push_back(now);
        return true;
    }
    UE_LOGW("director/glide: no stand beside (%.0f,%.0f,%.0f) took the teleport", to.X, to.Y, to.Z);
    return false;
}

DWORD WINAPI WalkThread(LPVOID arg) {
    std::unique_ptr<Start> start(static_cast<Start*>(arg));
    BackgroundWalk& w = *start->walk;
    ControlManager mgr;
    if (w.carry) AddCarryToProcesses(mgr, w.goal);
    else AddWalkToProcesses(mgr, w.goal);
    mgr.Run(w.goal, start->deadlineS);
    // The lab's fallback: a plain walk that did not arrive, and was neither cancelled nor ended with its session.
    if (!w.goal.reached && !w.carry && GlideOn() && !w.cancelled.load(std::memory_order_acquire) &&
        w.goal.epoch.value_or(WalkEpoch()) == WalkEpoch()) {
        bool ok = false;
        const int r = GT::RunAndWait([&w, &ok](std::atomic<int>& d) {
            void* p = coop::players::Registry::Get().Local();
            ok = p && TeleportBeside(p, w.goal.targetPos, w.goal.reachCm, &w.goal.route);
            d.store(1);
        });
        if (r != GT::kTaskFaulted && ok) {
            w.goal.reached = true;
            w.goal.failed = false;
        }
    }
    w.state.store(w.goal.reached ? 1 : 2);
    return 0;
}

}  // namespace

bool GlideOn() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::director_glide);
    return s;
}

std::shared_ptr<BackgroundWalk> StartBackgroundWalk(const ue_wrap::FVector& to, float reachCm, int deadlineS,
                                                    bool carry) {
    auto walk = std::make_shared<BackgroundWalk>();
    walk->goal.targetPos = to;
    walk->goal.reachCm = reachCm;
    walk->carry = carry;
    walk->goal.epoch = WalkEpoch();
    walk->goal.cancel = &walk->cancelled;
    auto* start = new Start{walk, deadlineS};
    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, start, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        delete start;
        walk->state.store(2);
    }
    return walk;
}

void CancelBackgroundWalk(BackgroundWalk& walk) {
    walk.cancelled.store(true, std::memory_order_release);
}

}  // namespace coop::director
