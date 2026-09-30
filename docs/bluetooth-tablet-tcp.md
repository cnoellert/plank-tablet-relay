# Wacom Bluetooth to Relay, TCP to Vision Pro

This branch adds Bluetooth Wacom raw-HID capture to the existing authenticated
TCP Relay. It keeps Bonjour discovery, the ExpressKey pairing chord, and the
current CPace/Noise pairing and session protocol. It does not use a Bluetooth
link between the Relay and Vision Pro.

## Tablet and permissions

The tested PTH-660 has USB product ID `056a:0357` and Bluetooth HID product ID
`056a:0360`. Linux presents a bonded Bluetooth tablet through UHID with
`hidraw` and `event` nodes under one HID ancestor. The `HID_ID` property uses
eight-digit vendor/product fields. On the development NUC, an observed
Bluetooth HID ancestor was `0005:056A:0360.0001`; the scoped `KERNELS` rule
matched the tablet's `hidraw` and all three `event` nodes. Their permissions
were `root:plank-relay 0660`.
The Relay worker accepts either bus only when all discovered Wacom interfaces
belong to one physical HID device. The idle pairing chord accepts a single Pad
with eight ExpressKeys from either of these two PTH-660 identities. A second
eligible Pad makes the chord unavailable rather than choosing one arbitrarily.

The TCP service runs as `plank-relay`. Existing USB udev rules may not match a
Bluetooth UHID device. The sample
[`70-plank-tablet-relay-bluetooth.rules`](../packaging/70-plank-tablet-relay-bluetooth.rules)
grants only the Wacom Bluetooth HID ancestor to that service group. Inspect the
actual `KERNELS` chain before installing it; do not broaden permissions to all
HID or input devices. After installing the scoped rule, trigger udev or
reconnect the tablet, then check **every** matching node:

```sh
udevadm info --attribute-walk --name=/dev/hidrawN
udevadm info --attribute-walk --name=/dev/input/eventN
udevadm test /sys/class/hidraw/hidrawN 2>&1 | grep -E 'GROUP|MODE|rules'
stat -c '%n %U:%G %a' /dev/hidrawN /dev/input/eventN
sudo -u plank-relay test -r /dev/input/eventN
sudo -u plank-relay test -r /dev/hidrawN
sudo -u plank-relay test -w /dev/hidrawN
```

Controller indices can change after a reboot; select the intended Bluetooth
controller by its hardware address, not by `hci0` or `hci1`. After bonding a
tablet in BlueZ, trust it and reconnect it so the input service can be
authorized. A successful bond alone did not hold the input connection on this
NUC.

Replace the `N` values with each Wacom node; inspect the corresponding
`/sys/class/input/eventN/device/id/{vendor,product}` and HID ancestor so an
unrelated device is not counted. `hidraw` must allow `O_RDWR`; every event node
must allow `O_RDONLY` and `EVIOCGRAB`. The runtime attachment preflight opens
every matching node before sending a `DEVICE` frame. If any is missing,
unreadable, or belongs to another Wacom tablet, attachment is refused and
retried after the device or permissions are repaired. `EVIOCGRAB` is checked
again by the worker before the Host reports the tablet attached.

## Live comparison

Keep the NUC on wired Ethernet and Vision Pro on the same local Wi-Fi for the
first comparison. With USB unplugged, wake the already bonded Wacom; confirm
BlueZ reports `Connected: yes` and the PTH-660's Bluetooth HID nodes appear.
Run one PLANK session and compare long curves, tip clicks, pressure, and
ExpressKeys against the USB baseline. Include a tablet sleep/wake and a clean
PLANK disconnect/reconnect. Do not claim equivalent drawing quality until the
long-curve timing and input behavior have been checked on hardware.

If the test passes, the NUC's Wi-Fi leg can be tested separately. Keep a wired
fallback during that change so Bluetooth input and network changes are not
confounded.

### Development NUC result, 2026-09-30

Two PTH-660 tablets bonded to the AX900 Bluetooth controller. The connection
held after BlueZ marked the second tablet trusted and explicitly reconnected
its input profile; the first bond attempt alone disconnected. The signed Vision
Pro network Client reached the Relay at `192.0.2.20:28990` across the test
network, and the Host attached the Bluetooth tablet. In Flame, pen movement,
tip clicks, and varying pressure worked. Long curves felt much less delayed
than the prior NUC-to-Vision-Pro Bluetooth path, though a little faceting
remained. The same tablet, switched to USB in the same PLANK session, was the
smoothest reference. This qualifies the Bluetooth-to-NUC plus network-to-AVP
path functionally; it does not establish USB-equivalent stroke quality.

Bonjour did not discover the Relay across the separate test subnets. The Client
used the NUC's manual address for this run. NUC Ethernet remained active, and
NUC Wi-Fi was not changed. For a USB comparison, block the tablet's BlueZ
device to prevent Bluetooth auto-reconnect from presenting a second interface;
unblock it before the next Bluetooth run.
