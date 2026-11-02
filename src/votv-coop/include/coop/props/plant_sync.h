// coop/props/plant_sync.h -- the growing plants: one garden, the host's.
//
// A plant (growingPlant_C, its trees and herbs) is an actor_save, not a prop, so no prop lane carried it, and
// every part of it was each peer's own: a plant the host put in a pot never grew on a client, a client's never
// on the host; each copy grew, dried, died and ripened on its own clock; a fruit tree hung fruit props of its
// own on every peer (addFruit spawns real props), so a client saw the host's fruit and its own; a berry bush
// one player picked stayed ripe for the other (its Amount is in no record); and the wild spawners (bushes,
// herbs, sugar cane, spores) seeded different plants on every peer.
//
// The host's garden is the one that counts. Its plants go out by their actor_save key, each with its class,
// place, plant row, pot (AttachParent), growth, water, fertilizer, fruit, dryness and harvest amount: on a
// change, every 15 s for each one regardless, and whole to a joiner at its world-ready. A client applies a
// record as the plant's own loadData does (the fields, then upd, updLoc, processGrown), spawns a plant it
// lacks, and destroys one the host no longer has. A client's copy hangs no fruit (addFruit) and does not die on
// its own clock (turnDead): the host's fruit arrives as host props, its death as the plant row.
//
// What a client's player does is an intent the host applies and re-publishes: a planting (a plant born here
// within a few seconds of the player's pot use or buried item), a watering, fertilizing or growinator shot (the
// plants near the player, read after the verb), a harvest or a discard (the plant gone, or its amount and
// fruit taken; only a plant whose play ended by a destroy is gone -- a garden whose sublevel streamed out
// sleeps under its identities and wakes with them). A planting the host refuses (an occupied pot) is taken
// back. A plant that appears on a client without a planting is kept (most often a streamed-in level's; the
// wild spawners themselves are refused on a client in spawn_authority). When both peers plant the same spot
// (a seed both see fall into a dug hole), the client takes the host's plant for its own by place and row.
//
// Wire: ReliableKind::PlantSync (BlobChunkPayload), both directions, never relayed. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::plant_sync {

void Install(coop::net::Session* session);

void Tick();

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// HOST: every plant is owed to this slot (its world-ready).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnPeerGone(int peerSlot);

void OnDisconnect();

}  // namespace coop::plant_sync
