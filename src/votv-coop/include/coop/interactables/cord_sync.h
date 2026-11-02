// coop/interactables/cord_sync.h -- the appliances' power cords (cord_C) and their sockets (cordSocket_C).
//
// The host's links are the canonical: which socket each cord's plug sits in and whether it carries power cross as
// CordState (on change, a 2 s refresh, every link to a joiner), and a client repaints its plugs from them. A
// client's connect is cancelled and sent as CordIntent; the host checks the sender's reach to the socket and the
// cable's length and connects its own copy. A client's unplug runs at once and is sent the same way.
// The plug's drag rides the player's one held-prop pose stream (local_streams) under a reserved key, "__mvcord_" and
// the cord's portable identity, while no other prop is held: a receiver moves the cord's head, the release closes it
// (remote_prop and the PropRelease dispatch hand both here first). Adapted from ElvardFIC/WatchersOfTheVoid (MIT).

#pragma once
#include <cstdint>
#include <string>
namespace coop::net { class Session; struct CordStatePayload; struct PropPoseSnapshot; struct PropReleasePayload; }
namespace coop::cord_sync {
void Install(net::Session* session);
void Tick();
void Apply(const net::CordStatePayload& p);
void OnIntent(const net::CordStatePayload& p, uint8_t senderSlot);
void SendAllToSlot(int slot);
// Reuse the per-player prop pose stream for the cord plug's moving head.
bool UpdateLocalDragPose(net::Session& session, bool anotherPropIsHeld);
bool IsDragPose(const net::PropPoseSnapshot& pose);
void ApplyDragPose(const net::PropPoseSnapshot& pose, uint8_t senderSlot);
// True for the reserved cord identity, including stale releases.
bool ApplyDragRelease(const net::PropReleasePayload& p, uint8_t senderSlot);
void OnPeerLeft(int slot);
void OnDisconnect();
}
