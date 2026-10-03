#include "capture_lease.hpp"
#include "drawing_status_server.hpp"
#include "identity.h"
#include "pad.h"
#include "pair_budget.h"
#include "pairing.h"
#include "protocol.h"
#include "tcp_pair_session.hpp"
#include "tcp_session.hpp"
#include "dnssd.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t stopping = 0;
int signal_write_fd = -1;

std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void stop_signal(int) {
    stopping = 1;
    if (signal_write_fd >= 0) {
        const std::uint8_t byte = 1;
        if (write(signal_write_fd, &byte, 1) != 1) return;
    }
}

bool inspect_open(int fd, std::uint8_t &mode) {
    timeval deadline{5, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &deadline, sizeof(deadline)) != 0) return false;
    std::uint8_t record[2 + PLTR_HEADER_SIZE + 1]{};
    const ssize_t size = recv(fd, record, sizeof(record),
                               MSG_PEEK | MSG_WAITALL);
    deadline = {0, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                      &deadline, sizeof(deadline));
    PltrFrame frame{};
    if (size != static_cast<ssize_t>(sizeof(record)) ||
        pltr_decode_record(record, sizeof(record), PLTR_CLIENT_TO_RELAY,
                            PLTR_PRE_AUTH, 1, &frame) != 0 ||
        frame.type != PLTR_OPEN) return false;
    mode = frame.payload[0];
    return true;
}

int listener(const char *address, std::uint16_t port, sockaddr_in *bound) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &target.sin_addr) != 1) return -1;
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (bind(fd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    // The public status interface reports what the listener is actually bound
    // to, observed from the socket, never re-derived configuration (contract
    // section 9). An operator-owned EnvironmentFile can override the unit
    // default, so the two can differ.
    if (bound != nullptr) {
        sockaddr_in observed{};
        socklen_t length = sizeof(observed);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&observed), &length) == 0 &&
            length == sizeof(observed) && observed.sin_family == AF_INET) {
            *bound = observed;
        } else {
            *bound = target;
        }
    }
    return fd;
}

// Publishes the public listener metadata on the abstract status name before the
// drawing listener accepts its first connection. One bind attempt; EADDRINUSE is
// logged and drawing traffic continues without a status interface (contract
// section 8.4). Absence never opens enrollment and never touches the capture
// lease.
void publish_drawing_status(PltrDrawingStatusServer &status,
                           const PltrIdentityStore &store,
                           const sockaddr_in &bound) {
    char text[INET_ADDRSTRLEN] = {0};
    const std::uint16_t port = ntohs(bound.sin_port);
    if (inet_ntop(AF_INET, &bound.sin_addr, text, sizeof(text)) == nullptr ||
        !status.publishListener(store.public_key, text, port)) {
        std::fputs("Drawing handoff status unavailable: the drawing listener "
                   "address cannot be published\n", stderr);
    }
    int error = 0;
    switch (status.bind(&error)) {
    case PltrDrawingStatusServer::BindResult::Bound:
        break;
    case PltrDrawingStatusServer::BindResult::Busy:
        std::fputs("Drawing handoff status name already bound (EADDRINUSE); "
                   "serving drawing traffic without it\n", stderr);
        break;
    case PltrDrawingStatusServer::BindResult::Error:
        std::fprintf(stderr, "Drawing handoff status name unavailable (%s); "
                     "serving drawing traffic without it\n",
                     std::strerror(error));
        break;
    }
}

bool acquire_pairing_lease(PltrCaptureLease &lease) {
    int error = 0;
    const auto result = lease.acquire(&error);
    if (result == PltrCaptureLease::AcquireResult::Acquired) return true;
    if (result == PltrCaptureLease::AcquireResult::Busy) {
        std::fputs("Wacom pairing unavailable: another tablet service owns "
                   "capture\n", stderr);
    } else {
        std::fprintf(stderr, "Wacom pairing lease unavailable (%s)\n",
                     std::strerror(error));
    }
    return false;
}

void usage() {
    std::fprintf(stderr,
                 "Usage: plank-tablet-relay serve|pair --state-dir DIR "
                 "[--bind IPv4] [--port 1..65535]\n"
                 "Defaults: bind 127.0.0.1, port 28990. Pairing reads the "
                 "one Wacom Pad over USB or Bluetooth.\n");
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        usage();
        return 2;
    }
    const bool pairing_mode = std::strcmp(argv[1], "pair") == 0;
    if (!pairing_mode && std::strcmp(argv[1], "serve") != 0) {
        usage();
        return 2;
    }
    const char *state_dir = nullptr;
    const char *bind_address = "127.0.0.1";
    std::uint16_t port = 28990;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (std::strcmp(argv[i], "--state-dir") == 0) {
            state_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--bind") == 0) {
            bind_address = argv[i + 1];
        } else if (std::strcmp(argv[i], "--port") == 0) {
            char *end = nullptr;
            errno = 0;
            const unsigned long value = std::strtoul(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                value == 0 || value > UINT16_MAX) {
                usage();
                return 2;
            }
            port = static_cast<std::uint16_t>(value);
        } else {
            usage();
            return 2;
        }
    }
    if (state_dir == nullptr) {
        usage();
        return 2;
    }
    PltrIdentityStore store{};
    if (pltr_identity_store_open(&store, state_dir) != 0) {
        std::fputs("Relay identity store unavailable or unsafe\n", stderr);
        return 1;
    }
    PltrPad pad{};
    pad.fd = -1;
    PltrCaptureLease pairing_lease;
    PltrPairing pairing{};
    if (pairing_mode && !acquire_pairing_lease(pairing_lease)) {
        pltr_identity_store_close(&store);
        return 1;
    }
    if (pltr_pairing_init(&pairing, &store,
                          reinterpret_cast<const std::uint8_t *>("NUC"),
                          3) != 0 ||
        (pairing_mode && pltr_pad_open(&pad, 0x056a, 0) != 0)) {
        std::fputs("Wacom Pad unavailable for pairing\n", stderr);
        pltr_pad_close(&pad);
        pairing_lease.release();
        pltr_identity_store_close(&store);
        return 1;
    }
    if (!pairing_mode) (void)pltr_pad_open(&pad, 0x056a, 0);
    sockaddr_in bound{};
    const int server = listener(bind_address, port, &bound);
    int signal_pipe[2];
    if (server < 0 || pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        std::fputs("Relay listener could not start\n", stderr);
        if (server >= 0) close(server);
        pltr_pairing_clear(&pairing);
        pltr_pad_close(&pad);
        pairing_lease.release();
        pltr_identity_store_close(&store);
        return 1;
    }
    signal_write_fd = signal_pipe[1];
    struct sigaction action{};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    PltrDrawingStatusServer drawing_status;
    // The pairing invocation is a separate short-lived process; the installed
    // unit runs "serve", and only that instance publishes the status name.
    if (!pairing_mode) publish_drawing_status(drawing_status, store, bound);
    PltrDnsSd publisher(store.public_key, port);
    if (!publisher.start())
        std::fputs("Local Relay discovery unavailable; manual address still works\n", stderr);
    if (pairing_mode) {
        const std::time_t wall_now = std::time(nullptr);
        if (wall_now < 0 ||
            pltr_pair_budget_reserve(&store,
                static_cast<std::uint64_t>(wall_now)) != 0 ||
            pltr_pairing_open(&pairing, monotonic_ms(), 0) != 0) {
            std::fputs("Pairing unavailable or locked; no window opened\n", stderr);
            stopping = 1;
        } else {
            publisher.setPairing(true);
        }
    }
    if (!stopping) {
        std::printf("Relay %s listening on %s:%u\n",
                     pairing_mode ? "pairing" : "session", bind_address, port);
        std::fflush(stdout);
    }
    int result = pairing_mode ? 1 : 0;
    std::uint64_t next_pad_retry = 0;
    while (!stopping) {
        const auto now = monotonic_ms();
        drawing_status.service(now);
        if (pltr_pairing_tick(&pairing, now) < 0) break;
        if (pairing.stage != PLTR_PAIR_WINDOW && publisher.pairing())
            publisher.setPairing(false);
        if (!pairing_mode && pairing.stage != PLTR_PAIR_WINDOW)
            pairing_lease.release();
        if (pairing_mode && pairing.stage != PLTR_PAIR_WINDOW) break;
        if (!pairing_mode && pad.fd < 0 && now >= next_pad_retry) {
            (void)pltr_pad_open(&pad, 0x056a, 0);
            next_pad_retry = now + 2000;
        }
        if (!pairing_mode && pad.fd >= 0) {
            const int chord = pltr_pad_chord(&pad, now);
            if (chord == 1 && pairing.stage == PLTR_PAIR_CLOSED) {
                if (!acquire_pairing_lease(pairing_lease)) continue;
                const std::time_t wall_now = std::time(nullptr);
                if (wall_now >= 0 &&
                    pltr_pair_budget_reserve(&store,
                        static_cast<std::uint64_t>(wall_now)) == 0 &&
                    pltr_pairing_open(&pairing, now, 0) == 0) {
                    publisher.setPairing(true);
                    std::fputs("Physical Wacom pairing window opened\n", stderr);
                } else {
                    pairing_lease.release();
                    std::fputs("Physical pairing refused or locked\n", stderr);
                }
            }
        }
        pollfd fds[3] = {{server, POLLIN, 0}, {signal_pipe[0], POLLIN, 0},
                         {pad.fd, POLLIN, 0}};
        const int ready = poll(fds, pad.fd >= 0 ? 3 : 2, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || fds[1].revents != 0) break;
        if (pad.fd >= 0 && fds[2].revents != 0) {
            if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                pltr_pad_close(&pad);
            } else if (fds[2].revents & POLLIN) {
                std::uint8_t key = 0;
                if (pltr_pad_read(&pad, monotonic_ms(), &key) < 0)
                    pltr_pad_close(&pad);
            }
        }
        if (!(fds[0].revents & POLLIN)) continue;
        const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        const int enabled = 1;
        (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                         &enabled, sizeof(enabled));
        const int dscp_ef = 46 << 2;
        (void)setsockopt(client, IPPROTO_IP, IP_TOS,
                         &dscp_ef, sizeof(dscp_ef));
        std::uint8_t mode = 0;
        if (inspect_open(client, mode)) {
            if (mode == 2 && pairing.stage == PLTR_PAIR_WINDOW && pad.fd >= 0) {
                const int session_result = pltr_run_tcp_pair_session(
                    client, pairing, pad, signal_pipe[0]);
                const bool budget_failed = session_result == 0 &&
                    pltr_pair_budget_succeeded(&store) != 0;
                if (budget_failed) {
                    std::fputs("Pairing succeeded but budget reset failed\n", stderr);
                }
                result = session_result == 0 && !budget_failed ? 0 : 1;
                publisher.setPairing(false);
                if (pairing_mode || budget_failed) { close(client); break; }
                pltr_pairing_clear(&pairing);
                if (pltr_pairing_init(&pairing, &store,
                      reinterpret_cast<const std::uint8_t *>("NUC"), 3) != 0) {
                    close(client);
                    break;
                }
                result = 0;
            } else if (mode == 1 && !pairing_mode) {
                if (pairing.stage == PLTR_PAIR_WINDOW) {
                    publisher.setPairing(false);
                    pltr_pairing_clear(&pairing);
                    if (pltr_pairing_init(&pairing, &store,
                          reinterpret_cast<const std::uint8_t *>("NUC"), 3) != 0) {
                        close(client);
                        break;
                    }
                }
                // The worker claims the Pad exclusively during a session.
                pltr_pad_close(&pad);
                pairing_lease.release();
                (void)pltr_run_tcp_session(client, store, signal_pipe[0]);
                if (!stopping) (void)pltr_pad_open(&pad, 0x056a, 0);
            }
        }
        close(client);
    }
    close(server);
    drawing_status.close();
    publisher.stop();
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    signal_write_fd = -1;
    pltr_pairing_clear(&pairing);
    pltr_pad_close(&pad);
    pairing_lease.release();
    pltr_identity_store_close(&store);
    return result;
}
