// coop/props/pry_crowbar_sync.h -- a plank's crowbar shows on every peer while it is pried.
//
// A plank (prop_pryablePlank_*) carries hidden pryingCrowbar_C children, one per pry spot. The crowbar's
// pry (prop_crowbar_C.activatePrying) calls the nearest child's begin(player), which destroys the crowbar
// in the prier's hand and shows the child in the plank; its end() hides the child again and spawns a new
// prop_crowbar_C (flung free, dropped, or back into the prier's hand). The destroy crosses by the prop
// lane, but the child's show never did, so for every other peer the crowbar vanished the moment the pry
// began. begin's exit (the child shown) and end's entry are watched on every peer; the change is sent
// with the plank's identity and the child's component name, and every other peer shows or hides its own
// copy of that child. Its bend follows the prier's mouse and is not carried; the crowbar is seen in the
// plank. Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net {
class Session;
struct PryCrowbarPayload;
}  // namespace coop::net

namespace coop::pry_crowbar_sync {

// Registers the begin and end watches; retried each tick until they take. Game thread.
void Install(coop::net::Session* session);
// Retries the watches, and hides a peer's crowbar whose end never came (the peer left mid-pry).
void Tick();

// A peer's crowbar out or back, its fields bounds-checked. Game thread.
void OnPryCrowbar(const coop::net::PryCrowbarPayload& p);

void OnDisconnect();

}  // namespace coop::pry_crowbar_sync
