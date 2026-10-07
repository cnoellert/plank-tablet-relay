#include "session_dispatcher.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <unistd.h>

int main() {
    const std::string lease_name = "plank-dispatcher-test-" +
                                   std::to_string(getpid());
    using Result = PltrCaptureLease::AcquireResult;
    std::uint8_t ready[] = {0x24, 0, 0, 0, 0};
    PltrFrame frame = {PLTR_SESSION_READY, 1, ready, sizeof(ready)};
    PltrCaptureLease other(lease_name);
    assert(other.acquire() == Result::Acquired);
    PltrSessionDispatcher busy([] {}, {}, lease_name);
    assert(!busy.accept(frame));
    assert(busy.status().epoch == 0); // no raw worker was created
    busy.close();
    assert(other.held()); // a failed attempt cannot release another owner
    other.release();

    PltrSessionDispatcher dispatcher([] {}, {}, lease_name);
    assert(dispatcher.accept(frame));
    assert(other.acquire() == Result::Busy);
    assert(!dispatcher.accept(frame));
    frame = {PLTR_RECONNECT_BEGIN, 2, nullptr, 0};
    assert(dispatcher.accept(frame));
    frame = {PLTR_RECONNECT_FINISH, 3, nullptr, 0};
    assert(dispatcher.accept(frame));
    std::uint8_t active[] = {0};
    frame = {PLTR_SESSION_ACTIVE, 4, active, sizeof(active)};
    assert(dispatcher.accept(frame));
    frame = {PLTR_SESSION_END, 5, active, sizeof(active)};
    assert(dispatcher.accept(frame));
    assert(other.acquire() == Result::Acquired);
    other.release();
    assert(!dispatcher.accept(frame));

    // A control frame before SESSION_READY must not create or drive a worker.
    PltrSessionDispatcher early([] {}, {}, lease_name);
    frame = {PLTR_HOST_FRAME, 1, active, sizeof(active)};
    assert(!early.accept(frame));
    frame = {PLTR_RECONNECT_BEGIN, 2, nullptr, 0};
    assert(!early.accept(frame));
    frame = {PLTR_SESSION_READY, 3, ready, sizeof(ready)};
    assert(early.accept(frame));
    assert(other.acquire() == Result::Busy);
    frame = {PLTR_SESSION_ACTIVE, 4, nullptr, 0};
    assert(!early.accept(frame)); // caller's failure cleanup must end ownership
    early.close();
    assert(other.acquire() == Result::Acquired);
    other.release();
    assert(!early.accept(frame));

    // Destruction also tears down the worker and frees the lease.
    {
        PltrSessionDispatcher disconnected([] {}, {}, lease_name);
        frame = {PLTR_SESSION_READY, 1, ready, sizeof(ready)};
        assert(disconnected.accept(frame));
        assert(other.acquire() == Result::Busy);
    }
    assert(other.acquire() == Result::Acquired);
    other.release();
    return 0;
}
