// coop/interactables/swinger_pose_sync.cpp -- see coop/interactables/swinger_pose_sync.h.

#include "coop/interactables/swinger_pose_sync.h"

#include "coop/interactables/interactable_sync.h"  // ContainerKey, ResolveContainer
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/local_streams.h"    // LastHeldActor
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_physics.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::swinger_pose_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr uint64_t kPeriodMs   = 66;    // 15 Hz while a lid is held or settling
constexpr uint64_t kSettleMs   = 2500;  // after the release, while it swings to rest
constexpr float    kMoveCm2    = 0.5f * 0.5f;
constexpr float    kTurnDeg    = 0.5f;

void*    g_lid = nullptr;       // the lid this peer swings, or swung last within the settle
int32_t  g_lidIdx = -1;
std::wstring g_lidKey;
uint64_t g_settleUntilMs = 0;
uint64_t g_nextMs = 0;
ue_wrap::FVector  g_lastLoc{};
ue_wrap::FRotator g_lastRot{};
bool g_haveLast = false;
uint64_t g_sent = 0, g_applied = 0, g_unresolved = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

float AngDiff(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 360.f);
    return d > 180.f ? 360.f - d : d;
}

void Forget() {
    g_lid = nullptr;
    g_lidIdx = -1;
    g_lidKey.clear();
    g_settleUntilMs = 0;
    g_haveLast = false;
}

void SendPose(coop::net::Session& s, void* lid, bool force) {
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(lid, loc) || !E::TryGetActorRotation(lid, rot)) return;
    if (!force && g_haveLast) {
        const float dx = loc.X - g_lastLoc.X, dy = loc.Y - g_lastLoc.Y, dz = loc.Z - g_lastLoc.Z;
        if (dx * dx + dy * dy + dz * dz <= kMoveCm2 && AngDiff(rot.Pitch, g_lastRot.Pitch) < kTurnDeg &&
            AngDiff(rot.Yaw, g_lastRot.Yaw) < kTurnDeg && AngDiff(rot.Roll, g_lastRot.Roll) < kTurnDeg)
            return;  // at rest: nothing new to say
    }
    coop::net::SwingerPosePayload p{};
    coop::net::WireKeyFromString(g_lidKey, p.key);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    if (!s.SendReliable(coop::net::ReliableKind::SwingerPose, &p, sizeof(p))) return;
    g_lastLoc = loc;
    g_lastRot = rot;
    g_haveLast = true;
    if (++g_sent == 1) UE_LOGI("[SWINGER] the first lid pose went out: key='%ls'", g_lidKey.c_str());
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!session.running() || !session.connected()) { Forget(); return; }
    const uint64_t now = NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kPeriodMs;

    void* held = coop::local_streams::LastHeldActor();
    if (held && ue_wrap::swinger::IsSwinger(held)) {
        if (held != g_lid) {
            g_lid = held;
            g_lidIdx = R::InternalIndexOf(held);
            g_lidKey = coop::interactable_sync::ContainerKey(held);
            g_haveLast = false;
            if (g_lidKey.empty())
                UE_LOGW("[SWINGER] a held lid %p has no container-lane key -- its swing stays here", held);
        }
        g_settleUntilMs = now + kSettleMs;  // held: the settle starts at the release
    }
    if (!g_lid) return;
    if (!R::IsLiveByIndex(g_lid, g_lidIdx)) { Forget(); return; }
    if (g_lidKey.empty()) { if (now >= g_settleUntilMs) Forget(); return; }
    const bool settling = now >= g_settleUntilMs;
    SendPose(session, g_lid, false);
    if (settling) {
        SendPose(session, g_lid, true);  // the rest pose, whatever moved last
        Forget();
    }
}

void OnSwingerPose(const coop::net::SwingerPosePayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* lid = coop::interactable_sync::ResolveContainer(key);
    if (!lid) {
        if (++g_unresolved <= 3) UE_LOGW("[SWINGER] no lid for key='%ls' here -- pose dropped", key.c_str());
        return;
    }
    // This peer's own player swings it now: its hand owns the lid.
    void* local = coop::players::Registry::Get().Local();
    if (local && E::IsMainPlayerGrabbing(local, lid)) return;
    if (lid == g_lid) Forget();  // the other peer took it over
    E::SetActorLocation(lid, ue_wrap::FVector{p.locX, p.locY, p.locZ});
    E::SetActorRotation(lid, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
    if (void* mesh = ue_wrap::prop::GetStaticMesh(lid)) {
        E::SetComponentLinearVelocity(mesh, 0.f, 0.f, 0.f);
        E::SetComponentAngularVelocity(mesh, 0.f, 0.f, 0.f);
    }
    if (++g_applied == 1) UE_LOGI("[SWINGER] the first peer lid pose applied here: key='%ls'", key.c_str());
}

void OnDisconnect() {
    if (g_sent || g_applied || g_unresolved)
        UE_LOGI("[SWINGER] session end -- poses sent=%llu applied=%llu unresolved=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_unresolved));
    g_sent = g_applied = g_unresolved = 0;
    Forget();
    g_nextMs = 0;
}

}  // namespace coop::swinger_pose_sync
