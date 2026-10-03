# Relay source checkpoint for Vision Client build 36

This clean snapshot carries the final raw Relay handoff/status implementation,
fixtures and reviewed local-status harness from `928a5a3`, on the already
published raw checkpoint `e9e0e1c`. Superseded contract review commits are kept
locally and are excluded from this publication range.

The status endpoint reports public drawing identity and actual listener
metadata without capturing the tablet. Its abstract Unix socket verifies peer
credentials. Tablet capture continues to share the existing lease with the
managed Setup service. No new service privilege is required.

The Client's drawing transport remains TCP. Bluetooth-to-Relay discovery,
setup and preview belong to the managed service and Setup app. A Bluetooth
Wacom bond is distinct from the headset-to-Relay connection.

The development binary and managed modules were installed separately with
rollback bundles; this checkpoint is source publication, not a fresh package
release or installed-service qualification. The native source suites and local
IPC proof have recorded evidence in the development review trail. This
publication does not authorize executing the privileged harness again.

Known Client issue: after changing resolution or frame rate, pen tip/buttons
can require another reconnect. Sustained sessions and sleep/wake qualification
remain open. No pairing state, credentials, captures or deployment addresses
are included here.
