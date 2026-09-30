# Linux Bluetooth relay package

`plank-tablet-relay-ble` installs the relay used by PLANK Tablet Setup: discover
the relay, approve the headset with three tablet-button presses, and view
authenticated position, pressure and button readings. It is separate from the
legacy `plank-tablet-relay` TCP/raw-HID workstation daemon. The two services
should not capture the same tablet at the same time.

## Hardware baseline

For new hardware, target **Bluetooth 5.0 or newer**, with both BR/EDR (Classic)
and Bluetooth LE, Linux firmware support, LE peripheral advertising, and
simultaneous tablet and headset connections. The version label alone does not
qualify an adapter. USB-connected tablets need only the relay's BLE link.

The tested Intel Wireless-AC 7265 is Bluetooth 4.2
([Intel specifications](https://www.intel.com/content/www/us/en/products/sku/83635/intel-dual-band-wirelessac-7265/specifications.html)).
It has carried concurrent Bluetooth Wacom input and authenticated AVP readings
with the compatibility settings below. The current protocol does not require
Bluetooth 5's optional 2M PHY. Bluetooth 4.0 could carry its basic BLE traffic,
but has not been qualified; do not promise support based on the version alone.
Qualification includes repeated discovery, pairing, reconnect, sleep/wake,
reboot, concurrent input and sustained readings on the exact chipset/firmware.

Tablet discovery uses Wacom vendor ancestry and pen/pad input capabilities,
not a tablet model or product-ID allowlist. With multiple matching tablets,
input stays offline until a tablet is selected in configuration. Once attached,
the running service retains that physical identity across sleep/wake.

## Install and configure

Use the package matching `dpkg --print-architecture`: `arm64` for 64-bit ARM
Linux, or `amd64` for x86-64. Automated package targets are **Ubuntu 24.04
arm64** (including the NanoPi Zero2's official Ubuntu image) and **Ubuntu
26.04 arm64/amd64**. Builds and clean-install tests run in containers on native
ARM64 and x86-64 runners. Bluetooth operation still requires
qualification on the board's kernel, radio and firmware. The existing physical
qualification is Ubuntu 26.04 amd64 with Intel 7265. There is no 32-bit `armhf`
build. Ubuntu 24.04 package CI does not yet qualify the NanoPi's actual
Bluetooth controller. The `.deb` format does not imply Debian or other Ubuntu compatibility;
OpenWrt/FriendlyWrt cannot install this package.

```sh
sudo apt install ./plank-tablet-relay-ble_*.deb
sudo editor /etc/plank-tablet-relay-ble/relay.conf
sudo plank-tablet-relay-ble --check-config
sudo systemctl restart plank-tablet-relay-ble
systemctl status plank-tablet-relay-ble
journalctl -u plank-tablet-relay-ble -b
```

Installation enables and starts the service. Defaults do not take ownership
of the adapter or apply chipset workarounds. Power the adapter on before use,
or explicitly set `exclusive_adapter = true` on a dedicated relay. That option
powers on the adapter, disables new OS-level bonding, and removes orphan kernel
advertisements only when BlueZ reports no active advertisements or discovery.
It retains existing tablet bonds. Do not run another advertising service on
an adapter configured as exclusive.

To pair a tablet to Linux, stop this service and follow
[headless tablet pairing](bluetooth-tablet-pairing.md), then restart it.
Headset approval happens inside the app; it does not need OS-level Bluetooth
pairing to the relay. The tablet can sleep without forgetting its bond or the
headset's saved approval. Wake it to resume input.

The service retries missing adapters/startup failures and re-registers after
BlueZ restarts. `active (running)` means GATT registration and advertising have
completed; it does not claim a tablet or headset is currently connected.

## Qualified Intel 7265 compatibility settings

The tested Intel 7265 / BlueZ 5.85 / visionOS 27 combination needed both:

1. `disable_controller_address_resolution = true` in `relay.conf`, applied
   before advertising and reapplied after the service restarts. This is a
   controller workaround; it does not remove BlueZ bonds or change the app's
   saved-key authentication. Use it only for hardware that needs it.
2. Disable BlueZ's optional `battery` plugin. Its GATT client attempted an
   authenticated battery read from the headset, triggering OS-level pairing
   and disconnecting the application link. This setting affects all devices
   on that BlueZ instance, including optional Bluetooth battery reporting.

The package does not silently change global BlueZ policy. On the qualified
Ubuntu host, whose existing `ExecStart` is `/usr/libexec/bluetooth/bluetoothd`,
an administrator can install this persistent override:

```ini
# /etc/systemd/system/bluetooth.service.d/90-plank-tablet-relay-ble.conf
[Service]
ExecStart=
ExecStart=/usr/libexec/bluetooth/bluetoothd --noplugin=battery
```

Check `systemctl cat bluetooth.service` first and preserve any existing
arguments or plugin exclusions. Then run `sudo systemctl daemon-reload` and
`sudo systemctl restart bluetooth.service`. This briefly disconnects Bluetooth
devices. Remove only this override and restart BlueZ to undo it.

## Saved state, updates and removal

The root-owned `0700` directory `/var/lib/plank-tablet-relay-ble` stores the
relay identity, approved headset public keys and pairing attempt budget.
Systemd and package updates retain it. Package removal and purge also retain
it deliberately. Back it up securely; deleting it changes the relay identity
and requires enrolling headsets again. BlueZ tablet bonds are stored separately
by BlueZ and are never removed by this package.

When migrating a foreground lab, stop it before copying its state into this
directory. Preserve ownership and `0600` file permissions; do not copy keys
into the source checkout. Never run two processes against the same state.
The native store locks itself and refuses unsafe ownership/permissions.

Use `sudo apt remove plank-tablet-relay-ble` to stop and remove the service.
An explicitly installed global BlueZ override remains under administrator
control and can be removed separately.

## Build and validation

Install `build-essential cmake ninja-build pkg-config libudev-dev python3
python3-dbus python3-gi debhelper dh-python curl ca-certificates git` in a matching
Ubuntu 24.04 or 26.04 builder. From a clean committed checkout run
`scripts/build-relay-deb.sh`.
The script snapshots that commit, verifies the pinned libsodium 1.0.22 archive,
builds it statically with PIC, runs its tests and the relay's assertions-enabled
tests, and creates `.deb`, `.buildinfo`, `.changes` and SHA-256 artifacts in
`artifacts/deb/<software-version>/<distribution>-<version>/<architecture>/`,
for example `artifacts/deb/0.2.0~visionos-tablet-setup.8/ubuntu-26.04/arm64/`.
The software version comes from `debian/changelog`; the exact Git commit is
retained in `source-commit.txt` and `provenance.json` with compiler/OS metadata.
Build natively on the target architecture; the script
rejects cross-builds because the packaged library and its tests must execute.
The prepared source remains under `build/deb`
for inspection. The package includes the libsodium license.

The `Linux relay packages` GitHub Actions workflow builds Ubuntu 24.04 arm64
and Ubuntu 26.04 arm64/amd64 packages, runs tests, checks each package with
lintian, and performs a configuration/native-library smoke check. The same
binaries are installed, smoke-tested and removed in fresh matching Ubuntu
containers, including Python bytecode cleanup. Download the matching
`relay-ubuntu24.04-arm64-<commit>`, `relay-ubuntu26.04-arm64-<commit>`, or
`relay-ubuntu26.04-amd64-<commit>` artifact
from the workflow run, then check `sha256sum -c SHA256SUMS` inside its package
directory. CI does not exercise a physical Bluetooth controller or systemd
reboot/recovery; those checks remain part of hardware qualification.

The service runs as root for BlueZ administration, raw controller setup and
read-only input access, with only `CAP_NET_ADMIN` and `CAP_NET_RAW`, restricted
device/address-family access and filesystem protections. It opens no TCP port.
Tablet data uses the existing CPace/Noise implementation. The three-press
initial approval scheme retains its documented nearby-attacker enrollment
tradeoff; it is not equivalent to comparing a random authentication code.
