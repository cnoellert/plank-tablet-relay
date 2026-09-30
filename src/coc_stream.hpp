#pragma once

#include <cstddef>

#include "identity.h"

// L2CAP LE Credit Based sockets preserve SDU boundaries. The PLTR session
// consumes a byte stream, so bridge each SDU to a bounded local stream and
// split outgoing bytes at the negotiated outgoing MTU. The caller owns coc_fd.
int pltr_run_coc_stream_session(int coc_fd, PltrIdentityStore &store,
                                int stop_fd, std::size_t outgoing_mtu);

// Transport-only entry point for socketpair tests. Both sockets are owned by
// the caller. Returns when either peer closes or stop_fd becomes readable.
int pltr_bridge_coc_stream(int coc_fd, int stream_fd, int stop_fd,
                           std::size_t outgoing_mtu);
