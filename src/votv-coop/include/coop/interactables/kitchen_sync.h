// coop/interactables/kitchen_sync.h -- the kitchen machines cook on the host: the microwave, the mini oven and the
// grill.
//
// The host's copy of each machine is the one that cooks. Its cooking state (ue_wrap/devices/kitchen_machine: run,
// power, mode, timer, the oven's power, the grill's fire, coals, lid and burn) crosses as KitchenState when it
// changes, as a heartbeat, and to a joiner; a client paints it and refuses its own copy's cooking (the microwave's
// wave, the oven's and the grill's ReceiveTick, the grill's sound and timeline), so food is cooked once, by the host,
// and crosses as the prop records do. A client's press of a button, its ignite or extinguish of the grill, run on the
// host as a KitchenIntent with the sender's puppet as the player; a coal or the lid a client put in or took off runs
// natively where it was made (the item is spent or born there, and crosses as a prop does) and its count or bit is
// written on the host's copy.
//
// Never here: the machines' doors (swinger_pose_sync), the cremator (its lever is toggle_verbs', its door a
// swinger), the coffee maker and vending machine (coffee_intent, coffee_state), the kitchen's own oven (the
// appliance lane). A machine held in a hand is the holder's and is left alone. Adapted from ElvardFIC/
// WatchersOfTheVoid src/votv-coop/src/coop/interactables/kitchen_sync.cpp (MIT), its watches by name for every class
// made rows of the three classes.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KitchenStatePayload;
struct KitchenIntentPayload;
}  // namespace coop::net

namespace coop::kitchen_sync {

// Registers the watches (each once; a refused one is said once). Game thread.
void Install(coop::net::Session* session);

// Both roles, once per gameplay tick: settles the watches. Host: runs the queued intents, publishes the machines
// whose state moved, the heartbeat and what a joiner is owed. Client: paints rows that waited for their actor, turns
// a running microwave's plate. Game thread.
void Tick();

// Client: the host's row. Host: a client's intent from `senderSlot`. The dispatcher checked the size. Game thread.
void OnState(const coop::net::KitchenStatePayload& p);
void OnIntent(const coop::net::KitchenIntentPayload& p, uint8_t senderSlot);

// Host: every machine's row is owed to `slot` once it is world-ready.
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();

// Client: the local player's lid take on a shared grill is running now (its actionOptionIndex with action 7), so
// the lid the grill's ubergraph finishes is a player's birth (coop/props/prop_spawn_authoring). Game thread.
bool LidTakeActive();

}  // namespace coop::kitchen_sync
