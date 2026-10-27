// coop/world/dimension_travel.h -- the run's other levels (the dimensions) with the session kept.
//
// VOTV sends a player to a dimension -- the labyrinth (sl_backroomsObsolete), the well pit, hell, the goop,
// the white void -- with lib_C::loadLevel(<level>), a level other than the gameplay map untitled_1. The
// session saw that world as "not gameplay", read it as a quit to the menu and ended (registry_reaper's
// RAM-balloon guard), and a client was left alone in its own world. Here:
//  - a peer's own travel to a level that is neither the menu nor untitled_1 marks this peer as in a
//    dimension; the menu guard and the world change's save-replay wait leave such a world alone;
//  - the host's own travel, to a dimension or back to untitled_1, is sent to every client (WorldTravel),
//    and each client travels to the same level, so every peer is in one world;
//  - a run-ending travel to the menu taken inside a dimension (a death there, the white void's end) goes
//    back to untitled_1 instead of ending the run, and on the host the clients follow it there.
// Gameplay layer (principle 7). Game thread.

#pragma once

#include <string>

namespace coop::net {
class Session;
struct WorldTravelPayload;
struct PeerLevelPayload;
}  // namespace coop::net

namespace coop::dimension_travel {

void Install(coop::net::Session* session);

// run_end_travel's seam: this peer's lib_C::loadLevel(level) is about to run, `own` when this peer's player
// authored it (not a puppet's graph). Marks the dimension state and, on the host, sends the travel.
// `viaSubArea`: the call came from lib_C's "Load Level as Sub Area"; `clearSubArea`: loadLevel's own.
void OnLocalTravel(const std::wstring& level, bool own, bool viaSubArea = false, bool clearSubArea = false);

// True while this peer's world is a dimension it travelled into during this session. Game thread; any
// thread reads a relaxed flag.
bool InDimension();

// The level a run-ending menu travel inside a dimension goes to instead: the gameplay map.
const wchar_t* ReturnLevel();

// CLIENT: the host travelled; travel to the same level. Game thread.
void OnWorldTravel(const coop::net::WorldTravelPayload& p);

// Every peer tells the others the level its world is (and "" while it travels); a puppet is not put into
// a level its peer is not in (a dimension builds itself toward every mainPlayer_C, a puppet included).
void Tick();                                   // game thread: this peer's arrival in a level is sent
void OnPeerLevel(int slot, const coop::net::PeerLevelPayload& p);
bool PoseAccepted(int slot);                   // false: that peer stands on another level than this one
bool PeerLevelKnown(int slot);                 // the peer has said its level at least once

// The census's seam (prop_drop_intent): a spawn finished by the gameplay mode's script. Its carried-equipment
// walk is read there, and a client's runaway array emptied in a dimension.
void OnGamemodeSpawn(void* gamemode);

// Session start and end reset the state.
void OnSessionStart();
void OnDisconnect();

}  // namespace coop::dimension_travel
