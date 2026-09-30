#include "wacom_preflight.hpp"

#include <cassert>
#include <linux/input.h>
#include <vector>

int main() {
    using Kind = PltrWacomNodeKind;
    using Node = PltrWacomNode;
    const std::vector<Node> bluetooth = {
        {Kind::Hidraw, "bluetooth:/sys/uhid/tablet-a", BUS_BLUETOOTH, 0x0360, true},
        {Kind::Hidraw, "bluetooth:/sys/uhid/tablet-a", BUS_BLUETOOTH, 0x0360, true},
        {Kind::Event, "bluetooth:/sys/uhid/tablet-a", BUS_BLUETOOTH, 0x0360, true},
        {Kind::Event, "bluetooth:/sys/uhid/tablet-a", BUS_BLUETOOTH, 0x0360, true},
        {Kind::Event, "bluetooth:/sys/uhid/tablet-a", BUS_BLUETOOTH, 0x0360, true},
    };
    assert(pltr_validate_wacom_nodes(bluetooth, BUS_BLUETOOTH, 0x056a, 0x0360, 2));
    assert(!pltr_validate_wacom_nodes(bluetooth, BUS_BLUETOOTH, 0x046d, 0x0360, 2));
    assert(!pltr_validate_wacom_nodes(bluetooth, BUS_USB, 0x056a, 0x0360, 2));
    assert(!pltr_validate_wacom_nodes(bluetooth, BUS_BLUETOOTH, 0x056a, 0x0357, 2));
    assert(!pltr_validate_wacom_nodes(bluetooth, BUS_BLUETOOTH, 0x056a, 0x0360, 1));

    auto nodes = bluetooth;
    nodes[4].readable = false;
    assert(!pltr_validate_wacom_nodes(nodes, BUS_BLUETOOTH, 0x056a, 0x0360, 2));
    nodes = bluetooth;
    nodes[4].physical_path = "bluetooth:/sys/uhid/tablet-b";
    assert(!pltr_validate_wacom_nodes(nodes, BUS_BLUETOOTH, 0x056a, 0x0360, 2));
    nodes = bluetooth;
    nodes.erase(nodes.begin() + 2, nodes.end());
    assert(!pltr_validate_wacom_nodes(nodes, BUS_BLUETOOTH, 0x056a, 0x0360, 2));
    nodes = bluetooth;
    nodes[0].readable = false;
    assert(!pltr_validate_wacom_nodes(nodes, BUS_BLUETOOTH, 0x056a, 0x0360, 2));

    const std::vector<Node> usb = {
        {Kind::Hidraw, "usb:/sys/usb/tablet-a", BUS_USB, 0x0357, true},
        {Kind::Hidraw, "usb:/sys/usb/tablet-a", BUS_USB, 0x0357, true},
        {Kind::Event, "usb:/sys/usb/tablet-a", BUS_USB, 0x0357, true},
    };
    assert(pltr_validate_wacom_nodes(usb, BUS_USB, 0x056a, 0x0357, 2));
}
