#pragma once
#include <cstdint>
namespace coop::net {class Session;struct PropFirePayload;}
namespace coop::prop_fire_sync {
void Install(net::Session* session);
void Tick();
// Whether the four fire-verb name watches are live; the hand flame lane (coop/player/hand_fire_sync) guards
// a hand mirror's verbs through them, so it waits on this.
bool WatchesLive();
// A burning client hand actor released as a world prop keeps its fire on the host.
void OnHandReleased(void* actor);
void Receive(const net::PropFirePayload& p,uint8_t sender);
void QueueConnect(int slot);
void OnPeerLeft(int slot);
void OnDisconnect();
}
