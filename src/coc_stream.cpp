#include "coc_stream.hpp"
#include "stream_session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr std::size_t kMaxSdu = 65535;
constexpr std::size_t kMaxOutgoingMtu = 4096;

bool send_bounded(int fd, const std::uint8_t *data, std::size_t size,
                  int stop_fd, bool packet) {
    while (size) {
        pollfd fds[2] = {{fd, POLLOUT, 0}, {stop_fd, POLLIN, 0}};
        const int result = poll(fds, stop_fd >= 0 ? 2 : 1, 1000);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) continue; // backpressure is not a broken link
        if (result < 0 || (stop_fd >= 0 && fds[1].revents) ||
            (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        const ssize_t sent = send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (sent <= 0 || (packet && static_cast<std::size_t>(sent) != size))
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool pump_packets_to_stream(int coc_fd, int stream_fd, int stop_fd) {
    std::array<std::uint8_t, kMaxSdu> packet{};
    while (true) {
        pollfd fds[2] = {{coc_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
        const int result = poll(fds, stop_fd >= 0 ? 2 : 1, 1000);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (stop_fd >= 0 && fds[1].revents) ||
            (fds[0].revents & (POLLERR | POLLNVAL))) return false;
        if (!(fds[0].revents & POLLIN)) {
            if (fds[0].revents & POLLHUP) return false;
            continue;
        }
        const ssize_t count = recv(coc_fd, packet.data(), packet.size(),
                                   MSG_DONTWAIT | MSG_TRUNC);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0 || static_cast<std::size_t>(count) > packet.size())
            return false; // never forward a truncated SDU
        if (!send_bounded(stream_fd, packet.data(), static_cast<std::size_t>(count),
                          stop_fd, false)) return false;
    }
}

bool pump_stream_to_packets(int coc_fd, int stream_fd, int stop_fd,
                            std::size_t outgoing_mtu) {
    std::array<std::uint8_t, kMaxOutgoingMtu> bytes{};
    while (true) {
        pollfd fds[2] = {{stream_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
        const int result = poll(fds, stop_fd >= 0 ? 2 : 1, 1000);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 || (stop_fd >= 0 && fds[1].revents) ||
            (fds[0].revents & (POLLERR | POLLNVAL))) return false;
        if (!(fds[0].revents & POLLIN)) {
            if (fds[0].revents & POLLHUP) return false;
            continue;
        }
        const ssize_t count = recv(stream_fd, bytes.data(), outgoing_mtu,
                                   MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return false;
        if (!send_bounded(coc_fd, bytes.data(), static_cast<std::size_t>(count),
                          stop_fd, true)) return false;
    }
}
} // namespace

int pltr_bridge_coc_stream(int coc_fd, int stream_fd, int stop_fd,
                           std::size_t outgoing_mtu) {
    if (coc_fd < 0 || stream_fd < 0 || coc_fd == stream_fd ||
        (stop_fd >= 0 && (stop_fd == coc_fd || stop_fd == stream_fd)) ||
        outgoing_mtu < 23 || outgoing_mtu > kMaxSdu) return -1;
    // The peer may advertise an MTU larger than our bounded scratch buffer.
    // Sending smaller SDUs is valid; rejecting the connection is not.
    outgoing_mtu = std::min(outgoing_mtu, kMaxOutgoingMtu);
    // Each direction blocks on the kernel socket instead of queuing packets in
    // user space. Closing either half wakes the other direction on teardown.
    std::thread inbound([&] {
        pump_packets_to_stream(coc_fd, stream_fd, stop_fd);
        shutdown(stream_fd, SHUT_RDWR);
        shutdown(coc_fd, SHUT_RD);
    });
    pump_stream_to_packets(coc_fd, stream_fd, stop_fd, outgoing_mtu);
    shutdown(coc_fd, SHUT_RDWR);
    shutdown(stream_fd, SHUT_RDWR);
    inbound.join();
    return 0;
}

int pltr_run_coc_stream_session(int coc_fd, PltrIdentityStore &store,
                                int stop_fd, std::size_t outgoing_mtu) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return -1;
    std::thread bridge([&] {
        pltr_bridge_coc_stream(coc_fd, pair[1], stop_fd, outgoing_mtu);
    });
    const int result = pltr_run_authenticated_stream_session(pair[0], store,
                                                               stop_fd, 1);
    shutdown(pair[0], SHUT_RDWR);
    shutdown(coc_fd, SHUT_RDWR);
    bridge.join();
    close(pair[0]);
    close(pair[1]);
    return result;
}
