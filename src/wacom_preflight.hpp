#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class PltrWacomNodeKind { Hidraw, Event };

struct PltrWacomNode {
    PltrWacomNodeKind kind;
    std::string physical_path;
    std::uint16_t bus;
    std::uint16_t product;
    bool readable;
};

// Require the same physical Wacom HID for all interfaces and every evdev node.
// In particular, an unreadable event node must not be silently omitted from a
// successful attachment and leave local buttons active while the Host is used.
bool pltr_validate_wacom_nodes(const std::vector<PltrWacomNode>& nodes,
                               std::uint16_t bus, std::uint32_t vendor,
                               std::uint32_t product,
                               std::uint16_t interface_count);

// Linux device snapshot taken immediately before the worker queues DEVICE.
bool pltr_wacom_nodes_ready(std::uint16_t bus, std::uint32_t vendor,
                            std::uint32_t product,
                            std::uint16_t interface_count);
