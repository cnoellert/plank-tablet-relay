#include "worker_bridge.hpp"
#include "protocol.h"
#include "../vendor/plank-client/plank.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>

static std::uint16_t le16(const std::uint8_t *bytes) {
    return std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8);
}

static std::uint32_t le32(const std::uint8_t *bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

static std::uint64_t monotonic_us() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return std::uint64_t(t.tv_sec) * 1000000 + std::uint64_t(t.tv_nsec) / 1000;
}

PltrWorkerBridge::PltrWorkerBridge(
    std::function<void()> wake,
    LinuxRawWacomInput::GenerationProvider generation_provider,
    DevicePreflight device_preflight)
    : wake_(std::move(wake)), device_preflight_(std::move(device_preflight)) {
    worker_ = std::make_unique<LinuxRawWacomInput>(
        [this](const unsigned char *bytes, std::size_t size) {
            return enqueue(bytes, size);
        },
        [] {},
        [](LinuxRawWacomInput::LogLevel level, const std::string &message) {
            std::fprintf(stderr, "Wacom %s: %s\n",
                         level == LinuxRawWacomInput::LogLevel::Warning ?
                             "warning" : "info", message.c_str());
        }, std::move(generation_provider));
}

PltrWorkerBridge::~PltrWorkerBridge() {
    worker_.reset(); // joins and releases the physical tablet before queue dies
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    queued_bytes_ = 0;
}

void PltrWorkerBridge::markFailed() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
    }
    if (wake_) wake_();
}

void PltrWorkerBridge::setActive(bool active) { worker_->setActive(active); }
void PltrWorkerBridge::beginReconnect() { worker_->beginReconnect(); }
void PltrWorkerBridge::finishReconnect() { worker_->finishReconnect(); }
void PltrWorkerBridge::handleControl(const std::uint8_t *bytes, std::size_t size) {
    if (bytes == nullptr || size > std::numeric_limits<unsigned int>::max()) return;
    worker_->handleControl(bytes, static_cast<unsigned int>(size));
    if (size == sizeof(PLANK_RAW_HID_WIRE_HEADER) + sizeof(std::int32_t) &&
        le32(bytes) == PLANK_RAW_HID_WIRE_MAGIC &&
        le16(bytes + 4) == PLANK_RAW_HID_WIRE_VERSION &&
        le16(bytes + 6) == PLANK_RAW_HID_ATTACH_RESULT &&
        le32(bytes + 16) == sizeof(std::int32_t)) {
        const std::uint8_t state =
            le32(bytes + sizeof(PLANK_RAW_HID_WIRE_HEADER)) == 0 &&
            worker_->ownsTablet() ? 3 : 7;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ != 0 && le16(bytes + 10) == generation_ &&
                status_.state != state) {
                status_.state = state;
                ++status_.epoch;
            }
        }
        if (wake_) wake_();
    }
}

bool PltrWorkerBridge::enqueue(const std::uint8_t *bytes, std::size_t size) {
    if (bytes == nullptr || size < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
        size > PLTR_MAX_PAYLOAD_SIZE - 8) {
        markFailed();
        return false;
    }
    if (le16(bytes + 6) == PLANK_RAW_HID_DEVICE) {
        if (size != sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                    sizeof(PLANK_RAW_HID_DEVICE_MESSAGE)) return false;
        const std::uint8_t *device = bytes + sizeof(PLANK_RAW_HID_WIRE_HEADER);
        if (!device_preflight_ ||
            !device_preflight_(le16(device + 2), le32(device + 4),
                               le32(device + 8), le16(device))) {
            std::fputs("Wacom warning: local Wacom nodes changed or a matching "
                       "node is unreadable; refusing Host attachment\n", stderr);
            return false;
        }
    }
    const std::uint64_t captured = le16(bytes + 6) == PLANK_RAW_HID_INPUT ?
                                   monotonic_us() : 0;
    std::array<std::uint8_t, PLTR_MAX_PAYLOAD_SIZE> payload{};
    for (unsigned i = 0; i < 8; ++i)
        payload[i] = static_cast<std::uint8_t>(captured >> (i * 8));
    std::copy(bytes, bytes + size, payload.begin() + 8);
    std::array<std::uint8_t, PLTR_MAX_FRAME_SIZE> scratch{};
    std::size_t written;
    if (pltr_encode_frame(PLTR_CLIENT_FRAME, 1, payload.data(), size + 8,
                          PLTR_RELAY_TO_CLIENT, PLTR_SECURE,
                          scratch.data(), scratch.size(), &written) != 0) {
        markFailed();
        return false;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ || queue_.size() >= MaxFrames ||
            size + 8 > MaxBytes - queued_bytes_) {
            failed_ = true;
        } else {
            queue_.push_back({captured, std::vector<std::uint8_t>(bytes, bytes + size)});
            queued_bytes_ += size + 8;
            accepted = true;
            const std::uint16_t type = le16(bytes + 6);
            std::uint8_t next_state = status_.state;
            if (type == PLANK_RAW_HID_DEVICE &&
                size >= sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                        sizeof(PLANK_RAW_HID_DEVICE_MESSAGE)) {
                const std::uint8_t *device = bytes + sizeof(PLANK_RAW_HID_WIRE_HEADER);
                const std::uint32_t vendor = le32(device + 4);
                const std::uint32_t product = le32(device + 8);
                if (vendor <= UINT16_MAX && product <= UINT16_MAX) {
                    status_.vendor = static_cast<std::uint16_t>(vendor);
                    status_.product = static_cast<std::uint16_t>(product);
                    status_.interface_count = static_cast<std::uint8_t>(
                        std::min<std::uint16_t>(le16(device), UINT8_MAX));
                }
                generation_ = le16(bytes + 10);
                next_state = 2;
            } else if (type == PLANK_RAW_HID_DETACH) {
                generation_ = 0;
                next_state = 0;
            } else if (type == PLANK_RAW_HID_SUSPEND) {
                next_state = 4;
            }
            if (next_state != status_.state) {
                status_.state = next_state;
                ++status_.epoch;
            }
        }
    }
    if (wake_) wake_();
    return accepted;
}

bool PltrWorkerBridge::pop(PltrQueuedTabletFrame &frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    frame = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= frame.plwh.size() + 8;
    return true;
}

bool PltrWorkerBridge::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

PltrWorkerStatus PltrWorkerBridge::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
