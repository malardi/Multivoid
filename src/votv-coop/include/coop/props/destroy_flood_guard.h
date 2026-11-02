// coop/props/destroy_flood_guard.h -- the host's budget on the prop destroys one client may command.
// A client's PropDestroy names a host prop by key or by eid and the host destroys it: the lane trusts
// the peer (event_dispatch_entity says so). On 02.11 a joining client's own save replay ran after its
// load episode had closed, and its pre-delete went out as 2434 keyed destroys in one second; the host
// applied every one, and its autosave wrote the wiped world. However the client side is fixed, one
// client must never be able to take the host's world in a burst, so the host meters each slot: a
// bucket of destroys that refills at a player's pace, and a destroy over it is refused, logged, and
// counted. A refused destroy leaves the host's prop alive, which is the authoritative copy, and the
// host gives it back to the peers (Tick): the client that destroyed it had lost it alone, and a lawful
// burst past the budget -- a big blast, a crate of loot -- must not leave that client a world the host
// does not have.

#pragma once

namespace coop::destroy_flood_guard {

// The host heard a PropDestroy from client slot `slot` naming `keyW` (empty for an eid-only destroy)
// and element `eid` (0 for none). True: apply it. False: refuse it, the slot is over its budget.
// Host only; a client never meters its host. Game thread.
bool Admit(int slot, const wchar_t* keyW, unsigned eid);

// HOST, each tick: a few of the props whose destroy was refused are expressed again
// (prop_snapshot::ExpressIncrementalSpawn), so the client that destroyed its copy gets the host's back;
// every other peer still has it and its apply is idempotent. Paced, so a refused flood of thousands
// drains over seconds. Game thread.
void Tick();

// The session is gone: the queue named its props.
void Reset();

}  // namespace coop::destroy_flood_guard
