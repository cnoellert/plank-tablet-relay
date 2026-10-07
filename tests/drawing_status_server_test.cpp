/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Local public-status socket plumbing: abstract AF_UNIX, SO_PEERCRED, bind
 * ordering, bounded connections, restart, and the untouched capture lease.
 * Contract revision plank-drawing-handoff-v1+r3, section 8.
 *
 * Build and test invocation, frozen by contract section 12.2:
 *   cmake -S . -B <build>            # no CMAKE_BUILD_TYPE, so NDEBUG is unset
 *   cmake --build <build> --parallel
 *   ctest --test-dir <build> --output-on-failure
 *
 * Explicit failure checks only, never bare assert.
 *
 * Linux only: abstract addresses and SO_PEERCRED do not exist on macOS, so this
 * test is registered inside the Linux guard. The portable serializer, literal
 * and peer-policy checks live in tests/drawing_status_test.c, which runs on both.
 *
 * Every name used here carries this test's suffix, so it collides with neither
 * plank-tablet-capture-v1 nor the production status name.
 */
#include "capture_lease.hpp"
#include "drawing_status_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string test_name(const char *suffix) {
    return std::string("plank-tablet-drawing-status-selftest-") +
           std::to_string(getpid()) + "-" + suffix;
}

int connect_to(const std::string &name, int *error_number) {
    sockaddr_un address{};
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (error_number) *error_number = 0;
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        if (error_number) *error_number = errno;
        close(fd);
        return -1;
    }
    return fd;
}

/* Drives the server from this process: send, then service until the reply is
 * read or the budget expires. Returns the bytes received (0 means the server
 * closed without a reply). */
std::string exchange(PltrDrawingStatusServer &server, const std::string &name,
                     const std::string &request, bool *connected) {
    int error = 0;
    const int fd = connect_to(name, &error);
    if (connected) *connected = fd >= 0;
    if (fd < 0) return std::string();
    timeval budget{0, 750000};  // the managed client's 0.75 s total budget
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &budget, sizeof(budget));
    if (!request.empty())
        (void)send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string reply;
    const std::uint64_t deadline = now_ms() + 750;
    while (now_ms() < deadline) {
        server.service(now_ms());
        char buffer[4096];
        const ssize_t size = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size > 0) {
            reply.append(buffer, static_cast<std::size_t>(size));
            if (!reply.empty() && reply.back() == '\n') break;
            continue;
        }
        if (size == 0) break;  // server closed
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    close(fd);
    return reply;
}

const std::uint8_t kPublicKey[32] = {
    0xb4, 0x80, 0x5f, 0x94, 0x79, 0x54, 0xdd, 0xad, 0x86, 0x45, 0xc4,
    0xa8, 0x7d, 0x1b, 0xea, 0xd9, 0xbc, 0xd2, 0xac, 0xeb, 0x3c, 0xac,
    0xfa, 0x13, 0x3d, 0xcd, 0xf1, 0x0f, 0x10, 0x48, 0x19, 0x1c};

const char *kRequest = "{\"op\":\"drawing-status\",\"version\":1}\n";

PltrDrawingStatusServer::IsolatedTestPolicy own_uid_policy(const char *suffix) {
    // The accepted-uid policy is supplied by the test, explicitly. Production
    // uses the default constructor, which hard-wires uid 0 (contract 8.3).
    PltrDrawingStatusServer::IsolatedTestPolicy policy;
    policy.name = test_name(suffix);
    policy.accepted_uid = static_cast<std::uint32_t>(getuid());
    return policy;
}

void production_defaults_are_frozen() {
    PltrDrawingStatusServer production;
    check(production.acceptedUid() == 0u,
          "the production constructor hard-wires peer uid 0");
    check(production.name() == PltrDrawingStatusServer::ProductionName,
          "the production constructor uses ProductionName");
    check(production.name() == std::string("plank-tablet-drawing-status-v1"),
          "the production abstract name is frozen");
    check(production.name() != std::string(PltrCaptureLease::ProductionName),
          "the status name is not the capture interlock name");
    check(!production.bound(), "constructing the server binds nothing");
}

void a_bound_name_answers_the_one_operation() {
    const auto policy = own_uid_policy("ready");
    PltrDrawingStatusServer server(policy);
    check(server.publishListener(kPublicKey, "0.0.0.0", 28990),
          "the wildcard production bind is publishable");
    int error = 0;
    check(server.bind(&error) == PltrDrawingStatusServer::BindResult::Bound,
          "the isolated test name binds");
    bool connected = false;
    const std::string reply = exchange(server, policy.name, kRequest, &connected);
    check(connected, "an accepted peer connects");
    check(reply ==
          "{\"version\":1,\"ok\":true,\"supported\":true,\"state\":\"ready\","
          "\"listener\":{\"drawingIdentity\":"
          "\"b4805f947954ddad8645c4a87d1bead9bcd2aceb3cacfa133dcdf10f1048191c\","
          "\"drawingProtocol\":{\"name\":\"pltr-raw-hid\",\"version\":1,"
          "\"rawHID\":1,\"linkType\":2},"
          "\"boundAddress\":\"0.0.0.0\",\"boundPort\":28990}}\n",
          "the ready response carries public data only");
    check(reply.find("private") == std::string::npos &&
          reply.find("clients") == std::string::npos &&
          reply.find("/var/lib") == std::string::npos,
          "the response carries no private key, allowlist or path");
    check(reply.size() <= 4096, "the response is within its bound");
    const std::string v2_request = "{\"op\":\"drawing-status\",\"version\":2}\n";
    const auto before = exchange(server, policy.name, v2_request, &connected);
    check(before.find("\"version\":2,\"bluetooth\":false") == 1, "unbound Bluetooth is not advertised");
    check(server.publishListener(kPublicKey, "0.0.0.0", 28990, true), "Bluetooth capability snapshots");
    const auto after = exchange(server, policy.name, v2_request, &connected);
    check(after.find("\"version\":2,\"bluetooth\":true") == 1, "bound Bluetooth is advertised in V2");
    check(exchange(server, policy.name, kRequest, &connected) == reply, "V1 response is byte-identical");
}

void a_refused_peer_gets_no_reply() {
    // The policy accepts a uid this process does not have, which is exactly the
    // production condition for a non-root peer.
    PltrDrawingStatusServer::IsolatedTestPolicy policy;
    policy.name = test_name("refused");
    policy.accepted_uid = static_cast<std::uint32_t>(getuid()) + 1u;
    PltrDrawingStatusServer server(policy);
    (void)server.publishListener(kPublicKey, "192.0.2.10", 28990);
    check(server.bind() == PltrDrawingStatusServer::BindResult::Bound,
          "the refusing server binds");
    bool connected = false;
    const std::string reply = exchange(server, policy.name, kRequest, &connected);
    check(connected, "an abstract name has no access control, so connect succeeds");
    check(reply.empty(), "a peer outside the policy gets no reply at all");
}

void an_invalid_request_is_refused_without_side_effect() {
    const auto policy = own_uid_policy("invalid");
    PltrDrawingStatusServer server(policy);
    (void)server.publishListener(kPublicKey, "192.0.2.10", 28990);
    check(server.bind() == PltrDrawingStatusServer::BindResult::Bound, "binds");
    const std::string reply = exchange(
        server, policy.name, "{\"op\":\"capture\",\"version\":1}\n", nullptr);
    check(reply == "{\"version\":1,\"ok\":false,"
                   "\"error\":\"Invalid drawing status request.\"}\n",
          "an unknown operation is an error reply with no side effect");
    const std::string second = exchange(server, policy.name, kRequest, nullptr);
    check(second.find("\"state\":\"ready\"") != std::string::npos,
          "the interface still answers the one legal operation afterwards");
}

void an_unusable_bind_reports_unavailable() {
    const auto policy = own_uid_policy("unusable");
    PltrDrawingStatusServer server(policy);
    check(!server.publishListener(kPublicKey, "239.0.0.1", 28990),
          "a bind entry point 1 cannot describe is refused");
    check(server.bind() == PltrDrawingStatusServer::BindResult::Bound, "binds");
    const std::string reply = exchange(server, policy.name, kRequest, nullptr);
    check(reply == "{\"version\":1,\"ok\":true,\"supported\":true,"
                   "\"state\":\"unavailable\","
                   "\"reason\":\"listener.noUsableAddress\"}\n",
          "an undescribable listener answers unavailable, not malformed metadata");
}

void a_held_name_is_not_rebound() {
    const auto policy = own_uid_policy("collision");
    PltrDrawingStatusServer first(policy);
    check(first.bind() == PltrDrawingStatusServer::BindResult::Bound, "first binds");
    PltrDrawingStatusServer second(policy);
    int error = 0;
    check(second.bind(&error) == PltrDrawingStatusServer::BindResult::Busy,
          "a second bind of a held abstract name is Busy");
    check(error == EADDRINUSE, "the reported error is EADDRINUSE");
    check(!second.bound(), "the losing server has no listener");
}

void absence_and_restart_are_distinguishable() {
    const auto policy = own_uid_policy("restart");
    int error = 0;
    check(connect_to(policy.name, &error) < 0, "an unbound name does not connect");
    check(error == ECONNREFUSED || error == ENOENT,
          "an unbound abstract name fails immediately with ECONNREFUSED or ENOENT");
    {
        PltrDrawingStatusServer server(policy);
        (void)server.publishListener(kPublicKey, "192.0.2.10", 28990);
        check(server.bind() == PltrDrawingStatusServer::BindResult::Bound, "binds");
        check(exchange(server, policy.name, kRequest, nullptr).find(
                  "\"state\":\"ready\"") != std::string::npos, "answers");
        server.close();
        error = 0;
        check(connect_to(policy.name, &error) < 0,
              "the name disappears when the server closes: no stale file");
        check(error == ECONNREFUSED || error == ENOENT, "absence is immediate");
    }
    PltrDrawingStatusServer restarted(policy);
    (void)restarted.publishListener(kPublicKey, "192.0.2.10", 28990);
    check(restarted.bind() == PltrDrawingStatusServer::BindResult::Bound,
          "the name is free again after a restart");
    check(exchange(restarted, policy.name, kRequest, nullptr).find(
              "\"state\":\"ready\"") != std::string::npos,
          "the restarted server answers");
}

void a_silent_peer_is_dropped_at_its_deadline() {
    const auto policy = own_uid_policy("silent");
    PltrDrawingStatusServer server(policy);
    (void)server.publishListener(kPublicKey, "192.0.2.10", 28990);
    check(server.bind() == PltrDrawingStatusServer::BindResult::Bound, "binds");
    const int fd = connect_to(policy.name, nullptr);
    check(fd >= 0, "the silent peer connects");
    if (fd < 0) return;
    const std::uint64_t accepted = now_ms();
    server.service(accepted);
    // Service past the 500 ms per-connection budget without ever sending.
    bool closed = false;
    const std::uint64_t deadline = accepted + 1500;
    while (now_ms() < deadline) {
        server.service(now_ms());
        char buffer[16];
        const ssize_t size = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size == 0) { closed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(closed, "a connection that never sends is dropped at its deadline");
    close(fd);
    // Slots are returned, so the interface still answers.
    check(exchange(server, policy.name, kRequest, nullptr).find(
              "\"state\":\"ready\"") != std::string::npos,
          "the handler slot is reusable after a timed-out peer");
}

void the_capture_lease_is_untouched() {
    const std::string lease_name =
        std::string("plank-tablet-capture-selftest-") + std::to_string(getpid());
    PltrCaptureLease lease(lease_name);
    check(lease.acquire() == PltrCaptureLease::AcquireResult::Acquired,
          "the isolated capture lease is acquired");
    const auto policy = own_uid_policy("lease");
    PltrDrawingStatusServer server(policy);
    (void)server.publishListener(kPublicKey, "192.0.2.10", 28990);
    check(server.bind() == PltrDrawingStatusServer::BindResult::Bound, "binds");
    check(lease.held(), "the lease is held before a status request");
    const std::string reply = exchange(server, policy.name, kRequest, nullptr);
    check(reply.find("\"state\":\"ready\"") != std::string::npos, "answers");
    check(lease.held(),
          "serving a status request neither releases nor probes the capture lease");
    PltrCaptureLease competitor(lease_name);
    check(competitor.acquire() == PltrCaptureLease::AcquireResult::Busy,
          "the capture interlock still refuses a competitor");
    lease.release();
    check(!lease.held(), "release still works");
    PltrCaptureLease successor(lease_name);
    check(successor.acquire() == PltrCaptureLease::AcquireResult::Acquired,
          "the capture lease is reacquirable after release, as before");
    // The status name and the capture name are different addresses: binding one
    // never affects the other.
    check(policy.name != lease_name, "the two abstract names differ");
    check(exchange(server, policy.name, kRequest, nullptr).find(
              "\"state\":\"ready\"") != std::string::npos,
          "the status interface is unaffected by capture-lease traffic");
}
} // namespace

int main() {
    production_defaults_are_frozen();
    a_bound_name_answers_the_one_operation();
    a_refused_peer_gets_no_reply();
    an_invalid_request_is_refused_without_side_effect();
    an_unusable_bind_reports_unavailable();
    a_held_name_is_not_rebound();
    absence_and_restart_are_distinguishable();
    a_silent_peer_is_dropped_at_its_deadline();
    the_capture_lease_is_untouched();
    if (failures != 0) {
        std::fprintf(stderr, "%d drawing-status server check(s) failed\n", failures);
        return 1;
    }
    std::printf("drawing_status_server_test: all checks passed\n");
    return 0;
}
