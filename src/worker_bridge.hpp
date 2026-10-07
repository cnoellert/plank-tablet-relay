#pragma once

#include "../vendor/plank-client/linuxrawwacom.h"
#include "wacom_preflight.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

struct PltrQueuedTabletFrame {
    std::uint64_t capture_time_us;
    std::vector<std::uint8_t> plwh;
};

struct PltrWorkerStatus {
    std::uint8_t state = 0;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::uint8_t interface_count = 0;
    std::uint8_t transport = 1;
    std::uint64_t epoch = 0;
};

// Bridges the existing Linux raw-Wacom worker to the bounded Relay output
// queue. The network thread alone encrypts and writes PLTR records; the worker
// thread only appends validated PLWH frames and wakes that thread.
class PltrWorkerBridge {
public:
    using DevicePreflight = std::function<bool(std::uint16_t, std::uint32_t,
                                                std::uint32_t, std::uint16_t)>;
    explicit PltrWorkerBridge(
        std::function<void()> wake,
        LinuxRawWacomInput::GenerationProvider generation_provider = {},
        DevicePreflight device_preflight = pltr_wacom_nodes_ready);
    ~PltrWorkerBridge();
    PltrWorkerBridge(const PltrWorkerBridge&) = delete;
    PltrWorkerBridge& operator=(const PltrWorkerBridge&) = delete;

    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const std::uint8_t *bytes, std::size_t size);
    bool pop(PltrQueuedTabletFrame &frame);
    bool failed() const;
    PltrWorkerStatus status() const;

    // Exposed for focused queue tests; production calls this from the worker.
    bool enqueue(const std::uint8_t *bytes, std::size_t size);

private:
    static constexpr std::size_t MaxFrames = 256;
    static constexpr std::size_t MaxBytes = 256 * 1024;
    mutable std::mutex mutex_;
    std::deque<PltrQueuedTabletFrame> queue_;
    std::size_t queued_bytes_ = 0;
    bool failed_ = false;
    PltrWorkerStatus status_;
    std::uint16_t generation_ = 0;
    std::function<void()> wake_;
    DevicePreflight device_preflight_;
    std::unique_ptr<LinuxRawWacomInput> worker_;
    void markFailed();
};
