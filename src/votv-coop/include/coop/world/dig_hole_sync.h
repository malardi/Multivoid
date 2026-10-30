// coop/world/dig_hole_sync.h -- a shovel's hole is dug on every peer.
//
// The shovel's dig notify, in the player's own graph, traces the ground under the blade and spawns a
// dirtHole_C there with the ground's material: an actor, not a prop, so no prop lane carried it, and a dig
// was seen by the digger alone (a client's dig never reached the host, a host's never reached a client).
// The hole's BeginPlay is watched on every peer; a hole this peer dug is sent (its transform and matType),
// the host relays it, and every other peer spawns the same hole. Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net {
class Session;
struct DirtHolePayload;
}  // namespace coop::net

namespace coop::dig_hole_sync {

// Registers the BeginPlay watch; retried each tick until it takes. Game thread.
void Install(coop::net::Session* session);
void Tick();

// A peer's hole from the wire, its bounds already checked: dig it here. Game thread.
void OnDirtHole(const coop::net::DirtHolePayload& p);

void OnDisconnect();

}  // namespace coop::dig_hole_sync
