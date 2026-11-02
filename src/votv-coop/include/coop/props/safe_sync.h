// coop/props/safe_sync.h -- the safe opens once, on the host.
//
// A safe (prop_safe_C) cracked by its dial or broken open runs `opened`, which creates its contents. Each peer
// ran its own: a client that cracked one got contents only it had (props the host never knew), and the host's
// safe stayed locked. Now a client's `opened` is cancelled -- its dial feedback stays -- and sent to the host
// as a request; the host checks the reach, unlocks its own safe through the game's `hacked` (the contents are
// born once, the host's props, on every peer through the prop lanes) and publishes the safe's record, so every
// copy reads open. A refused request gets the host's record back, so the client's dial does not leave it a
// lock the host does not have. A client's own record never moves a safe's lock (an intent gate). Ported from
// the WatchersOfTheVoid fork (MIT).
//
// Wire: ReliableKind::SafeUnlock (SafeUnlockPayload), client -> host, never relayed. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct SafeUnlockPayload;
}

namespace coop::safe_sync {

void Install(net::Session* session);
void Tick();
void Receive(const net::SafeUnlockPayload& p, uint8_t sender);
void OnDisconnect();

}  // namespace coop::safe_sync
