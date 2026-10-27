// coop/interactables/swinger_pose_sync.h -- a lid swung by hand is where the swinging peer's is.
//
// A swinger (prop_swinger_C: a fridge, microwave, safe or cabinet door) is a physical lid on a hinge, and a
// player opens it by grabbing and swinging it. The container lane carries its open bit at the game's
// Open/Close verbs, and replays an edge as the game's own Open(), which is a kick: the far copy moved a
// little, or not at all. While this peer's player holds a lid, and for a short settle after letting it go,
// the lid's world pose is sent at 15 Hz (SwingerPose, relayed by the host), and every other peer teleports
// its copy there and stills it. Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net {
class Session;
struct SwingerPosePayload;
}  // namespace coop::net

namespace coop::swinger_pose_sync {

// Both roles: the held lid's pose out, on the period. Game thread, every pump tick.
void Tick(coop::net::Session& session);

// A peer's lid pose from the wire, its bounds already checked. Game thread.
void OnSwingerPose(const coop::net::SwingerPosePayload& p);

void OnDisconnect();

}  // namespace coop::swinger_pose_sync
