#pragma once

#include <string>

// Shared with the managed Setup service. The abstract Linux socket exists
// only while its owning file descriptor is open; there is no stale lock file.
class PltrCaptureLease {
public:
    static constexpr const char *ProductionName = "plank-tablet-capture-v1";
    enum class AcquireResult { Acquired, Busy, Error };

    // Alternate names are for isolated tests; production uses ProductionName.
    explicit PltrCaptureLease(std::string name = ProductionName);
    ~PltrCaptureLease();
    PltrCaptureLease(const PltrCaptureLease &) = delete;
    PltrCaptureLease &operator=(const PltrCaptureLease &) = delete;

    AcquireResult acquire(int *error_number = nullptr) noexcept;
    void release() noexcept;
    bool held() const noexcept { return fd_ >= 0; }

private:
    std::string name_;
    int fd_ = -1;
};
