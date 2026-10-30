// coop/world/dig_hole_sync.cpp -- see coop/world/dig_hole_sync.h.

#include "coop/world/dig_hole_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::dig_hole_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowHoleBeginPlay, "dig_hole_sync", L"dirtHole_C", L"ReceiveBeginPlay", Declared);
constexpr int kTag = 0x44494748;  // 'DIGH'

// A dig takes a second of animation; past this many a second something else is spawning holes, and the
// rest stay local.
constexpr int kMaxPerSecond = 4;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;
bool g_refused = false;

void* g_holeCls = nullptr;  // dirtHole_C
int32_t g_matOff = -2;      // dirtHole_C.matType

// Set while this peer digs a peer's hole, so its own BeginPlay is not sent back.
bool g_mirrorSpawning = false;

uint64_t g_windowMs = 0;
int g_inWindow = 0;
uint64_t g_sent = 0, g_spawned = 0, g_dropped = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ResolveClass() {
    if (!g_holeCls) g_holeCls = OI::ClassByName(L"dirtHole_C");
    if (g_holeCls && g_matOff == -2) g_matOff = R::FindPropertyOffset(g_holeCls, L"matType");
}

bool IsHole(void* obj) {
    ResolveClass();
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    return cls && g_holeCls && R::IsDescendantOfAny(cls, &g_holeCls, 1);
}

void OnBeginPlayPost(const sg::Call& call) {
    if (g_mirrorSpawning) return;  // a peer's hole, dug here
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    if (!call.object || !IsHole(call.object)) return;
    // A client's world load lays the save's holes: not a dig.
    if (s->role() == coop::net::Role::Client && !coop::net_pump::IsInAnnouncedWorld(call.object)) return;
    const uint64_t now = NowMs();
    if (now - g_windowMs >= 1000) { g_windowMs = now; g_inWindow = 0; }
    if (++g_inWindow > kMaxPerSecond) { ++g_dropped; return; }

    coop::net::DirtHolePayload p{};
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(call.object, loc) || !E::TryGetActorRotation(call.object, rot)) return;
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.matType = g_matOff >= 0
        ? *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(call.object) + g_matOff) : 0;
    if (p.matType < 0 || p.matType > 16) p.matType = 0;
    if (!s->SendReliable(coop::net::ReliableKind::DirtHole, &p, sizeof(p))) return;
    if (++g_sent <= 5)
        UE_LOGI("[DIG] a hole dug here went out: pos=(%.0f,%.0f,%.0f) matType=%d", loc.X, loc.Y, loc.Z, p.matType);
}

struct SpawnCtx { const coop::net::DirtHolePayload* p; };

// Before BeginPlay: the ground's material and the facing, as the digger's graph sets them on its own hole.
void BeforeFinish(void* ctx, void* actor) {
    const auto* p = static_cast<SpawnCtx*>(ctx)->p;
    if (g_matOff >= 0) *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(actor) + g_matOff) = p->matType;
    E::SetActorRotation(actor, ue_wrap::FRotator{p->rotPitch, p->rotYaw, p->rotRoll});
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered || g_refused) return;
    if (sg::WatchName(kRowHoleBeginPlay, kTag, nullptr, &OnBeginPlayPost)) {
        g_registered = true;
        return;
    }
    g_refused = true;
    UE_LOGE("[DIG] the script-body gate refused the hole BeginPlay watch -- a dig stays on the digger's screen");
}

void Tick() {
    if (!g_registered && !g_refused) Install(g_session.load(std::memory_order_acquire));
}

void OnDirtHole(const coop::net::DirtHolePayload& p) {
    ResolveClass();
    if (!g_holeCls) {
        // Nobody has dug on this peer yet this run, so the class may not be loaded: load it by its path.
        g_holeCls = R::FindClass(L"dirtHole_C");
        if (g_holeCls) g_matOff = R::FindPropertyOffset(g_holeCls, L"matType");
    }
    if (!g_holeCls) {
        ++g_dropped;
        UE_LOGW("[DIG] a peer's hole at (%.0f,%.0f,%.0f) is not dug here: dirtHole_C is not loaded", p.locX, p.locY,
                p.locZ);
        return;
    }
    SpawnCtx ctx{&p};
    g_mirrorSpawning = true;
    void* a = E::SpawnActor(g_holeCls, ue_wrap::FVector{p.locX, p.locY, p.locZ}, false, &BeforeFinish, &ctx);
    g_mirrorSpawning = false;
    if (!a) { ++g_dropped; return; }
    E::SetActorLocation(a, ue_wrap::FVector{p.locX, p.locY, p.locZ});
    E::SetActorRotation(a, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
    if (++g_spawned <= 5)
        UE_LOGI("[DIG] a peer's hole dug here at (%.0f,%.0f,%.0f) matType=%d", p.locX, p.locY, p.locZ, p.matType);
}

void OnDisconnect() {
    if (g_sent || g_spawned || g_dropped)
        UE_LOGI("[DIG] session end -- holes sent=%llu dug for peers=%llu dropped=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_spawned),
                static_cast<unsigned long long>(g_dropped));
    g_sent = g_spawned = g_dropped = 0;
}

}  // namespace coop::dig_hole_sync
