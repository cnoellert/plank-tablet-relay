// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "bluetooth_drawing_server.hpp"
#include "identity.h"
#include <array>
#include <cstdint>
#include <string>

// Mutation has its own root-only endpoint. The v1 public status interface
// remains read-only. No tablet nodes, capture lease or drawing session is used.
class PltrDrawingEnrollment {
public:
    static constexpr const char *ProductionName = "plank-tablet-drawing-enrollment-v1";
    static constexpr std::uint64_t GrantLifetimeMs = 120000;
    explicit PltrDrawingEnrollment(PltrIdentityStore &store)
        : store_(store), listener_({ProductionName, 0}) {}
    struct IsolatedTestPolicy { std::string name; std::uint32_t uid; };
    PltrDrawingEnrollment(PltrIdentityStore &store, IsolatedTestPolicy policy)
        : store_(store), listener_({std::move(policy.name), policy.uid}) {}
    ~PltrDrawingEnrollment();
    bool bind() { return listener_.bind(); }
    void service(std::uint64_t now);
    // A Noise-authenticated Client claims only its exact live grant. Claiming
    // burns the grant for other attempts; failure never adds an allowlist entry.
    bool claim(const std::uint8_t id[16], const std::uint8_t client[32], std::uint64_t now);
    bool commit(const std::uint8_t id[16], const std::uint8_t client[32], std::uint64_t now);
private:
    struct Grant {
        std::array<std::uint8_t,16> id{};
        std::array<std::uint8_t,32> client{};
        std::uint64_t deadline = 0;
        unsigned state = 0; // 1 pending, 2 claimed, 3 consumed/canceled tombstone
    };
    struct Connection {
        int fd = -1;
        std::uint64_t deadline = 0;
        std::array<std::uint8_t,87> request{};
        std::array<std::uint8_t,22> reply{};
        std::size_t received = 0, sent = 0;
        bool replying = false;
    };
    std::uint8_t command(const std::uint8_t *request, std::uint64_t now);
    Grant *find(const std::uint8_t id[16]);
    static void drop(Connection &connection);
    PltrIdentityStore &store_;
    PltrBluetoothDrawingServer listener_;
    std::array<Grant,16> grants_{};
    std::array<Connection,4> connections_{};
};

// Dedicated PLEN/1 Noise proof on the existing TCP listener. Fully bounded,
// no capture, no raw frames and no modifications to the legacy drawing wire.
int pltr_run_enrollment(int fd, PltrIdentityStore &store,
                       PltrDrawingEnrollment &grants, int stop_fd);
