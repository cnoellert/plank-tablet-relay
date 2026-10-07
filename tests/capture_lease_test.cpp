#include "capture_lease.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    assert(std::string(PltrCaptureLease::ProductionName) ==
           "plank-tablet-capture-v1");
    const std::string name = "plank-capture-lease-test-" + std::to_string(getpid());
    using Result = PltrCaptureLease::AcquireResult;

    PltrCaptureLease first(name), second(name);
    assert(first.acquire() == Result::Acquired);
    assert(first.held());
    assert(second.acquire() == Result::Busy);
    assert(!second.held());
    first.release();
    assert(second.acquire() == Result::Acquired);
    second.release();

    // A separately implemented peer must contend on exactly NUL + name.
    const int peer = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(peer >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    const auto length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name.size());
    assert(bind(peer, reinterpret_cast<sockaddr *>(&address), length) == 0);
    assert(first.acquire() == Result::Busy);
    assert(close(peer) == 0);
    assert(first.acquire() == Result::Acquired);
    first.release();

    // _exit skips C++ destructors: kernel fd cleanup must free the lease.
    int ready[2], done[2];
    assert(pipe(ready) == 0 && pipe(done) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        close(done[1]);
        PltrCaptureLease child_lease(name);
        if (child_lease.acquire() != Result::Acquired ||
            write(ready[1], "1", 1) != 1) _exit(1);
        char signal;
        if (read(done[0], &signal, 1) != 1) _exit(2);
        _exit(0);
    }
    close(ready[1]);
    close(done[0]);
    char signal;
    assert(read(ready[0], &signal, 1) == 1);
    assert(second.acquire() == Result::Busy);
    assert(write(done[1], "1", 1) == 1);
    close(ready[0]);
    close(done[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(second.acquire() == Result::Acquired);
    second.release();
    return 0;
}
