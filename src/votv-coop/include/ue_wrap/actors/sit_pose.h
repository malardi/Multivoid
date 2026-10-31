// ue_wrap/actors/sit_pose.h -- the seated pose of a player body: the kerfur AnimBP's own sit, the
// sequence the Kerfur Omega plays on the ATV, and the offset that puts the posed body on the seat.
//
// The game seats a player through mainPlayer_C::sit (sittingOn, then K2_AttachToActor onto it) and
// leaves the body's AnimBP in its standing idle, so every skin -- the native kel included, and every
// puppet on the other screens -- stands through the chair or the ATV saddle. Player skins share the
// kerfurOmega skeleton and its AnimBP (AnimBlueprint_kerfurOmega_regular_C), and that AnimBP already
// carries the sit: `isOnAtv`, which the Omega's ATV ride raises. Raising it on a player body, with
// `useLegIK` off so the foot trace does not drag the folded legs to the floor, is what seats it.
//
// The sit clip is authored for the ATV saddle, so the body still has to be moved onto the seat. The
// capsule is NOT moved: it stays where the game or the pose stream put it, so standing up never
// starts from inside the floor. What moves is the ACharacter::Mesh slot on its parent (the capsule);
// mesh_playerVisible rides that slot, so both rendered bodies move together.
//
// Engine-wrapper layer (principle 7): no coop state; the caller owns which body sits and where.
// Adapted from ElvardFIC/WatchersOfTheVoid ue_wrap/actors/sit_pose (MIT). Game thread throughout.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::sit_pose {

// True iff `actor` is a chair the game seats a player on: a live prop whose class name begins with
// "prop_sit" (the chairs, and the parent chair a sitBox passes to mainPlayer_C::sit). The ATV, which
// seats its driver through the same sittingOn field, is an APawn, not a prop, and answers false.
bool IsChair(void* actor);

// Raise or drop the sit on every kerfur-family AnimInstance of `pawn` (mesh_playerVisible and the
// inherited ACharacter::Mesh slot): isOnAtv = sitting, useLegIK = !sitting, each through its own
// bool mask so a packed neighbour is never touched. A seated pawn is also remembered, so the puppet
// post-update hook (AfterPuppetAnimUpdate) re-asserts both flags after the AnimBP's own update, which
// derives useLegIK from the movement mode every frame. No-op on a null or dead pawn, and on an
// AnimInstance that is not kerfur-family (a foreign AnimBP holds other fields at those offsets).
void DriveSitting(void* pawn, bool sitting);

// Forget `pawn` without touching its memory: a puppet being destroyed, whose AnimInstances die with
// it. Safe on a dead pointer.
void Forget(void* pawn);

// The puppet head-gate hook's second job (ue_wrap/actors/puppet_spawn.cpp, post
// BlueprintUpdateAnimation, puppet instances only): when `actor` is a seated pawn, write the sit
// flags again on `animInstance`, after the update that recomputed them and before the graph reads
// them. One compare against an empty table when nobody sits.
void AfterPuppetAnimUpdate(void* actor, void* animInstance);

// Half the body's length (the capsule half height, 88 on the native kel; out-of-range or unreadable
// reads 88), and one head, an eighth of the whole length. The sit clip does not change either.
float ModelHalfLength(void* pawn);
float ModelHead(void* pawn);

// The seat offset of one body, held by the caller (one per puppet): the component moved and the
// relative location it was authored at, captured on the first shift and put back by ClearMeshShift.
struct MeshShift {
    void*   component = nullptr;  // the moved component; identity only, never dereferenced unchecked
    int32_t componentIdx = -1;    // its object-array slot, for the liveness test
    FVector base{};               // the relative location before the first shift
    bool    held = false;         // a shift is applied and `base` is owed back
};

// Move the visible body `forwardCm` along the pawn's facing and `upCm` along its up, both in the
// pawn's own frame (the capsule is the Mesh slot's parent, so the offset is a relative location):
// on a level chair that is world up and forward, and on a banked ATV seat the body stays on the
// tilted saddle. The first call captures the authored relative location into `s`. False, and
// nothing moved, on a null or dead pawn, no Mesh slot, a non-finite offset or a write that did not
// dispatch.
bool ApplyMeshShift(void* pawn, MeshShift& s, float forwardCm, float upCm);

// Put the body back at its authored relative location and release `s`. A component that died (the
// puppet went with it) is released without a write. Safe on an empty `s`.
void ClearMeshShift(MeshShift& s);

}  // namespace ue_wrap::sit_pose
