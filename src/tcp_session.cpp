#include "tcp_session.hpp"
#include "stream_session.hpp"

int pltr_run_tcp_session(int socket_fd, PltrIdentityStore &store, int stop_fd) {
    return pltr_run_authenticated_stream_session(socket_fd, store, stop_fd, 2);
}
