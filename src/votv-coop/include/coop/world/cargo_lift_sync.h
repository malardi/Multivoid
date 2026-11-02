// coop/world/cargo_lift_sync.h -- the cargo lift, one trip for everyone: the host's.
//
// A lift (cargoLift_C) is a placed controller whose platform is a physics body on a timeline, and each peer ran
// its own: a client's button press moved its own platform only, and the host's trip never reached a client.
// Now a client's press is a request (its own press is cancelled), the host presses the button for that player
// after a reach check, and the host's lift is the canonical one: each start and each rest is one revision a
// client replays natively -- the same button on an aligned platform, so the trip runs with the game's own
// physics and obstruction checks -- with a heartbeat for a late joiner. Ported from the WatchersOfTheVoid fork
// (MIT).
//
// Wire: ReliableKind::CargoLift (CargoLiftPayload), client -> host requests and host -> client canonicals,
// never relayed. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CargoLiftPayload;
}

namespace coop::cargo_lift_sync {

void Install(net::Session* session);
void Tick();
void Receive(const net::CargoLiftPayload& p, uint8_t sender);
// HOST: a joiner gets every lift's canonical at the next scan.
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();

}  // namespace coop::cargo_lift_sync
