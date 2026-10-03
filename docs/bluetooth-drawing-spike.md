# Bluetooth raw drawing development spike

This development branch adds a local stream listener named
`plank-tablet-drawing-ble-v1`. Only a uid-0 peer can connect. The listener passes
that stream to the existing drawing session with Bluetooth Noise link type 1.
The remote Client must still authenticate using an already approved drawing
identity. TCP keeps link type 2 and its current listener.

The managed Relay owns LE L2CAP and bridges opaque bytes from its channel 3 to
this socket. It must verify that the socket server is the configured raw-service
account before sending any bytes. The raw daemon owns its keys, authorization,
raw HID dispatcher, Host replies, generations and existing capture lease. Neither
accepting a local socket nor discovering a Bluetooth radio authorizes a Client.

This is a development spike for an already approved Relay, not new enrollment or
a complete installer. The frozen network handoff fixtures are unchanged. The
status endpoint still describes TCP routes; a future versioned handoff must
advertise Bluetooth capabilities before this becomes a product path.

## Focused verification

On Linux with libsodium >= 1.0.19 and assertions enabled, CTest includes the
Bluetooth client codec and stream session, plus listener peer denial and name
collision. The existing capture-busy and unknown-key cases run for both link
types. The remaining acceptance gate is real wireless drawing on AVP with the
Relay network disabled, including Host control replies and reconnects.
