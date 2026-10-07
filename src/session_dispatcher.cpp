#include "session_dispatcher.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <utility>
#include <vector>

PltrSessionDispatcher::PltrSessionDispatcher(
    std::function<void()> wake,
    LinuxRawWacomInput::GenerationProvider generation_provider,
    std::string capture_lease_name)
    : wake_(std::move(wake)),
      generation_provider_(std::move(generation_provider)),
      capture_lease_(std::move(capture_lease_name)) {}

PltrSessionDispatcher::~PltrSessionDispatcher() { close(); }

bool PltrSessionDispatcher::accept(const PltrFrame &frame) {
    if (ended_) return false;
    switch (frame.type) {
    case PLTR_SESSION_READY:
        if (worker_ != nullptr || frame.payload_size != 5) return false;
        {
            int lease_error = 0;
            const auto acquired = capture_lease_.acquire(&lease_error);
            if (acquired != PltrCaptureLease::AcquireResult::Acquired) {
                if (acquired == PltrCaptureLease::AcquireResult::Busy) {
                    std::fputs("Wacom capture busy: another tablet service owns "
                               "the tablet; refusing PLANK session\n", stderr);
                } else {
                    std::fprintf(stderr, "Wacom capture lease unavailable (%s); "
                                 "refusing PLANK session\n",
                                 std::strerror(lease_error));
                }
                return false;
            }
        }
        try {
            worker_ = std::make_unique<PltrWorkerBridge>(wake_, generation_provider_);
            worker_->setActive(frame.payload[4] != 0);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "Wacom worker failed to start: %s\n", error.what());
            worker_.reset();
            capture_lease_.release();
            return false;
        } catch (...) {
            std::fputs("Wacom worker failed to start; refusing PLANK session\n",
                       stderr);
            worker_.reset();
            capture_lease_.release();
            return false;
        }
        return true;
    case PLTR_SESSION_ACTIVE:
        if (!worker_ || frame.payload_size != 1) return false;
        worker_->setActive(frame.payload[0] != 0);
        return true;
    case PLTR_RECONNECT_BEGIN:
        if (!worker_ || frame.payload_size != 0) return false;
        worker_->beginReconnect();
        return true;
    case PLTR_RECONNECT_FINISH:
        if (!worker_ || frame.payload_size != 0) return false;
        worker_->finishReconnect();
        return true;
    case PLTR_HOST_FRAME:
        if (!worker_ || frame.payload_size == 0) return false;
        worker_->handleControl(frame.payload, frame.payload_size);
        return true;
    case PLTR_SESSION_END:
    case PLTR_GOODBYE:
        close();
        return true;
    case PLTR_PING:
    case PLTR_PONG:
        return true;
    default:
        return false;
    }
}

int PltrSessionDispatcher::next(PltrLink &link, std::uint8_t *out,
                                 std::size_t capacity, std::size_t *written) {
    if (out == nullptr || written == nullptr || ended_ || !worker_ ||
        worker_->failed()) return -1;
    *written = 0;
    PltrQueuedTabletFrame frame;
    if (!worker_->pop(frame)) return 0;
    // The queue is single-use. A failed encryption must close the link rather
    // than silently discard this tablet report and continue with later ones.
    if (frame.plwh.size() > PLTR_MAX_PAYLOAD_SIZE - 8) return -1;
    std::vector<std::uint8_t> payload(8 + frame.plwh.size());
    for (unsigned i = 0; i < 8; ++i)
        payload[i] = static_cast<std::uint8_t>(frame.capture_time_us >> (i * 8));
    std::memcpy(payload.data() + 8, frame.plwh.data(), frame.plwh.size());
    return pltr_link_send(&link, PLTR_CLIENT_FRAME,
                           payload.data(), payload.size(),
                           out, capacity, written) == 0 ? 1 : -1;
}

void PltrSessionDispatcher::close() {
    ended_ = true;
    worker_.reset();
    capture_lease_.release();
}

bool PltrSessionDispatcher::failed() const {
    return worker_ && worker_->failed();
}

PltrWorkerStatus PltrSessionDispatcher::status() const {
    return worker_ ? worker_->status() : PltrWorkerStatus{};
}
