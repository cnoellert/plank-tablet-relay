#pragma once

#include "capture_lease.hpp"
#include "identity.h"

#include <string>

// Serve one already-accepted local TCP connection. The caller owns the socket
// and can wake stop_fd to end the session. There is deliberately no listener
// here: pairing and discovery must be completed before exposing this path.
// Returns 0 for a clean Client session end, -1 for timeout, stop or failure.
int pltr_run_tcp_session(
    int socket_fd, PltrIdentityStore &store, int stop_fd,
    std::string capture_lease_name = PltrCaptureLease::ProductionName);
