// coop/interactables/shared_screen_sync.h -- every peer sees what the base computer's operator sees.
//
// The laptop's tab already crosses (coop/interactables/laptop_tab); this lane carries the rest of the desktop
// as its operator sees it: pages, what is shown, scroll offsets, texts (ue_wrap/desk/shared_screen).
// Presentation only: no press or verb is replayed. The operator is the peer holding the "laptop" claim
// (coop/interactables/device_occupancy), or the host while nobody does. The host mints a lease per holder; a
// client operator reports snapshots under it, and the host's canonical is what every other peer applies. A new
// operator first repaints the screen it inherits (three refusals on a readable UI and it resumes anyway), and
// a client lets go of the claim only once its final screen is sent, on a short bound. Read at 4 Hz; a text-only
// change travels at most once a second, any other at most twice. Late join: the canonical at world-ready.
// Wire: ReliableKind::SharedScreen (BlobChunkPayload; coop/interactables/shared_screen_wire.h), never relayed.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/interactables/shared_screen_sync.cpp (MIT).
// Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::shared_screen_sync {

// Keep the session and register the claim's release hold (idempotent; called every pump tick).
void Install(coop::net::Session* session);

// Both roles: the lease, the inherited repaint, the operator's capture and the host's sends.
void Tick();

// SharedScreen chunks: the host takes the operator's reports, a client takes the host's canonicals.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// HOST: the canonical to a joiner at its world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// A slot's occupant left: its half assemblies, its report order and what it was owed.
void OnPeerGone(int peerSlot);

void OnDisconnect();

}  // namespace coop::shared_screen_sync
