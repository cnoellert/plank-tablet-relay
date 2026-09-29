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

This change enables a Bluetooth tablet to feed the **existing TCP Relay**
after Linux hardware qualification. It does not yet carry the production
stream from the Relay to Vision Pro over Bluetooth. That second hop needs:

1. A production byte-stream transport with bounded buffering and backpressure.
   Evaluate LE Credit Based L2CAP for raw-HID volume; the diagnostic GATT
   indication queue is not a throughput qualification for production input.
2. The existing CPace/Noise pairing, approved-key lookup and `PltrLink`
   protocol on that transport, with peer binding and per-session shutdown.
3. The same `PltrSessionDispatcher` and worker ownership rules as TCP, so
   disconnect releases held input and a new session cannot inherit old state.
4. A Vision Pro connection path that selects TCP or Bluetooth while preserving
   the existing preflight, focus and reconnection behavior.

Offline validation covers the HID identity parser and existing protocol tests.
The full Linux worker build and physical checks remain required: tablet sleep
and wake, pad and pen grouping, tip/pressure, exclusive event grab, USB/BT
switching, simultaneous nearby tablets, and reconnect with a held stroke.
