// coop/creatures/walker_mirror.h -- one walking mannequin for everyone.
//
// The walking mannequin (prop_wMannequin_C) is a prop with a brain: its tick chases a player through an
// AI child pawn, stops while it is looked at, picks up weapons and deals damage on timers. The host's
// walker crossed to clients as a plain prop mirror, and that mirror ran the same brain on each client,
// so every peer saw its own walker in its own place, chasing its own player; holding one snapped it to
// the holder. On a client the walker's brain entries (its tick, its five timers, its nav path update)
// are refused, and the host sends each walker's pose to the clients at 10 Hz as a position correction
// (PropSnapPos) whenever it moved, so there is one walker, where the host's is. Gameplay layer
// (principle 7). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::walker_mirror {

void Install(coop::net::Session* session);
void Tick(coop::net::Session& session);
void OnDisconnect();

}  // namespace coop::walker_mirror
