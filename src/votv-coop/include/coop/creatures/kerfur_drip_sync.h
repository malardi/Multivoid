// coop/creatures/kerfur_drip_sync.h -- what a kerfur Omega wears is the same on every peer.
//
// An Omega's accessories are kerfurOmega_C.drip, a list of row names in list_kerfurDrip: equipItem(player)
// adds the held accessory's name and calls updateDrip, which rebuilds the worn meshes from the list (and the
// held accessory is destroyed, which the prop lane carries); unequipItem takes one off the same way. All of it
// ran on the dresser's machine alone, so to every other peer the robot stayed undressed. updateDrip's exit is
// watched on every peer and the whole list is sent with the npc lane's eid; every other peer writes its copy's
// list and runs updateDrip itself. The host also sends every Omega's list every few seconds, which dresses a
// late joiner's and a fresh conversion's mirror. Gameplay layer (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurDripPayload;
}  // namespace coop::net

namespace coop::kerfur_drip_sync {

void Install(coop::net::Session* session);
void Tick();

// A peer's list for one Omega, its count bounds-checked. Game thread.
void OnKerfurDrip(const coop::net::KerfurDripPayload& p, int senderSlot);

void OnDisconnect();

}  // namespace coop::kerfur_drip_sync
