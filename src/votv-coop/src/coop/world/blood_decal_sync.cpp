// coop/world/blood_decal_sync.cpp -- see coop/world/blood_decal_sync.h.

#include "coop/world/blood_decal_sync.h"

#include "coop/net/protocol.h"
#include "coop/element/death_seam.h"  // a splat's end: cleaned away
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/grime.h"  // ReadType, ReadProcess, WriteProcessAndApply
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

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

// Every live splat's place, taken at its BeginPlay (and again after a peer copy is placed): by the
// splat's end the engine has already torn its root down and the actor reads (0,0,0), so the end
// is sent with the place remembered here.
struct Placed { int32_t idx; ue_wrap::FVector loc; };
std::unordered_map<void*, Placed> g_placed;

void Remember(void* a) {
    ue_wrap::FVector l{};
    if (!a || !E::TryGetActorLocation(a, l)) return;
    if (g_placed.size() > 20000) g_placed.clear();  // a guard; a run never lays this many
    g_placed[a] = Placed{R::InternalIndexOf(a), l};
}

bool NearZero(const ue_wrap::FVector& l) { return std::fabs(l.X) < 1.f && std::fabs(l.Y) < 1.f && std::fabs(l.Z) < 1.f; }

void OnBeginPlayPost(const sg::Call& call) {
    {
        bool d = false;
        if (call.object && IsBlood(call.object, d)) Remember(call.object);  // every splat, the save's own too
    }
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
    p.op = coop::net::blood_op::kSpawn;
    if (!s->SendReliable(coop::net::ReliableKind::BloodDecal, &p, sizeof(p))) return;
    if (++g_sent <= 3)
        UE_LOGI("[BLOOD] a splat went out: pos=(%.0f,%.0f,%.0f) type=%d drip=%u", loc.X, loc.Y, loc.Z, p.type,
                static_cast<unsigned>(p.drip));
}

// A splat's end. The sponge's last stroke destroys a blood decal (its process, which grime_sync syncs, does
// not read on blood's material), so the end itself is sent: every other peer removes the splat at that place.
bool g_endSubscribed = false;
// The splats this peer removed for a peer: their ends reach OnBloodEnd on a later drain, and are not sent back
// (an echo would remove a neighbouring splat on the sender, splats land centimetres apart).
std::vector<void*> g_erasedForPeer;
uint64_t g_erasesSent = 0, g_erasesApplied = 0;

void OnBloodEnd(const coop::element::death_seam::ActorEnd& end) {
    ue_wrap::FVector remembered{};
    bool haveRemembered = false;
    if (auto it = g_placed.find(end.actor); it != g_placed.end()) {
        if (it->second.idx == end.actorIndex) { remembered = it->second.loc; haveRemembered = true; }
        g_placed.erase(it);
    }
    if (end.streamedOut) return;
    for (size_t i = 0; i < g_erasedForPeer.size(); ++i) {
        if (g_erasedForPeer[i] == end.actor) { g_erasedForPeer.erase(g_erasedForPeer.begin() + i); return; }
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    // Read only while the slot still holds the ended actor under its serial (death_seam's contract).
    if (R::ObjectAt(end.actorIndex) != end.actor || R::SlotSerial(end.actorIndex) != end.actorSerial) return;
    bool drip = false;
    if (!IsBlood(end.actor, drip)) return;
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(end.actor, loc) || NearZero(loc)) {
        if (!haveRemembered) {
            UE_LOGW("[BLOOD] a cleaned splat with no known place -- its erase is not sent");
            return;
        }
        loc = remembered;
    }
    if (NearZero(loc)) return;
    coop::net::BloodDecalPayload p{};
    p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
    p.sclX = p.sclY = p.sclZ = 1.f;
    p.drip = drip ? 1 : 0;
    p.op = coop::net::blood_op::kErase;
    if (s->SendReliable(coop::net::ReliableKind::BloodDecal, &p, sizeof(p)) && ++g_erasesSent <= 3)
        UE_LOGI("[BLOOD] a splat was cleaned here at (%.0f,%.0f,%.0f) -- erase sent", loc.X, loc.Y, loc.Z);
}

void CollectBlood(void* ctx, void* obj, int32_t /*index*/) { static_cast<std::vector<void*>*>(ctx)->push_back(obj); }

void EraseAt(const coop::net::BloodDecalPayload& p) {
    ResolveClasses();
    std::vector<void*> all;
    if (g_bloodCls) OI::ForEachInstance(g_bloodCls, &CollectBlood, &all);
    if (g_dripCls) OI::ForEachInstance(g_dripCls, &CollectBlood, &all);
    if (NearZero(ue_wrap::FVector{p.locX, p.locY, p.locZ})) return;  // an older peer's placeless erase
    void* best = nullptr;
    float bestD2 = 8.f * 8.f;  // within 8 cm: the copy stands where the sender's stood
    for (void* a : all) {
        ue_wrap::FVector l{};
        if (!E::TryGetActorLocation(a, l)) continue;
        if (NearZero(l)) {  // dying already: its root is gone; the remembered place says where it was
            auto it = g_placed.find(a);
            if (it == g_placed.end()) continue;
            l = it->second.loc;
        }
        const float dx = l.X - p.locX, dy = l.Y - p.locY, dz = l.Z - p.locZ;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = a; }
    }
    if (!best) return;
    if (g_erasedForPeer.size() >= 64) g_erasedForPeer.erase(g_erasedForPeer.begin());
    g_erasedForPeer.push_back(best);
    E::DestroyActor(best);
    if (++g_erasesApplied <= 3) UE_LOGI("[BLOOD] a peer's cleaned splat removed here at (%.0f,%.0f,%.0f)", p.locX, p.locY, p.locZ);
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
    if (!g_endSubscribed && coop::element::death_seam::Install()) {
        const bool a = coop::element::death_seam::SubscribeClass(L"grime_blood2_C", &OnBloodEnd);
        g_endSubscribed = a;  // grime_blood2_drip_C is its subclass: one subscription covers both
    }
}

void OnBloodDecal(const coop::net::BloodDecalPayload& p) {
    if (p.op == coop::net::blood_op::kErase) { EraseAt(p); return; }
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
    Remember(a);
    // The decal's BeginPlay rolls its own Type when the class asks for a random one, so the sender's Type is
    // written again after it and the material re-made from it. The Type is half of the key grime_sync cleans
    // by (with the quantised position): a copy of another Type would never take the sender's sponge.
    int32_t got = -1;
    ue_wrap::grime::ReadType(a, got);
    if (g_typeOff >= 0 && got != p.type) {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a) + g_typeOff) = p.type;
        if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(a), L"SetMaterial")) R::CallFunction(a, fn, nullptr);
        float proc = 1.f;
        if (ue_wrap::grime::ReadProcess(a, proc)) ue_wrap::grime::WriteProcessAndApply(a, proc);
    }
    if (++g_spawned <= 3)
        UE_LOGI("[BLOOD] a peer splat spawned here at (%.0f,%.0f,%.0f) type=%d (BeginPlay had rolled %d)", p.locX,
                p.locY, p.locZ, p.type, got);
}

void OnDisconnect() {
    if (g_sent || g_spawned || g_dropped)
        UE_LOGI("[BLOOD] session end -- splats sent=%llu spawned=%llu dropped=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_spawned),
                static_cast<unsigned long long>(g_dropped));
    g_sent = g_spawned = g_dropped = 0;
    g_erasesSent = g_erasesApplied = 0;
    g_erasedForPeer.clear();
    // g_placed stays: the splats outlive the session, and a next session cleans them too.
}

}  // namespace coop::blood_decal_sync
