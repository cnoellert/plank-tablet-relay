# Upstream agent handoff: Linux relay to Apple Vision Pro over Bluetooth

Prepared 2026-09-28. This is an implementation brief for the coding agent
working on the upstream PLANK tablet relay and visionOS client.

## Task and reference implementation

Make the Linux relay establish and retain its BLE connection to Apple Vision
Pro (AVP), while receiving Wacom input over a separate Classic Bluetooth link.
Port the verified compatibility fixes and recovery behavior described below.
Preserve application authentication, encryption, existing tablet bonds and
saved client identities.

The reference implementation is in
[`instinctual/plank-tablet-relay`, branch `visionos-tablet-setup`](https://github.com/instinctual/plank-tablet-relay/tree/visionos-tablet-setup).
Use commit
[`35a253f`](https://github.com/instinctual/plank-tablet-relay/tree/35a253f)
as the fixed source snapshot for this brief. The upstream repository is
[`cnoellert/plank-tablet-relay`](https://github.com/cnoellert/plank-tablet-relay).

The working BLE implementation is the separate `plank-tablet-relay-ble`
diagnostic/readings service and PLANK Tablet Setup app. The legacy
`plank-tablet-relay` TCP/raw-HID workstation daemon is a different service.
Successful BLE readings do not mean production raw-HID forwarding over BLE
has already been implemented. Inspect the current upstream architecture and
adapt these changes to its BLE transport; do not replace its wire protocol
with the diagnostic snapshot protocol accidentally.

## What the physical investigation established

The tested combination was an Intel Wireless-AC 7265 controller, Ubuntu 26.04,
BlueZ 5.85 and visionOS 27. The tablet used during qualification was a PTH-660;
that model is a test fixture, not a product restriction.

1. AVP could discover the relay but connecting timed out.
2. A tablet-free echo test reproduced the failure, including with the Wacom
   powered off. Tablet decoding and button approval were not required to
   reproduce this initial link problem.
3. The AVP passed against a Mac BLE peripheral. An independent Mac central
   passed against the Linux relay. Bumble on the same Linux radio also passed.
4. A controlled comparison using BlueZ and the unchanged headset build passed
   with controller LE address resolution disabled, failed with it enabled,
   then passed again with it disabled.
5. After that connection fix, a separate BlueZ battery-service interaction
   caused disconnects during application authorization. Disabling BlueZ's
   optional battery plugin removed that failure.

We isolated a controller setting, not the ultimate firmware/kernel defect.
Do not claim that AVP private addresses are invalid, that all BlueZ adapters
are affected, or that replacing BlueZ with Bumble is necessary. Bumble was
an isolation tool; the working packaged relay uses BlueZ.

## Change 1: opt-in controller address-resolution workaround

Add a per-adapter configuration option named, or equivalent to:

```ini
[relay]
disable_controller_address_resolution = false
```

Keep it disabled by default. Enable it for the qualified Intel 7265 setup or
other hardware on which the same failure has been reproduced.

Before registering the relay advertisement, send:

```text
HCI command: LE Set Address Resolution Enable
Opcode:      0x202D (OGF 0x08, OCF 0x002D)
Parameters:  00 (disabled)
Linux raw HCI packet, including command packet type:
             01 2D 20 01 00
```

This controls controller-side LE private-address resolution. It is not the
same instruction as changing BlueZ's `Privacy` setting, deleting resolving
keys, forcing a fixed AVP address, or turning off application encryption.
Those are not the changes established by the experiment.

Implement the command as a bounded operation:

- Use the selected adapter, not an unconditional `hci0`.
- Require the adapter to be powered and no discovery or advertisement active.
  Do not take over an unrelated application's scan or advertisement.
- Filter HCI events for Command Complete/Command Status and the exact opcode.
  Require matching Command Complete with status zero. A successful Command
  Status alone is not completion; an unrelated completion is not success.
- Use a monotonic deadline; the reference allows three seconds. Reject
  controller errors, truncated events and timeouts, including Command
  Disallowed. An explicitly requested workaround that fails must not be
  reported as applied or followed by a false ready state.
- Obtain the required Linux controller permissions. The reference service
  runs as root with its capability set limited to `CAP_NET_ADMIN` and
  `CAP_NET_RAW`; upstream may use another appropriately scoped arrangement.

Reference:
[`tools/ble_lab/controller.py`, `disable_address_resolution()`](https://github.com/instinctual/plank-tablet-relay/blob/35a253f/tools/ble_lab/controller.py).
Original implementation commit:
[`c7454e3`](https://github.com/instinctual/plank-tablet-relay/commit/c7454e3d1a235f3b30e3c1af60e01845c11ba0a9).
Use the later reference snapshot for managed-service recovery as well.

## Change 2: prevent unsolicited battery polling from breaking authorization

BlueZ also acted as a GATT client of the connected headset. Its optional
`battery` plugin attempted to read AVP's Battery Level characteristic. The
trace showed an ATT authentication error, an SMP security request, a rejected
OS-level Bluetooth pairing attempt, and then a local disconnect. This
happened even without a tablet button press.

For the qualified relay, exclude that plugin from `bluetoothd`:

```ini
# /etc/systemd/system/bluetooth.service.d/90-plank-tablet-relay-ble.conf
# Example only: confirm the existing command with systemctl cat first.
[Service]
ExecStart=
ExecStart=/usr/libexec/bluetooth/bluetoothd --noplugin=battery
```

This path matches the tested Ubuntu host. Preserve the actual daemon path,
other arguments and any existing plugin exclusions on the target system.
Apply the override during setup, reload systemd, restart BlueZ and restart
the relay through the initialization sequence described here.

This is a BlueZ daemon configuration change, not a GATT callback fix. It
affects optional battery reporting across that BlueZ instance. Make it an
explicit, documented host compatibility option, with removal of this drop-in
as its rollback. Our `.deb` does not silently install this global override.

The headset's PLANK authorization is an application protocol; it does not
require an OS-level AVP bond. Retain the Wacom's existing Classic Bluetooth
bond. Do not "fix" the failure by auto-accepting arbitrary system pairing or
bypassing PLANK authorization.

## Startup, recovery and transport behavior

The initialization order should be:

```text
Select adapter and verify required BlueZ interfaces
  -> check for conflicting discovery/advertising
  -> power adapter if explicitly configured as dedicated to this relay
  -> recover orphan advertisements only under that dedicated-adapter policy
  -> apply address-resolution workaround if enabled; await success
  -> register GATT application
  -> register connectable peripheral advertisement
  -> report ready only after both registrations succeed
```

The command changes controller state; it is not persistent controller
firmware configuration. Handle adapter removal, power-off and loss/restart
of `org.bluez`. Tear down stale sessions and registrations, retry with
backoff, and reapply the workaround before advertising again. The reference
exits on these failures and uses systemd restart/re-registration. Other
controller operations can re-enable resolution; do not assume it will stay
disabled indefinitely on a shared adapter.

We observed a kernel-retained advertisement even when BlueZ reported zero
active instances. On an explicitly dedicated adapter, query kernel
advertising instances and remove orphan instances before applying the HCI
command. Do not clear another service's advertisements. Avoid an unbounded
interactive `btmgmt` subprocess: it hung with service stdin redirected to
`/dev/null`. The reference uses bounded HCI management requests. See
[`5386dc4`](https://github.com/instinctual/plank-tablet-relay/commit/5386dc48dab7064459190cf496ade3657ada9956).

Keep application authorization separate from connection establishment:

- The diagnostic service uses client writes with response and server
  indications. Its characteristics use `write`/`indicate` flags without
  requiring an OS-level encrypted/authenticated pairing before PLANK's
  application handshake. Do not globally weaken unrelated characteristics.
- Subscribe to replies before sending protocol requests. Fragment the byte
  stream for ATT MTU, preserve order, and allow one unconfirmed indication
  at a time. Bound queues and response deadlines.
- Bind a session to its current connected peer. Persist the authenticated
  public-key identity, not the headset's rotating Bluetooth address or name.
- Preserve CPace confirmation, saved-key checks and Noise encryption. Echo
  success must never authorize tablet access.
- Discover tablets using Wacom ancestry and input capabilities. Do not add
  a PTH-660-specific device ID, address or button-code requirement.

## Files to inspect or port

All paths below are relative to the fixed fork snapshot linked above.

| File | Responsibility |
| --- | --- |
| `tools/ble_lab/controller.py` | Bounded HCI workaround and orphan-advertisement recovery |
| `tools/ble_lab/bluez.py` | Startup ordering, GATT registration, readiness and disconnect/restart handling |
| `tools/ble_lab/config.py`, `packaging/ble/relay.conf` | Strict configuration and opt-in defaults |
| `tools/ble_lab/service.py`, `debian/plank-tablet-relay-ble.service` | Managed startup and retry, state preservation, privileges |
| `tools/ble_lab/transport.py` | Peer binding, fragmentation, acknowledged indications and bounded echo |
| `tools/ble-tablet-lab.py` | Tablet-free diagnostic entry point |
| `apple/RelaySetupKit/RelayBluetooth.swift` | Core Bluetooth discovery, subscription, write and echo client |
| `tests/ble_controller_test.py`, `tests/ble_service_test.py`, `tests/ble_transport_test.py` | Existing automated regression coverage |
| `docs/bluetooth-headset-lab.md`, `docs/linux-ble-package.md` | Physical investigation and installed-host configuration |

Port the relevant behavior and tests; commit boundaries also contain lab and
packaging changes, so inspect dependencies before cherry-picking.

## Acceptance tests

First reproduce and fix the radio connection independently of any tablet.
Stop the installed BLE relay before starting a foreground probe. Keep BlueZ
running, select and power the intended adapter, and ensure it is not scanning
or advertising for another service. In the reference checkout:

```sh
sudo python3 tools/ble-tablet-lab.py --transport-only --adapter hci0 \
  --disable-controller-address-resolution
```

Replace `hci0` if needed. In PLANK Tablet Setup, select the relay and run
**Test Bluetooth connection**. With the Wacom powered off, AVP must compare
three fresh payloads of 64, 512 and 1024 bytes byte-for-byte: 1600 bytes in
each direction. Discovery, a connected flag, or server-side write counts
alone do not constitute a pass. For another upstream client, implement an
equivalent bounded diagnostic without changing the production wire contract.

On the affected Intel setup, repeat the disabled/enabled/disabled comparison
with identical client software. Restore or explicitly set the controller
state between trials: omitting the command-line option does not undo a
previous disable command. New hardware may pass in both states; that is not
a reason to force the workaround on it.

Then test the complete relay:

1. Connect the bonded Wacom and authorize the headset with three presses of
   the same supported tablet button while approval is pending. Verify actual
   pen position, pressure and button updates on AVP.
2. Hold an authorization session open without pressing a tablet button. It
   may reach its application timeout, but must not disconnect because BlueZ
   tried to authenticate a battery read. Inspect `btmon` and service logs.
3. Disconnect/reconnect using saved application trust, without enrolling again.
4. Leave the tablet untouched for at least 15 minutes measured from the final
   input. Record any sleep/disconnect; wake it as needed and verify input
   returns without pairing.
   This is a requested qualification test, not a previously proven precise
   manufacturer sleep timeout.
5. Restart the relay, restart BlueZ, power-cycle the adapter and reboot the
   host. Verify re-registration and readings, unchanged saved relay identity
   and tablet bond, and bounded recovery when the adapter is unavailable.
6. Exercise failure paths: busy adapter, unsupported/rejected HCI command,
   timeout, malformed response and failed GATT/advertisement registration.
   None may announce successful readiness.

The reference controller tests also check unrelated completions, successful
Command Status before Command Complete, adapter selection and bounded
management responses. Service tests check safe defaults, busy-adapter refusal,
readiness ordering and recovery triggers. Preserve that coverage in a port.

Current package targets are Ubuntu 24.04 `arm64` and Ubuntu 26.04 `arm64`/
`amd64`. Package tests do not qualify an ARM board's radio. Report the exact
controller, firmware, kernel, BlueZ and visionOS versions used for physical
acceptance, and distinguish automated results from device tests still pending.

## Separate later error: "Writing is not permitted"

If connection and echo work but a fresh enrollment attempt fails immediately
for an already-known headset key, inspect
[`f23e36b`](https://github.com/instinctual/plank-tablet-relay/commit/f23e36b1cccdb34714de41f87845f60a2556f63a).
That fix permits physically approved re-enrollment of an existing client key
while retaining existing trust on cancellation/failure. It still requires
the full application proof. This was a different failure from the initial
private-address connection timeout and the battery-triggered disconnect.

Deliver the adapted code, regression tests, host configuration/rollback
instructions and a concise qualification report. Do not claim hardware
support based solely on a Bluetooth version number or successful discovery.
