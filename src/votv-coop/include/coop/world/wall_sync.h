// coop/world/wall_sync.h -- the player-built concrete walls are the host's (ue_wrap/actors/wall).
//
// Each outline, wall and wet wall is one save record under its key. The host's records cross as WallSave blobs: a
// placement, a change (material fed, damage, a repair, a wet wall drying), a removal, and every record to a joiner;
// a client applies each to the actor its key names, or spawns it. A client's outline (the wall builder's addWall)
// takes a minted "mvpl_" key and its record goes to the host, which checks the sender's reach and spawns its own,
// the canonical. What spends material or makes a wall runs on the host alone: a client refuses its own copies'
// material overlap, buildWall, drying tick and break debris; its damage to a wall is sent as an amount the host
// deals, its creative build as a request. A client's repair (the trowel's fix) is refused without spending cement.
//
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/world/placed_save_sync.cpp, wall_build_sync.cpp and
// wall_creative_sync.cpp (MIT): the wall family alone, out of a lane that also carried the garden, hoses and dug
// holes (ours are plant_sync and dig_hole_sync), its watches made rows of the wall classes.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::wall_sync {

// Registers the watches and the placement seam (FinishSpawningActor). Retried until settled. Game thread.
void Install(coop::net::Session* session);

// Host: the census of walls, the material fed, the records owed. Client: the host's records applied, its own
// placement and requests sent. Game thread.
void Tick();

// One WallSave chunk from `senderSlot`. Game thread.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// Host: every wall's record is owed to the joiner (sent to all, which a seated client takes as a repaint).
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();

}  // namespace coop::wall_sync
