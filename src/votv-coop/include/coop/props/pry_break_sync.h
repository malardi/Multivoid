// coop/props/pry_break_sync.h -- two prop verbs that only ever ran on the machine that used them: a
// crowbar's pry of a boarded-up plank, and a container's break.
//
// A plank (prop_pryablePlank_C and its edges-only child) is a Static prop nailed in place; a crowbar's
// crowbarOpen on it is what lets it go. The game runs that on the prying peer alone and the wall-attach
// lane (prop_stick_sync) never sees it, since a plank carries no comp_wallAttachable. Here the pry is
// watched on every role: once the plank's own Static and frozen are off, the prying peer sends the
// release on the stick lane's PropStickState (flags 0, the unstick), which the host relays, and every
// other peer lets its copy go from the sender's pose with the sender's velocity.
//
// A container (prop_container_C: crates, boxes, barrels) breaks through broken() or broken_fire(), whose
// Blueprint spawns the loot and destroys the container. A client's spawns are its own (the host's spawn
// lane does not take them), so a client's break left the loot on its screen alone. On a client the two
// verbs are refused at the body and sent to the host as ContainerBreakIntent; the host runs the same verb
// on its own copy, inside the sender's reach and at a bounded rate, and its spawn and destroy lanes carry
// the result to every peer. Gameplay layer (principle 7). Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct ContainerBreakIntentPayload;
struct PropStickStatePayload;
}  // namespace coop::net

namespace coop::pry_break_sync {

// Registers the watches (crowbarOpen, broken, broken_fire); retried each tick until settled.
void Install(coop::net::Session* session);

// Both roles: settles the watches, and sends the release of each plank a pry freed here. Host: runs the
// queued container breaks, each sender at a bounded rate. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's container break from the wire, its format already checked by the dispatcher. Queued,
// and run by Tick. Game thread.
void OnContainerBreakIntent(coop::net::Session& session, const coop::net::ContainerBreakIntentPayload& payload,
                            uint8_t senderSlot);

// Receiver of a PropStickState unstick (prop_stick_sync calls it before its wall-attach check): true when
// `prop` is a pryable plank, whose copy this call let go; false leaves the message to the stick lane.
bool TryApplyPlankFree(void* prop, const coop::net::PropStickStatePayload& p, const std::wstring& keyW,
                       uint8_t senderPeerSlot);

// The session ended: the pending planks, the queues and the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::pry_break_sync
