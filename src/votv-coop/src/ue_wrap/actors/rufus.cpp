// ue_wrap/actors/rufus.cpp -- see ue_wrap/actors/rufus.h. The steps mirror ue_wrap/actors/fossilhound.cpp, whose
// mesh hangs on a spring arm the same way.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/actors/rufus.cpp (MIT).

#include "ue_wrap/actors/rufus.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ue_wrap::rufus {
namespace {

namespace P = profile;
namespace R = reflection;

constexpr const wchar_t* kFamilyClass = L"rufus_C";
constexpr const wchar_t* kAnimClass = L"rufus_Skeleton_AnimBlueprint_C";

R::InstanceOffset g_springArm{L"SpringArm"};
R::InstanceOffset g_pawn{L"pawn"};  // on the AnimBP: the body whose velocity it reads

// EAttachmentRule::KeepRelative, which BeginPlay passes for all three rules.
constexpr uint8_t kKeepRelative = 0;

// A live object in the pointer slot `off` bytes into `base`, or null.
void* LiveAt(void* base, size_t off) {
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Says a step failed, once per step (the family is one class pair, so the step alone keys it). False, so a step's
// failure reads `ok = Failed(...)`.
bool Failed(void* actor, const char* step) {
    static const char* seen[4] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (std::strcmp(seen[i], step) == 0) return false;
    if (count < 4) seen[count++] = step;
    UE_LOGW("rufus: %ls mirror presentation step '%s' did not run", R::ClassNameOf(actor).c_str(), step);
    return false;
}

// Is `actor` of the family: its class chain compared with rufus_C by FName, the name resolved once (NAME_None is
// not latched, so an early call asks again).
bool InFamily(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    static R::FName sName{};
    static bool sResolved = false;
    if (!sResolved) {
        if (!fname_utils::Resolved()) return false;
        const R::FName name = fname_utils::StringToFName(kFamilyClass);
        if (name.ComparisonIndex == 0 && name.Number == 0) return false;
        sName = name;
        sResolved = true;
    }
    int hops = 0;
    for (void* cls = R::ClassOf(actor); cls && hops < 16; cls = R::SuperStructOf(cls), ++hops) {
        const R::FName& n = R::NameOf(cls);
        if (n.ComparisonIndex == sName.ComparisonIndex && n.Number == sName.Number) return true;
    }
    return false;
}

// The family member's mesh (ACharacter::Mesh), or null.
void* BodyMesh(void* actor) {
    return InFamily(actor) ? LiveAt(actor, P::off::ACharacter_Mesh) : nullptr;
}

// Mesh->K2_AttachToComponent(SpringArm, None, KeepRelative x3, true).
bool AttachMesh(void* actor, void* mesh) {
    const int32_t off = g_springArm.Of(actor);
    void* arm = off >= 0 ? LiveAt(actor, static_cast<size_t>(off)) : nullptr;
    void* fn = arm ? R::FindDispatchFunctionCached(R::ClassOf(mesh), L"K2_AttachToComponent") : nullptr;
    ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName none{};
    f.Set<void*>(L"Parent", arm);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", kKeepRelative);
    f.Set<uint8_t>(L"RotationRule", kKeepRelative);
    f.Set<uint8_t>(L"ScaleRule", kKeepRelative);
    f.Set<bool>(L"bWeldSimulatedBodies", true);
    return Call(mesh, f);
}

// anim.pawn = this, when the mesh's animation instance is the class's AnimBP. A mesh with another animation
// blueprint is the game's own failed cast: nothing to write.
bool WritePawn(void* actor, void* mesh) {
    void* anim = LiveAt(mesh, P::off::USkeletalMesh_AnimScriptInstance);
    void* base = object_index::ClassByName(kAnimClass);
    void* animCls = anim ? R::ClassOf(anim) : nullptr;
    if (!animCls || !base || !R::IsDescendantOfAny(animCls, &base, 1)) return true;
    const int32_t pawnOff = g_pawn.Of(anim);
    if (pawnOff < 0) return false;
    *reinterpret_cast<void**>(static_cast<uint8_t*>(anim) + pawnOff) = actor;
    return true;
}

}  // namespace

bool ApplyBeginPlayPresentation(void* actor) {
    if (!InFamily(actor)) return false;
    void* mesh = LiveAt(actor, P::off::ACharacter_Mesh);
    if (!mesh) return Failed(actor, "mesh");
    bool ok = true;
    if (!AttachMesh(actor, mesh)) ok = Failed(actor, "attach");
    if (!WritePawn(actor, mesh)) ok = Failed(actor, "anim pawn");
    return ok;
}

bool ReadBodyYaw(void* actor, float& outYaw) {
    void* mesh = BodyMesh(actor);
    if (!mesh) return false;
    FRotator rot{};
    if (!engine::TryGetComponentWorldRotation(mesh, rot) || !std::isfinite(rot.Yaw)) return false;
    outYaw = rot.Yaw;
    return true;
}

void DriveBodyYaw(void* actor, float yaw) {
    if (!std::isfinite(yaw)) return;
    void* mesh = BodyMesh(actor);
    if (!mesh) return;
    FRotator rot{};
    if (!engine::TryGetComponentWorldRotation(mesh, rot)) return;
    rot.Yaw = yaw;
    engine::SetComponentWorldRotation(mesh, rot);
}

}  // namespace ue_wrap::rufus
