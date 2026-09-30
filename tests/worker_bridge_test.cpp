#include "worker_bridge.hpp"
#include "../vendor/plank-client/plank.h"

#include <cassert>
#include <cstdint>

static void le16(std::uint8_t *p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

static void le32(std::uint8_t *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

int main() {
    unsigned wakeups = 0;
    PltrWorkerBridge bridge([&] { ++wakeups; }, {},
                            [](std::uint16_t, std::uint32_t,
                               std::uint32_t, std::uint16_t) { return true; });
    std::uint8_t frame[sizeof(PLANK_RAW_HID_WIRE_HEADER) + 1] = {};
    le32(frame, PLANK_RAW_HID_WIRE_MAGIC);
    le16(frame + 4, PLANK_RAW_HID_WIRE_VERSION);
    le16(frame + 6, PLANK_RAW_HID_SUSPEND);
    assert(bridge.enqueue(frame, sizeof(frame) - 1));
    PltrQueuedTabletFrame item;
    assert(bridge.pop(item));
    assert(item.capture_time_us == 0 &&
           item.plwh.size() == sizeof(frame) - 1 && wakeups == 1);
    assert(!bridge.pop(item));

    le16(frame + 6, PLANK_RAW_HID_INPUT);
    le32(frame + 16, 1);
    frame[sizeof(frame) - 1] = 7;
    assert(bridge.enqueue(frame, sizeof(frame)));
    assert(bridge.pop(item));
    assert(item.capture_time_us != 0 && item.plwh.back() == 7);

    std::uint8_t device[sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                        sizeof(PLANK_RAW_HID_DEVICE_MESSAGE)] = {};
    le32(device, PLANK_RAW_HID_WIRE_MAGIC);
    le16(device + 4, PLANK_RAW_HID_WIRE_VERSION);
    le16(device + 6, PLANK_RAW_HID_DEVICE);
    le16(device + 10, 0x1234);
    le32(device + 16, sizeof(PLANK_RAW_HID_DEVICE_MESSAGE));
    le16(device + sizeof(PLANK_RAW_HID_WIRE_HEADER), 2);
    le32(device + sizeof(PLANK_RAW_HID_WIRE_HEADER) + 4, 0x056a);
    le32(device + sizeof(PLANK_RAW_HID_WIRE_HEADER) + 8, 0x0357);
    PltrWorkerBridge unreadable([] {}, {},
        [](std::uint16_t, std::uint32_t,
           std::uint32_t, std::uint16_t) { return false; });
    assert(!unreadable.enqueue(device, sizeof(device)));
    assert(unreadable.status().state == 0);
    assert(!unreadable.pop(item));
    assert(bridge.enqueue(device, sizeof(device)));
    const PltrWorkerStatus attaching = bridge.status();
    assert(attaching.state == 2 && attaching.vendor == 0x056a &&
           attaching.product == 0x0357 && attaching.interface_count == 2 &&
           attaching.epoch != 0);
    assert(bridge.pop(item));

    // A Host success code alone does not prove that the Relay owns the local
    // event nodes. This bridge has no discovered tablet, so STATUS must fail.
    std::uint8_t accepted[sizeof(PLANK_RAW_HID_WIRE_HEADER) + 4] = {};
    le32(accepted, PLANK_RAW_HID_WIRE_MAGIC);
    le16(accepted + 4, PLANK_RAW_HID_WIRE_VERSION);
    le16(accepted + 6, PLANK_RAW_HID_ATTACH_RESULT);
    le16(accepted + 10, 0x1234);
    le32(accepted + 16, 4);
    bridge.handleControl(accepted, sizeof(accepted));
    assert(bridge.status().state == 7);

    std::uint8_t detach[sizeof(PLANK_RAW_HID_WIRE_HEADER)] = {};
    le32(detach, PLANK_RAW_HID_WIRE_MAGIC);
    le16(detach + 4, PLANK_RAW_HID_WIRE_VERSION);
    le16(detach + 6, PLANK_RAW_HID_DETACH);
    assert(bridge.enqueue(detach, sizeof(detach)));
    assert(bridge.status().state == 0);
    assert(bridge.pop(item));

    le16(frame + 6, PLANK_RAW_HID_SUSPEND);
    le32(frame + 16, 0);
    for (unsigned i = 0; i < 256; ++i)
        assert(bridge.enqueue(frame, sizeof(frame) - 1));
    assert(!bridge.enqueue(frame, sizeof(frame) - 1));
    assert(bridge.failed());
    return 0;
}
