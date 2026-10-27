// coop/props/open_container_sync.h -- what lies in an open container (a basket, a bowl, a plate, a fridge
// shelf) goes where the container goes, on every peer.
//
// An open container's ReceiveTick appends each prop resting in its box to itemsInside and attaches it
// (setPropProps, K2_AttachToActor), so on the host the contents ride along when the container is carried.
// The pose lanes carry the container alone: an attached child is neither held nor driven, so a client
// kept its copies where they were, outside the container it saw carried away, where they fought the
// physics they were left in (the endless knock). The host now reads every open container's itemsInside
// on a short period and sends a position correction (PropSnapPos, with the prop's frozen and sleep) for
// each item that moved since the last one it sent, so a client's copies follow and come to rest where
// the host's are. The garbage container is left to garbage_sync. Gameplay layer (principle 7). Game
// thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::open_container_sync {

// HOST: on the period, the corrections for the items that moved inside an open container. Game thread,
// every pump tick; between periods it costs one clock read.
void Tick(coop::net::Session& session);

// Session end: what was last sent is forgotten.
void OnDisconnect();

}  // namespace coop::open_container_sync
