/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See drawing_status_server.hpp. Linux only: abstract AF_UNIX addresses. */
#include "drawing_status_server.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr const char *kInvalidRequest = "Invalid drawing status request.";
}

PltrDrawingStatusServer::PltrDrawingStatusServer() noexcept
    : name_(ProductionName), accepted_uid_(PLTR_DRAWING_STATUS_ACCEPTED_UID) {
    publishUnavailable("service.absent");
}

PltrDrawingStatusServer::PltrDrawingStatusServer(const IsolatedTestPolicy &policy)
    : name_(policy.name), accepted_uid_(policy.accepted_uid) {
    publishUnavailable("service.absent");
}

PltrDrawingStatusServer::~PltrDrawingStatusServer() { close(); }

bool PltrDrawingStatusServer::publishListener(const std::uint8_t public_key[32],
                                             const char *bound_address,
                                             std::uint16_t bound_port, bool bluetooth_available) noexcept {
    const int written = pltr_drawing_status_ready(public_key, bound_address,
                                                  bound_port, response_,
                                                  sizeof(response_));
    if (written <= 0) {
        /* The listener is bound to something entry point 1 cannot describe. Say
         * so rather than emitting a field no consumer may accept. */
        publishUnavailable("listener.noUsableAddress");
        return false;
    }
    response_length_ = static_cast<std::size_t>(written);
    // V1 remains byte-identical. Both replies are snapshotted at startup.
    const int v2 = std::snprintf(response_v2_, sizeof(response_v2_),
        "{\"version\":2,\"bluetooth\":%s,%s", bluetooth_available ? "true" : "false",
        response_ + std::strlen("{\"version\":1,"));
    response_v2_length_ = v2 > 0 && static_cast<std::size_t>(v2) < sizeof(response_v2_) ?
        static_cast<std::size_t>(v2) : 0;
    return response_v2_length_ != 0;
}

void PltrDrawingStatusServer::publishUnavailable(const char *reason) noexcept {
    const int written = pltr_drawing_status_unavailable(reason, response_,
                                                         sizeof(response_));
    if (written <= 0) {
        const int fallback = pltr_drawing_status_unavailable(
            "service.invalid", response_, sizeof(response_));
        response_length_ = fallback > 0 ? static_cast<std::size_t>(fallback) : 0;
        std::memcpy(response_v2_, response_, response_length_);
        response_v2_length_ = response_length_;
        return;
    }
    response_length_ = static_cast<std::size_t>(written);
    std::memcpy(response_v2_, response_, response_length_);
    response_v2_length_ = response_length_;
}

PltrDrawingStatusServer::BindResult PltrDrawingStatusServer::bind(
    int *error_number) noexcept {
    if (error_number) *error_number = 0;
    if (listen_fd_ >= 0) return BindResult::Bound;
    sockaddr_un address{};
    if (name_.empty() || name_.size() > sizeof(address.sun_path) - 1) {
        if (error_number) *error_number = EINVAL;
        return BindResult::Error;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error_number) *error_number = errno;
        return BindResult::Error;
    }
    address.sun_family = AF_UNIX;
    // Abstract address bytes are exactly NUL + name, without a trailing NUL,
    // matching src/capture_lease.cpp:28-32.
    std::memcpy(address.sun_path + 1, name_.data(), name_.size());
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name_.size());
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0 ||
        listen(fd, PLTR_DRAWING_STATUS_BACKLOG) != 0) {
        const int error = errno;
        ::close(fd);
        if (error_number) *error_number = error;
        return error == EADDRINUSE ? BindResult::Busy : BindResult::Error;
    }
    listen_fd_ = fd;
    return BindResult::Bound;
}

void PltrDrawingStatusServer::drop(Connection &connection) noexcept {
    if (connection.fd >= 0) ::close(connection.fd);
    connection.fd = -1;
    connection.received = connection.sent = connection.reply_length = 0;
    connection.replying = false;
    connection.deadline = 0;
}

void PltrDrawingStatusServer::accepting(std::uint64_t now_ms) noexcept {
    for (auto &slot : connections_) {
        if (slot.fd >= 0) continue;
        const int fd = accept4(listen_fd_, nullptr, nullptr,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        ucred peer{};
        socklen_t peer_length = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) != 0 ||
            peer_length != sizeof(peer) ||
            !pltr_drawing_status_peer_accepted(
                static_cast<std::uint32_t>(peer.uid), accepted_uid_)) {
            // Contract section 8.3: close with no reply, no error body, no
            // diagnostic that reveals whether the service exists, and nothing
            // logged from the peer's input.
            ::close(fd);
            continue;
        }
        slot.fd = fd;
        slot.deadline = now_ms + PLTR_DRAWING_STATUS_CONNECTION_MS;
        slot.received = slot.sent = slot.reply_length = 0;
        slot.replying = false;
    }
}

void PltrDrawingStatusServer::progress(Connection &connection,
                                       std::uint64_t now_ms) noexcept {
    if (!connection.replying) {
        while (connection.received < sizeof(connection.request)) {
            const ssize_t size = recv(connection.fd,
                                      connection.request + connection.received,
                                      sizeof(connection.request) - connection.received,
                                      0);
            if (size == 0) { drop(connection); return; }
            if (size < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                drop(connection);
                return;
            }
            connection.received += static_cast<std::size_t>(size);
            if (std::memchr(connection.request, '\n', connection.received) != nullptr)
                break;
        }
        if (std::memchr(connection.request, '\n', connection.received) == nullptr) {
            // Over the request bound without a terminator.
            const int written = pltr_drawing_status_error(
                kInvalidRequest, connection.reply, sizeof(connection.reply));
            connection.reply_length = written > 0 ? static_cast<std::size_t>(written) : 0;
            connection.replying = true;
            if (connection.reply_length == 0) { drop(connection); return; }
        } else if (const int version = pltr_drawing_status_request_version(
                       connection.request, connection.received)) {
            const char *response = version == 2 ? response_v2_ : response_;
            connection.reply_length = version == 2 ? response_v2_length_ : response_length_;
            if (connection.reply_length == 0) { drop(connection); return; }
            std::memcpy(connection.reply, response, connection.reply_length);
            connection.replying = true;
        } else {
            const int written = pltr_drawing_status_error(
                kInvalidRequest, connection.reply, sizeof(connection.reply));
            connection.reply_length = written > 0 ? static_cast<std::size_t>(written) : 0;
            connection.replying = true;
            if (connection.reply_length == 0) { drop(connection); return; }
        }
    }
    while (connection.sent < connection.reply_length) {
        const ssize_t size = send(connection.fd, connection.reply + connection.sent,
                                  connection.reply_length - connection.sent,
                                  MSG_NOSIGNAL);
        if (size <= 0) {
            if (size < 0 && errno == EINTR) continue;
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            drop(connection);
            return;
        }
        connection.sent += static_cast<std::size_t>(size);
    }
    (void)now_ms;
    drop(connection);
}

void PltrDrawingStatusServer::service(std::uint64_t now_ms) noexcept {
    if (listen_fd_ < 0) return;
    pollfd fds[1 + PLTR_DRAWING_STATUS_BACKLOG]{};
    int count = 0;
    fds[count].fd = listen_fd_;
    fds[count].events = POLLIN;
    ++count;
    for (auto &slot : connections_) {
        if (slot.fd < 0) continue;
        fds[count].fd = slot.fd;
        fds[count].events = static_cast<short>(slot.replying ? POLLOUT : POLLIN);
        ++count;
    }
    if (poll(fds, static_cast<nfds_t>(count), 0) < 0) {
        if (errno != EINTR) return;
    }
    if (fds[0].revents & POLLIN) accepting(now_ms);
    for (auto &slot : connections_) {
        if (slot.fd < 0) continue;
        if (now_ms >= slot.deadline) { drop(slot); continue; }
        short revents = 0;
        for (int index = 1; index < count; ++index)
            if (fds[index].fd == slot.fd) revents = fds[index].revents;
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!(revents & (POLLIN | POLLOUT))) { drop(slot); continue; }
        }
        if (revents & (POLLIN | POLLOUT)) progress(slot, now_ms);
    }
}

void PltrDrawingStatusServer::close() noexcept {
    for (auto &slot : connections_) drop(slot);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
}
