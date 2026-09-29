# Production Bluetooth integration status

This branch starts from Alan's `visionos-tablet-setup` work. That branch's
`plank-tablet-relay-ble` service and Tablet Setup app authenticate a headset
and send bounded diagnostic readings. The production PLTR/raw-HID stream still
runs through `plank-tablet-relay` over TCP. Do not replace `PLTR_CLIENT_FRAME`
with the 80-byte `PLTR_INPUT_SAMPLE` diagnostic payload: the latter cannot
reproduce the workstation's Wacom device, pressure and control path.

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
paths have unit and Linux package tests, but no physical CoC link test yet.

This change enables a Bluetooth tablet to feed the **existing TCP Relay**
after Linux hardware qualification. It also implements the Relay end of a
production Bluetooth channel. The Vision Pro app still uses TCP for live
sessions. Completing the second hop needs:

1. A Vision Pro CoreBluetooth connection path that reads the PSM and opens
   `CBPeripheral.openL2CAPChannel`, using the BLE-specific Noise prologue.
2. A pairing path in the PLANK app to provision its own Client key into the
   Bluetooth service. Alan's separate Tablet Setup app uses a different
   Keychain namespace; its pairing does not authorize the PLANK app.
3. Physical LE CoC qualification on the NUC or NanoPi: PSM allocation,
   sustained raw-HID throughput, reconnects, and simultaneous Wacom Bluetooth.

Offline validation covers the HID identity parser and existing protocol tests.
The full Linux worker build and physical checks remain required: tablet sleep
and wake, pad and pen grouping, tip/pressure, exclusive event grab, USB/BT
switching, simultaneous nearby tablets, and reconnect with a held stroke.
