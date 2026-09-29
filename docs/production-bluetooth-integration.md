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

The production session runner now accepts the already-established ordered
stream plus its Noise link type. The TCP entry point still selects type 2;
an LE entry point can select type 1 and run the same approved-key lookup,
`SESSION_READY` gate and raw-HID worker. A socketpair test exercises both
types. This does not create or advertise a Bluetooth listener.

This change enables a Bluetooth tablet to feed the **existing TCP Relay**
after Linux hardware qualification. It does not yet carry the production
stream from the Relay to Vision Pro over Bluetooth. That second hop needs:

1. A production byte-stream transport with bounded buffering and backpressure.
   Evaluate LE Credit Based L2CAP for raw-HID volume; the diagnostic GATT
   indication queue is not a throughput qualification for production input.
2. The existing CPace pairing and saved relay identity on that transport.
   Bind the accepted stream to its current peer and close it with the session.
3. A Vision Pro connection path that selects TCP or Bluetooth while preserving
   the existing preflight, focus and reconnection behavior.

Offline validation covers the HID identity parser and existing protocol tests.
The full Linux worker build and physical checks remain required: tablet sleep
and wake, pad and pen grouping, tip/pressure, exclusive event grab, USB/BT
switching, simultaneous nearby tablets, and reconnect with a held stroke.
