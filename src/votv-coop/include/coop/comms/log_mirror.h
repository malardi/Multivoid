// coop/comms/log_mirror.h -- a client's logs, mirrored to the host's machine while the session runs.
//
// The host's player is the one who reads a session's logs (and hands them on); a client's live in a
// folder on another machine. Each client streams the growing tail of its multivoid.log, and its
// UE4SS.log, to the host as LogChunk messages, from a thread of its own -- so a client whose game
// thread has stopped still delivers the lines that say where. The host appends them to
// <ExeDir>\coop_logs\peer<slot>_<file>, one file per peer and log, rewritten at each session.
// Paced (about 20 KB/s); a client that joins sends only the last 512 KB of what was written before.
// Network layer consumer; the client's thread and the host's game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::log_mirror {

void Install(coop::net::Session* session);   // starts the client's streaming thread once per process
void OnChunk(int slot, const void* payload, size_t len);  // HOST, game thread: a client's chunk
void OnDisconnect();                          // HOST: closes the files

}  // namespace coop::log_mirror
