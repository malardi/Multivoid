// coop/world/pipe_burst_sync.h -- the basement's water pipes burst and are fixed on the host's word.
//
// The basement drain (basementDrain_C) bursts a random pipe of its own on a timer, and an explosion
// damages one (burstWaterPipe_C.addDamage); a toolbox fixes it (toolboxFix, which a client's repair runs on
// the host through pry_break_sync's service lane). Each peer rolled its own burst, so the peers' basements
// flooded from different pipes and a client's fix never reached the host. A client's roll is refused here;
// the host reads every pipe's `damaged` once a second, sends a change at once and the whole set every few
// seconds (a late joiner's seed, and the correction for a client's own explosion damage), and a client
// applies it through the pipe's own state(damaged). The water level each peer simulates from it.
// Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net {
class Session;
struct PipeStatePayload;
}  // namespace coop::net

namespace coop::pipe_burst_sync {

void Install(coop::net::Session* session);
void Tick();

// The host's state of one pipe. Game thread (client).
void OnPipeState(const coop::net::PipeStatePayload& p);

void OnDisconnect();

}  // namespace coop::pipe_burst_sync
