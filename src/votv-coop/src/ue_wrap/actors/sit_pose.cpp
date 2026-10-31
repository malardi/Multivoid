// ue_wrap/actors/sit_pose.cpp -- see ue_wrap/actors/sit_pose.h. The kerfur AnimBP's sit flags and
// the seat offset of the visible body. Adapted from ElvardFIC/WatchersOfTheVoid (MIT).

#include "ue_wrap/actors/sit_pose.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace ue_wrap::sit_pose {
namespace {

namespace P = profile;
namespace R = reflection;
namespace E = engine;

// ---- the AnimBP's two flags -------------------------------------------------------------------
// Both are declared on AnimBlueprint_kerfurOmega_regular_C; the skerfuro, skeleton and converted
// skin AnimBPs subclass it, so one resolution on the base class holds for every instance the gate
// below admits. Resolved through FindBoolProperty, byte and mask, and written through the mask, so
// a bool the cooker packed beside them is never overwritten. The base class is looked up in the
// object index on every use (one lookup, no walk) and the offsets re-resolved when it comes back as
// a different object, instead of a process-lifetime latch on a Blueprint class.
struct Flags {
    void*   base = nullptr;   // the kerfur regular AnimBP class these offsets were resolved on
    int32_t sitOff = -1;  uint8_t sitMask = 0;   // isOnAtv
    int32_t ikOff  = -1;  uint8_t ikMask  = 0;   // useLegIK
    bool    warned = false;
};
Flags g_flags;

// The resolved flags for `anim`, or null when `anim` is not a live kerfur-family AnimInstance or
// the class has no isOnAtv (a recook that renamed it: the sit is then not shown, said once).
const Flags* FlagsFor(void* anim) {
    if (!anim || !R::IsLive(anim)) return nullptr;
    void* base = object_index::ClassByName(P::name::AnimBPKerfurRegularClass);
    if (!base) return nullptr;
    void* cls = R::ClassOf(anim);
    if (!cls || (cls != base && !R::IsDescendantOfAny(cls, &base, 1))) return nullptr;
    if (g_flags.base != base) {
        const bool warned = g_flags.warned;
        g_flags = Flags{};
        g_flags.base = base;
        g_flags.warned = warned;
        if (!R::FindBoolProperty(base, L"isOnAtv", g_flags.sitOff, g_flags.sitMask) || g_flags.sitMask == 0)
            g_flags.sitOff = -1;
        if (!R::FindBoolProperty(base, L"useLegIK", g_flags.ikOff, g_flags.ikMask) || g_flags.ikMask == 0)
            g_flags.ikOff = -1;
        if (g_flags.sitOff < 0 && !g_flags.warned) {
            g_flags.warned = true;
            UE_LOGW("sit_pose: isOnAtv not found on %ls -- seated players are shown standing",
                    P::name::AnimBPKerfurRegularClass);
        } else if (g_flags.sitOff >= 0) {
            UE_LOGI("sit_pose: resolved isOnAtv@0x%X/0x%02X useLegIK@0x%X/0x%02X on %ls", g_flags.sitOff,
                    g_flags.sitMask, g_flags.ikOff, g_flags.ikMask, P::name::AnimBPKerfurRegularClass);
        }
    }
    return g_flags.sitOff >= 0 ? &g_flags : nullptr;
}

void WriteMasked(void* obj, int32_t off, uint8_t mask, bool value) {
    if (!obj || off < 0 || mask == 0) return;
    uint8_t& byte = *(reinterpret_cast<uint8_t*>(obj) + off);
    byte = static_cast<uint8_t>((byte & ~mask) | (value ? mask : 0));
}

void WriteSitFlags(void* anim, bool sitting) {
    const Flags* f = FlagsFor(anim);
    if (!f) return;
    WriteMasked(anim, f->sitOff, f->sitMask, sitting);
    WriteMasked(anim, f->ikOff, f->ikMask, !sitting);
}

void* AnimOf(void* meshComponent) {
    if (!meshComponent || !R::IsLive(meshComponent)) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(meshComponent) +
                                     P::off::USkeletalMesh_AnimScriptInstance);
}

// ---- the seated table -------------------------------------------------------------------------
// The pawns DriveSitting raised, by pointer and object-array slot, so the post-update hook can tell
// a seated puppet from a standing one without a coop lookup. Sized past the session's 32 peers; a
// full table only costs the re-assert, never the immediate write.
struct Seated { void* pawn = nullptr; int32_t idx = -1; };
constexpr int kSeatedMax = 64;
Seated g_seated[kSeatedMax];
int    g_seatedCount = 0;

void Remember(void* pawn) {
    int freeAt = -1;
    for (int i = 0; i < kSeatedMax; ++i) {
        if (g_seated[i].pawn == pawn) { g_seated[i].idx = R::InternalIndexOf(pawn); return; }
        // A row whose body went without a Forget (a puppet collected under its owner) is freed here.
        if (g_seated[i].pawn && !R::IsLiveByIndex(g_seated[i].pawn, g_seated[i].idx)) {
            g_seated[i] = Seated{};
            --g_seatedCount;
        }
        if (!g_seated[i].pawn && freeAt < 0) freeAt = i;
    }
    if (freeAt < 0) return;
    g_seated[freeAt] = Seated{pawn, R::InternalIndexOf(pawn)};
    ++g_seatedCount;
}

// ---- the relative-location write --------------------------------------------------------------
// USceneComponent::K2_SetRelativeLocation, the canonical path the puppet spawn's Mesh settle uses,
// resolved once on SceneComponent (a native function: one object for the process).
bool SetRelativeLocation(void* component, const FVector& loc) {
    if (!component || !R::IsLive(component)) return false;
    if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) return false;
    static void* sFn = nullptr;
    if (!sFn) {
        if (void* sc = R::FindClass(P::name::SceneComponentClass)) sFn = R::FindFunction(sc, L"K2_SetRelativeLocation");
    }
    if (!sFn) return false;
    ParamFrame f(sFn);
    if (!f.valid() || !f.Set<FVector>(L"NewLocation", loc) || !f.Set<bool>(L"bSweep", false) ||
        !f.Set<bool>(L"bTeleport", true))
        return false;
    return Call(component, f);
}

}  // namespace

bool IsChair(void* actor) {
    if (!actor || !R::IsLive(actor) || !prop::IsDescendantOfProp(actor)) return false;
    return R::ClassNameOf(actor).rfind(L"prop_sit", 0) == 0;
}

void DriveSitting(void* pawn, bool sitting) {
    if (!pawn || !R::IsLive(pawn)) return;
    WriteSitFlags(AnimOf(puppet::GetMeshPlayerVisibleComponent(pawn)), sitting);
    WriteSitFlags(AnimOf(puppet::GetNativeBodyMeshComponent(pawn)), sitting);
    if (sitting) Remember(pawn);
    else Forget(pawn);
}

void Forget(void* pawn) {
    if (!pawn || g_seatedCount == 0) return;
    for (auto& s : g_seated) {
        if (s.pawn == pawn) { s = Seated{}; --g_seatedCount; return; }
    }
}

void AfterPuppetAnimUpdate(void* actor, void* animInstance) {
    if (g_seatedCount == 0 || !actor || !animInstance) return;
    for (const auto& s : g_seated) {
        if (s.pawn != actor) continue;
        // The slot as it was when the pawn sat: a recycled address in a dead puppet's slot is not it.
        if (R::IsLiveByIndex(actor, s.idx)) WriteSitFlags(animInstance, true);
        return;
    }
}

float ModelHalfLength(void* pawn) {
    const float half = (pawn && R::IsLive(pawn)) ? E::GetActorCharacterHalfHeight(pawn) : 0.f;
    return (std::isfinite(half) && half > 10.f && half <= 1000.f) ? half : 88.f;
}

float ModelHead(void* pawn) { return ModelHalfLength(pawn) * 0.25f; }

bool ApplyMeshShift(void* pawn, MeshShift& s, float forwardCm, float upCm) {
    if (!pawn || !R::IsLive(pawn) || !std::isfinite(forwardCm) || !std::isfinite(upCm)) return false;
    // The Mesh slot: mesh_playerVisible is attached under it, so one write moves both bodies. A body
    // without the slot moves mesh_playerVisible alone.
    void* comp = puppet::GetNativeBodyMeshComponent(pawn);
    if (!comp || !R::IsLive(comp)) comp = puppet::GetMeshPlayerVisibleComponent(pawn);
    if (!comp || !R::IsLive(comp)) return false;
    // A different component than the one held (the slot was rebuilt): the old one is put back first.
    if (s.held && (s.component != comp || !R::IsLiveByIndex(s.component, s.componentIdx))) ClearMeshShift(s);
    if (!s.held) {
        const FVector base = E::GetComponentRelativeLocation(comp);
        if (!std::isfinite(base.X) || !std::isfinite(base.Y) || !std::isfinite(base.Z)) return false;
        s.component = comp;
        s.componentIdx = R::InternalIndexOf(comp);
        s.base = base;
    }
    // The parent is the capsule, whose frame is the actor's: X forward, Z up. A body already there
    // (the offset re-applied every pose) costs one raw read, no dispatch.
    const FVector target{s.base.X + forwardCm, s.base.Y, s.base.Z + upCm};
    if (s.held) {
        const FVector now = E::GetComponentRelativeLocation(comp);
        if (std::fabs(now.X - target.X) < 0.01f && std::fabs(now.Y - target.Y) < 0.01f &&
            std::fabs(now.Z - target.Z) < 0.01f)
            return true;
    }
    if (!SetRelativeLocation(comp, target)) return false;
    s.held = true;
    return true;
}

void ClearMeshShift(MeshShift& s) {
    if (!s.held) { s = MeshShift{}; return; }
    const MeshShift old = s;
    s = MeshShift{};
    if (R::IsLiveByIndex(old.component, old.componentIdx)) SetRelativeLocation(old.component, old.base);
}

}  // namespace ue_wrap::sit_pose
