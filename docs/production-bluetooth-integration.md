# Production Bluetooth integration status

This branch starts from Alan's `visionos-tablet-setup` work. The
`plank-tablet-relay-ble` service now carries the production PLTR/raw-HID stream
over authenticated LE Credit Based L2CAP. The TCP Relay remains available.
`PLTR_INPUT_SAMPLE` is only a diagnostic reading and cannot reproduce the
workstation's Wacom device, pressure and control path.

The production raw-HID worker now discovers a bonded Bluetooth Wacom by its
Linux HID ancestor (`HID_ID` bus 5, Wacom vendor), and matches its hidraw and
evdev nodes to that same ancestor. It still recognizes USB Wacoms by their
physical USB parent. Multiple physical candidates leave raw capture offline
instead of selecting one arbitrarily. The worker snapshot comes from
`cnoellert/plank-client` commit `4b0b569b847a708d0b55535c715f8e16ef906b65`.

The production session runner accepts an already-established ordered stream
plus its Noise link type. The TCP entry point selects type 2. The LE Credit
Based L2CAP entry point selects type 1 and runs the same approved-key lookup,
`SESSION_READY` gate and raw-HID worker. A bounded adapter converts L2CAP SDUs
into that ordered stream and splits outgoing records at the negotiated MTU.
The BlueZ service publishes a read-only PSM characteristic and accepts one
production channel while pairing is idle. It shares the exact identity store
held by the pairing service; a second daemon cannot safely open it. These
paths have unit and Linux package tests. A physical Vision Pro has established
the LE CoC link with the AX900 on an Ubuntu 26.04 NUC, passed saved-key Noise
identity verification, and forwarded USB Wacom movement, tip clicks and
pressure to a Linux Host. The NanoPi Zero2 bound and advertised the production
LE listener before its 5 V power input was damaged by a 12 V adapter; no
physical headset stream was qualified on that board.

The signed Vision Pro development Client implements CoreBluetooth discovery,
physical-button pairing, PSM reading, `openL2CAPChannel`, and the BLE-specific
Noise prologue. The Client and Relay negotiate report batching through a
reserved bit in `SESSION_READY`; older Clients continue to receive individual
`PLTR_CLIENT_FRAME` records. The batch contains only ordered raw-HID input
reports. Attach, detach, suspend and Host control retain their individual
messages. The AVP validates the entire batch before delivering any report.

Remaining hardware qualification includes:

1. Longer sessions and repeated headset sleep/reconnect while the Wacom is
   connected over Bluetooth to the NUC.
2. Resolving LE delivery bursts and intermittent Relay backpressure before
   treating Bluetooth drawing as equivalent to the network Relay. A physical
   continuous-stroke run briefly queued 73 reports (oldest 355 ms); bounded
   AVP replay reduced delay, but long curves still appeared faceted.
3. Ensuring all tip, button and pressure transitions survive extended use.

The Ubuntu package build runs 25 Relay tests, libsodium tests and an extracted
package smoke check. The physical NUC test supplements these checks but does
not replace tablet sleep/wake, USB/BT switching, simultaneous nearby tablets
or extended held-stroke/reconnect trials.
