# Relay drawing handoff contract

Contract revision: `plank-drawing-handoff-v1+r3`

This document is the frozen version 1 contract for handing a verified drawing
Relay connection from the standalone Setup app to PLANK. It is normative. Every
consumer — the raw drawing Relay, the managed Relay service, the Setup app and
the native PLANK Client — implements exactly what is written here. Shared
positive and negative test vectors live in
`tests/fixtures/plank-drawing-handoff-v1/` of this repository and are mirrored
byte-for-byte into the Client and Setup repositories.

Keywords MUST, MUST NOT, SHOULD and MAY carry their usual normative force.

A protocol revision is a change to this document. It happens here first and is
reviewed before any consumer edit. Do not widen a bound, add a field, relax a
rejection, or rename a reason identifier in a consumer repository.

Revision history, kept short deliberately:

* `r1` — first freeze. **Superseded. Do not implement r1.** Its local IPC design
  was unreachable under the installed service restrictions (§1.8), it conflated
  the local listener metadata with the managed descriptor, and its version-check
  ordering was ambiguous.
* `r2` — superseded. Redesigned local IPC with a measured proof, three separate
  validation entry points, explicit conversion from listener metadata to routes,
  raw-byte duplicate-member detection, canonical base64url enforcement, and
  corrected version ordering. Its production server-authorization text and its
  account-resolution rule were both wrong; see below.
* `r3` — current. Six bounded corrections: production server authorization is
  **uid 0 only** and any test policy is injected rather than configurable (§8.3);
  the raw service account is resolved **by name**, with directory ownership as
  corroboration only and fail-closed behaviour (§8.3a); the privileged harness is
  made safe to run — unique run identifiers, exclusive creation, validated
  overrides, cleanup restricted to what it created, interruption handling and
  verified cleanup that affects the exit status (§8.5); every proof assertion is
  made decisive (§8.5); the non-object `descriptor`/`listener` cases, the
  name-collision outcome and the status raw-bytes rule are specified (§7.3, §7.4,
  §8.4); and the evidence and publication records are corrected (§8.5, §13).

## 0. Pinned source provenance

Every `file:line` citation in this document resolves at exactly these commits.

| Repository | Commit | Role |
| --- | --- | --- |
| `cnoellert/plank-tablet-relay` (raw drawing Relay) | `e9e0e1cbe22d42839db3db1b5fd509823bdb51cf` | canonical home of this contract and the shared fixtures |
| `cnoellert/plank-client` (native PLANK) | `1ab229ce4740b6b79ae367bef527d487c9e0827f` | fixture mirror |
| `cnoellert/plank-avp-relay` (managed Relay and Setup) | **`7b477b3c75d91fbde667e5b4df4c46a36ea15219`**, branch `codex/managed-capture-lease-onto-main` | **current managed source**; all managed citations below are pinned here |
| — upstream base of that rebase | `812bae39bfa54c30f06961962e6ab51845cf3752` | context |
| — pre-rebase provenance | `5a915a7cc817b39c150e32975643ae94f9c25e78` | PR #1 head, preserved and untouched. **Not** the current source; cite it only as history. |

The managed work was rebased onto current upstream `main`. Every managed file this
contract cites is **byte-identical across that rebase** — `tools/avp_relay/local_service.py`,
`tools/avp_relay/core.py`, `tools/avp_relay/network_routes.py`,
`apple/RelaySetupKit/TabletManagement.swift` and `debian/plank-avp-relay.service`
— and every line citation still resolves exactly: `local_service.py:118` is still
`os.chmod(path, 0o600)`, `local_service.py:136` is still the `SO_PEERCRED`
unpack, `debian/plank-avp-relay.service:18` is still `User=root`, and `:32` is
still `RestrictAddressFamilies=AF_UNIX AF_BLUETOOTH AF_INET AF_INET6`. **The
managed unit is unchanged, so the §8 IPC design and its capability reasoning
survive the rebase intact.** Only the commit identifier changed.

## 1. Confirmed current formats

Every statement below was read from executable source at the pinned commits of §0,
or measured on real hardware where §1.8 says so. Nothing here is inferred from
documentation.

### 1.1 Raw drawing service identity

| Property | Value | Source |
| --- | --- | --- |
| Identity key type | Noise static key pair, X25519-shaped, 32-byte private and 32-byte public | `src/identity.h:16-19`, `src/noise.h:11` |
| Private key storage | `identity.key`, raw 32 bytes, mode `0600`, in a `0700` directory owned by the service account; fails closed on permissive modes | `src/identity.c:16`, `src/identity.c:21-26`, `src/identity.c:148-171` |
| Public key derivation | `pltr_noise_public_key` from the stored private key; never persisted separately | `src/identity.c:172-174` |
| Approved Client allowlist | `paired-clients.json`, `{"version":1,"clients":["<64 lowercase hex>",…]}`, at most 16 entries, strict parser | `src/identity.c:90-142`, `src/identity.h:11` |
| Canonical public-key text form | **lowercase hexadecimal, exactly 64 characters, exactly 32 bytes** | `src/identity.c:124`, `src/identity.c:132-135` |
| Discovery fingerprint (`rid`) | first 8 bytes of BLAKE2b-256 over the 32-byte public key, lowercase hex, 16 characters; advisory only | `src/dnssd.cpp:12-22` |
| Discovery service type | `_plank-tablet._tcp`, TXT `v=1`, `rid=<16 hex>`, `pair=0`/`pair=1`; advisory only | `src/dnssd.cpp:33-37`, `src/dnssd.cpp:106-110` |

### 1.2 Raw drawing protocol

| Property | Value | Source |
| --- | --- | --- |
| Frame magic | `PLTR_MAGIC` = `0x504c5452` (ASCII `PLTR`) | `src/protocol.h:11` |
| Protocol version | `PLTR_VERSION` = `1` | `src/protocol.h:12` |
| Header size | 16 bytes | `src/protocol.h:13` |
| Maximum payload | 8192 bytes; maximum frame 8208; maximum record body 8224 | `src/protocol.h:14-16` |
| Record framing | 2-byte little-endian length prefix, then the frame or Noise ciphertext | `src/link.c:22-30`, `src/link.c:60-64` |
| HELLO body | 18 bytes: minimum version LE16 = 1, maximum version LE16 = 1, role byte (1 Relay, 2 Client), raw-HID capability LE32 at offset 8 = `1` (named `RAW_HID_V2` in source), version-string length byte, version string | `src/link.c:68-78` |
| Link type | `1` Bluetooth LE, `2` TCP; bound into the Noise prologue so a cross-link session is impossible | `src/noise.h:33-37` |
| Noise handshake sizes | message one 96 bytes, message two 48 bytes | `src/link.c:160-161`, `src/link.c:184-186` |
| Approval point | the responder refuses to emit message two unless the decrypted Client static key is already in the allowlist | `src/link.h:22-25`, `src/link.c:164`, `src/noise.h:45-46` |

### 1.3 Raw drawing listener

| Property | Value | Source |
| --- | --- | --- |
| Socket family | **`AF_INET` only** — a single IPv4 literal is parsed with `inet_pton(AF_INET, …)` | `src/main.cpp:63-78` |
| Bind request | `--bind <IPv4 literal>`, default `127.0.0.1`; set in production through `PLANK_RELAY_BIND` in `/etc/default/plank-tablet-relay` | `src/main.cpp:114`, `src/main.cpp:123-124`, `packaging/plank-tablet-relay.service:10-12`, `packaging/plank-tablet-relay.env.example` |
| Port | `--port 1..65535`, default `28990` | `src/main.cpp:115`, `src/main.cpp:125-134` |
| Backlog | 1 | `src/main.cpp:73` |
| Service account | `plank-relay:plank-relay`, `StateDirectory` mode `0700`, `UMask=0077`, `ProtectSystem=strict` | `packaging/plank-tablet-relay.service:8-23` |
| Capture interlock | abstract `AF_UNIX` datagram name `\0plank-tablet-capture-v1`, process-lifetime, no stale lock file | `src/capture_lease.hpp:9`, `src/capture_lease.cpp:28-38` |

**This is the most important correction to the plan's assumptions.** The plan's
descriptor table says "at most eight TCP address/port candidates". The raw
drawing listener binds exactly one IPv4 literal and has no IPv6 socket at all.
Two consequences are frozen here:

1. §9 states how the managed service converts one configured listener into
   between zero and eight routes without inventing an address the listener does
   not answer on.
2. **Version 1 `routes` are IPv4 only.** An IPv6 literal is refused
   (`route.address.familyUnsupported`, §7.6). Version 1 does not introduce a new
   address family. A parser must still *recognise* an IPv6 literal so it can
   report a precise reason, but it never accepts one.

### 1.4 Managed service authenticated status

| Property | Value | Source |
| --- | --- | --- |
| Managed identity text form | lowercase hex, 64 characters, 32 bytes, returned as `relayKey` with `enrollmentVersion: 1` | `tools/avp_relay/native.py:112-116`, `tools/avp_relay/core.py:82-83` |
| Owner-only route hints | `tcpPort` and `networkAddresses` are added to the `op:"status"` response only when `authenticated and peer == self.owner` | `tools/avp_relay/core.py:87-89` |
| Status envelope | `{"version":1,"id":<int>,"ok":<bool>,…}` | `tools/avp_relay/tablets.py:131`, `tools/avp_relay/tablets.py:226` |
| Response bound | 4096 bytes; `candidates` are shed one at a time to fit, then the response fails | `tools/avp_relay/core.py:90-95` |
| Local address collection | `usable_addresses` refuses unspecified, loopback, multicast, link-local, `0/8`, `>= 224.0.0.0`, IPv4-mapped IPv6; caps at 8; deduplicates | `tools/avp_relay/network_routes.py:10-28` |
| Interface enumeration | `/sys/class/net` names, `SIOCGIFFLAGS`/`SIOCGIFADDR` on an `AF_INET` socket, `/proc/net/if_inet6` for global IPv6; deliberately avoids `AF_NETLINK` | `tools/avp_relay/network_routes.py:31-63` |
| Strict command key set | `if set(command) != expected: raise ValueError('Invalid network command fields.')` | `tools/avp_relay/core.py:59-62` |

### 1.5 Established local IPC mechanism

The managed repository has exactly one local-IPC idiom, used by the USB network
helper and the Wi-Fi helper. Version 1 reuses its **shape** — see §8 for why it
cannot reuse its filesystem placement.

| Property | Value | Source |
| --- | --- | --- |
| Transport | `AF_UNIX` `SOCK_STREAM`, one newline-terminated JSON object per direction | `tools/avp_relay/local_service.py:116-157`, `tools/avp_relay/gadget_client.py:23-41` |
| Socket location | `/run/plank-avp-relay/<helper>/control.sock`, parent directory mode `0700` | `tools/avp_relay/gadget_client.py:6`, `tools/avp_relay/local_service.py:100` |
| Socket permissions | `os.chmod(path, 0o600)` after bind | `tools/avp_relay/local_service.py:118` |
| Peer authorization | `SO_PEERCRED`; a non-zero uid gets no response at all | `tools/avp_relay/local_service.py:136-137` |
| Request bound | 1024 bytes including the newline | `tools/avp_relay/local_service.py:145`, `tools/avp_relay/gadget_client.py:25-26` |
| Response bound | 4096 bytes including the newline | `tools/avp_relay/local_service.py:155`, `tools/avp_relay/gadget_client.py:34-36` |
| Concurrency | backlog 4, at most 4 handlers, 0.2 s accept timeout, 0.5 s per-connection I/O timeout, 0.75 s client timeout | `tools/avp_relay/local_service.py:109-127`, `:138`, `tools/avp_relay/gadget_client.py:29` |
| Absence handling | `FileNotFoundError`/`ConnectionRefusedError` on a status read returns a declared `unavailable` fallback; any other `OSError` is a distinct busy condition, never "unsupported" | `tools/avp_relay/gadget_client.py:42-49` |

Those helpers work because **they run in the same unit tree as their client**:
`plank-avp-relay-wifi.service` and `plank-avp-relay-usb.service` both run
`User=root` (`packaging/ble/plank-avp-relay-wifi.service:19`), so a `0600`
root-owned socket is reachable by a root client. The raw drawing service runs
`User=plank-relay`. That difference is the whole problem, and §1.8 measures it.

### 1.6 Existing Apple-side decoding and bounds

| Property | Value | Source |
| --- | --- | --- |
| Status payload bound | 4096 bytes checked before decoding | `apple/RelaySetupKit/TabletManagement.swift:50` |
| Envelope gate | `version == 1` and `id == request` before any field use | `apple/RelaySetupKit/TabletManagement.swift:52` |
| Address bounds | at most 8 `networkAddresses`, each at most 64 UTF-8 bytes | `apple/RelaySetupKit/TabletManagement.swift:63-64` |
| Port bound | `tcpPort` must be greater than 0 | `apple/RelaySetupKit/TabletManagement.swift:62` |
| Identity decoding | `relayKey` must be exactly 64 UTF-8 bytes of hex, parsed two characters at a time, gated on `enrollmentVersion == 1` | `apple/RelaySetupKit/TabletManagement.swift:86-96` |
| Authorization gate for routes | `headsetAuthorized == true` and `enrollmentIdentity == relayKey` before any route is produced | `apple/RelaySetupKit/TabletManagement.swift:70-73` |
| Route acceptance | port > 0, key exactly 32 bytes, at most 8 hosts; IPv4 first octet not 0, not 127, below 224, not `169.254/16`; IPv6 no `%`, first byte not `0x00`, not `0xff`, not `fe80::/10`; deduplicated | `apple/RelaySetupKit/RelayNetworkSettings.swift:9-26` |
| Keychain account form | `relay-network-v1:<64 lowercase hex>` for network trust, `relay-ble-v1:<lowercase UUID>` for Bluetooth trust | `apple/RelaySetupKit/SetupState.swift` (`RelayAddress.keychainAccount`) |
| Tolerant status fields | newer members are optional so an older relay stays decodable | `apple/RelaySetupKit/TabletManagement.swift:32-46` |

### 1.7 Client saved drawing identity

| Property | Value | Source |
| --- | --- | --- |
| Keychain service | `la.instinctual.PLANK.Vision.tabletRelay.v1` | `visionos-native/Sources/Services/PlankRelayPairing.swift:29` |
| Client private key account | `client-private`, 32 bytes, device-local, never exported | `visionos-native/Sources/Services/PlankRelayPairing.swift:65-75` |
| Pin accounts today | `relay-<address>:<port>` for a manual endpoint, `relay-service-<name>.<domain>` for a discovered service | `visionos-native/Sources/Services/PlankRelayPairing.swift:79-85` |
| Pin validity test | the stored value must be exactly 32 bytes | `visionos-native/Sources/Services/PlankRelayPairing.swift:121-123` |
| Cross-label pin copying already happens | `useSavedPairing` and `useManualPairing` copy a pinned key between the two account labels | `visionos-native/Sources/Services/PlankRelayPairing.swift:391-455` |
| Selection state | `UserDefaults` keys `plank.vision.relayMode`, `plank.vision.relayServiceName`, `plank.vision.relayServiceDomain`, `plank.vision.relayAddress`, `plank.vision.relayPort` | `visionos-native/Sources/Services/PlankRelayPairing.swift:87-118` |
| Registered URL schemes | **none** — `visionos-native/Info.plist` has no `CFBundleURLTypes` key at this checkpoint | `visionos-native/Info.plist` |

### 1.8 Measured service constraints

These were measured, not reasoned about. The host was a **Rocky Linux 9.7 /
systemd 252 / x86_64 workstation** ("proof host" below). Rocky Linux is **not the
Ubuntu Relay target** and nothing here should be read as equivalent to it; these
results establish Linux kernel and systemd behaviour only (§8.5). The client
side ran under a transient unit carrying the managed service's restrictions
copied verbatim from `debian/plank-avp-relay.service:18-35`; the server side ran
as a different, unprivileged uid under the raw service's restrictions from
`packaging/plank-tablet-relay.service:8-23`. §8.5 records the commands and
output.

Separately, the **installed development Relay NUC** (Ubuntu 26.04.1, systemd 259)
was inspected read-only. This is the real Relay target and it confirms the same
facts at runtime rather than only in a unit file:

| Runtime observation on the installed Ubuntu Relay | Significance |
| --- | --- |
| `systemctl show plank-avp-relay` reports `CapabilityBoundingSet=cap_net_admin cap_net_raw`, `User=root`, `PrivateTmp=yes`, `ProtectSystem=strict` | The **installed** managed service has no `CAP_DAC_OVERRIDE`. It therefore cannot traverse a `0700` directory or open a `0600` socket owned by `plank-relay`. This is installed reality, not a proposal, and it is the decisive reason r1 is withdrawn. |
| `systemctl show plank-tablet-relay` reports `User=plank-relay`, `PrivateTmp=yes`, `ProtectSystem=strict`, `ActiveState=active` | The raw drawing service really does run under a different, unprivileged account, and it is running. The uid asymmetry of §1.5 is live. |
| The raw drawing service is listening on `0.0.0.0:28990` | **Wildcard bind is the production reality.** See §9: wildcard expansion is the primary conversion path, not an edge case. |
| `28991` listens on both IPv4 and IPv6 on that host | Context only. That is the **managed** channel, separate from the `28990` drawing channel. It does **not** license widening the drawing contract to IPv6 (§7.6). |

| Fact | Measurement |
| --- | --- |
| The managed service has no DAC-override capability | `debian/plank-avp-relay.service:19` is `CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW` with no `AmbientCapabilities=`, and the installed service confirms it at runtime (above). The restricted client observed `CapEff=0000000000003000`, exactly bits 12 and 13 (`CAP_NET_ADMIN`, `CAP_NET_RAW`). `CAP_DAC_OVERRIDE` (bit 1) and `CAP_DAC_READ_SEARCH` (bit 2) are absent, so uid 0 is subject to ordinary permission checks. |
| A `0700` directory plus `0600` socket owned by the raw service account is **unreachable** | `connect()` returned `EACCES(13)`. This is the r1 design and it does not work. |
| A world-accessible filesystem socket **is** reachable under the same restrictions | With `RuntimeDirectoryMode=0755` and socket mode `0666`, `connect()` succeeded. So `ProtectSystem=strict` and `PrivateTmp=yes` do not block a filesystem-socket connect; the r1 failure is purely DAC. |
| An **abstract** `AF_UNIX` socket is reachable | Server bound `\0<name>` as uid 65534; the restricted root client connected and read the response. |
| An abstract name has no access control | A second process binding the same abstract name got `EADDRINUSE(98)`. First binder wins, and any process in the namespace may be that binder or a client. |
| Peer credentials are available and sufficient on both ends | The server read `SO_PEERCRED` and refused an unprivileged peer by closing with zero bytes. The client read `SO_PEERCRED` on its connected socket and refused a squatting server at uid 62462. |
| Expected-uid discovery works without any new permission | Under the exact managed restrictions, `os.stat('/run/<rt-dir>')` on a `0700` directory owned by another uid returned `uid=65534 mode=700`, while `os.listdir` on the same directory failed `EACCES`. The managed service can learn the owner uid without being able to read anything inside. |

## 2. Three validation entry points

There are three distinct schemas. r1 conflated the first two and that was a real
defect: a status descriptor has no `requestID`, and requiring one would make
every honest status response invalid. Each entry point has its own required,
optional and forbidden member sets, and its own ordered check list. A consumer
MUST select the entry point from the channel the bytes arrived on, never guess
from content.

| # | Entry point | Channel | Validated by |
| --- | --- | --- | --- |
| 1 | **Local listener metadata** | raw drawing service → managed service, over the local status socket (§8) | the managed service |
| 2 | **Managed status descriptor** | managed service → Setup, inside the authenticated `op:"status"` response (§6.4) | Setup, and the managed service before it emits |
| 3 | **App-link descriptor** | Setup → PLANK, through the app link (§6) | PLANK |

Member sets at a glance. `—` means the member MUST NOT appear; a member that
appears where it is forbidden is an unknown member for that entry point.

| Member | 1 local metadata | 2 status descriptor | 3 app-link descriptor |
| --- | --- | --- | --- |
| `version` | required, in the **envelope**, exactly 1 | **required**, inside `descriptor`, exactly 1 | **required**, exactly 1 |
| `ok` | required (envelope), exactly `true` | — | — |
| `supported` | required (envelope), boolean | required, boolean | — |
| `state` | required (envelope), `ready` or `unavailable` | required, `ready` or `unavailable` | — |
| `reason` | required when `state` is `unavailable` | required when `state` is `unavailable` | — |
| `listener` | required when `state` is `ready` | — | — |
| `descriptor` | — | required when `state` is `ready` | — |
| `drawingIdentity` | required, inside `listener` | required, inside `descriptor` | required |
| `drawingProtocol` | required, inside `listener` | required, inside `descriptor` | required |
| `boundAddress` | required, inside `listener` | — | — |
| `boundPort` | required, inside `listener` | — | — |
| `routes` | — | required, inside `descriptor` | required |
| `requestID` | — | — | **required** |
| `displayName` | — | — | required |
| `managementIdentity` | — | — | required |

Reason identifiers are namespaced so a fixture pins which entry point produced
it: `metadata.*` for entry point 1, `status.*` for entry point 2, `link.*` and
`url.*`/`encoding.*` for entry point 3. Identifiers for members that are shared
across entry points — `drawingIdentity.*`, `drawingProtocol.*`, `routes.*`,
`route.*` — and the byte-level identifiers `payload.*` are shared, because the
check is literally the same check.

Setup composes the app-link descriptor from the status descriptor plus
`version`, a **freshly minted** `requestID`, a `displayName`, and the
`managementIdentity` it already holds as `enrollmentIdentity`
(`apple/RelaySetupKit/TabletManagement.swift:86-96`). Setup MUST NOT copy a
`requestID` or `displayName` out of a relay response, and MUST NOT expect either
to be present in one.

## 3. Local listener metadata (entry point 1)

The raw drawing service reports the truth about its listener. **Wildcard
(`0.0.0.0`) and loopback (`127/8`) are permitted here, and only here**,
because they are what the listener is actually bound to. Route-level refusals
apply to the *converted* routes (§9), not to this metadata.

The whole response object is validated. Envelope members, exactly these:

| Member | JSON type | Required value | Reject identifier |
| --- | --- | --- | --- |
| `version` | number | integer, exactly 1 | `metadata.envelope.version.missing`, `metadata.envelope.version.type`, `metadata.envelope.version.unsupported` |
| `ok` | boolean | exactly `true`. A response with `ok: false` is an error reply, not metadata, and is handled as `service.invalid`. | `metadata.envelope.ok.missing`, `metadata.envelope.ok` |
| `supported` | boolean | `true` or `false` | `metadata.envelope.supported.missing`, `metadata.envelope.supported.type` |
| `state` | string | exactly `ready` or `unavailable` | `metadata.envelope.state.missing`, `metadata.envelope.state` |
| `listener` | object | present if and only if `state` is `ready` | `metadata.envelope.listener.missing`, `metadata.envelope.listener.unexpected` |
| `reason` | string | present if and only if `state` is `unavailable`; 1 to 64 UTF-8 bytes, characters `[a-zA-Z.]` only | `metadata.envelope.reason.missing`, `metadata.envelope.reason.unexpected`, `metadata.envelope.reason` |

Any other envelope member is `metadata.unknownMember`. In particular `routes`,
`requestID`, `displayName` and `managementIdentity` are forbidden anywhere in
this response.

The `listener` object, exactly these four members, all required:

| Member | JSON type | Encoding and bounds | Reject identifier |
| --- | --- | --- | --- |
| `drawingIdentity` | string | 64 lowercase hexadecimal characters, 32 bytes (§5.1) | `drawingIdentity.*` |
| `drawingProtocol` | object | exactly as §5.2 | `drawingProtocol.*` |
| `boundAddress` | string | 7 to 15 UTF-8 bytes, a canonical IPv4 dotted-quad; accepted iff it is exactly `0.0.0.0`, or its first octet is neither `0` nor in `224..255` | `metadata.boundAddress.*` |
| `boundPort` | number | integer, 1 to 65535 (`src/main.cpp:129-130`) | `metadata.boundPort.missing`, `metadata.boundPort` |

`boundAddress` identifiers: `.missing`, `.type`, `.tooLong`, `.notLiteral`
(including host names), `.familyUnsupported` (any IPv6 literal),
`.nonCanonical`, and `.invalid` for a literal in a class a listener cannot
usefully bind — `0/8` other than the unspecified address itself, `224/4`,
`240/4` and the all-ones broadcast address. Loopback and `169.254/16` are
**accepted** here; they are legal binds and they are refused later, at
conversion, with a specific reason.

Any other member inside `listener` is `metadata.unknownMember`.

## 4. Managed status descriptor (entry point 2)

What the managed service places in its authenticated status, and what Setup
decodes. The wrapper, exactly these members:

| Member | JSON type | Required value |
| --- | --- | --- |
| `supported` | boolean | `true` on a relay that implements this contract |
| `state` | string | exactly `ready` or `unavailable` |
| `descriptor` | object | present if and only if `state` is `ready` |
| `reason` | string | present if and only if `state` is `unavailable`; one of the identifiers in §11 |

Any other wrapper member is `status.unknownMember`. The wrapper itself carries no
`version`; the descriptor does.

The `descriptor` object has **exactly four** members, all required:
`version` (integer, exactly 1), `drawingIdentity`, `drawingProtocol` and
`routes` — that is, exactly what the managed service genuinely needs to describe
a usable drawing endpoint.

`requestID`, `displayName` and `managementIdentity` are **forbidden**. All three
are not optional, not ignored, and above all **not required**: requiring any of
them would make every honest status response invalid, which was the r1 defect.
`requestID` exists only per app link and is minted by Setup at the moment of the
tap. `managementIdentity` is omitted because it is already in the same status
response as `relayKey` (`tools/avp_relay/core.py:82-83`). `displayName` is
omitted because `hostname` is already there
(`tools/avp_relay/tablets.py:131`). A descriptor carrying any of the three is
`status.unknownMember`.

`version` is carried **inside** the descriptor even though the surrounding status
response already has its own `version: 1`
(`tools/avp_relay/tablets.py:131`). The two version different things: the
outer one versions the tablet-setup status protocol, the inner one versions this
handoff subtree. A relay and a Setup app can therefore disagree about the handoff
subtree without either having to lie about the outer envelope, and the
version-privileged check ordering of §7.1 applies uniformly at all three entry
points.

`drawingIdentity`, `drawingProtocol` and `routes` are validated exactly as in
§5.1, §5.2 and §5.3. **Wildcard and loopback addresses can never appear here**:
they are legal only in the raw local listener metadata (§3) and are resolved or
refused during conversion (§9).

## 5. Shared field definitions

### 5.1 Identities

`drawingIdentity` and `managementIdentity` are each a string of **exactly 64
lowercase hexadecimal characters** decoding to 32 bytes. Uppercase hexadecimal
is refused. Base64 in any form is refused.

One canonical form means a 32-byte comparison against a saved pin can never
disagree with a string comparison, and it matches both existing surfaces
(`src/identity.c:124`, `apple/RelaySetupKit/TabletManagement.swift:86-96`).
Identifiers: `.missing`, `.type`, `.invalid`.

Where both appear (entry point 3), `drawingIdentity` MUST differ from
`managementIdentity`; equality is `drawingIdentity.collidesWithManagement`.

### 5.2 `drawingProtocol`

Exactly these four members, all required, no others.

| Member | JSON type | Required value | Reject identifier |
| --- | --- | --- | --- |
| `name` | string | exactly `"pltr-raw-hid"`, spelling out the on-wire magic `PLTR` (`src/protocol.h:11`) in a form safe for JSON and logs | `drawingProtocol.name` |
| `version` | number | integer, exactly `1` — `PLTR_VERSION` (`src/protocol.h:12`) | `drawingProtocol.version` |
| `rawHID` | number | integer, exactly `1` — the raw-HID capability value written at HELLO offset 8 (`src/link.c:74`). The source comment names that value `RAW_HID_V2`; the wire value is `1` and the wire value is what this field carries. | `drawingProtocol.rawHID` |
| `linkType` | number | integer, exactly `2` — TCP (`src/noise.h:33`). `1` means Bluetooth LE and MUST be refused: a drawing handoff is a TCP handoff and the link type is bound into the Noise prologue, so accepting `1` would guarantee a failed handshake. | `drawingProtocol.linkType` |

A missing member is `drawingProtocol.<member>.missing`. Any unexpected member is
`drawingProtocol.unknownMember`. A non-object is `drawingProtocol.type`.

### 5.3 `routes` and `routes[i]`

`routes` is an array of 1 to 8 elements (`routes.type`, `routes.count`,
`routes.missing`). Each element is an object (`route.type`) with exactly these
members; `address` and `port` are required, `interface` and `kind` are optional.
JSON `null` is never an accepted value for an optional member; absence is
expressed by absence (`route.nullMember`).

| Member | JSON type | Encoding and bounds | Reject identifier |
| --- | --- | --- | --- |
| `address` | string | 1 to 64 UTF-8 bytes (matching `apple/RelaySetupKit/TabletManagement.swift:64`). A canonical IPv4 dotted-quad from an accepted class (§7.6). IPv6 is refused in version 1. | `route.address.*` |
| `port` | number | integer, 1 to 65535 | `route.port.missing`, `route.port` |
| `interface` | string | 1 to 15 UTF-8 bytes, characters `[A-Za-z0-9._-]` only. The kernel interface name; 15 is `IFNAMSIZ - 1`, the same truncation already applied at `tools/avp_relay/network_routes.py:46`. | `route.interface` |
| `kind` | string | exactly one of `wired`, `wireless`, `other` | `route.kind` |

Any other member is `route.unknownMember`.

## 6. App link (entry point 3)

### 6.1 Exact form

```
plank-vision://handoff/v1?d=<payload>
```

| Component | Frozen value |
| --- | --- |
| Scheme | `plank-vision`, compared after ASCII-lowercasing (RFC 3986 makes the scheme case-insensitive). Registered by the Client in `visionos-native/Info.plist` under `CFBundleURLTypes`; there is no existing scheme to preserve (§1.7). |
| Host | exactly `handoff` |
| Path | exactly `/v1` |
| Query | exactly one item, name exactly `d`, value non-empty |
| Fragment | MUST be absent |
| User info, port | MUST be absent |
| Payload encoding | base64url **without padding**, RFC 4648 §5 alphabet `A–Z a–z 0–9 - _`, in **canonical** form (§6.3) |
| Payload content | the UTF-8 JSON descriptor of §6.5 |

The version lives in the path as well as in the descriptor. A future
`plank-drawing-handoff-v2` uses path `/v2`, so a version 1 parser refuses it at
the path check without decoding an unknown payload. The in-payload `version` is
checked too, so a mislabelled payload cannot slip through either.

Base64url without padding is chosen because `=` must be percent-encoded in a
query component, `+` is commonly decoded as a space in query components, and `/`
invites path confusion. Removing padding removes the last degree of freedom, so
one descriptor has exactly one legal encoding. Percent-escapes inside the `d`
value are a rejection, not something to decode first: there is nothing legal to
escape.

### 6.2 Length limits

| Limit | Value | Reason |
| --- | --- | --- |
| Decoded payload | at most **4096 bytes** | Fixed by the plan, and equal to the managed response bound already enforced at `tools/avp_relay/core.py:90-95` and `apple/RelaySetupKit/TabletManagement.swift:50`. |
| Decoded payload minimum | 2 bytes | The shortest possible JSON object. |
| Encoded `d` value | 1 to **5462** characters | `ceil(4096 × 4 / 3) = 5462` is the exact unpadded base64url length of a 4096-byte payload. This is the matching encoded-length guard: it is checked **before** decoding, so an oversized link never allocates a decode buffer. |
| Whole absolute URL | at most **5490 bytes** | `len("plank-vision://handoff/v1?d=") = 28` plus 5462. Checked first, before any component is examined. |
| Encoded length modulo 4 | MUST NOT be 1 | No unpadded base64 string has length ≡ 1 (mod 4). |

The guard is tight by construction: no URL that passes the 5490-byte check can
decode to more than 4096 bytes, because 5462 base64url characters decode to
exactly 4096 bytes. The 4096-byte limit therefore applies to the **decoded
compact payload**, and nothing else needs a second length check.

### 6.3 Canonical base64url

A permissive base64 decoder ignores the unused low bits of the final quantum, so
two different `d` values can decode to the same bytes. That is a second
canonicalisation hole and it is closed here.

* A final quantum of **2 characters** encodes 1 byte. The low **4** bits of the
  second character's alphabet index MUST be zero.
* A final quantum of **3 characters** encodes 2 bytes. The low **2** bits of the
  third character's alphabet index MUST be zero.

The required check, and the one consumers SHOULD implement because it needs no
bit arithmetic: **re-encode the decoded bytes as unpadded base64url and compare
the result to the original `d` value byte-for-byte.** Any difference is
`encoding.nonCanonical`.

Measured: Python's `base64.urlsafe_b64decode` accepts `'AR'` and `'AQF'` and
returns the same bytes as the canonical `'AQ'` and `'AQE'`, while the re-encode
comparison rejects both. Swift's `Data(base64Encoded:)` is equally permissive
about pad bits. Counting on a decoder to reject these is not sufficient.

### 6.4 The canonical example is semantic, not byte-identical

`link/valid/known-identity.url` and `link/valid/known-identity.json` describe the
**same descriptor** but are **not the same bytes**:

| Artefact | Size | Form |
| --- | --- | --- |
| `link/valid/known-identity.json` | **650 bytes** | pretty-printed, two-space indent, trailing newline |
| the `d` payload decoded from `link/valid/known-identity.url` | **506 bytes** | compact, no insignificant whitespace |
| the `d` value itself | 675 characters | unpadded base64url |

Both parse to equal objects. Consumers MUST compare **parsed values**, never file
lengths and never raw bytes, when relating a `.url` fixture to its `.json`
sibling. A test that asserts the decoded payload equals the `.json` file byte-for-byte
is wrong and will fail. The 4096-byte limit applies to the decoded compact
payload (506 bytes here), not to the pretty-printed fixture.

### 6.5 Descriptor members

Exactly these seven members, all required, no others.

| Member | JSON type | Encoding and bounds | Reject identifier |
| --- | --- | --- | --- |
| `version` | number | integer, exactly `1` | `version.missing`, `version.type`, `version.unsupported` |
| `requestID` | string | exactly 36 characters matching `^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`, lowercase only. Duplicate handling only (§10.5). **Not an authorization token.** | `requestID.missing`, `requestID.type`, `requestID.invalid` |
| `displayName` | string | 1 to 64 UTF-8 bytes; no code point below U+0020, no U+007F, not entirely white space. Display only; never used for trust, matching or storage keys. | `displayName.missing`, `displayName.type`, `displayName.invalid` |
| `managementIdentity` | string | §5.1 | `managementIdentity.*` |
| `drawingIdentity` | string | §5.1, and MUST differ from `managementIdentity` | `drawingIdentity.*` |
| `drawingProtocol` | object | §5.2 | `drawingProtocol.*` |
| `routes` | array | §5.3 | `routes.*`, `route.*` |

Any other member is `link.unknownMember`.

## 7. Validation rules

Order is normative. Every consumer evaluates the checks in this order and
reports the **first** failing identifier, so three independent implementations
reject the same input for the same reason. Fixtures are built so that exactly one
check fails per negative fixture.

**No network activity, no Keychain read or write, no Bonjour query, no DNS
resolution and no socket creation may occur before validation completes.**
Validation is pure and testable on any platform.

### 7.1 App-link ordered checks (entry point 3)

URL stage:

1. `url.length` — absolute URL at most 5490 bytes.
2. `url.scheme` — ASCII-lowercased scheme equals `plank-vision`.
3. `url.host` — host equals `handoff`; no user info, no port.
4. `url.path` — path equals `/v1`.
5. `url.fragment` — no fragment.
6. `url.query` — exactly one query item; its name is exactly `d`; its value is
   non-empty. Zero items, two or more items, a different name, a repeated `d`, or
   an empty value all fail here.

Encoding stage:

7. `encoding.length` — the `d` value is 1 to 5462 characters and its length
   modulo 4 is not 1.
8. `encoding.alphabet` — every character is in `[A-Za-z0-9_-]`. Any `=`, `+`,
   `/`, `%`, white space or other character fails here.
9. `encoding.nonCanonical` — re-encoding the decoded bytes as unpadded base64url
   reproduces the `d` value exactly (§6.3).
10. `payload.length` — the decoded byte count is 2 to 4096.

Payload stage:

11. `payload.utf8` — the decoded bytes are well-formed UTF-8.
12. `payload.json` — the bytes parse as JSON with no trailing content.
    Insignificant JSON white space is permitted anywhere JSON permits it,
    including before the first and after the last token; a second JSON value
    after the object is trailing content and fails here.
13. `payload.notObject` — the top-level value is a JSON object.
14. `payload.duplicateMember` — §7.2.

Version stage, before the member-set check:

15. `version.missing`, then `version.type`, then `version.unsupported`.

**This ordering is the r1 correction.** In r1 a payload that both lacked
`version` and carried an unknown member could be claimed by either the version
check or the member-set check. `version` is now privileged and fully resolved
first — presence, then type, then value — so an unsupported or absent version
always reports as such regardless of what else is wrong. Two fixtures pin it:
`link/invalid/version-missing-with-unknown-member.json` must report
`version.missing`, and
`link/invalid/version-unsupported-with-unknown-member.json` must report
`version.unsupported`.

Member-set and field stage:

16. `link.unknownMember` — the top-level member-name set contains no name outside
    the seven of §6.5.
17. `<member>.missing` — for each of the remaining six required members, in the
    table order of §6.5.
18. Field type and value checks in the table order of §6.5: `requestID`,
    `displayName`, `managementIdentity`, `drawingIdentity`,
    `drawingIdentity.collidesWithManagement`, `drawingProtocol` (member set, then
    `name`, `version`, `rawHID`, `linkType`), then `routes` (`routes.type`,
    `routes.count`, then each element in index order, §7.6).

### 7.2 Duplicate member names

Duplicate member names MUST be detected by a **scan of the raw decoded bytes,
before any dictionary decode**. Counting keys after the fact is insufficient
because the duplicate has already been collapsed: measured, Python's
`json.loads('{"a":1,"a":2}')` returns `{'a': 2}`, and Swift's
`JSONSerialization` and `Decodable` behave the same way. Nothing downstream can
tell that a duplicate was present.

The scan is normative in three respects:

* It covers **every object at every depth**, not just the top level. A duplicate
  inside `drawingProtocol` or inside a `routes` element is equally a rejection.
* It compares member names **after unescaping**. Two names that decode to the
  same string are duplicates even when their source bytes differ. Measured:
  `json.loads('{"a":1,"a":2}')` also returns `{'a': 2}`, so
  `"a"` and `"a"` must be treated as the same member name, as must any other
  escape — `A` for `A`, a surrogate pair, `\/` for `/` — that decodes to an
  equal string.
* Duplicates are compared **within one object scope only**. The same name
  appearing in two sibling objects, for example `address` in two different
  `routes` elements, is normal and MUST NOT be rejected.

Identifier: `payload.duplicateMember`. Fixtures:
`link/invalid/payload-duplicate-member.json` (top level),
`link/invalid/payload-duplicate-member-nested.json` (inside `drawingProtocol`),
`link/invalid/payload-duplicate-member-escaped.json` (`a`-style escape at
the top level), and `link/invalid/payload-duplicate-member-escaped-nested.json`
(escape inside a `routes` element).

### 7.3 Status-descriptor ordered checks (entry point 2)

The bytes arrive inside an already length-checked and envelope-checked
authenticated status response
(`apple/RelaySetupKit/TabletManagement.swift:50-52`), so there is no URL or
encoding stage.

1. `payload.notObject` — `drawingHandoff` is a JSON object.
2. `payload.duplicateMember` — §7.2, over the `drawingHandoff` subtree, **scanned
   from the original response bytes**. See the note below: this check is
   impossible after a tolerant envelope decode.
3. `status.unknownMember` — wrapper member names are within
   `{supported, state, descriptor, reason}`.
4. `status.supported.missing`, `status.supported.type`.
5. `status.state.missing`, `status.state`.
6. `status.descriptor.missing` / `status.descriptor.unexpected`,
   `status.reason.missing` / `status.reason.unexpected` — exactly one of
   `descriptor` and `reason` is present, matching `state`.
7. When `state` is `unavailable`: `status.reason` — the value is one of the
   identifiers in §11. Validation stops here; there is no descriptor.
8. `status.descriptor.type` — `descriptor` is a JSON object. This is checked
   **before** anything inside it, so a `descriptor` that is a string, array,
   number, boolean or `null` reports `status.descriptor.type` and never a
   member-level reason. Fixture:
   `status/invalid/descriptor-not-object.json`.
9. `version.missing`, then `version.type`, then `version.unsupported`, inside
   `descriptor`. Version is privileged here for the same reason as §7.1.
10. `status.unknownMember` — the `descriptor` member-name set is exactly
    `{version, drawingIdentity, drawingProtocol, routes}`. **`requestID`,
    `displayName` and `managementIdentity` are not required here and the presence
    of any of them is a rejection.**
11. `drawingIdentity.missing`, `drawingProtocol.missing`, `routes.missing`.
12. Field checks: `drawingIdentity` (§5.1), `drawingProtocol` (§5.2), `routes`
    (§5.3, §7.6).

**Raw bytes before tolerant decoding.** The surrounding `TabletSetupStatus` decoder
is deliberately tolerant so an older relay stays decodable
(`apple/RelaySetupKit/TabletManagement.swift:32-46`), and a tolerant decode into a
dictionary **collapses duplicate member names before anything can observe them** —
the same loss measured for the app link in §7.2. A consumer MUST therefore retain
access to the **original response bytes** and run the duplicate scan over them
before or alongside the envelope decode. Decoding first and scanning the resulting
object is not an implementation of this check; it cannot detect a duplicate at all.
In practice the bytes are already in hand: `TabletSetupStatus.decode` receives
`Data` and checks its length before decoding
(`apple/RelaySetupKit/TabletManagement.swift:48-50`), so the scan runs on that same
`Data`.

### 7.4 Local-metadata ordered checks (entry point 1)

1. `payload.length` — the response is 2 to 4096 bytes including the newline
   (`tools/avp_relay/gadget_client.py:34-36`).
2. `payload.utf8`.
3. `payload.json`.
4. `payload.notObject`.
5. `payload.duplicateMember` — §7.2.
6. `metadata.envelope.version.missing`, then `.type`, then
   `metadata.envelope.version.unsupported`. Version is privileged here for the
   same reason as §7.1, and uses the same `.missing`/`.type`/`.unsupported`
   suffixes as every other entry point.
7. `metadata.unknownMember` — envelope member names are within
   `{version, ok, supported, state, listener, reason}`.
8. `metadata.envelope.ok.missing`, `metadata.envelope.ok`.
9. `metadata.envelope.supported.missing`, `metadata.envelope.supported.type`.
10. `metadata.envelope.state.missing`, `metadata.envelope.state`.
11. `metadata.envelope.listener.missing` / `.unexpected`,
    `metadata.envelope.reason.missing` / `.unexpected` — exactly one, matching
    `state`.
12. When `state` is `unavailable`: `metadata.envelope.reason`. Validation stops.
13. `metadata.envelope.listener.type` — `listener` is a JSON object. Checked
    **before** anything inside it, so a `listener` that is a string, array,
    number, boolean or `null` reports `metadata.envelope.listener.type` and never
    a member-level reason. Fixture: `local/invalid/listener-not-object.json`.
14. `metadata.unknownMember` — the `listener` member-name set is exactly
    `{drawingIdentity, drawingProtocol, boundAddress, boundPort}`.
15. `drawingIdentity.missing`, `drawingProtocol.missing`,
    `metadata.boundAddress.missing`, `metadata.boundPort.missing`.
16. Field checks in that order: `drawingIdentity` (§5.1), `drawingProtocol`
    (§5.2), `boundAddress` (§7.5), `boundPort` (1 to 65535).

#### 7.4a Deterministic literal pre-checks

Measured divergence, and the reason this subsection exists. Python 3 and Swift's
`Network` framework agree on the textual form of every ordinary literal these
fixtures use render identically in both: the IPv6 literals `::`, `::1`,
`fe80::1`, `ff02::1`, `fd00::1`, `2001:db8::2` and `2001:db8:1::beef`, the
documentation-range IPv4 literal `192.0.2.10`, the unspecified and loopback
addresses, the all-ones broadcast address, and one host literal from each of
`240/4`, `224/4` (specifically `239.x`), `169.254/16`, `0/8` and `127/8`. The
concrete host literals live in the JSON fixtures, which the publication guard
correctly treats as data rather than documentation; this document names the
prefixes instead. They **disagree on exactly three pathological forms**:

| Literal | Python `ipaddress` | Swift `IPv4Address`/`IPv6Address` |
| --- | --- | --- |
| `192.000.002.010` | **rejected** outright (leading zeros, the CVE-2021-29921 class) | **accepted**, silently normalised to `192.0.2.10` |
| `::ffff:192.0.2.10` | renders `::ffff:c000:20a` | renders `::ffff:192.0.2.10` |
| `::192.0.2.10` | renders `::c000:20a` | renders `::192.0.2.10` |

Left to the platform parsers, the same input would therefore produce different
reason identifiers in different consumers — `notLiteral` in Python, `nonCanonical`
in Swift — which is precisely what this contract exists to prevent. Leading-zero
IPv4 is also a classic parser-confusion and SSRF vector, so "whatever the platform
does" is not an acceptable answer.

Therefore every consumer applies these **deterministic checks on the raw text and
raw bytes, before and around the platform parser**. None of them depends on how a
platform renders an address.

1. **Digits-and-dots only** (the text is non-empty and contains only ASCII
   `0`–`9` and `.`): it must be exactly four `.`-separated groups, each 1 to 3
   ASCII digits with a value of at most 255, or it is `notLiteral`. If any group
   is longer than one character and begins with `0`, it is `nonCanonical`.
   Otherwise it is a canonical IPv4 literal — a text that passes this grammar is
   canonical by construction, so no round-trip is needed. Continue at the class
   checks.
2. **Contains `:`** — an IPv6 candidate. Any uppercase ASCII letter in the text is
   `nonCanonical`; this is a pure string test, so `2001:DB8::2` fails identically
   everywhere without consulting a parser.
3. **Anything else** is `notLiteral`.

Then parse the IPv6 candidate with the platform parser. A parse failure is
`notLiteral` — this is what refuses `192.0.2.10:28990`, which contains both a dot
and a colon but is not a literal. On success, test the **16 raw bytes**, never the
text:

4. If bytes 0 through 9 are zero and bytes 10 and 11 are `0xff 0xff`, the address
   is IPv4-mapped: `mapped`. If bytes 0 through 11 are all zero and the remaining
   32-bit value is greater than 1, it is IPv4-compatible: `mapped`. Both are byte
   tests and are identical in every implementation.
5. Only now re-render and compare to the input text; a difference is
   `nonCanonical`. This is what refuses the non-minimal `2001:db8:0:0:0:0:0:2`,
   where Python and Swift agree.

The `mapped` test precedes the round-trip deliberately: a mapped address is
refused whatever its textual form, and the two platforms render it differently, so
asking for a canonical round-trip first would make the reason
implementation-dependent for an address that is rejected either way.

### 7.5 `boundAddress` ordered checks

1. `metadata.boundAddress.missing` — the member is absent.
2. `metadata.boundAddress.type` — not a string.
3. `metadata.boundAddress.tooLong` — above 15 UTF-8 bytes, the longest IPv4
   literal.
4. The §7.4a deterministic pre-checks, reporting
   `metadata.boundAddress.notLiteral`, `metadata.boundAddress.nonCanonical` or
   `metadata.boundAddress.mapped` as that subsection specifies.
5. `metadata.boundAddress.familyUnsupported` — the literal is IPv6.
6. `metadata.boundAddress.invalid` — the literal is in a class a listener cannot
   usefully bind: `0/8` other than the unspecified address itself, `224/4`,
   `240/4`, and the all-ones broadcast address.

Everything else is accepted, explicitly including the unspecified address,
`127/8` and `169.254/16`. Those are honest binds; they are refused at
conversion (§9)
with a reason that tells the operator what to change.

### 7.6 Route validation

Element count is checked before contents, so a 9-element array is `routes.count`
and never a per-route reason. Elements are validated in index order.

**A route that fails validation rejects the entire descriptor.** It is not
silently dropped. A descriptor is produced by the authenticated managed service
from its own listener configuration, so a bad route means the producer is wrong
or the payload was altered; discarding it would hide that. This deliberately
differs from `networkRelayRoutes`
(`apple/RelaySetupKit/RelayNetworkSettings.swift:9-26`), which *filters*
discovery hints on a different channel for a different reason. Workstream D
validates the descriptor strictly here, and may then hand the surviving routes to
that existing filter, where it will be a no-op.

Per-route `address` checks, in this exact order, reporting the first failure:

1. `route.address.missing` — the member is absent.
2. `route.address.type` — not a string.
3. `route.address.tooLong` — above 64 UTF-8 bytes.
4. `route.address.scoped` — the literal contains `%` anywhere. A zone index is
   only meaningful on the machine that produced it, and the existing Apple
   validator already refuses `%`.
5. The §7.4a deterministic pre-checks, reporting `route.address.notLiteral`,
   `route.address.nonCanonical` or `route.address.mapped`. Canonical form is
   required so three implementations using three different parsers cannot
   disagree about whether two routes are the same route, and §7.4a is what makes
   that true in practice rather than in principle.
6. Class refusals, evaluated for whichever family parsed:
   * `route.address.unspecified` — `0.0.0.0`, anything in `0/8`, `::`.
   * `route.address.loopback` — `127/8`, `::1`.
   * `route.address.linkLocal` — `169.254/16`, `fe80::/10`.
   * `route.address.multicast` — `224/4`, `ff00::/8`.
   * `route.address.broadcast` — the all-ones broadcast address and the reserved
     space above multicast, `240/4`.
7. `route.address.familyUnsupported` — the literal is a valid, canonical,
   otherwise-acceptable **IPv6** address. Version 1 refuses it.

The class checks precede the family gate on purpose: an unacceptable IPv6 literal
reports the precise class that disqualifies it rather than being flattened into
"wrong family", so every refused class has one unambiguous fixture in both
families.

**Why IPv6 is refused in version 1.** The raw drawing listener creates an
`AF_INET` socket and parses its bind argument with `inet_pton(AF_INET, …)`
(`src/main.cpp:63-78`). There is no IPv6 socket to connect to, so an accepted
IPv6 route could only ever produce a failed connection that looks like a network
fault. Version 1 does not add an address family; adding one is a deliberate later
slice that changes the listener, the service's `RestrictAddressFamilies` policy
and this contract together, not a side effect of a permissive parser. Workstream
B MUST NOT place an IPv6 address in a descriptor, and the conversion in §9 passes
`{socket.AF_INET}` for exactly this reason.

Accepted IPv4 classes are everything the refusals above leave: the rules already
enforced at `apple/RelaySetupKit/RelayNetworkSettings.swift:14-17` — first octet
not 0, not 127, below 224, and not in `169.254/16`.

Private and carrier-grade-NAT unicast addresses are accepted. This is a
local-network product and trust comes from the pinned key, not from the address
class; refusing RFC 1918 would refuse the normal case. No fixture carries an
RFC 1918 literal, because this repository publishes reserved documentation ranges
only and a private-looking address in Git invites exactly the confusion the
repository policy forbids.

Routes byte-identical in `address` and `port` after validation are
**deduplicated**, not rejected, matching
`apple/RelaySetupKit/RelayNetworkSettings.swift:22-23`. Deduplication happens
after all per-route checks, so an invalid duplicate still produces its own
reason. At least one route MUST survive; zero surviving routes is `routes.count`.

### 7.7 Interface metadata scope

`interface` and `kind` are the only interface metadata in version 1. They are
**advisory and display-only**. They MUST NOT influence acceptance, rejection,
ordering, preference, retry policy, trust or storage. A consumer that ignores
them entirely is still conformant.

Forbidden in interface metadata, absolutely: MAC or BSSID values, SSIDs or Wi-Fi
network names, netmasks or prefix lengths, gateways, DNS servers, VLAN
identifiers, driver or firmware names, signal strength, link speed, host names,
and any identifier derived from hardware.

PLANK shows the interface name when present and the coarse kind when present.
When both are absent it says **Network** and MUST NOT infer Wi-Fi from the fact
that the headset itself uses Wi-Fi.

### 7.8 Unknown-member policy

**Unknown members are rejected**, at every level of all three entry points.

Justification:

1. It matches the established policy in this codebase. The managed service
   already compares the full key set and refuses any difference
   (`tools/avp_relay/core.py:59-62`).
2. The payload is attacker-controlled and crosses an application boundary. An
   unexpected member is a bug, a probe, or a newer descriptor — none of which
   should be silently discarded in a security-relevant parse.
3. Strict rejection makes the fixture set total. Every byte of a valid descriptor
   is specified, so three independent implementations converge instead of each
   tolerating a different superset.
4. Forward compatibility is provided by the version, not by tolerance.

This is deliberately **stricter than** the managed *status* decoder, which is
tolerant on purpose so an older relay stays decodable
(`apple/RelaySetupKit/TabletManagement.swift:32-46`). Those are different
channels with different reasons. The tolerance boundary is exact: the
`TabletSetupStatus` envelope stays tolerant, and the `drawingHandoff` subtree
inside it is strict. Workstream D MUST NOT implement the handoff decoder as a
plain tolerant `Decodable`: it must compare the decoded member-name set against
the frozen set explicitly.

## 8. Local public-status interface

The raw drawing service publishes its public listener metadata to the managed
service over a local socket. The raw service is the server; the managed service
is the client.

### 8.1 Why the r1 filesystem design is withdrawn

r1 specified a `0600` socket in a `0700` `RuntimeDirectory` owned by
`plank-relay`, and claimed the managed service could reach it because it runs as
`User=root`. **That claim was false.**
`debian/plank-avp-relay.service:19` sets
`CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW` with no
`AmbientCapabilities=`, so the managed service's effective capability set is
`0x3000` — `CAP_NET_ADMIN` and `CAP_NET_RAW` only. Without `CAP_DAC_OVERRIDE`
and `CAP_DAC_READ_SEARCH`, uid 0 is subject to ordinary permission checks and can
neither traverse the `0700` directory nor open the `0600` socket. Measured:
`connect()` returned `EACCES(13)` (§8.5, result **A**).

Three ways to make a filesystem socket reachable were considered and rejected:

* **Add `CAP_DAC_OVERRIDE`.** Refused by the operator, and correctly: it
  overrides every discretionary access check in the system, not just this one
  socket. A capability that broad to publish four public fields is not a trade
  worth making.
* **Add a shared group and `SupplementaryGroups=`, with the directory `0750` and
  the socket `0660`.** Refused: it creates a persistent system group whose
  membership grants standing access to anything later placed in that directory,
  and it is a packaging change in both units that outlives this feature.
* **Make the directory and socket world-accessible** (`0755`/`0666`). Measured to
  work (§8.5, result **A0**), but it gives every local process a connect path and
  a world-writable socket in the filesystem. The only remaining control would be
  the server-side peer check — which is exactly the control the abstract socket
  already provides, without the world-writable inode.

### 8.2 The chosen design: abstract socket, two-way peer credentials

| Property | Frozen value |
| --- | --- |
| Transport | `AF_UNIX` `SOCK_STREAM`, one newline-terminated JSON object per direction, the idiom of §1.5 |
| Address | **abstract**: a NUL byte followed by `plank-tablet-drawing-status-v1`, with no trailing NUL, exactly as the capture lease forms its address (`src/capture_lease.cpp:28-32`) |
| Name reuse | MUST NOT reuse `plank-tablet-capture-v1` (`src/capture_lease.hpp:9`). That name is a capture interlock; binding it would steal tablet ownership. |
| Name injection | Follow the established pattern exactly: a `ProductionName` constant plus a constructor parameter that lets isolated tests inject an alternate name, as `PltrCaptureLease` already does (`src/capture_lease.hpp:9`, and `:12-13` "Alternate names are for isolated tests; production uses ProductionName"). Production always uses `ProductionName`. |
| Test names | Must collide with neither `plank-tablet-capture-v1` nor the production status name. Namespace them to this project and add a test suffix; the shipped harness uses `plank-tablet-drawing-status-proof-only-v1`. Unrelated abstract names already exist on these hosts (for example `@ISCSIADM_ABSTRACT_NAMESPACE`), so a bare or generic name is not acceptable. |
| Why abstract works | Abstract addresses live in the network namespace, so filesystem DAC and `PrivateTmp=` do not apply. Neither unit sets `PrivateNetwork=` or `NetworkNamespacePath=`, so both share the host network namespace, and `AF_UNIX` is already permitted by `RestrictAddressFamilies=` (`debian/plank-avp-relay.service:32`). Measured reachable (§8.5, result **B**). |
| Permission envelope | **No unit change at all.** No new capability, no group, no `SupplementaryGroups=`, no `RuntimeDirectory=`, no `ReadWritePaths=`, no relaxation of any directive in either unit. This is strictly less invasive than r1, which needed a new `RuntimeDirectory=`. |
| Request bound | 256 bytes including the newline |
| Response bound | 4096 bytes including the newline |
| Timeouts | server 0.5 s per connection; client 0.75 s total |
| Concurrency | listen backlog 4, at most 4 concurrent handlers, non-blocking accept |

The cost, stated plainly: **an abstract name has no access control.** Any process
in the network namespace can connect to it, and any process can bind it first.
Measured: a second bind of a held abstract name returns `EADDRINUSE(98)`, so the
first binder wins (§8.5, result **D**). The whole security of this channel
therefore rests on peer-credential verification, which is mandatory on both ends.

### 8.3 Two-way peer-credential verification

**Server side — the raw drawing service. Production accepts peer uid `0` only.**
On every accepted connection, before reading a request and before writing a byte,
read `getsockopt(SOL_SOCKET, SO_PEERCRED)` and require the peer uid to be exactly
`0`, the managed service. Any other peer — including a peer running as the raw
service's own account — gets the connection **closed with no reply**: no error
body, no diagnostic that reveals whether the service exists, and nothing logged
from the peer's input. This matches
`tools/avp_relay/local_service.py:136-137`, which returns without a response for
any non-zero uid.

This is a correction to r2, which also allowed "the server's own effective uid so
in-tree tests can drive it". That was wrong twice over: it widened the production
boundary, and it contradicted the measured evidence — §8.5 result **C** shows a
client running under the raw-server account being **refused**, which is only
correct behaviour if uid 0 is the sole accepted peer. The contract text and the
observed result now agree.

**Test credential policy is injected, never configured.** A test that needs to
drive the server under a different uid MUST pass the accepted-uid policy in
explicitly — a constructor parameter or function argument supplied by the test
itself, in the manner of `PltrCaptureLease`'s alternate-name parameter
(`src/capture_lease.hpp:12-13`). It MUST NOT be reachable from production:

* not a default value — the production constructor takes no policy argument and
  hard-wires uid 0;
* not an environment variable the installed service reads;
* not a key in `/etc/default/plank-tablet-relay` or any other configuration file;
* not a command-line flag on the installed `ExecStart=` line;
* not a compile-time define that ships enabled in the package.

An installed relay therefore has exactly one accepted peer uid and no
configuration surface that can change it.

**Client side — the managed service.** After `connect()` succeeds and **before
reading or trusting a single byte**, read
`getsockopt(SOL_SOCKET, SO_PEERCRED)` on the connected socket and require the
server's uid to equal the resolved raw drawing service account uid (§8.3a). On
mismatch, report `drawingUnavailable` with reason **`service.peerUnverified`** and
read nothing. Measured: a squatting server at a different uid was refused (§8.5,
result **D**).

`service.peerUnverified` is deliberately distinct from `service.invalid` and is
**not** collapsed into it: `service.invalid` means the raw service answered with
malformed metadata, `service.peerUnverified` means something that is not the raw
service answered at all. Both surface the same user-visible `drawingUnavailable`
state, so nothing is widened; the log line and the fixture are more precise.

Without the client-side check, any unprivileged local process could bind the
abstract name before the raw service starts and feed the managed service a
fabricated drawing identity and route set, which the managed service would then
sign into its authenticated status and Setup would offer as **Use in PLANK**. The
check is what makes an unauthenticated namespace-wide address acceptable, and it
is not optional.

#### 8.3a Resolving the raw drawing service account

The expected server uid is the uid of the **packaged raw drawing service
account**, resolved **by name**. The name is the one the unit declares,
`User=plank-relay` (`packaging/plank-tablet-relay.service:8-9`). This is a
correction to r2, which resolved the uid from the owner of
`/var/lib/plank-tablet-relay`; a directory owner is not a service account, and a
wrong or tampered directory owner would silently have become the trusted uid.

Normative procedure, in order:

1. **Resolve by name.** Look up the account name `plank-relay` through the
   platform account database (`getpwnam`). The name is the authority. The managed
   service MUST NOT compile in a numeric uid, MUST NOT assume `0`, and MUST NOT
   derive the uid from any filesystem object as the primary source.
2. **Verify the peer against that resolved uid**, and against nothing else.
3. **Optional corroboration.** An implementation MAY additionally check the
   ownership of the raw service's `StateDirectory`,
   `/var/lib/plank-tablet-relay`. If it does, that check is **corroboration
   only** and never a source of truth. It MUST:
   * `lstat` the path and **reject a symlink** outright;
   * reject ownership that **disagrees** with the uid resolved in step 1;
   * never substitute the directory owner for the resolved account.
4. **Fail closed on any failure.** If the name does not resolve, if the account
   database is unavailable, if corroboration disagrees, or if the path is a
   symlink, the managed service reports `drawingUnavailable` with
   `service.peerUnverified` and accepts no peer. It MUST NOT fall back to a
   hard-coded uid, MUST NOT accept any uid, and MUST NOT retry in a loop.
   The one exception is a cleanly absent installation: if the account name does
   not exist **and** the `StateDirectory` does not exist, the raw service is not
   installed and the correct report is `service.absent`.

Reading `/etc/passwd` through `getpwnam` needs no new permission: the managed
unit's `ProtectSystem=strict` leaves the filesystem readable, and no capability is
involved. Measured (§8.5, result **E**) that the restricted client can also
`stat` the `0700` `StateDirectory` without being able to read inside it, so
corroboration is available at no cost in privilege.

**Substitute accounts are harness-only.** The unprivileged accounts the
reachability harness uses to stand in for the raw service — `nobody` and
`DynamicUser` — exist solely because the harness MUST NOT create the real
`plank-relay` account on a test host. They are a harness device and have no place
in production code, production configuration, or any shipped default. Production
resolves `plank-relay` by name and nothing else.

### 8.4 Request, response, ordering and squat behaviour

Request — exactly two members, no others:

```json
{"op":"drawing-status","version":1}
```

`drawing-status` is the **only** accepted operation; the interface is read-only.
Any other `op`, any extra member, a missing member, or a payload over the bound
yields `{"version":1,"ok":false,"error":"<short text>"}` and has no side effect.

Response, metadata available:

```json
{"version":1,"ok":true,"supported":true,"state":"ready","listener":{ … }}
```

Response, metadata not available:

```json
{"version":1,"ok":true,"supported":true,"state":"unavailable","reason":"service.absent"}
```

The response carries **nothing else**. Specifically it MUST NOT carry, and the
raw service MUST NOT be able to be made to emit: any private key or derivative
(the key is loaded into `PltrIdentityStore.private_key` from a `0600` file in a
`0700` directory, `src/identity.c:148-171`, and no operation here reads, derives
from or reports it), the paired-client allowlist or any Client public key, tablet
reports or samples, capture-lease state, pairing-window state, tablet bond
information, the Wacom device identity, file paths, or process identifiers.

The interface grants **no mutation of any kind**: no capture acquisition, no
pairing window, no tablet node open, no allowlist change, no configuration write.
It is a read of state the service already holds. The abstract capture-lease name
(`src/capture_lease.hpp:9`) is untouched, and a status request MUST NOT cause the
raw service to acquire, release or probe that lease.

Ordering and lifecycle:

* The raw service binds the abstract name **before** its TCP drawing listener
  accepts its first connection, and binds exactly once — never per request.
* If the bind fails with `EADDRINUSE`, the raw service logs the condition and
  **continues serving drawing traffic without a status interface**. Drawing is
  the product; the handoff is a convenience. The handoff reports unavailable.
* The name disappears when the process exits. There is no stale file, no lock and
  nothing to clean up, exactly as for the capture lease.
* If the managed service finds the name **absent** (`ECONNREFUSED`/`ENOENT` on
  connect), it reports `service.absent`. Absent means nothing is bound.
* If the name **is bound but held by a uid that is not the resolved raw service
  account**, it reports `service.peerUnverified` — not `service.absent`. The
  connection succeeded and something answered; the client's own peer check is what
  rejected it. Conflating the two would report a hostile squatter as a missing
  service. It **MUST NOT retry in a loop**; it reports unavailable and waits for
  the next status request from the headset, so the condition cannot be used as a
  timing or liveness probe.

### 8.5 Reachability harness, its isolation, and what has actually been run

The harness is `tests/local-status-reachability-proof.sh`. It is self-contained,
parameterised so it needs no edits on another host, and **deliberately isolated**.

**r3 safety properties, each a correction to r2.** r2's harness had real hazards
on a shared host: fixed unit names that its cleanup would stop whether or not that
invocation created them; a destructive `rm -rf` of the scratch directory *before*
claiming it; four completely unvalidated environment overrides; and only an `EXIT`
trap. r3 fixes all four:

* **Unique per-invocation run identifier** in every unit name, runtime directory
  and abstract socket name, so two runs or two users cannot collide.
* **Exclusive creation** of scratch space — `mkdir` without `-p`, which fails if
  the directory exists. A pre-existing directory is **never deleted to make
  room**; the run aborts instead.
* **Validated overrides**, refusing rather than sanitising. The validators reject
  the production socket name `plank-tablet-drawing-status-v1`, the live capture
  lease name `plank-tablet-capture-v1`, any name without a `proof` marker, path
  traversal, symlinked scratch bases and symlinked parents, paths outside an
  allowed root, relative paths, `root`, the production `plank-relay` account, a
  nonexistent account, and empty or odd characters.
* **Cleanup restricted to resources this invocation successfully created.** A unit
  name is tracked only after its `systemd-run` returns zero, so the harness can
  never stop a unit it did not start.
* **INT and TERM handled** as well as `EXIT`.
* **Every privileged step bounded** by `timeout`.
* **Cleanup verified, and the verification affects the exit status.** A surviving
  tracked unit or directory makes the run fail and names what was left behind.
* **Decisive assertions.** C requires both the expected exit status *and* exactly
  zero received bytes. D requires a squatter uid that is determinable, nonzero and
  different from the expected uid, otherwise the step is INVALID. D2 requires
  `EADDRINUSE` specifically and appears in the summary. E checks the probe's own
  exit status, not only its text. A server that fails to start, fails to become
  active, or fails to report a successful bind is an **INVALID proof**, never a
  silent pass; its output is captured rather than discarded.
* **Self-testable without root.** `PROOF_SOURCE_ONLY=1` makes the file define its
  functions and return without creating, starting or deleting anything, so the
  validators, the created-resource tracking, the cleanup verification and the
  C/D/D2/E decision functions can all be exercised unprivileged.

**The r3 harness has not been executed.** No privileged rerun is authorized. The
observed results recorded below were produced by the r2 harness on the proof host
and remain valid evidence for the kernel and systemd behaviour they test; the r3
changes are to the harness's safety and assertion strength, not to the mechanism
under test. The isolation properties:

* It is **not** registered with `add_test`, is **not** in the default ctest set,
  and is **not** invoked by any build script. A reviewer runs the ordinary suites
  without triggering anything privileged. It is only ever run by hand.
* It **refuses to run** unless it can actually apply the full managed restriction
  set and observe `CapEff=0000000000003000` as uid 0. A harness that silently ran
  unconstrained would "prove" the withdrawn r1 design works, so this is a hard
  precondition, not a warning.
* It **exits non-zero** if the A0 sanity gate fails or if the negative control
  does not fail specifically with `EACCES`.
* It creates no package, user, group, `/etc` entry or persistent unit; uses
  transient `systemd-run` units, an existing unprivileged account and
  `DynamicUser` only; binds no LAN-reachable port; and removes everything it
  creates through an `EXIT` trap.
* Its abstract test name is `plank-tablet-drawing-status-proof-only-v1`, which
  collides with neither the capture-lease name nor the production status name.

The client property set it applies, copied verbatim from
`debian/plank-avp-relay.service:18-35`:

```
--property=User=root
--property=CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW
--property=NoNewPrivileges=yes
--property=ProtectSystem=strict
--property=ProtectHome=yes
--property=PrivateTmp=yes
--property=ProtectKernelTunables=yes
--property=ProtectKernelModules=yes
--property=ProtectKernelLogs=yes
--property=ProtectControlGroups=yes
--property=RestrictRealtime=yes
--property=RestrictNamespaces=yes
--property=RestrictAddressFamilies=AF_UNIX AF_BLUETOOTH AF_INET AF_INET6
--property=LockPersonality=yes
--property=DevicePolicy=closed
```

The server property set, from `packaging/plank-tablet-relay.service:8-23`, with
`User=` substituted: the harness MUST NOT create the real `plank-relay` account,
so an existing unprivileged account stands in for it. **The substitute uid is a
harness device only.** Production code resolves the real service account at the
moment of use and fails closed (§8.3, §8.8).

#### What has been run, and what has not

| Host | Status |
| --- | --- |
| **Proof host** — Rocky Linux 9.7, systemd 252, x86_64 | **Executed** on 2026-09-30 under the **r2** harness; the **r3** harness has not been run anywhere. All seven checks observed as below. This is **Linux kernel and systemd behaviour evidence only**: capability semantics, abstract-socket namespace scope, and `SO_PEERCRED` availability. Rocky Linux is **a different distribution from the Ubuntu Relay target** and these results **do not** qualify the Ubuntu service or stand in for it. |
| **Ubuntu Relay target** — Ubuntu 26.04.1, systemd 259 | **NOT executed. Outstanding gate.** Privileged authentication on that host is **outstanding**, not impossible: the run needs an operator-authenticated `sudo` at the moment of execution. Nothing about the target prevents the proof. `systemd-run --user` is not a substitute, because a user unit runs as an unprivileged uid with no capabilities and so is not the condition under test. |

Observed on the proof host. Every client run reported
`CapEff=0000000000003000`, confirming the harness was actually constrained.

| Result | What it shows | Observed | Exit |
| --- | --- | --- | --- |
| **A0** harness sanity: filesystem socket, `RuntimeDirectoryMode=0755`, socket `0666` | `ProtectSystem=strict` and `PrivateTmp=yes` do not block a filesystem-socket connect, so result A is DAC and not the mount | `RESULT=OK state=ready boundAddress=0.0.0.0 boundPort=28990` | 0 |
| **A** negative control: the withdrawn r1 design, `RuntimeDirectoryMode=0700`, socket `0600`, owned by uid 65534 | the r1 design is unreachable | `RESULT=CONNECT_FAILED errno=EACCES(13) Permission denied` | 3 |
| **B** chosen design: abstract name, server uid 65534 | the abstract socket is reachable and the client verified the server's uid | `RESULT=OK state=ready boundAddress=0.0.0.0 boundPort=28990` | 0 |
| **C** third unprivileged peer (uid 65534) connects to the abstract name | the server's peer check refuses it with no reply | client `RESULT=CLOSED_WITHOUT_REPLY bytes=0`; server journal `SERVER REFUSED peer uid=65534 (expected 0): closing with no reply` | 6 |
| **D** squatting server, `DynamicUser=yes` → uid 62933, holding the abstract name | the client's peer check refuses it | `RESULT=REFUSED_BY_CLIENT reason=service.peerUnverified server_uid=62933 expected=65534` | 4 |
| **D′** a second bind of the held abstract name | first binder wins, so squatting is a real hazard and the client-side check is mandatory | `SECOND_BIND=FAILED errno=98 Address already in use` | 0 |
| **E** expected-uid discovery against a `0700` directory owned by uid 65534 | the restricted client can learn the owner uid without reading inside | `STAT=OK uid=65534 mode=700` and `LISTDIR=FAILED errno=EACCES` | 0 |

A0 is the credibility gate. If A0 had failed, the harness would have been blocking
filesystem sockets for some unrelated reason and result A would have proved
nothing; the harness exits non-zero in that case rather than reporting a pass.

#### What the operator must supply to close the Ubuntu gate

One step, no investigation required:

1. A shell on the Ubuntu Relay target able to run
   `sudo bash tests/local-status-reachability-proof.sh`, with `sudo` authenticated
   interactively by the operator at that moment. Nothing else is needed — no
   package, no account, no unit, no configuration change, and no network access.
2. Confirmation that `systemd-run`, `python3` and an unprivileged account
   (`nobody` by default, overridable with `PROOF_SERVER_ACCOUNT`) are present.
3. The harness output. It prints a `SUMMARY` block and exits 0 only if every
   check passes, including the `EACCES` negative control.

The harness needs nothing from the installed PLANK deployment: it reads no PLANK
state, touches no PLANK unit, and never binds either real abstract name.

### 8.6 Behaviour when the raw service is absent, restarting, slow or wrong

The managed service maps every local failure to an explicit unavailable state.
None of these may disconnect the headset, acquire the capture lease, open an
input node, open a pairing window, open enrollment, or weaken service
confinement.

| Local condition | `state` | `reason` |
| --- | --- | --- |
| Abstract name not bound, or `/var/lib/plank-tablet-relay` absent | `unavailable` | `service.absent` |
| Timeout or other `OSError` — restarting, slow, handler slots full | `unavailable` | `service.busy` |
| Server's peer uid is not the raw service account | `unavailable` | `service.peerUnverified` |
| Response over 4096 bytes, or it fails entry point 1 (§7.4), or `ok` is false | `unavailable` | `service.invalid` |
| Listener bound in `127/8` | `unavailable` | `listener.loopbackOnly` |
| Wildcard bind with no usable address, or a bound literal in a refused route class | `unavailable` | `listener.noUsableAddress` |
| Valid, but the status response cannot fit the member | `unavailable` | `response.tooLarge` |

`service.busy` MUST NOT be reported as unsupported or as absent: a restarting
service is transient and the correct instruction is to refresh, exactly as the
established client already distinguishes these
(`tools/avp_relay/gadget_client.py:42-49`).

The managed service MUST validate the response against entry point 1 before
converting it, and MUST validate the assembled descriptor against entry point 2
before placing it in the authenticated status. It MUST NOT forward unvalidated
raw-service bytes to the headset.

### 8.7 No unit change is required

**The r2 design needs no change to either service unit.** This is the preferred
outcome and it is now the frozen one.

r1 required a new `RuntimeDirectory=plank-tablet-relay` on the raw service unit.
r2 does not: an abstract `AF_UNIX` address needs no directory, no file, no mode,
no owner and no `RuntimeDirectory=`. `AF_UNIX` is already permitted by the managed
unit's `RestrictAddressFamilies=` (`debian/plank-avp-relay.service:32`), and the
raw unit sets no `RestrictAddressFamilies=` at all. Both units already share the
host network namespace because neither sets `PrivateNetwork=` or
`NetworkNamespacePath=`.

Therefore, normatively:

* No new capability in either unit. No `AmbientCapabilities=`, no addition to
  `CapabilityBoundingSet=`.
* No new group, no `SupplementaryGroups=`, no `User=`/`Group=` change.
* No `RuntimeDirectory=`, `ReadWritePaths=`, `ReadOnlyPaths=`,
  `InaccessiblePaths=` or `StateDirectory=` change.
* No relaxation of any existing directive in either unit, and no change to any
  installed unit as deployed.
* No change to either service's trust store, credentials, or identity files.

Workstream B implements this **without editing any `.service` file.** If an
implementation finds it needs a unit change, that is a contract question for A,
not a local fix.

### 8.8 Bounded behaviour, with explicit limits

Every failure mode has a finite bound and lands in a named outcome. No hang, no
unbounded retry, no retry storm.

| Condition | Bound | Outcome |
| --- | --- | --- |
| **Startup ordering** — the managed service connects before the raw service has bound the abstract name | one `connect()` attempt per status request, no retry inside the request. `connect()` on an unbound abstract address fails immediately with `ECONNREFUSED`; there is no DNS, no backoff and nothing to wait on. | `drawingUnavailable`, `service.absent` |
| **Server unavailable** — not installed, stopped, or crashed | as above; additionally, if `/var/lib/plank-tablet-relay` is absent the client reports absent without attempting a connection at all | `drawingUnavailable`, `service.absent` |
| **Server present but slow or saturated** — restarting, all four handler slots busy | total client budget **0.75 s** per status request, covering connect, send and receive, exactly `tools/avp_relay/gadget_client.py:29`. The server's own per-connection budget is **0.5 s**. On expiry the client closes the socket and gives up for this request. | `drawingUnavailable`, `service.busy` |
| **Abstract-name collision** — the name is already bound when the raw service starts | the raw service attempts the bind **once** at startup, never retries, logs `EADDRINUSE`, and continues serving drawing traffic with no status interface. Drawing is the product; the handoff is a convenience. | **From the client's side the outcome depends on who holds the name, and the client cannot and need not tell why.** A name held by a uid that is not the resolved raw service account yields `drawingUnavailable`, **`service.peerUnverified`** — the client's own check fires, because something answered and it was not the raw service. Only a name that is **not bound at all** yields `service.absent`. |
| **Failed credential verification** — the peer uid is not the resolved raw service account | checked once, immediately after `connect()` and before any read. The client closes the socket, reads nothing, and **does not retry** within or across this status request. | `drawingUnavailable`, `service.peerUnverified` |
| **Failed account resolution** — `getpwnam("plank-relay")` does not resolve, the account database is unavailable, corroborating `StateDirectory` ownership disagrees with the resolved uid, or that path is a symlink | one resolution attempt and at most one `lstat`, no retry. **Fail closed**: the client MUST NOT proceed, MUST NOT hard-code or assume a uid, and MUST NOT accept the peer. | `drawingUnavailable`, `service.peerUnverified` |
| **Cleanly absent installation** — the account name does not exist **and** the `StateDirectory` does not exist | one resolution attempt, no retry | `drawingUnavailable`, `service.absent` |
| **Malformed metadata** | the response is bounded at 4096 bytes and validated once against entry point 1 | `drawingUnavailable`, `service.invalid` |

Rate discipline: the managed service reads the local status **at most once per
authenticated status request from the headset**, which is itself paced by Setup's
one-second polling loop
(`apple/RelaySetupKit/TabletManagement.swift:190`). It MUST NOT poll the local
socket on a timer of its own, MUST NOT retry a failed read before the next headset
request, and MUST NOT treat any local failure as a reason to close the headset
connection.

Account resolution is **explicit and fails closed**. The managed service resolves
the raw Relay service account at the moment of use, from the owner of
`/var/lib/plank-tablet-relay` (§8.3). It MUST NOT compile in a uid, MUST NOT
assume `0`, MUST NOT assume the first peer it sees is correct, and MUST NOT
proceed on any resolution or verification failure. The only permitted outcomes of
a failed check are `service.absent` and `service.peerUnverified`.

## 9. Converting listener metadata into routes

This is the only place where wildcard and loopback binds are resolved, and the
only place that produces `routes`. **Wildcard and loopback MUST NOT escape into
the managed descriptor** (§4).

Input: a `listener` object that passed entry point 1, plus the host's IPv4
inventory. Output: either 1 to 8 validated routes, or an unavailable reason.

**Wildcard expansion is the primary production path.** On the installed
development Relay the raw drawing service is bound to `0.0.0.0` on its drawing
port (§1.8), so step 2 is the common case and must be given that weight in
implementation and in tests. Loopback-only is the *unconfigured default*, not the
installed one.

How the bind is set, at role level only: the raw service unit carries its own
`Environment=PLANK_RELAY_BIND=127.0.0.1` and an optional
`EnvironmentFile=-/etc/default/plank-tablet-relay`
(`packaging/plank-tablet-relay.service:10-12`). An operator-owned, root-only
environment file overrides the unit default, and that is how a deployed relay
comes to be bound to the wildcard address. There is no systemd drop-in involved.
**This contract changes neither service's configuration**, does not read that
file, and does not require it to exist. Its contents and any host's addresses are
operator deployment state and stay out of Git.

Because configuration can be overridden from outside the unit, the raw service
MUST report the address it is **actually bound to**, observed from its own
listening socket, and MUST NOT re-read or re-derive configuration to answer a
status request. Re-reading configuration would report what the service was told,
not what it is doing, and the two can differ across a restart or an edit.

1. `boundAddress` is in `127/8` → **`listener.loopbackOnly`**. The drawing
   service has no local-network route. This is the **unconfigured default** — what
   the unit's own `Environment=` yields when no operator override exists
   (`src/main.cpp:114`, `packaging/plank-tablet-relay.service:10`) — so it is the
   expected state on a relay that has not yet been configured for headset access,
   and it MUST read as a clear unavailable state with actionable text, not as an
   error. A host that happens to have usable addresses does **not** rescue a
   loopback bind.
2. `boundAddress` is exactly `0.0.0.0` → **the production case.** The listener
   answers on every IPv4 interface. Build candidates with
   `tools/avp_relay.network_routes.local_addresses({socket.AF_INET})` and pair
   each with `boundPort`. That function already enumerates without `AF_NETLINK`,
   already refuses unspecified, loopback, multicast, link-local, `0/8`,
   `>= 224.0.0.0` and IPv4-mapped IPv6, already deduplicates, and already caps at
   8 (`tools/avp_relay/network_routes.py:10-28`). **Reuse it. Do not write new
   address-collection code and do not change the installed socket-family policy.**
   If it yields no addresses → **`listener.noUsableAddress`**.
3. Otherwise `boundAddress` is a literal. If it passes the route rules of §7.6 →
   exactly one route. If it does not — most realistically a `169.254/16`
   bind, which is a legal bind but a refused route class → **
   `listener.noUsableAddress`**.
4. The resulting route list is deduplicated and capped at 8, then the descriptor
   is assembled and MUST pass entry point 2 (§7.3) before being placed in the
   authenticated status. A conversion that produced zero routes never reaches
   entry point 2; it reports the reason from step 2 or 3.

The family set is `{AF_INET}` because the listener has no IPv6 socket (§1.3,
§7.6). The managed service MUST NOT add an IPv6 address the drawing listener
cannot accept, and MUST NOT substitute its own management port for the drawing
port.

Conversion fixtures, each naming its outcome, live under `conversion/` and carry
the listener metadata, the simulated host inventory and the expected result:

| Fixture | Outcome |
| --- | --- |
| `conversion/wildcard-expansion.json` | `conversion.ok` — `0.0.0.0` expands to the usable IPv4 addresses |
| `conversion/wildcard-expansion-drops-refused.json` | `conversion.ok` — the inventory contains loopback, link-local, multicast, the unspecified address and a duplicate; expansion drops every refused class and succeeds with the remainder |
| `conversion/wildcard-expansion-caps-at-eight.json` | `conversion.ok` — twelve usable addresses cap at eight, in inventory order |
| `conversion/bound-literal.json` | `conversion.ok` — a single bound literal becomes exactly one route |
| `conversion/loopback-only.json` | `listener.loopbackOnly` — bound `127.0.0.1`, the installed default |
| `conversion/no-usable-address.json` | `listener.noUsableAddress` — wildcard bind, nothing in the inventory survives |
| `conversion/bound-link-local.json` | `listener.noUsableAddress` — a legal `169.254/16` bind that is a refused route class |

## 10. Trust and lifecycle requirements

These are the security core of version 1.

### 10.1 Restated boundaries

Verbatim, because these are the product boundaries and not implementation detail:

* Version 1 hands PLANK an endpoint for an **already approved** drawing Relay.
* Discovery, endpoint hints and app links **authorize nothing**.
* Setup authorization and PLANK drawing approval stay **distinct**.
* Private keys **never transfer**.
* An unknown drawing identity goes to the **existing explicit physical approval
  flow**.
* Identity-keyed migration is **additive and rollback-safe**, and conflicting
  pins produce an **explicit error with nothing written**.
* An active desktop session is **never interrupted** by a link.
* Capture ownership, input readiness and disconnect cleanup are **preserved**.
* Do **not** add seamless route failover.
* Do **not** add a Bluetooth drawing transport to native PLANK.
* Change **nothing** about video, codec or frame rate.

### 10.2 Setup authorization is not PLANK authorization

Setup approval authorizes Setup to manage the relay. It does not authorize PLANK
to draw. App-link data MUST NEVER become enrollment authority. There is no code
path in which a received descriptor causes a key to be added to the raw service's
allowlist (`src/identity.c:207-220`) or to PLANK's Keychain pins.

An unknown `drawingIdentity` MUST route to PLANK's existing explicit
drawing-approval flow, with the user performing the existing physical approval. It
MUST NEVER be auto-approved, pre-approved, or approved because a trusted Setup app
sent it. Outcome identifier `trust.unknownDrawingIdentity`.

### 10.3 Identities never substitute for one another

A `managementIdentity` MUST NEVER replace, overwrite, seed, or be written as a
`drawingIdentity` pin, and the reverse is equally forbidden. The two are stored in
separate records and labeled separately in every surface. A descriptor whose
`drawingIdentity` equals its `managementIdentity` is rejected outright
(`drawingIdentity.collidesWithManagement`) because no honest relay produces that
and accepting it would invite exactly this confusion.

A route MUST NEVER make a new key trusted. A pin MUST NEVER be written from
discovery, from a Bonjour TXT record, from an app link, or from any
unauthenticated source. The advisory-only status of discovery
(`src/dnssd.hpp:13-14`) is unchanged.

If a descriptor's `drawingIdentity` differs from the pin PLANK already holds for
that relay, PLANK MUST NOT replace the pin, MUST NOT delete the existing approval,
and MUST surface an identity-change state for the user to resolve explicitly.
Outcome identifier `trust.drawingIdentityMismatch`. This mirrors the existing
`RelaySetupError.identityChanged` policy on the Setup side.

### 10.4 Handshake before route acceptance

PLANK MUST complete the existing authenticated drawing handshake against the saved
pin before accepting a route as usable. A route is a hint to be proven, never a
fact. Concretely: `pltr_client_link_create` with the saved 32-byte pin, the Noise
exchange, and both HELLO messages (`src/client_link.h:13-21`,
`src/link.c:179-204`) complete before PLANK reports the connection or forwards any
input. A reachable TCP port is not acceptance.

### 10.5 Duplicate request handling

`requestID` exists so that the same tap, delivered twice, does not produce two
confirmations or two connection attempts.

* PLANK keeps a bounded set of the most recent 16 accepted `requestID` values.
* The set is **process-lifetime and non-persistent**. Persisting it would turn a
  UUID into a stored credential-shaped object, and a legitimate retry after an app
  restart must still work.
* A descriptor whose `requestID` is already in the set is deduplicated: no second
  confirmation, no second connection attempt, and the existing presentation is
  raised instead. Outcome identifier `requestID.duplicate`.
* A fresh `requestID` confers nothing. A repeated `requestID` proves nothing. It
  is never an authorization, never a capability, and never replay protection for
  the drawing session — the Noise handshake provides that.

### 10.6 An active desktop session is not interrupted

A handoff arriving while a PLANK desktop session is active MUST be declined, with
an offer to use it after disconnect. PLANK MUST NOT cancel the session, MUST NOT
drop the link, and MUST NOT interrupt a stroke because another application opened
a URL. Outcome identifier `trust.sessionActive`.

### 10.7 No background migration, no seamless failover, no new transport

Version 1 has no background network migration and no mid-stroke failover. On link
loss PLANK MUST release input, end the stroke, show a recoverable connection
state, and use freshly verified routes for the **next** connection. It MUST NOT
replay old input, MUST NOT buffer input across a link loss, and MUST NOT claim
seamless failover. The old connection is cancelled and finished before a
replacement starts.

Version 1 also adds **no Bluetooth drawing transport to native PLANK**. The
descriptor's `drawingProtocol.linkType` is fixed at `2` (TCP) and `1` (Bluetooth
LE) is refused (§5.2). Bluetooth remains a Setup management rendezvous only
(`tools/avp_relay/core.py:84-86`).

**Do not confuse this with the managed Setup's Bluetooth-only test mode.** The
managed source at §0's pinned commit contains a `RelayTestTransport` selector with
a `bluetoothOnly` mode that filters routes to `linkType == 1` and fails closed
rather than falling back to the network. That is a **management-side qualification
control** in the Setup app, used to exercise the Bluetooth management rendezvous
deliberately. It is not a drawing transport, it has no bearing on this contract,
and its existence is not a precedent for accepting `linkType: 1` in a drawing
descriptor. A handoff descriptor with `linkType: 1` is refused with
`drawingProtocol.linkType` regardless of which test transport Setup is using. Capture ownership, input readiness gating and
disconnect cleanup are preserved exactly as they are today; nothing in this
contract changes the shared capture lease, and nothing here changes video, codec
or frame-rate behaviour.

### 10.8 Nothing forbidden travels in the link

The app link MUST NEVER contain, in any encoding: a private key or any derivative,
a pairing code or approval code, a Wi-Fi password or any network credential, Host
credentials or session tokens, tablet samples or pen reports, the Client allowlist
or any Client public key, capture control, or any command. The member set of §6.5
is closed and strictly enforced, which is what makes this checkable rather than
aspirational.

### 10.9 Migration preserves every existing approval

PLANK's saved pins are indexed by address or by discovered service name today
(§1.7). Workstream C introduces identity-indexed records. That migration is
additive, non-destructive and rollback-safe.

* Migration MUST NEVER delete, overwrite, supersede or invalidate an existing
  approval. Legacy records stay readable after migration, and a rollback to the
  previous Client build must still find its pins.
* Migration MUST NEVER write a pin from discovery, from a Bonjour TXT record, from
  an app link, or from any unauthenticated source. It only re-indexes 32-byte
  values already stored in the Keychain.
* The Client private key stays device-local and is never re-derived, regenerated
  or exported by migration
  (`visionos-native/Sources/Services/PlankRelayPairing.swift:65-75`).

**Key conflict, which the plan does not cover.** The legacy store can hold
*different* 32-byte keys under different saved accounts. `relay-<address>:<port>`
and `relay-service-<name>.<domain>` are separate accounts
(`visionos-native/Sources/Services/PlankRelayPairing.swift:79-85`) and
`useSavedPairing` and `useManualPairing` already copy a pinned key between those
labels (`:391-455`). Identity-indexing generalizes that copy; it MUST NOT become a
silent resolver.

| Outcome | Condition | Required behaviour |
| --- | --- | --- |
| `migration.clean` | Every legacy account that would map to one identity-indexed record holds the same 32-byte key. | Create the identity-indexed record. Leave every legacy record in place and readable. |
| `migration.pinConflict` | Two or more legacy accounts that would map to one identity-indexed record hold **different** 32-byte keys. | Surface an explicit conflict state. Write nothing. Leave every existing record exactly as it was. |

In `migration.pinConflict` the Client MUST NOT silently pick one key, MUST NOT
prefer the newest, MUST NOT prefer the discovered service over the manual address
or the reverse, MUST NOT merge the records, and MUST NOT delete either. It stops
and asks, because two different keys under two accounts means two different Relays
were approved and only the user knows which one this descriptor refers to. A
conflict blocks the handoff for that identity; it does not block the rest of the
app and it does not reset pairing.

Fixtures: `migration/legacy-matching-keys.json` and
`migration/legacy-single-account.json` for `migration.clean`,
`migration/legacy-divergent-keys.json` for `migration.pinConflict`. Each describes
the legacy accounts, the selection defaults, the arriving descriptor and the
expected outcome.

**Member naming, and why.** Each legacy account entry names its stored value
`pinnedDrawingIdentity`, not `key`. The value is a **pinned public drawing
identity** — the 32-byte Noise static *public* key of a Relay the user already
approved (§1.1, §5.1) — and calling it a "key" was both inaccurate and actively
misleading: it reads as secret material when it is the opposite. It also tripped
the repository publication guard's `generic-api-key` rule, which pattern-matches a
member named `key` adjacent to a 64-hex value. The rename is the substantive fix;
the guard is not disabled, no rule is exempted, and no inline allow-comment is
used — the guard runs with `--ignore-gitleaks-allow`, so such comments are
deliberately ineffective.

**Synthetic provenance, recorded so no future reader re-litigates it.** No fixture
in this contract contains real key material. Every identity value is the
**SHA-256 of a documented ASCII label**, listed under `syntheticIdentities` in
`MANIFEST.json` with its label, so anyone can recompute it and confirm:

| Fixture role | Label hashed to produce the value |
| --- | --- |
| known drawing identity | `plank-drawing-handoff-v1/synthetic/drawing/known` |
| unknown drawing identity | `plank-drawing-handoff-v1/synthetic/drawing/unknown` |
| mismatched drawing identity | `plank-drawing-handoff-v1/synthetic/drawing/mismatched` |
| management identity | `plank-drawing-handoff-v1/synthetic/management` |

A SHA-256 digest of a published label is not a private key, is not derived from
one, and corresponds to no real Relay. These are 32-byte values of the right
*shape* for a public identity, so that length and encoding rules can be tested
without any real material.

## 11. Outcome names

Three distinguishable outcomes, named here so Workstream B's managed response and
Workstream D's Setup UI agree. These names are the contract vocabulary and appear
in both repositories' tests.

| Outcome | Meaning | Managed status | Setup behaviour |
| --- | --- | --- | --- |
| `handoffReady` | Descriptor present and usable. | `drawingHandoff.state == "ready"` with a descriptor that passes §7.3 | **Use in PLANK** is enabled. Setup stops and releases any running tablet test first, then opens the link with public metadata only. |
| `drawingUnavailable` | The relay answered, but the drawing service is missing or not ready. `reason` ∈ `service.absent`, `service.busy`, `service.invalid`, `service.peerUnverified`, `listener.loopbackOnly`, `listener.noUsableAddress`, `response.tooLarge`. | `drawingHandoff.state == "unavailable"` | The action is visibly unavailable with the specific reason. Setup MUST NOT offer a handoff, MUST NOT type an address on PLANK's behalf, and MUST NOT retain capture or reset pairing. |
| `handoffUnsupported` | Authorization or protocol error, or a relay too old to report the field. `reason` ∈ `relay.tooOld` (member absent), `authorization.required` (`headsetAuthorized != true`), `authorization.failed`, `protocol.error`. | `drawingHandoff` absent, or the authenticated request failed | Setup explains that the relay must be updated or the headset reauthorized. Kept distinct from `drawingUnavailable` so a missing drawing service is never reported as an authorization problem. |

A failed PLANK launch leaves `handoffReady` unchanged, retains no capture and
resets no pairing state.

## 12. Portability and test rigor

### 12.1 Portability requirement

Descriptor construction, encoding, parsing and validation MUST live in
platform-portable units with no Linux-only dependencies, so their tests compile
and run on macOS. Only socket and IPC plumbing may be Linux-guarded.

| Repository | Requirement |
| --- | --- |
| Raw drawing Relay | The descriptor and metadata serializer is a new C translation unit registered **above** the `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard at `CMakeLists.txt:17`, linking only `plank_relay_protocol` and libc — no libsodium, no libudev, no `AF_UNIX`. Its `add_test` is likewise outside the guard, so it joins `protocol_test` and `session_test` in the macOS ctest run. The abstract-socket server, the `SO_PEERCRED` check and the account resolution go inside the Linux guard. |
| Managed Python | The validator and the conversion are stdlib-only modules in the style of `tools/avp_relay/network_routes.py` (no dbus, no BlueZ, no netlink, no `/sys` read at import). Their tests run as `python3 tests/<name>.py` on macOS. `network_routes.local_addresses` is Linux-only while `usable_addresses` is pure: depend only on pure helpers, and inject the address inventory in tests. |
| Setup Apple | The decoder is a Swift file in `RelaySetupKit` with an `add_executable`/`add_test` pair inside the existing `if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")` block of `apps/tablet-setup/CMakeLists.txt`, in the established `precondition` style. |
| Client Apple | The parser compiles with `swiftc` against Foundation only — no SwiftUI, UIKit or RealityKit — so a standalone `@main` test in `visionos-native/Tests/` can build it on macOS, in the style of `visionos-native/Tests/PlankTabletInputPolicyTests.swift`. |

Baseline at the pinned checkpoints, measured on macOS: this repository configures
and passes `protocol_test` and `session_test`; the managed repository passes
`python3 tests/local_service_test.py`, `python3 tests/capture_lease_test.py` and
`python3 tests/network_routes_test.py` while its dbus-dependent tests do not run.
Keep that property.

### 12.2 Test rigor

A test suite that cannot be made to fail is not evidence. Two ways to get a
vacuously passing suite were measured on the development Mac at these checkpoints,
and both are live hazards in these repositories.

**Measured hazard 1 — C `assert` under `NDEBUG`.** CMake's `Release`,
`MinSizeRel` and `RelWithDebInfo` configurations define `NDEBUG`, which compiles
`assert` out entirely. A probe with an opaque always-false assertion printed
`NDEBUG defined` / `assert did NOT fire` and exited 0 under
`-DCMAKE_BUILD_TYPE=RelWithDebInfo`, and trapped with exit 134 with no build type
set. This repository's existing `tests/protocol_test.c` and `tests/session_test.c`
use C `assert` exclusively (20 and 21 occurrences), so configuring them
`RelWithDebInfo` makes both pass without checking anything.

**Measured hazard 2 — Swift `assert` under `-O`.** `swiftc -O` removes `assert`.
The same opaque always-false probe printed `assert did NOT fire` at `-O` and
trapped at `-Onone`; `precondition` trapped at both. The Client's existing
standalone tests `PlankTabletInputPolicyTests.swift`,
`PlankSessionStartupDeadlineTests.swift` and `PlankWacomPreflightTests.swift` use
`assert`, so compiling them `-O` makes the whole suite pass vacuously. At
`-Onone` all three genuinely pass.

Therefore, normative for every consumer:

1. **New assertions never use a form that optimization can remove.** Use
   `precondition` in Swift — matching the Setup repository's existing style — or an
   explicit failure check (`if !condition { exit(1) }`, an explicit non-zero
   return, or an explicit raise). Never bare `assert`, in C or in Swift, in any new
   handoff test.
2. **The exact compiler and test flags are recorded** in a comment at the top of
   the test file and in the CMake or script line that builds it.
3. **One deliberate failing negative control per harness, not one per revision.**
   Each of the four harnesses below must have its own recorded negative control:
   invert one assertion, confirm *that harness* reports failure with a non-zero
   exit status, restore it, and report the inverted assertion and the observed
   failure. A negative control in one harness says nothing about another.

Frozen per-repository flags, so B, C and D cannot each guess differently:

| Harness | Build and test invocation |
| --- | --- |
| Raw drawing Relay (C, ctest) | `cmake -S . -B <build>` with **no** `CMAKE_BUILD_TYPE` (or `-DCMAKE_BUILD_TYPE=Debug`), then `cmake --build <build> --parallel` and `ctest --test-dir <build> --output-on-failure`. Never configure the handoff fixture test under a configuration that defines `NDEBUG`. New handoff tests use explicit failure checks so they stay valid even if someone configures `RelWithDebInfo`. **`tests/local-status-reachability-proof.sh` is NOT part of this set** and MUST NOT be added to it: it is privileged, root-only and hand-run. Nobody running the ordinary suite triggers it. |
| Managed Python | `python3 tests/<name>.py`, pass or fail by exit code. Never `python3 -O`, which strips `assert`. Prefer `unittest` assertions (`self.assertEqual`), which are ordinary calls and cannot be optimized away — the style already used by `tests/local_service_test.py` and `tests/network_routes_test.py`. |
| Setup Apple (Swift, ctest) | `scripts/build-tablet-setup.sh macos`, which configures the Xcode generator and runs `cmake --build … --config Debug` then `ctest --test-dir … -C Debug --output-on-failure` (`scripts/build-tablet-setup.sh:63-71`). `Debug` means `-Onone`, so `assert` would fire there — but new tests still use `precondition` so the result does not depend on the configuration. |
| Client Apple (standalone `swiftc`) | `swiftc -Onone -DPLANK_TABLET_RELAY <Sources…> visionos-native/Tests/<Name>Tests.swift -o <out> && <out>`. A single-file `@main` test additionally needs `-parse-as-library`; a multi-file invocation does not. `-Onone` is mandatory because the existing suite relies on `assert`; new handoff assertions use `precondition` so they survive either setting. |

Every consumer MUST apply **identical acceptance and rejection behaviour** for
every rule in §7, including the four rules introduced in r2 —
`encoding.nonCanonical`, raw-byte duplicate-member detection, the privileged
version ordering, and the separated entry points. Each has at least one negative
fixture, and a consumer that accepts a fixture the manifest marks as a rejection
is non-conformant regardless of how reasonable its own behaviour seems.

## 13. Remaining gates

These are explicitly **not** done. Do not read any part of this contract as
closing them.

| Gate | Status |
| --- | --- |
| **Final service qualification on the Ubuntu Relay target** | **Outstanding — not completed.** The §8.5 harness has **not** been executed on the Ubuntu Relay target. What is outstanding is **privileged authentication on that host**: the run needs an operator-authenticated `sudo` at the moment of execution. Nothing about the target makes the proof impossible. (`systemd-run --user` is not a substitute, because a user unit runs as an unprivileged uid with no capabilities and is not the condition under test.) The Rocky Linux 9.7 / systemd 252 results in §8.5 establish Linux **kernel and systemd** behaviour only and do **not** qualify the Ubuntu service; Rocky Linux is a different distribution. The harness MUST be run unchanged on the Ubuntu target before promotion; §8.5 lists the one step the operator must supply. |
| Further privileged proof runs | **Gated on operator review of the harness.** The harness is delivered for review first. It is isolated from every default test set and build script, so ordinary verification never triggers it. |
| Live headset qualification | Outstanding, Workstream E. Compilation and fixtures are not headset acceptance. |
| Installed-package verification of the abstract name | Outstanding. The production name must be confirmed on an installed relay, not only in a harness. |
| Upstream managed-repository integration | Out of scope for this contract. Managed citations are pinned to `7b477b3c75d91fbde667e5b4df4c46a36ea15219` per §0; A does not rebase, merge, cherry-pick or fetch-and-integrate anything in that repository. |

## 14. Loading the shared fixtures

Fixtures live in `tests/fixtures/plank-drawing-handoff-v1/` and are mirrored
byte-for-byte into:

* `visionos-native/Tests/Fixtures/plank-drawing-handoff-v1/` in the Client
  repository.
* `tests/fixtures/plank-drawing-handoff-v1/` in the managed Relay and Setup
  repository.

`MANIFEST.json` carries the contract revision, the SHA-256 of this document, and
the SHA-256, byte count, **entry point** and single expected outcome identifier for
every fixture file, plus the ordered state-dependent scenarios.

Directory layout names the entry point, so a consumer cannot apply the wrong one:

| Directory | Entry point |
| --- | --- |
| `link/valid`, `link/invalid`, `link/trust` | 3 — app-link descriptor |
| `status/valid`, `status/invalid` | 2 — managed status descriptor |
| `local/valid`, `local/invalid` | 1 — local listener metadata |
| `conversion/` | the §9 conversion, listener metadata in and routes or a reason out |
| `migration/` | the §10.9 Keychain migration, not a payload to parse |

File conventions, so a hash comparison means the same thing in all three
repositories:

* `.url` files contain exactly one app link followed by a single LF. Hash the file
  as stored; strip the one trailing LF before parsing. No CR anywhere.
* `.json` files are the payload bytes as they are to be validated. Several carry
  deliberate formatting — indentation, insignificant white space, a duplicate
  member — because that formatting is the thing under test. **Do not reformat a
  fixture.** Reformatting changes its hash and breaks all three repositories.
* `.bin` files are raw bytes that are deliberately not well-formed UTF-8.
* A `.url` fixture and its `.json` sibling match **semantically, not
  byte-for-byte** (§6.4). Compare parsed values.

Every consumer MUST load the fixture **files**. Retyping a vector inline defeats
the purpose: a contract revision must break a test, not quietly diverge. At least
one test per repository MUST assert the SHA-256 of `MANIFEST.json` against a
literal constant, so contract drift fails a test.

| Consumer | How to load |
| --- | --- |
| Raw and managed C tests | A CMake `add_test` executable. Pass the fixture directory in as a compile definition from CMake (`target_compile_definitions(… PRIVATE PLANK_HANDOFF_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plank-drawing-handoff-v1")`) and read files with `fopen`. Do not embed fixture bytes in the source. |
| Managed Python tests | `python3 tests/<name>.py`, pass or fail by exit code. Resolve the directory as `Path(__file__).resolve().parents[1] / 'tests/fixtures/plank-drawing-handoff-v1'`, the idiom of `tests/local_service_test.py:9`. Walk `MANIFEST.json`, verify each SHA-256, then assert each expectation. |
| Setup Apple tests | A Swift `@main` executable registered in `apps/tablet-setup/CMakeLists.txt` inside the `Darwin` block, using `precondition`, built by `scripts/build-tablet-setup.sh macos` which then runs ctest. Pass the directory with `set_tests_properties(… PROPERTIES ENVIRONMENT "PLANK_HANDOFF_FIXTURES=…")` and read it with `ProcessInfo`/`Data(contentsOf:)`. |
| Client tests | A standalone Swift `@main` program in `visionos-native/Tests/`, compiled with `swiftc -Onone -DPLANK_TABLET_RELAY` against the `Sources` files, in the structure of `visionos-native/Tests/PlankTabletInputPolicyTests.swift` but using `precondition` rather than `assert` (§12.2). Resolve the fixture directory relative to `#filePath`. |

Fixtures use reserved example addresses only — `192.0.2.0/24`, `198.51.100.0/24`,
`203.0.113.0/24`, `2001:db8::/32`, and the name `relay.example` — and synthetic
keys only. Each synthetic identity is the SHA-256 of a documented ASCII label,
recorded in `MANIFEST.json`, so anyone can confirm that no real key is present.

## 15. Ownership map

All three implementation agents share these repositories. Preserve each other's
changes, integrate against this pinned contract, and never revert another
workstream's work to make your own apply.

| Workstream | Owns | Depends on |
| --- | --- | --- |
| A — contract lead | This document, the shared fixtures and the reachability harness. Trust and lifecycle decisions. No implementation code. | Starting checkpoints |
| B — Relay services | **Linux only.** Raw Relay `src/main.cpp` and the new local public-status module and its tests. Managed `tools/avp_relay/core.py`, its status plumbing, the conversion, and their tests. | A |
| C — native PLANK | Client `visionos-native/Info.plist`, `Sources/PlankVisionApp.swift`, `Sources/Services/PlankRelayPairing.swift`, `Sources/Services/PlankRelayLiveLink.swift`, the new handoff/identity-route model, `Sources/Views/SettingsView.swift`, tests and CMake wiring. | A, and B's descriptor fixtures |
| D — standalone Setup | **Apple only.** `apple/RelaySetupKit/TabletManagement.swift`, `apple/RelaySetupKit/SetupCoordinator.swift`, the new descriptor model, `apps/tablet-setup/Sources/TabletSetupView.swift`, model and UI tests, build wiring. | A, and B's descriptor response |
| E — integrator | Cross-repository evidence, provenance, signed builds, installation sequence, live qualification. | B, C, D |

**B and D share the managed Relay repository and MUST NOT both edit the Apple
Setup library.** B owns the Linux managed response — `tools/avp_relay/core.py`,
its status plumbing, the service package policy and their tests — plus the raw
Relay `src/main.cpp` and the new local public-status module. D owns the Apple
decoding — `apple/RelaySetupKit/*` and `apps/tablet-setup/Sources/*` — in that same
repository. Neither crosses into the other's set.

**No `.service` file is edited by anyone.** §8.7 is frozen: the design requires no
unit change. A unit change is a contract question for A.

Protocol revisions come back to A before any consumer edit. If an implementation
discovers that a frozen value cannot work, stop, report it, and let A revise this
document and the fixtures; do not work around it locally.
