// ue_wrap/actors/torch.h -- engine access for the torch (prop_torch_C and below). Principle-7 engine-wrapper layer: no
// network or coop state.
//
// A torch burns with a flame of its own, not a burning prop's: it overrides the prop's ignite(fuel), attemptIgnite and
// extinguishFire, and keeps `burning` and `fuel` members of its own beside its particle (`eff_fire`), light and sound.
// Its ignite lights those (no comp_flammable_C is made), its tick spends the fuel, and extinguishFire puts it out. So
// the hand flame lane (coop/player/hand_fire_sync) reads a torch through here, and lights or puts out a mirror of one
// with the torch's own verbs.

#pragma once

namespace ue_wrap::torch {

// True iff `actor` is a torch (prop_torch_C or a subclass). Game thread.
bool IsTorch(void* actor);

// Its own flame: `burning` and `fuel`; false for anything that is not one, or a non-finite fuel. Game thread.
bool Read(void* actor, bool& burning, float& fuel);

// The torch's own ignite(fuel) and extinguishFire, resolved on its class. False when not a torch or the call
// could not run. Game thread.
bool Ignite(void* actor, float fuel);
bool Extinguish(void* actor);

// Overwrite its fuel (a burning mirror follows the owner's). Game thread.
bool WriteFuel(void* actor, float fuel);

}  // namespace ue_wrap::torch
