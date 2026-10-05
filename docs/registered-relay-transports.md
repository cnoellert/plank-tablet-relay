# Registered Relay drawing connections

This extension keeps the frozen `plank-drawing-handoff-v1+r3` contract and
fixtures intact. V1 remains network-only. V2 adds a Bluetooth rendezvous hint;
it never transfers Setup's connection or grants approval.

## User boundary

Setup discovers and authorizes the Relay, manages tablets and offers **Use in
PLANK**. PLANK lists registered Relays and offers Automatic, Bluetooth and
Network when those routes are known. Off opens no tablet link. Transport choices
belong to a drawing identity, persist across reconnects and change only after a
live desktop session disconnects. The former global development switch is ignored.

Automatic tries network routes in descriptor order, then Bluetooth. Bluetooth
never falls back to Network; Network never falls back to Bluetooth. Existing
network-only entries retain their approvals and routes. A fresh Bluetooth Setup
handoff adds that route to the same entry. Connection status names the route that
completed the authenticated drawing handshake, not the saved preference.

## Versioned metadata

The root-only local status request accepts version 2 in addition to the original
version 1. A ready V2 response has the same listener fields, `version: 2` and a
required boolean `bluetooth`, snapshotted from the raw daemon's successful local
Bluetooth bridge bind at startup. V1 response bytes, peer policy, four slots,
500 ms server bound and 4096-byte response limit are unchanged.

An authenticated Setup status request opts in with `drawingHandoffVersion: 2`.
The managed service makes one local status exchange with the same total 750 ms
budget. Its descriptor has `version: 2`, the existing drawing identity/protocol
and network routes. When Bluetooth is bound it adds `bluetooth: {"linkType": 1}`.
No usable network route is required if this capability is present. Malformed,
unverified or unavailable metadata stays optional and cannot break Setup.

Setup adds `peripheralIdentifier`, a nonzero lowercase UUID from a Bluetooth
route whose private Setup pin matches the authenticated management identity.
The app link uses `plank-vision://handoff/v2?d=...`. Its Bluetooth object contains
exactly `linkType: 1` and `peripheralIdentifier`; the remaining seven members are
the V1 members with version 2. Routes contain zero to eight entries; zero is
allowed only with Bluetooth. All original route/identity/duplicate/length checks
apply. A link path and payload version must agree. Without a verified Bluetooth
hint, Setup emits a network V1 link, or declines a route-less handoff.

The UUID is a same-headset rendezvous hint, not a portable address or trust
anchor. PLANK retrieves/scans for that identifier only. Changed or unavailable
identifiers require a new Setup handoff; names and nearby devices are never
substituted. Successful resolution across the two apps remains a live acceptance
check. Every drawing connection proves the independently approved raw identity;
Bluetooth uses Noise link type 1, network uses link type 2.

## First-time approval

The existing Setup approval receipt and single-use PLEN/1 grant remain unchanged.
Client proof can run over either drawing byte transport, including Bluetooth
without network routes. Connection attempts share a 40-second deadline, with a
six-second network attempt bound. Once proof bytes are sent no alternate route
may replay that grant. The raw Bluetooth bridge recognizes PLEN/1 separately
from drawing and never opens a physical approval window. Proof does not create a
capture worker. Raw HID and reverse workstation control semantics are unchanged.

## Candidate qualification

Source checks and signed app builds precede one live pass: Setup over Bluetooth,
Use in PLANK, Bluetooth drawing and reconnect, Network drawing, then Automatic
with an unavailable network route. No new Host package is required. Complete
package rebuilding with the final published raw pin and live migration remain
separate from the development file installation. Existing package CI results do
not qualify this extension or fresh first-time Bluetooth-only enrollment.
