# Tablet setup app and PLANK connection boundary

## Current boundary

The Setup-mediated enrollment candidate is documented in
[Setup drawing enrollment](setup-drawing-enrollment.md). It adds explicit
**Allow PLANK** approval through authenticated Setup, followed by the Client
proving the drawing identity before saving its pin. Live acceptance is pending.
The following build-36 checkpoint remains the prior handoff record.

### Vision Client build 36 checkpoint

Use a separate headset app to commission and manage the headless Relay box.
Keep PLANK's tablet UI limited to choosing a configured Relay, establishing
PLANK's own trusted connection, and showing session readiness and errors. Keep
the tablet stream bound to the PLANK desktop session. The setup app must not be
required to remain open while drawing.

Setup now registers a Relay with PLANK through an identity-bound handoff.
PLANK presents an already configured Relay picker plus a Setup entry; commissioning
and network configuration stay in Setup. The handoff does not copy private keys
or treat an address as authorization. PLANK proves/pins the drawing identity
and uses the advertised network drawing route. Labels distinguish discovery,
Setup control/preview transport, and the PLANK drawing route.

The following September 30 rollout description predates that handoff.
The guarded development services now coexist on the test Relay box; physical
Setup app handoff qualification remains open. The working `plank-tablet-relay` service listens on TCP 28990 and
forwards Wacom raw-HID reports to the PLANK Client. Alan's current
[`plank-avp-relay`](https://github.com/instinctual/plank-avp-relay) source
(main `b21f2c6d`, version 0.6.2, reviewed September 30, 2026) includes the
standalone Tablet Setup app, its shared Apple client library, and a managed
Linux service on TCP 28991. Its package documentation explicitly separates
that service from the older raw-HID protocol. The unmodified services can both
open tablet input nodes, so the shared ownership guard is required on **both**
before running them together.

## Ownership

| Component | Owns | Does not own |
| --- | --- | --- |
| Setup app | NUC discovery and commissioning, choosing USB/Bluetooth tablet, Bluetooth enrollment, network setup, detailed diagnostics, service updates | Active PLANK desktop session or pen transport |
| Headless Relay service | One tablet attachment, tablet state and permissions, approved Client identities, encrypted input stream, session lifecycle | Desktop/Flame credentials |
| PLANK Client | Select a Relay registered through Setup, verify/pin its drawing identity, show ready/failed state, start and end the tablet link with its desktop session | BlueZ pairing, Wi-Fi provisioning, NUC administration |

The first implementation step retains both ports for compatibility, but there
must be **one active tablet reader**. The managed service may report passive
USB/Bluetooth discovery and network status while PLANK streams. Pairing,
tablet selection, and live input tests must obtain the same exclusive capture
lease as the raw-HID worker. A busy setup action should say that PLANK is
using the tablet; it must not steal the device. PLANK obtains the lease before
starting its worker and releases it only after the worker stops. A single
headless authority with separate management and stream endpoints is the
cleaner final shape. Network or Bluetooth configuration requiring root should
remain behind a narrow privileged helper; the stream service currently runs
as the unprivileged `plank-relay` user.

For the two-service transition, both Linux processes bind the same abstract
Unix datagram socket, NUL followed by `plank-tablet-capture-v1`. The kernel
permits one binder; the socket closes automatically on process death. Setup
must not hold it for passive status or network management, and PLANK's idle
Pad reader must not start a physical approval window while Setup owns it.
This is a local ownership interlock, not a network authorization mechanism.

## Historical trust and discovery design — September 30, 2026

Wacom-to-NUC Bluetooth bonding is separate from PLANK-to-Relay trust. The
current PLANK Client stores its Relay identity in its device-local Keychain.
Alan's standalone setup app uses a different app identity and Keychain
namespace from PLANK. Setup approval therefore cannot silently authorize a
PLANK stream. The first coexistence release keeps PLANK's existing 28990
pairing. A later explicit handoff can have PLANK provide its public key and a
one-time request identifier, let the authenticated Setup app approve that key,
and require PLANK to prove its private key and pin the Relay identity. Preserve
existing approved 28990 Clients through a tested migration. Do not copy
private keys through preferences, URLs, or files, and do not interpret an
address passed from Setup as an authorization.

The legacy BLE store may approve the PLANK Client rather than the standalone
Setup app. Namespace migration correctly retains that approval, but it does
not authorize the separate app. The development rollout encountered this:
the migrated approval also existed in the raw Relay's Client list, while Setup
failed its secure handshake. Documented managed-service ownership recovery
backed up enrollment metadata, retained the Relay identity and tablet bonds,
and reopened enrollment for Setup. The raw service's approval and process
were retained. USB **Finish Setup** subsequently authorized standalone Setup.
Live BLE readings then reached the half-second input-backlog guard during
strokes, closing capture and releasing the lease each time. This was a transport
capacity failure, not missing approval. Network readings and physical handoff
still require acceptance; do not reset ownership to mask a transport failure.

Bonjour discovery works on a multicast-reachable local network; it did not
cross the separate subnets used in the development test. PLANK must retain a
saved address/manual fallback until a qualified cross-subnet rendezvous exists.
The managed Setup development revision 0.6.3 learns bounded listener addresses
through authenticated tablet status over Bluetooth, then pins the same Relay
identity on a TCP reading connection. It does not rely on cross-subnet Bonjour;
TCP routing must still be available. These hints identify the managed 28991
service, not the raw PLANK stream. The setup app should make the raw Relay's
stable identity and reachable endpoint easy to hand to PLANK without requiring
the user to memorize an IP address.
The current Setup app has no PLANK app link or shared Keychain group, and its
`_plank-avp-relay._tcp` advertisement is distinct from PLANK's
`_plank-tablet._tcp` advertisement. Its current app target is visionOS 27;
PLANK's existing headset Client and TestFlight distribution must be checked
separately before depending on Setup for all users.

## Rollout gates

1. The current 0.6.2 Setup app and Linux service source are available. Review
   the package's install scripts and service changes before touching the
   working NUC. The managed package starts a root service, prepares hardware,
   and may restart Bluetooth; installing it is not a passive sidecar action.
2. Implement and test a single-device ownership interlock in **both** services,
   including setup during an active PLANK session, tablet unplug/replug,
   Bluetooth reconnect, and
   service restart. Test crash cleanup and that an idle Setup service does not
   open evdev nodes. Never launch both current capture loops against the same
   Wacom without this interlock.
3. Define the Client identity handoff and its user-visible recovery/reset
   flow. Confirm the two wire protocols and identity-store formats explicitly;
   shared cryptographic primitives do not make their application protocols
   interchangeable.
4. Keep the current USB-to-NUC / TCP-to-PLANK path as the reference. Qualify
   Bluetooth-to-NUC / TCP-to-PLANK against it for tip, buttons, pressure,
   long-curve timing, sleep/wake, and repeated desktop reconnects.
5. Once setup owns commissioning and these gates pass, reduce PLANK Settings
   to a compact Relay Connection section: selected Relay, connection/identity
   status, Change Relay, and a link to Setup for device administration.
   Preserve a visible readiness state and a way to continue a desktop session
   without a tablet.

The Bluetooth-to-NUC plus TCP-to-Vision-Pro development run passed movement,
tip clicks, and pressure with far less delay than Bluetooth between NUC and
headset. USB was still the smoothest reference for long curves. See
[Bluetooth TCP setup and test](bluetooth-tablet-tcp.md).

## Safe deployment order

1. Build and test both guarded services off the live NUC service paths, then
   verify that their leases contend with each other on Linux.
2. Back up the working raw-HID executable, unit, configuration, and state.
   Install the guarded 28990 service first and repeat the USB drawing check.
3. Install the managed 28991 package only after the guarded stream is active.
   Its installer prepares radio hardware and may restart Bluetooth. Verify
   idle Setup leaves input nodes unopened while 28990 streams.
4. Exercise Setup's busy response during a PLANK session, then end the
   session and verify Setup can test the tablet. Verify the reverse handoff,
   USB/Bluetooth reconnection, and service crash cleanup.
5. Keep the known-good 28990 build and state available for rollback. Do not
   switch PLANK's trust or wire protocol as part of this deployment.

## Development rollout evidence — September 30, 2026

The guarded raw service and managed Setup development rebuild are installed.
The managed installer replaced the obsolete BLE lab package and retained its
identity separately from the raw service. Required package dependencies were
installed; both listeners and the managed service helpers are running. The raw
executable and process stayed unchanged through this installation.

Installed-code checks passed passive USB inventory without opening input,
explicit USB capture, exclusive lease contention, input descriptor closure,
and immediate lease reacquisition. During an actual PLANK desktop connection,
the raw process held the lease while the managed service had zero input
descriptors; its real status endpoint reported `captureBusy=true` and
`captureActive=false`. Before connection, the lease was free and Setup stayed
passive. With both services running, the operator confirmed immediate tip
clicks, held dragging and varying pressure over the existing raw USB/TCP path.
The latency comparison with Alan's readings tool remains subjective; no
matched timing measurement has been made. Physical Setup app busy-state,
reverse handoff and repeated recovery tests are still required; do not treat
the Linux checks as headset Setup UI acceptance.

A separate Host pressure policy was necessary for reliable fresh pen input.
Xorg's `MatchTag` consumes `ID_INPUT.tags`, so the mirrored-device udev rule
must set that property as well as its udev tag. With the corrected scoped rule,
a fresh attachment automatically disabled pressure recalibration and passed
tip clicks, held dragging and varying pressure. The first desktop connection
after a graphical-session restart still had a connection error; retry succeeded.
That startup issue remains separate and unresolved.

The matching signed Setup app and managed package are now at development
version 0.6.3. Apple model/protocol checks passed 18/18; the native Linux package
passed 33/33 and its extracted installation smoke. Synthetic network tests use
isolated lease namespaces so a production PLANK reader does not alter their
results. Installation retained managed Relay identity and headset approval,
and left the raw binary/process unchanged. The operator joined the headset-side
Wi-Fi network through Setup; its DHCP connection is saved and TCP discovery
is reachable through that interface. Physical continuous readings and both
directions of the Setup/PLANK handoff remain pending.

## Accepted wireless checkpoint — September 30, 2026

Setup development version 0.6.4 and its matching managed package passed
continuous USB and Bluetooth readings on the physical headset. Setup capture
and the raw PLANK desktop stream passed the reverse ownership handoff without
restarting either service. Earlier pending UI and handoff items above describe
their respective rollout stages.

The raw drawing service was then bound to the configured LAN interfaces. A fresh
Client connection used the Relay's Wi-Fi address; the Ethernet cable was
unplugged and the active drawing socket and route were independently confirmed
on Wi-Fi. The operator reported smooth input. This accepts the Wi-Fi drawing
path for the development setup; prolonged sessions and interruption recovery
still need qualification. Address selection in PLANK remains manual, and the
managed and drawing services retain separate protocols and trust records.

The next integration target is a versioned Setup-to-PLANK connection handoff.
No automatic identity transfer or seamless interface failover is implemented by
this checkpoint. Preserve the working capture lease and drawing protocol while
that contract is designed and tested.
