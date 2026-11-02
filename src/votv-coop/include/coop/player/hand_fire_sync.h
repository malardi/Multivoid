// coop/player/hand_fire_sync.h -- the flame of the item in a player's hand. Ported from the
// WatchersOfTheVoid fork (MIT), on our hand generations.
//
// A world prop's fire is the host's (coop/props/prop_fire_sync), but the item in a hand is no world
// prop: it is the owner's local actor, and every other peer shows a display mirror of it
// (coop/player/hand_item). So the owner speaks for its hand's flame. It samples its hand actor's fire
// (burning, fuel, fuel speed) four times a second and sends a HandFire, tied to the hand generation,
// when the flame lights, goes out, or drifts from the fuel the receivers age it to, and every 2 s
// while it burns. The host keeps and relays a client's; each peer applies a sample to the mirror of
// that exact generation, aged by the time since it arrived, and to no other: a sample for an item
// that has left the hand matches no mirror.
//
// A torch burns with a flame of its own (ue_wrap/actors/torch): its sample says so (kind 1) and carries the
// torch's fuel, and a mirror of it is lit and put out with the torch's own ignite and extinguishFire.
//
// On a mirror the fire's own verbs stay the display's: the game's ignite and extinguishFire run there only
// while this lane applies a sample, and fireDamage and ignited do not run (the owner's item does the damage
// and its class's own effects). prop_fire_sync's name watches ask MirrorVerbRuns, one watch per function. A burning item released from the hand is a world prop's fire from then on
// (prop_fire_sync::OnHandReleased). Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct HandFirePayload; }

namespace coop::hand_fire_sync {

void Install(coop::net::Session* session);
void Tick();
void Receive(const coop::net::HandFirePayload& payload, uint8_t senderSlot);
// Host: a world-ready joiner is owed every hand's flame.
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();

// Whether a fire verb on `mirror`, a hand display mirror, may run: ignite or extinguishFire (`applyVerb`) while
// this lane applies a sample to that mirror, nothing else. Game thread.
bool MirrorVerbRuns(void* mirror, bool applyVerb);

}  // namespace coop::hand_fire_sync
