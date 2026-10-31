// coop/interactables/window_stroke_sync.h -- the base's big bay window (Ad_window_C), whose dirt is a
// render target wiped one sponge dab at a time, replicated as dabs on ReliableKind::WindowStroke.
// Gameplay/network layer (principle 7); the engine is reached only through ue_wrap::window_canvas.
//
// SYMMETRIC, presser-authored. A stroke runs only on the peer whose camera launched it, so that
// peer observes its own dab (a Func pre-hook on UCanvas::K2_DrawMaterial filtered to a window frame
// with an open stroke session), takes the draw's own corner, edge and brush material -- the stroking
// sponge's, held or thrown -- reads that material's opac/col at the tick and sends the dab. Every other peer
// replays it through the window's own Canvas session, and the host relays a client's dab to the
// other clients, so the host's world -- and its save -- carries every peer's wipes. A joiner gets the
// window as it was in the host's transferred save.
// Known limits: a dab whose brush material is gone by the tick falls back to the held sponge's brush
// and is dropped without one; dabs that arrive while a signal plays wait for playback to end; the
// periodic dirt splotch still rolls on each peer independently.
// The pre-hook's capture of the draw arguments is adapted from ElvardFIC/WatchersOfTheVoid
// src/votv-coop/src/coop/interactables/window_stroke_sync.cpp (MIT), which reads the brush at the
// native draw rather than from the hand.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WindowStrokePayload;
}  // namespace coop::net

namespace coop::window_stroke_sync {

// Store the session and register the window with the shared scan. Idempotent. Game thread.
void Install(coop::net::Session* session);

// A WindowStroke arrived (payload already range-checked by the dispatch): queue the dab for replay.
void OnReliable(const coop::net::WindowStrokePayload& payload, uint8_t senderPeerSlot);

// Per net-pump tick: install the stroke observer once, send observed dabs, replay received ones.
void Tick();

// Session teardown: drop queued dabs and release the brush material.
void OnDisconnect();

}  // namespace coop::window_stroke_sync
