/* Local public-status server: abstract AF_UNIX SOCK_STREAM, Linux only.
 *
 * Contract: docs/relay-drawing-handoff-contract.md revision
 * plank-drawing-handoff-v1+r3, section 8. Read-only. It publishes four public
 * fields and grants no mutation of any kind: no capture acquisition, no pairing
 * window, no tablet node open, no allowlist change, no configuration write. The
 * capture interlock name (src/capture_lease.hpp:9) is never touched.
 *
 * Only the socket plumbing and the SO_PEERCRED check live here; the serializer
 * and the literal validator are in the portable drawing_status.c so their tests
 * run on macOS (contract section 12.1).
 */
#pragma once

#include "drawing_status.h"

#include <cstdint>
#include <string>

class PltrDrawingStatusServer {
public:
    static constexpr const char *ProductionName = PLTR_DRAWING_STATUS_NAME;

    enum class BindResult { Bound, Busy, Error };

    /* Alternate names and accepted uids are for isolated tests; production uses
     * the default constructor, which hard-wires ProductionName and uid 0. A
     * test must build this struct itself: there is no default value for it, no
     * environment variable, no configuration key, no command-line flag and no
     * shipped compile-time define that can reach it (contract section 8.3). */
    struct IsolatedTestPolicy {
        std::string name;
        std::uint32_t accepted_uid;
    };

    PltrDrawingStatusServer() noexcept;
    explicit PltrDrawingStatusServer(const IsolatedTestPolicy &policy);
    ~PltrDrawingStatusServer();
    PltrDrawingStatusServer(const PltrDrawingStatusServer &) = delete;
    PltrDrawingStatusServer &operator=(const PltrDrawingStatusServer &) = delete;

    /* Snapshot the response once, before the name is bound. Nothing is read
     * from the identity store, the allowlist or any configuration per request.
     * Returns false when the listener could not be described, in which case the
     * server answers with the unavailable reason instead. */
    bool publishListener(const std::uint8_t public_key[32],
                         const char *bound_address,
                         std::uint16_t bound_port, bool bluetooth_available = false) noexcept;
    void publishUnavailable(const char *reason) noexcept;

    /* One bind attempt, never per request and never retried. */
    BindResult bind(int *error_number = nullptr) noexcept;

    /* Non-blocking: accepts, services and expires connections that are already
     * ready, then returns. Each connection is bounded by
     * PLTR_DRAWING_STATUS_CONNECTION_MS from its accept. */
    void service(std::uint64_t now_ms) noexcept;

    void close() noexcept;
    bool bound() const noexcept { return listen_fd_ >= 0; }
    const std::string &name() const noexcept { return name_; }
    std::uint32_t acceptedUid() const noexcept { return accepted_uid_; }

private:
    struct Connection {
        int fd = -1;
        std::uint64_t deadline = 0;
        std::size_t received = 0;
        std::size_t sent = 0;
        bool replying = false;
        char request[PLTR_DRAWING_STATUS_REQUEST_MAX];
        char reply[PLTR_DRAWING_STATUS_RESPONSE_MAX];
        std::size_t reply_length = 0;
    };

    void accepting(std::uint64_t now_ms) noexcept;
    void progress(Connection &connection, std::uint64_t now_ms) noexcept;
    void drop(Connection &connection) noexcept;

    std::string name_;
    std::uint32_t accepted_uid_;
    int listen_fd_ = -1;
    char response_[PLTR_DRAWING_STATUS_RESPONSE_MAX];
    std::size_t response_length_ = 0;
    char response_v2_[PLTR_DRAWING_STATUS_RESPONSE_MAX];
    std::size_t response_v2_length_ = 0;
    Connection connections_[PLTR_DRAWING_STATUS_BACKLOG];
};
