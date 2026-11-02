// ue_wrap/actors/torch.h -- engine access for the torch (prop_torch_C and below). Principle-7 engine-wrapper layer: no
// network or coop state.
//
// A torch burns with a flame of its own, not a burning prop's: it keeps `burning` and `fuel` members of its own beside
// its particle (`eff_fire`), light and sound. Its ignite(fuel) (bytecode read in the lab) runs the prop's ignite, and a
// tick later its own extinguishFire, which puts that prop flame out; then, with fuel above 0, it sets `burning` and
// calls upd(), which shows the light, the sound and the particle and turns the tick on (the tick spends the fuel).
// extinguishFire clears `burning` and calls upd() again. A gas can fills the fuel; a fresh torch has none.
//
// So the hand flame lane (coop/player/hand_fire_sync) reads a torch through here, and shows a mirror of one the way the
// torch shows its own flame: the members written and upd() called, without the prop flame the ignite passes through.

#pragma once

namespace ue_wrap::torch {

// True iff `actor` is a torch (prop_torch_C or a subclass). Game thread.
bool IsTorch(void* actor);

// Its own flame: `burning` and `fuel`; false for anything that is not one, or a non-finite fuel. Game thread.
bool Read(void* actor, bool& burning, float& fuel);

// Show `burning` on a display copy of a torch: the fuel (when burning) and the flag written, then the torch's own
// upd(). False when not a torch, a bad fuel, or upd() could not run. Game thread.
bool Show(void* actor, bool burning, float fuel);

}  // namespace ue_wrap::torch
