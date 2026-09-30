#pragma once

#include "capture_lease.hpp"
#include "link.h"
#include "worker_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Applies already authenticated and validated Client frames to the Linux
// tablet worker. Owns one worker only between SESSION_READY and SESSION_END.
class PltrSessionDispatcher {
public:
    explicit PltrSessionDispatcher(
        std::function<void()> wake,
        LinuxRawWacomInput::GenerationProvider generation_provider = {},
        std::string capture_lease_name = PltrCaptureLease::ProductionName);
    ~PltrSessionDispatcher();
    PltrSessionDispatcher(const PltrSessionDispatcher&) = delete;
    PltrSessionDispatcher& operator=(const PltrSessionDispatcher&) = delete;

    bool accept(const PltrFrame &frame);
    // Returns 1 for one encrypted CLIENT_FRAME record, 0 for empty, -1 for
    // queue or link failure. The caller writes the record on its network thread.
    int next(PltrLink &link, std::uint8_t *out, std::size_t capacity,
             std::size_t *written);
    void close();
    bool failed() const;
    PltrWorkerStatus status() const;

private:
    std::function<void()> wake_;
    LinuxRawWacomInput::GenerationProvider generation_provider_;
    PltrCaptureLease capture_lease_;
    std::unique_ptr<PltrWorkerBridge> worker_;
    bool ended_ = false;
};
