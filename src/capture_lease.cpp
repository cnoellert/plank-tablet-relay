#include "capture_lease.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

PltrCaptureLease::PltrCaptureLease(std::string name) : name_(std::move(name)) {}
PltrCaptureLease::~PltrCaptureLease() { release(); }

PltrCaptureLease::AcquireResult PltrCaptureLease::acquire(
    int *error_number) noexcept {
    if (error_number) *error_number = 0;
    if (fd_ >= 0) return AcquireResult::Acquired;
    sockaddr_un address{};
    if (name_.empty() || name_.size() > sizeof(address.sun_path) - 1) {
        if (error_number) *error_number = EINVAL;
        return AcquireResult::Error;
    }
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error_number) *error_number = errno;
        return AcquireResult::Error;
    }
    address.sun_family = AF_UNIX;
    // Abstract address bytes are exactly NUL + name, without a trailing NUL.
    std::memcpy(address.sun_path + 1, name_.data(), name_.size());
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name_.size());
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0) {
        const int error = errno;
        close(fd);
        if (error_number) *error_number = error;
        return error == EADDRINUSE ? AcquireResult::Busy : AcquireResult::Error;
    }
    fd_ = fd;
    return AcquireResult::Acquired;
}

void PltrCaptureLease::release() noexcept {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}
