// coop/world/blood_decal_sync.h -- a blood splat lands on every peer.
//
// A player who bleeds feeds bloodDecalMaster_C's particles, and each particle that lands spawns a
// grime_blood2_C (or its drip child): a decal actor, not a prop, so no prop lane carried it, and the
// splatter of a player was seen by that player alone. The decal's BeginPlay is watched on every peer; a
// decal this peer's own run spawned is sent (its transform, its material Type, drip or not), the host
// relays it, and every other peer spawns the same decal at that transform with its orientation fixed.
// The two copies then sit at one world position, which is what grime_sync keys a decal's cleaning on,
// so a sponge on either peer cleans both. Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net {
class Session;
struct BloodDecalPayload;
}  // namespace coop::net

namespace coop::blood_decal_sync {

// Registers the BeginPlay watch; retried each tick until it takes. Game thread.
void Install(coop::net::Session* session);
void Tick();

// A peer's decal from the wire, its bounds already checked: spawn it here. Game thread.
void OnBloodDecal(const coop::net::BloodDecalPayload& p);

void OnDisconnect();

}  // namespace coop::blood_decal_sync
