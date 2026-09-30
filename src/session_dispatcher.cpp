#include "session_dispatcher.hpp"

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

PltrSessionDispatcher::PltrSessionDispatcher(
    std::function<void()> wake,
    LinuxRawWacomInput::GenerationProvider generation_provider)
    : wake_(std::move(wake)),
      generation_provider_(std::move(generation_provider)) {}

PltrSessionDispatcher::~PltrSessionDispatcher() { close(); }

bool PltrSessionDispatcher::accept(const PltrFrame &frame) {
    if (ended_) return false;
    switch (frame.type) {
    case PLTR_SESSION_READY:
        if (worker_ != nullptr || frame.payload_size != 5) return false;
        batch_enabled_ = ((std::uint32_t(frame.payload[0]) |
            (std::uint32_t(frame.payload[1]) << 8) |
            (std::uint32_t(frame.payload[2]) << 16) |
            (std::uint32_t(frame.payload[3]) << 24)) &
            PLTR_FEATURE_FRAME_BATCH) != 0;
        worker_ = std::make_unique<PltrWorkerBridge>(wake_, generation_provider_);
        worker_->setActive(frame.payload[4] != 0);
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
    if (batch_enabled_ && frame.plwh.size() >= 8 &&
        (std::uint16_t(frame.plwh[6]) | (std::uint16_t(frame.plwh[7]) << 8)) ==
            PLANK_RAW_HID_INPUT) {
        std::vector<PltrQueuedTabletFrame> frames;
        frames.push_back(std::move(frame));
        std::size_t payload_size = 1 + 10 + frames.front().plwh.size();
        PltrQueuedTabletFrame adjacent;
        while (frames.size() < PLTR_MAX_BATCH_FRAMES &&
               worker_->popAdjacentInput(adjacent)) {
            payload_size += 10 + adjacent.plwh.size();
            if (payload_size > PLTR_MAX_PAYLOAD_SIZE ||
                adjacent.plwh.size() > std::numeric_limits<std::uint16_t>::max())
                return -1;
            frames.push_back(std::move(adjacent));
        }
        if (frames.size() > 1) {
            std::vector<std::uint8_t> payload(payload_size);
            payload[0] = static_cast<std::uint8_t>(frames.size());
            std::size_t offset = 1;
            for (const auto &item : frames) {
                for (unsigned i = 0; i < 8; ++i)
                    payload[offset + i] = static_cast<std::uint8_t>(
                        item.capture_time_us >> (i * 8));
                offset += 8;
                payload[offset++] = static_cast<std::uint8_t>(item.plwh.size());
                payload[offset++] = static_cast<std::uint8_t>(item.plwh.size() >> 8);
                std::memcpy(payload.data() + offset, item.plwh.data(), item.plwh.size());
                offset += item.plwh.size();
            }
            return pltr_link_send(&link, PLTR_CLIENT_FRAME_BATCH,
                                  payload.data(), payload.size(),
                                  out, capacity, written) == 0 ? 1 : -1;
        }
        frame = std::move(frames.front());
    }
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
}

bool PltrSessionDispatcher::failed() const {
    return worker_ && worker_->failed();
}

PltrWorkerStatus PltrSessionDispatcher::status() const {
    return worker_ ? worker_->status() : PltrWorkerStatus{};
}
