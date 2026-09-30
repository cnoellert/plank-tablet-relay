#include "wacom_preflight.hpp"
#include "../vendor/plank-client/wacomidentity.h"

#include <libudev.h>

#include <cstdlib>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <unistd.h>

namespace {

struct Physical {
    std::string path;
    std::uint16_t bus = 0;
    std::uint16_t product = 0;
};

Physical physicalWacom(udev_device* device) {
    if (device == nullptr) return {};
    udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
        device, "usb", "usb_device");
    const char* vendor = parent != nullptr ?
        udev_device_get_sysattr_value(parent, "idVendor") : nullptr;
    const char* product = parent != nullptr ?
        udev_device_get_sysattr_value(parent, "idProduct") : nullptr;
    const char* path = parent != nullptr ? udev_device_get_syspath(parent) : nullptr;
    if (vendor != nullptr && product != nullptr && path != nullptr) {
        char* vendor_end = nullptr;
        char* product_end = nullptr;
        const unsigned long vendor_id = strtoul(vendor, &vendor_end, 16);
        const unsigned long product_id = strtoul(product, &product_end, 16);
        if (vendor_end != vendor && *vendor_end == '\0' &&
            product_end != product && *product_end == '\0' &&
            vendor_id == 0x056a && product_id <= UINT16_MAX)
            return {std::string("usb:") + path, BUS_USB,
                    static_cast<std::uint16_t>(product_id)};
    }

    parent = udev_device_get_parent_with_subsystem_devtype(device, "hid", nullptr);
    const char* hid_id = parent != nullptr ?
        udev_device_get_property_value(parent, "HID_ID") : nullptr;
    path = parent != nullptr ? udev_device_get_syspath(parent) : nullptr;
    PlankWacomHidIdentity identity{};
    if (path != nullptr && plankParseWacomHidIdentity(hid_id, &identity))
        return {std::string("bluetooth:") + path, identity.bus,
                identity.product};
    return {};
}

bool collect(udev* context, const char* subsystem, PltrWacomNodeKind kind,
             std::vector<PltrWacomNode>& nodes) {
    udev_enumerate* enumeration = udev_enumerate_new(context);
    if (enumeration == nullptr) return false;
    const bool scanned =
        udev_enumerate_add_match_subsystem(enumeration, subsystem) == 0 &&
        udev_enumerate_scan_devices(enumeration) == 0;
    if (!scanned) {
        udev_enumerate_unref(enumeration);
        return false;
    }
    udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumeration)) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        if (device == nullptr) {
            udev_enumerate_unref(enumeration);
            return false;
        }
        const char* name = udev_device_get_sysname(device);
        if (kind == PltrWacomNodeKind::Event &&
            (name == nullptr || strncmp(name, "event", 5) != 0)) {
            udev_device_unref(device);
            continue;
        }
        const Physical physical = physicalWacom(device);
        if (!physical.path.empty()) {
            const char* path = udev_device_get_devnode(device);
            bool readable = false;
            if (path != nullptr) {
                const int flags = (kind == PltrWacomNodeKind::Hidraw ? O_RDWR : O_RDONLY) |
                                  O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
                const int fd = open(path, flags);
                if (fd >= 0) {
                    readable = true;
                    close(fd);
                }
            }
            nodes.push_back({kind, physical.path, physical.bus,
                             physical.product, readable});
        }
        udev_device_unref(device);
    }
    udev_enumerate_unref(enumeration);
    return true;
}

} // namespace

bool pltr_validate_wacom_nodes(const std::vector<PltrWacomNode>& nodes,
                               std::uint16_t bus, std::uint32_t vendor,
                               std::uint32_t product,
                               std::uint16_t interface_count) {
    if (vendor != 0x056a || product > UINT16_MAX ||
        (bus != BUS_USB && bus != BUS_BLUETOOTH) || interface_count == 0)
        return false;
    std::string physical;
    unsigned hidraw_count = 0;
    unsigned event_count = 0;
    for (const auto& node : nodes) {
        if (node.physical_path.empty() || !node.readable ||
            node.bus != bus || node.product != product)
            return false;
        if (physical.empty()) physical = node.physical_path;
        else if (physical != node.physical_path) return false;
        if (node.kind == PltrWacomNodeKind::Hidraw) ++hidraw_count;
        else ++event_count;
    }
    return hidraw_count == interface_count && event_count != 0;
}

bool pltr_wacom_nodes_ready(std::uint16_t bus, std::uint32_t vendor,
                            std::uint32_t product,
                            std::uint16_t interface_count) {
    udev* context = udev_new();
    if (context == nullptr) return false;
    std::vector<PltrWacomNode> nodes;
    const bool enumerated =
        collect(context, "hidraw", PltrWacomNodeKind::Hidraw, nodes) &&
        collect(context, "input", PltrWacomNodeKind::Event, nodes);
    udev_unref(context);
    return enumerated && pltr_validate_wacom_nodes(
        nodes, bus, vendor, product, interface_count);
}
