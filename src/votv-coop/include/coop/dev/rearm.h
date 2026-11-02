// coop/dev/rearm.h -- the lab's re-arm of the drills inside one session. The lab's `drill <row> <value>` command
// holds a drill's row in memory and bumps the epoch here, then sends every drill back to its first phase (the same
// reset a session's end gives them). A drill whose knob is read through Latched reads it again at the next epoch; one
// that latches it in a plain static keeps what it read at its first use until the next launch. Dev only.

#pragma once

#include <atomic>
#include <cstdint>

namespace coop::dev::rearm {

// The current epoch: 0 until the first re-arm. Any thread.
uint32_t Epoch();

// A new epoch: every Latched knob is read again at its next use. Any thread.
void Bump();

// `slot`, read by `read()` at its first use and again at the first use after each Bump; `seen` is the slot's own epoch,
// initialised to ~0u by the caller. For a drill's knob, read each tick: off, it costs one atomic load. Game thread.
template <class T, class Read>
const T& Latched(T& slot, uint32_t& seen, Read read) {
    const uint32_t e = Epoch();
    if (seen != e) {
        slot = read();
        seen = e;
    }
    return slot;
}

}  // namespace coop::dev::rearm
