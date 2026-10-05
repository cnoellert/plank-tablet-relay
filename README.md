# PLANK tablet Relay

This is the separate Linux Relay for a Wacom tablet used with the native
PLANK Vision Pro Client. The Relay will read the tablet locally and forward its
existing raw-HID frames to a paired Client. The Client owns the authenticated
Host session; the Relay has no Host credentials.

**Current state:** the bounded `PLTR` frame and byte-stream parsers are tested,
and the Client's raw Wacom worker is vendored and build-checked on Linux. The
Noise IK handshake and transport module passes the published Noise-C vector
for the specified cipher suite, including encrypted traffic in both
directions. The CPace ristretto255/SHA-512 core passes the pinned CFRG draft
vector; its direction-specific confirmation tags are independently checked.
A Relay-side session gate enforces `HELLO`, `SESSION_READY`, reconnect and end
ordering. The shared link layer now joins framing, Noise IK, approved-Client
lookup, encrypted `HELLO`, and session frames. A test exercises that flow over
TCP bound to `127.0.0.1` and checks rejection of an unpaired key, a mismatched
link type and altered ciphertext. A locked local identity store persists the
Relay key and up to 16 approved Client public keys. The development binary can
bind a LAN address only when explicitly requested.
The pairing engine now covers a 120-second window, five tablet keys, CPace
confirmation, a 60-second attempt deadline, and a 10-minute lockout after
three failures. It persists a Client key only after verifying the Client's
confirmation tag. Its framed pairing state machine and the Client counterpart
pass an end-to-end test: the Client pins the Relay key only after verifying the
Relay's confirmation tag. The Intuos Pro PTH-660's eight physical ExpressKeys
were checked on the development NUC in order against codes 256 through 263.
Bluetooth LE and production packaging remain to be implemented.
The NUC's Wacom Pad evdev node is readable by the `plank-relay` service user;
the ExpressKey mapping and 5/15-second hold detector are compiled and tested.
Only the five-second pairing action is wired into the running service; the
15-second forget-all action is still unimplemented.
The existing raw-Wacom worker now has a bounded, validated output queue for
the network thread. Worker overflow marks the link failed instead of dropping
individual tablet reports. An authenticated session dispatcher routes Client
control frames into that worker and encrypts queued tablet frames. A
single-connection TCP driver has bounded writes, handshake and heartbeat
deadlines, clean end handling, a stop descriptor, and tablet state updates from
the worker's Wacom and Host frames. A real socket test covers
the Noise handshake, initial status, session start and end, and rejection of an
unpaired Client. The `plank-tablet-relay` development command now offers
single-connection `serve` and physical-key `pair` modes over TCP. It binds
loopback by default and accepts a specific IPv4 bind address only when given
explicitly. The pairing mode reserves an attempt in an owner-only, atomically
written state file before opening the window; a ten-minute lockout survives
command restarts. A real socket and simulated Pad-event test covers the full
pairing exchange. The signed native visionOS Client now has a pairing screen,
Keychain identity pinning, and a session link that forwards validated Wacom
frames through the existing Host raw-HID channel. It starts the link only after
Host tablet support is negotiated and closes it with the desktop session.
On the development NUC and a physical Vision Pro, local TCP pairing completed
with the five ExpressKeys. A live PLANK session then carried pen motion, tip and
side buttons, and varying pressure into GNOME Settings and Autodesk Flame.
The tablet stopped controlling Linux when PLANK lost focus and resumed when it
became active. The Client also resumed pen clicks and pressure after the Relay
service restarted during a session. After a full NUC reboot, the service
started automatically and a fresh Vision Pro session again carried pen clicks
and varying pressure. After the headset was removed for one minute, pen
movement and pressure returned immediately on wake. Bluetooth LE and
production packaging remain. During an active session, unplugging the tablet's
USB cable and reconnecting it also recovered without restarting PLANK: movement
returned first, followed a few seconds later by tip clicks and varying
pressure. Restarting the Relay service while the pen tip was held down in
GNOME's tablet test area ended that stroke cleanly; new pen input worked after
the link reconnected. The Host showed one Wacom device set after recovery.
The Relay now saves each raw-HID attachment generation before sending it.
In a live same-session check, two consecutive service restarts attached as
generations 2 and 3; tip clicks and varying pressure worked after each restart.
Relay software version 0.1.1 reports `STATUS=attached` only after Linux has
successfully claimed every local Wacom event node. If claiming fails, it
suspends the Host tablet and reports attach rejection. The Vision Pro Client
uses this stronger signal in its six-gate Wacom preflight.
The running service now watches the idle Wacom Pad. Holding its first and last
ExpressKeys together for five seconds opens the same bounded pairing window as
the `pair` command, without stopping the service or using a terminal. Avahi
advertises `_plank-tablet._tcp` with the protocol version, public-key
fingerprint prefix, and live pairing-window state. The dev NUC advertises the
service on its local 192.168.86 network; the signed Vision Pro Client can browse
for it and connect through the Bonjour service endpoint. The physical-chord
and Vision Pro discovery flow still need a live acceptance check. Manual
address entry remains available when multicast does not cross networks.
The TCP Relay can now recognize a bonded Bluetooth Wacom under Linux UHID as
well as a USB Wacom. It pins the Client's Bluetooth-aware raw-HID worker and
checks that every local Wacom event node is accessible before Host attachment.
For the tested PTH-660, the idle pairing chord accepts USB `056a:0357` or
Bluetooth `056a:0360`; multiple eligible Pads are rejected. A development NUC
and physical Vision Pro passed live Bluetooth tablet movement, clicks, and
pressure over the authenticated TCP link. Long curves were still slightly less
smooth than USB. See [Bluetooth TCP setup and test](docs/bluetooth-tablet-tcp.md)
for the scoped permissions and live result, and [setup app boundary](docs/setup-app-boundary.md)
for the proposed separation between headless setup and PLANK's connection UI.

Development service entry points (use the Relay service account that owns the
0700 state directory, and confirm the Pad key order before physical pairing):

```sh
plank-tablet-relay pair --state-dir /var/lib/plank-tablet-relay
plank-tablet-relay serve --state-dir /var/lib/plank-tablet-relay
```

Both default to `127.0.0.1:28990`. `--bind IPv4` explicitly selects a LAN
address for a local-network trial. The `pair` command remains available for
recovery. In normal service mode, hold ExpressKeys 1 and 8 for five seconds
while no tablet session is active, then select the nearby Relay in Vision Pro
Settings and press the five displayed ExpressKeys. A pairing window lasts
120 seconds and admits one attempt. A live tablet session is not interrupted
by a pairing chord or replaced by a second Client.

The raw worker and the separate managed Setup service coordinate tablet access
through one Linux abstract `AF_UNIX` datagram socket. Its address bytes are a
leading NUL followed by `plank-tablet-capture-v1`, with no trailing NUL. Each
service binds a nonblocking, close-on-exec socket before starting exclusive
tablet work and holds its descriptor until that work stops. A second bind fails
immediately; closing the descriptor or ending the process frees the address.
The Relay obtains this lease after authenticated `SESSION_READY`, before
constructing the Wacom worker, and releases it after the worker joins. When
Setup owns capture, the Relay logs the specific conflict and closes the new
session; the current wire format has no distinct busy message for the Client.
Explicit pairing and the five-second physical pairing window use the same
lease. The idle Pad reader remains read-only and nonexclusive; a pairing chord
cannot open a window while Setup owns capture. Both services must include this
guard before they run together on a NUC.

`packaging/plank-tablet-relay.service` is a systemd unit for an unprivileged
`plank-relay` user. It expects the binary at
`/usr/local/libexec/plank-tablet-relay` and an owner-only
`/var/lib/plank-tablet-relay` state directory. Set `PLANK_RELAY_BIND` in
`/etc/default/plank-tablet-relay` to the Relay's LAN IPv4 address; its safe
default is loopback. The tablet's `hidraw` and event nodes must be readable by
the service account, and the firewall must permit TCP 28990 only from the
intended local network. For automatic discovery, install and run Avahi on the
Relay and keep the headset on the same multicast-enabled local network. The
service remains usable by manual address when Avahi or multicast is unavailable.

Build and test:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The Noise test is built on Linux when `pkg-config` finds libsodium 1.0.19 or
newer. If libsodium is installed in a private prefix, set `PKG_CONFIG_PATH` to
its `lib/pkgconfig` directory before configuring. A missing or older version
disables the Noise target; it must be present for a Relay build that opens a
link. On the development NUC, libsodium 1.0.22 was built into a temporary
project-local prefix from its immutable source archive after verifying its
Minisign signature with the [publisher's documented key](https://doc.libsodium.org/installation).

The current client integration lives on the
[`codex/visionos-client` branch](https://github.com/cnoellert/plank-client/tree/codex/visionos-client).
The wire format is implemented and exercised by the Relay and Client tests;
a canonical shared contract revision and test vectors remain to be pinned.

The identity store requires an existing owner-only `0700` directory. It
creates `identity.key`, `paired-clients.json`, and `store.lock` as `0600` files.
The worker also reserves each raw-HID attachment generation in
`tablet-generation.bin` as an owner-only `0600` file before sending `DEVICE`.
That counter continues across Relay process restarts, so a reconnect does not
reuse the generation of the tablet still attached to the Host. Unsafe or
malformed counter files prevent attachment.
The allowlist is strict version-1 JSON with lowercase hexadecimal public keys:
`{"version":1,"clients":["<64 hex digits>"]}`. Existing files with unsafe
permissions, links, or malformed content fail closed. Keys enter that list
only after a completed pairing exchange; there is no network pairing endpoint
or manual bypass yet.

`tests/noise_vector.inc` is a subset of the public
[Noise-C test vectors](https://github.com/rweather/noise-c/tree/master/tests/vector)
(MIT licensed). The production Noise prologue is fixed to
`PLANK-TABLET-RELAY/1` plus a one-byte link type, so Bluetooth LE and TCP
sessions cannot be swapped.

The CPace suite is pinned to
[`draft-irtf-cfrg-cpace-21`](https://datatracker.ietf.org/doc/draft-irtf-cfrg-cpace/21/),
Appendix B.3. Its code-derived intermediate key must be confirmed in both
directions before pairing keys are stored. The confirmation construction is
specified in the PLANK plan and tested here; it is a PLANK protocol choice,
not a claim that the CFRG draft defines those tags.

Licensed under GPL-3.0-or-later, consistent with the PLANK Client worker that
will be adapted here.

Registered Relay transport selection is specified in
[Registered Relay drawing connections](docs/registered-relay-transports.md).
