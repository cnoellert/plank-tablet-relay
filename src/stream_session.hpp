#pragma once

#include "identity.h"

#include <cstdint>

// Serve one already-accepted ordered byte-stream socket. The caller owns the
// socket and can wake stop_fd to stop the session. link_type 1 is Bluetooth
// LE, 2 is TCP; it is included in the Noise prologue, so records cannot be
// replayed between transports. An approved Client key and SESSION_READY are
// required before the raw-HID worker starts.
// Returns 0 for a clean Client session end, -1 for timeout, stop or failure.
int pltr_run_authenticated_stream_session(int socket_fd,
                                          PltrIdentityStore &store,
                                          int stop_fd, std::uint8_t link_type);
