// coop/session/desync_watch.h -- a world the two peers no longer share is said in the game, not found a session
// later in the logs.
//
// Every 5 s the host sends a digest of its world -- how many keyed props its index holds -- and a world-ready
// client answers with its own. Both compare: past a tolerance (a few props, or half a percent of a large world:
// each peer's own hand item and a prop mid-birth differ for a moment) for three digests running, each peer says
// once in the chat feed that the worlds parted, with both numbers and where the report button is, and logs a
// [DESYNC] line; back within it, the episode ends with a log line. A join's load and a world change are left
// alone (no digest is judged before the client's world is settled).
//
// Wire: ReliableKind::WorldDigest (WorldDigestPayload): op 0 the host's digest, op 1 a client's answer. Not
// relayed. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WorldDigestPayload;
}

namespace coop::desync_watch {

void Install(net::Session* session);
void Tick();
void Receive(const net::WorldDigestPayload& p, uint8_t sender);
void OnPeerLeft(int slot);
void OnDisconnect();

}  // namespace coop::desync_watch
