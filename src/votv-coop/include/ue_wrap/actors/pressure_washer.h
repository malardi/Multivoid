// ue_wrap/actors/pressure_washer.h -- engine access for the pressure washer (prop_waterShooter_C and below).
// Principle-7 engine-wrapper layer: no network or coop state. Ported from the WatchersOfTheVoid fork (MIT).
//
// The washer is a prop with two members of its own: `level`, its water (0..10), and `fire`, the trigger held
// while it is in a hand. Its tick, while `fire` is set, spends water and spawns the damaging water
// projectiles; a hand display mirror (coop/player/hand_extinguisher_sync) must never run that, so the
// mirror's tick is turned off and its spray is a particle burst with its sound, and no projectile.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::pressure_washer {

// True iff `actor` is a washer (prop_waterShooter_C or a subclass). Game thread.
bool IsWasher(void* actor);

// The trigger and the water; false for anything that is not one, or a non-finite level. Game thread.
bool Read(void* actor, bool& firing, float& level);

// A display mirror's water (0..10): its tick is turned off and `fire` cleared, so it spawns nothing and
// spends nothing. Game thread.
bool SetRemoteLevel(void* actor, float level);

// One burst of the washer's own particle (eff_waterThrower) at `origin` along `aim`, with its sound
// attenuated; no gameplay projectile. False when the assets are not loaded yet (asked again at most once a
// second). Game thread.
bool EmitRemote(void* actor, const FVector& origin, const FRotator& aim);

}  // namespace ue_wrap::pressure_washer
