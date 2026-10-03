#include "tcp_session.hpp"

#include "link.h"
#include "session_dispatcher.hpp"
#include "tcp_io.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t monotonic_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

void write_le64(std::uint8_t *out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

bool send_frame(PltrLink &link, int fd, std::uint16_t type,
                const std::uint8_t *payload, std::size_t payload_size) {
    std::array<std::uint8_t, 2 + PLTR_MAX_RECORD_BODY_SIZE> output{};
    std::size_t written = 0;
    return pltr_link_send(&link, type, payload, payload_size,
                          output.data(), output.size(), &written) == 0 &&
           pltr_send_all(fd, output.data(), written);
}

bool send_initial_status(PltrLink &link, int fd) {
    const std::uint8_t status[] = {0, 0, 0, 0, 0, 0, 1, 0};
    return send_frame(link, fd, PLTR_STATUS, status, sizeof(status));
}

bool send_worker_status(PltrLink &link, int fd, const PltrWorkerStatus &current) {
    const std::uint8_t status[] = {
        current.state,
        static_cast<std::uint8_t>(current.vendor),
        static_cast<std::uint8_t>(current.vendor >> 8),
        static_cast<std::uint8_t>(current.product),
        static_cast<std::uint8_t>(current.product >> 8),
        current.interface_count, current.transport, 0 // empty HID name
    };
    return send_frame(link, fd, PLTR_STATUS, status, sizeof(status));
}
} // namespace

int pltr_run_stream_session(int socket_fd, PltrIdentityStore &store, int stop_fd,
                            unsigned link_type, std::string capture_lease_name) {
    if ((link_type != 1 && link_type != 2) || socket_fd < 0 || store.directory_fd < 0 ||
        (stop_fd >= 0 && stop_fd == socket_fd)) return -1;
    PltrLink link{};
    if (pltr_link_init(&link, PLTR_NOISE_RESPONDER, store.private_key,
                       nullptr, pltr_identity_store_approve, &store, link_type) != 0)
        return -1;
    const int wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0) {
        pltr_link_clear(&link);
        return -1;
    }
    int result = -1;
    {
        PltrSessionDispatcher dispatcher([wake_fd] {
            const std::uint64_t one = 1;
            if (write(wake_fd, &one, sizeof(one)) !=
                static_cast<ssize_t>(sizeof(one))) return;
        }, [&store] {
            std::uint16_t generation = 0;
            return pltr_identity_store_next_generation(&store, &generation) == 0 ?
                generation : std::uint16_t{0};
        }, std::move(capture_lease_name));
        auto last_receive = Clock::now();
        auto last_ping = last_receive;
        bool hello_seen = false;
        std::uint64_t sent_status_epoch = 0;
        std::array<std::uint8_t, 4096> input{};
        std::array<std::uint8_t, 2 * (2 + PLTR_MAX_RECORD_BODY_SIZE)> reply{};
        std::array<std::uint8_t, 2 + PLTR_MAX_RECORD_BODY_SIZE> output{};
        while (true) {
            const auto now = Clock::now();
            if (now - last_receive > (hello_seen ? std::chrono::seconds(3) :
                                                   std::chrono::seconds(10))) break;
            if (dispatcher.failed()) break;
            if (hello_seen && now - last_ping >= std::chrono::seconds(1)) {
                std::uint8_t ping[16] = {};
                write_le64(ping, monotonic_us());
                write_le64(ping + 8, monotonic_us());
                if (!send_frame(link, socket_fd, PLTR_PING, ping, sizeof(ping))) break;
                last_ping = now;
            }
            if (hello_seen && link.relay_session.stage == PLTR_RELAY_READY) {
                const PltrWorkerStatus current_status = dispatcher.status();
                if (current_status.epoch != sent_status_epoch) {
                    if (!send_worker_status(link, socket_fd, current_status)) break;
                    sent_status_epoch = current_status.epoch;
                }
                bool output_failed = false;
                for (unsigned i = 0; i < 256; ++i) {
                    std::size_t written = 0;
                    const int next = dispatcher.next(link, output.data(),
                                                      output.size(), &written);
                    if (next == 0) break;
                    if (next < 0 || !pltr_send_all(socket_fd, output.data(), written)) {
                        output_failed = true;
                        break;
                    }
                }
                if (output_failed) break;
            }
            pollfd fds[3] = {{socket_fd, POLLIN, 0}, {wake_fd, POLLIN, 0},
                             {stop_fd, POLLIN, 0}};
            const nfds_t count = stop_fd >= 0 ? 3 : 2;
            const int ready = poll(fds, count, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (stop_fd >= 0 && fds[2].revents != 0) ||
                (fds[0].revents & (POLLERR | POLLNVAL)) ||
                ((fds[0].revents & POLLHUP) && !(fds[0].revents & POLLIN))) break;
            if (fds[1].revents & POLLIN) {
                std::uint64_t count_value;
                if (read(wake_fd, &count_value, sizeof(count_value)) !=
                    static_cast<ssize_t>(sizeof(count_value))) break;
            }
            if (!(fds[0].revents & POLLIN)) continue;
            const ssize_t received = recv(socket_fd, input.data(), input.size(),
                                           MSG_DONTWAIT);
            if (received < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (received <= 0) break;
            last_receive = Clock::now();
            std::size_t offset = 0;
            bool invalid = false;
            while (offset < static_cast<std::size_t>(received)) {
                std::size_t consumed = 0, reply_size = 0;
                PltrFrame frame{};
                const int handled = pltr_link_receive(
                    &link, input.data() + offset,
                    static_cast<std::size_t>(received) - offset, &consumed,
                    reply.data(), reply.size(), &reply_size, &frame);
                if (handled < 0 || consumed == 0 ||
                    (reply_size && !pltr_send_all(socket_fd, reply.data(), reply_size))) {
                    invalid = true;
                    break;
                }
                offset += consumed;
                if (handled == 0) continue;
                if (!hello_seen && link.stage == PLTR_LINK_READY) {
                    hello_seen = true;
                    last_ping = Clock::now();
                    if (!send_initial_status(link, socket_fd)) {
                        invalid = true;
                        break;
                    }
                }
                if (frame.type == PLTR_PING) {
                    std::uint8_t pong[32];
                    std::memcpy(pong, frame.payload, 16);
                    write_le64(pong + 16, monotonic_us());
                    write_le64(pong + 24, monotonic_us());
                    if (!send_frame(link, socket_fd, PLTR_PONG,
                                    pong, sizeof(pong))) invalid = true;
                } else if (frame.type != 0 && !dispatcher.accept(frame)) {
                    invalid = true;
                }
                if (invalid) break;
                if (link.stage == PLTR_LINK_CLOSED) {
                    result = 0;
                    break;
                }
            }
            if (invalid || result == 0) break;
        }
        dispatcher.close();
    }
    close(wake_fd);
    pltr_link_clear(&link);
    return result;
}

int pltr_run_tcp_session(int fd, PltrIdentityStore &store, int stop_fd,
                         std::string lease) {
    return pltr_run_stream_session(fd, store, stop_fd, 2, std::move(lease));
}
