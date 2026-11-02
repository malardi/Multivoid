// coop/props/destroy_flood_guard.h -- the host's budget on the prop destroys one client may command.
// A client's PropDestroy names a host prop by key or by eid and the host destroys it: the lane trusts
// the peer (event_dispatch_entity says so). On 02.11 a joining client's own save replay ran after its
// load episode had closed, and its pre-delete went out as 2434 keyed destroys in one second; the host
// applied every one, and its autosave wrote the wiped world. However the client side is fixed, one
// client must never be able to take the host's world in a burst, so the host meters each slot: a
// bucket of destroys that refills at a player's pace, and a destroy over it is refused, logged, and
// counted. A refused destroy leaves the host's prop alive, which is the authoritative copy; the
// client's local loss is the smaller harm.

#pragma once

namespace coop::destroy_flood_guard {

// The host heard a PropDestroy from client slot `slot` naming `keyW` (empty for an eid-only destroy)
// and element `eid` (0 for none). True: apply it. False: refuse it, the slot is over its budget.
// Host only; a client never meters its host. Game thread.
bool Admit(int slot, const wchar_t* keyW, unsigned eid);

}  // namespace coop::destroy_flood_guard
