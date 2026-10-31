// coop/world/madness_sync.h -- the Madness Combat event's music and kill counter on every peer.
//
// The funny console's madness.combat runs on the host (coop/interactables/sat_console_table), whose event master
// plays its music as a 2D sound and shows its kill counter on the host's screen alone: a client heard and saw
// nothing of it. The host reads its master four times a second and sends the state on change and every second
// while it runs (a late joiner's seed), once more when it ends; a client starts the same music at the host's
// position, shows the same counter and writes its count. Presentation only: the event's grunts and weapons are
// not carried here. Gameplay layer (principle 7). Game thread.
// Adapted from ElvardFIC/WatchersOfTheVoid src/votv-coop/src/coop/world/madness_sync.cpp (MIT).

#pragma once

namespace coop::net {
class Session;
struct MadnessStatePayload;
}  // namespace coop::net

namespace coop::madness_sync {

void Install(coop::net::Session* session);
void Tick();

// The host's state of the event. Game thread (client).
void OnMadnessState(const coop::net::MadnessStatePayload& p);

void OnDisconnect();

}  // namespace coop::madness_sync
