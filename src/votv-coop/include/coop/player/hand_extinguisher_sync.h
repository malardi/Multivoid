// coop/player/hand_extinguisher_sync.h -- the spray and the charge of a fire extinguisher or a pressure
// washer in a player's hand. Ported from the WatchersOfTheVoid fork (MIT), on our hand generations.
//
// The item in a hand is the owner's local actor; every other peer shows a display mirror of it
// (coop/player/hand_item), spawned at its class default: a full extinguisher that never sprays. So the owner
// samples its hand at 10 Hz -- for an extinguisher (tool 0) its `firing` and `life`, for a washer (tool 1)
// its `fire` and `level` -- and sends a HandExtinguisher, tied to the hand generation, on a change and every
// 0.5 s while it sprays (1 s otherwise). The host keeps and relays a client's. A peer writes the charge onto
// the mirror of that exact generation and shows the spray there: an extinguisher's own particle and sound,
// turned along the puppet's aim, with the mirror's tick off (the game's spray traces from this machine's
// camera and spends charge); a washer's particle burst at 5 Hz, with no projectile. A spray not refreshed for
// 1.5 s is shown stopped.
//
// The fork's lane also carries the water bucket, the concrete bucket and the trowel. Here their stock is the
// prop's save record (coop/props/prop_record_refresh), so this lane leaves them alone: two lanes writing one
// field is the parallel path the record lane forbids. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct HandExtinguisherPayload; }

namespace coop::hand_extinguisher_sync {

void Install(coop::net::Session* session);
void Tick();
void Receive(const coop::net::HandExtinguisherPayload& payload, uint8_t senderSlot);
// Host: a world-ready joiner is owed every hand's spray and charge.
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();

}  // namespace coop::hand_extinguisher_sync
