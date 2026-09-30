#pragma once

#include "link.h"
#include "worker_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// Applies already authenticated and validated Client frames to the Linux
// tablet worker. Owns one worker only between SESSION_READY and SESSION_END.
class PltrSessionDispatcher {
public:
    explicit PltrSessionDispatcher(
        std::function<void()> wake,
        LinuxRawWacomInput::GenerationProvider generation_provider = {});
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
    bool batchEnabled() const { return batch_enabled_; }
    bool pendingInput() const { return worker_ && worker_->hasQueuedInput(); }

private:
    std::function<void()> wake_;
    LinuxRawWacomInput::GenerationProvider generation_provider_;
    std::unique_ptr<PltrWorkerBridge> worker_;
    bool ended_ = false;
    bool batch_enabled_ = false;
};
