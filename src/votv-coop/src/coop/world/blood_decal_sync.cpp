// coop/world/blood_decal_sync.cpp -- see coop/world/blood_decal_sync.h.

#include "coop/world/blood_decal_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::blood_decal_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowGrimeBeginPlay, "blood_decal_sync", L"grime_C", L"ReceiveBeginPlay", Declared);
constexpr int kTag = 0x424C4F44;  // 'BLOD'

// A bleeding player lays many splats a second; past this many a second the rest stay local, so a
// flood never crowds the reliable channel.
constexpr int kMaxPerSecond = 20;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;
bool g_refused = false;

void* g_bloodCls = nullptr;  // grime_blood2_C
void* g_dripCls  = nullptr;  // grime_blood2_drip_C
int32_t g_typeOff = -2;      // grime_C.Type
int32_t g_randOff = -2;      // grime_C.randomOrientation
uint8_t g_randMask = 0;

// Set while this peer spawns a peer's decal, so its own BeginPlay is not sent back.
bool g_mirrorSpawning = false;

uint64_t g_windowMs = 0;
int g_inWindow = 0;
uint64_t g_sent = 0, g_spawned = 0, g_dropped = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ResolveClasses() {
    if (!g_bloodCls) g_bloodCls = OI::ClassByName(L"grime_blood2_C");
    if (!g_dripCls)  g_dripCls  = OI::ClassByName(L"grime_blood2_drip_C");
    if (g_bloodCls && g_typeOff == -2) {
        g_typeOff = R::FindPropertyOffset(g_bloodCls, L"Type");
        int32_t off = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(g_bloodCls, L"randomOrientation", off, mask)) { g_randOff = off; g_randMask = mask; }
        else g_randOff = -1;
    }
}

bool IsBlood(void* obj, bool& drip) {
    ResolveClasses();
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls || !g_bloodCls) return false;
    if (g_dripCls && R::IsDescendantOfAny(cls, &g_dripCls, 1)) { drip = true; return true; }
    drip = false;
    return R::IsDescendantOfAny(cls, &g_bloodCls, 1);
}

void OnBeginPlayPost(const sg::Call& call) {
    if (g_mirrorSpawning) return;  // a peer's decal, spawned here
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    bool drip = false;
    if (!call.object || !IsBlood(call.object, drip)) return;
    if (s->role() == coop::net::Role::Client && !coop::net_pump::IsInAnnouncedWorld(call.object)) return;  // the save's own
    const uint64_t now = NowMs();
    if (now - g_windowMs >= 1000) { g_windowMs = now; g_inWindow = 0; }
    if (++g_inWindow > kMaxPerSecond) { ++g_dropped; return; }

    coop::net::BloodDecalPayload p{};
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(call.object, loc) || !E::TryGetActorRotation(call.object, rot)) return;
    const ue_wrap::FVector scl = E::GetActorScale3D(call.object);
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
    p.sclX = scl.X; p.sclY = scl.Y; p.sclZ = scl.Z;
    p.type = g_typeOff >= 0 ? *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(call.object) + g_typeOff) : 0;
    if (p.type < 0 || p.type > 64) p.type = 0;
    p.drip = drip ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::BloodDecal, &p, sizeof(p))) return;
    if (++g_sent == 1)
        UE_LOGI("[BLOOD] the first splat went out: pos=(%.0f,%.0f,%.0f) type=%d drip=%u", loc.X, loc.Y, loc.Z, p.type,
                static_cast<unsigned>(p.drip));
}

struct SpawnCtx { const coop::net::BloodDecalPayload* p; };

// Before BeginPlay: the material variant, and no random spin, so the copy faces as the sender's does.
void BeforeFinish(void* ctx, void* actor) {
    const auto* p = static_cast<SpawnCtx*>(ctx)->p;
    uint8_t* base = reinterpret_cast<uint8_t*>(actor);
    if (g_typeOff >= 0) *reinterpret_cast<int32_t*>(base + g_typeOff) = p->type;
    if (g_randOff >= 0) base[g_randOff] = static_cast<uint8_t>(base[g_randOff] & ~g_randMask);
    E::SetActorRotation(actor, ue_wrap::FRotator{p->rotPitch, p->rotYaw, p->rotRoll});
    E::SetActorScale3D(actor, ue_wrap::FVector{p->sclX, p->sclY, p->sclZ});
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered || g_refused) return;
    if (sg::WatchName(kRowGrimeBeginPlay, kTag, nullptr, &OnBeginPlayPost)) {
        g_registered = true;
        return;
    }
    g_refused = true;
    UE_LOGE("[BLOOD] the script-body gate refused the grime BeginPlay watch -- blood stays on the screen it fell on");
}

void Tick() {
    if (!g_registered && !g_refused) Install(g_session.load(std::memory_order_acquire));
}

void OnBloodDecal(const coop::net::BloodDecalPayload& p) {
    ResolveClasses();
    void* cls = p.drip ? g_dripCls : g_bloodCls;
    if (!cls) cls = g_bloodCls;
    if (!cls) {
        ++g_dropped;
        return;  // the class is not loaded here yet: nothing has bled on this peer this run
    }
    SpawnCtx ctx{&p};
    g_mirrorSpawning = true;
    void* a = E::SpawnActor(cls, ue_wrap::FVector{p.locX, p.locY, p.locZ}, false, &BeforeFinish, &ctx);
    g_mirrorSpawning = false;
    if (!a) { ++g_dropped; return; }
    // BeginPlay may snap the decal to the surface under it; the sender's own decal already did, so its
    // transform is the settled one.
    E::SetActorLocation(a, ue_wrap::FVector{p.locX, p.locY, p.locZ});
    E::SetActorRotation(a, ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
    E::SetActorScale3D(a, ue_wrap::FVector{p.sclX, p.sclY, p.sclZ});  // FinishSpawning re-applied the spawn transform
    if (++g_spawned == 1) UE_LOGI("[BLOOD] the first peer splat spawned here at (%.0f,%.0f,%.0f)", p.locX, p.locY, p.locZ);
}

void OnDisconnect() {
    if (g_sent || g_spawned || g_dropped)
        UE_LOGI("[BLOOD] session end -- splats sent=%llu spawned=%llu dropped=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_spawned),
                static_cast<unsigned long long>(g_dropped));
    g_sent = g_spawned = g_dropped = 0;
}

}  // namespace coop::blood_decal_sync
