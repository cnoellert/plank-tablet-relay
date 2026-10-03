// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Opaque authenticated raw drawing bytes only. This listener never authorizes
// keys or claims tablet capture. The ordinary session dispatcher does both.
class PltrBluetoothDrawingServer {
public:
    static constexpr const char *ProductionName = "plank-tablet-drawing-ble-v1";
    struct IsolatedTestPolicy { std::string name; std::uint32_t uid; };
    PltrBluetoothDrawingServer() : name_(ProductionName), uid_(0) {}
    explicit PltrBluetoothDrawingServer(IsolatedTestPolicy p)
        : name_(std::move(p.name)), uid_(p.uid) {}
    ~PltrBluetoothDrawingServer() { if (fd_ >= 0) ::close(fd_); }
    PltrBluetoothDrawingServer(const PltrBluetoothDrawingServer &) = delete;
    PltrBluetoothDrawingServer &operator=(const PltrBluetoothDrawingServer &) = delete;
    int fd() const { return fd_; }
    bool bind() {
        sockaddr_un address{};
        if (fd_ >= 0 || name_.empty() || name_.size() >= sizeof(address.sun_path)) return false;
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path + 1, name_.data(), name_.size());
        int candidate = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (candidate < 0) return false;
        if (::bind(candidate, reinterpret_cast<sockaddr *>(&address),
                   offsetof(sockaddr_un, sun_path) + 1 + name_.size()) != 0 ||
            listen(candidate, 1) != 0) { ::close(candidate); return false; }
        fd_ = candidate;
        return true;
    }
    int accept() const {
        int peer = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer < 0) return -1;
        ucred credentials{};
        socklen_t size = sizeof(credentials);
        if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
            size != sizeof(credentials) || credentials.uid != uid_) {
            ::close(peer); errno = EACCES; return -1;
        }
        return peer;
    }
private:
    std::string name_;
    std::uint32_t uid_;
    int fd_ = -1;
};
