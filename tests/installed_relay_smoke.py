# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise extracted package files, not the source/build directory."""
from pathlib import Path
import json
import struct
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
private = root / 'usr/lib/plank-tablet-relay-ble'
library = private / 'libplank_ble_lab.so'
assert library.is_file(), 'Packaged ctypes library missing or renamed'
launcher = root / 'usr/bin/plank-tablet-relay-ble'
assert launcher.read_text().splitlines()[0] == '#!/usr/bin/python3 -I'
units = list(root.rglob('plank-tablet-relay-ble.service'))
assert len(units) == 1, 'Packaged systemd service missing or duplicated'
assert 'DeviceAllow=char-hidraw rw' in units[0].read_text().splitlines(), \
    'Raw Wacom capture needs read/write hidraw access'
sys.path.insert(0, str(private))
from ble_lab.config import read_settings
from ble_lab.native import Native
from ble_lab.bluez import Server  # Check installed imports and dependencies.

settings = read_settings(root / 'etc/plank-tablet-relay-ble/relay.conf')
assert not settings.exclusive_adapter and not settings.disable_controller_address_resolution
with tempfile.TemporaryDirectory() as directory:
    state = Path(directory)
    native = Native(library, state)
    native.close()
    key = (state / 'identity.key').read_bytes()
    # A previously approved app can rediscover the same relay under a new
    # peripheral identifier. Its retained client key must reach local approval.
    client_key = bytes(range(32))
    clients = json.dumps({'version': 1, 'clients': [client_key.hex()]}, separators=(',', ':'))
    (state / 'paired-clients.json').write_text(clients)
    native = Native(library, state)
    native.tablet(False)
    assert not native.observing
    def record(kind, sequence, payload):
        body = struct.pack('<IHHII', 0x504c5452, 1, kind, sequence, len(payload)) + payload
        return struct.pack('<H', len(body)) + body
    start = record(16, 1, b'\x03') + record(32, 2, bytes(48) + client_key + b'\x05smoke')
    replies = []
    for offset in range(0, len(start), 20):
        replies.extend(native.receive(start[offset:offset+20]))
    assert native.approval_pending == 2
    assert len(replies) == 1 and replies[0][18:22] == bytes([1, 0, 0, 3])
    native.disconnect()
    assert native.approval_pending == 0
    native.close()
    assert (state / 'paired-clients.json').read_text() == clients
    assert (state / 'identity.key').read_bytes() == key
    assert (state / 'identity.key').stat().st_mode & 0o777 == 0o600
print('PASS: extracted package imports, native re-approval, retained trust and default policy')
