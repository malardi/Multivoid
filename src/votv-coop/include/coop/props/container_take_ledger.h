// coop/props/container_take_ledger.h -- CLIENT: what this player took out of a world container and
// carried away, held until the host has had its say on the edit that took it.
//
// A take is two moves the game makes in one extract: the container's propInventory.takeObj drops the
// record from the container, and mainPlayer.putObjectInventory2 adds a record of the same class to the
// player's own store (GObjStack[0]). coop/props/container_contents_sync ships the first move to the host
// as a slice the host arbitrates; the second goes nowhere but this peer's own profile stream. So a
// refused slice put the container back -- the host re-publishes its truth -- and left the item in the
// player's inventory, which the host then stored with the profile: one item, two places. Two players
// taking the last burger at once left the loser a copy.
//
// The ledger is the other half of that answer. At the verb edge it notes the take (the container, the
// class taken, what was carried); at the next tick the record the take became in the carried store;
// at the ship, the slice that carried the take to the host. A refusal answer names its slice
// (container_slice_wire::PackRefusal), and every take that slice carried is taken back out of the
// player's inventory. Acceptance says nothing on the wire -- the author is left out of the relay -- so
// a take is settled by silence: by a later slice of the same container being answered, or by age.
// Game thread. The host keeps nothing here.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::props::container_take_ledger {

// A take is forgotten, taken as accepted, after this. The host answers a refusal within a round trip,
// or from its pen when the slice waited there -- whose TTL is 30 s (container_park::kTtlMs) -- so this
// outlasts any answer the host can still give.
inline constexpr uint64_t kSettleMs = 45000;
// Takes held at once. Human-rate; the oldest goes past it.
inline constexpr size_t kMaxTakes = 64;

// At the takeObj verb edge, on a world container's inventory `inv` of element `eid`, before the body
// runs: `index` is the record about to leave. Reads the container's record and the carried store.
void NoteTake(uint32_t eid, void* inv, int32_t index, uint64_t nowMs);

// The other direction, a put. The inventory screen moves an item into a container as the game's own
// pair: the player's store gives up its record (getObject -> takeObj on the player's own inventory), an
// actor is born from it, and addObject takes that actor into the container. Refused, the container went
// back and the item was in neither place. So the record is noted as it leaves the player's store, and a
// put of an actor carrying that record's key into a world container right after is held like a take:
// a refusal of the slice it ships in gives the record back to the store.
//
// At the takeObj verb edge on this player's own store, before the body runs.
void NotePersonalTake(void* inv, int32_t index, uint64_t nowMs);
// At the addObject verb edge on a world container's inventory, before the body runs: `actor` is going
// in. Held only when it is the record the player's own store gave up a moment ago -- a put of a held
// or thrown world prop is not (see the .cpp).
void NotePut(uint32_t eid, void* actor, uint64_t nowMs);

// Each tick, ahead of the drain that ships slices: finds the carried record each new take became, and
// forgets what has settled by age.
void Tick(uint64_t nowMs);

// The slice `sliceId` (container_slice_wire::SliceId) of container `eid` went to the host: the takes
// from it not yet shipped rode in it.
void NoteShipped(uint32_t eid, uint64_t sliceId);

// The host refused slice `sliceId` of container `eid` and re-published its truth. Every take that
// slice carried leaves the carried store again and every put returns to it; the edits of that
// container shipped before it are settled. The number of records taken back or returned.
size_t OnRefused(uint32_t eid, uint64_t sliceId);

// The session ended.
void Reset();

}  // namespace coop::props::container_take_ledger
