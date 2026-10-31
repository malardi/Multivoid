// ue_wrap/actors/rufus.h -- engine access for Rufus (rufus_C, and thiccfus_C below it), the funny console's
// creature. What a client does for its mirror of one, whose own Blueprint graph is refused: the part of BeginPlay
// that is presentation (the mesh hung on the spring arm, the animation instance told whose body it animates), and
// the visible body's facing, which hangs on that arm apart from the actor's root. Engine-wrapper layer (principle
// 7): reflected reads, writes and calls, with no gameplay, network or coop state. Game thread.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/ue_wrap/actors/rufus.cpp (MIT).
// Implementation: src/ue_wrap/actors/rufus.cpp.

#pragma once

namespace ue_wrap::rufus {

// Every call is a no-op (false) on an actor that is not a live rufus_C or a class below it (thiccfus_C): the class
// chain compared by name, the name resolved once.
//
// BeginPlay's presentation, in its order: the mesh re-attached to the SpringArm (KeepRelative for all three rules,
// welded), then the mesh's animation instance's `pawn` set to the actor, only when that instance's class is, or
// derives from, rufus_Skeleton_AnimBlueprint_C. Nothing of BeginPlay's HUD, AI, sound or children is run. True when
// every step ran; false, and said once per step, when one did not resolve or its call faulted (the later step still
// runs). Game thread.
bool ApplyBeginPlayPresentation(void* actor);

// The visible body's facing: the mesh's world yaw. Read is false when `actor` is not of the family or has no live
// mesh. Drive sets the mesh's world yaw and keeps its pitch and roll; it does nothing on another class. Game thread.
bool ReadBodyYaw(void* actor, float& outYaw);
void DriveBodyYaw(void* actor, float yaw);

}  // namespace ue_wrap::rufus
