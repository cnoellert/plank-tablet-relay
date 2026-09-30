# SPDX-License-Identifier: GPL-3.0-or-later
import ctypes
import queue
import socket
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ble_lab.coc import ProductionCoC, _SockaddrL2, _bind_le_public


class Listener:
    def __init__(self, psm=0x80):
        self.psm = psm
        self.accepted = queue.Queue()

    def fileno(self):
        return 53

    def getsockname(self):
        return ('00:11:22:33:44:55', self.psm)

    def listen(self, backlog):
        assert backlog == 1

    def settimeout(self, seconds):
        pass

    def accept(self):
        try:
            return self.accepted.get(timeout=0.05), None
        except queue.Empty:
            raise socket.timeout()

    def close(self):
        pass


class Channel:
    def __init__(self):
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def fileno(self):
        return 37

    def shutdown(self, how):
        pass


class CoCTests(unittest.TestCase):
    def setUp(self):
        for name, value in (('AF_BLUETOOTH', 31), ('BTPROTO_L2CAP', 0)):
            patcher = patch.object(socket, name, value, create=True)
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_advertised_psm_and_identity_lifetime(self):
        listener = Listener()
        entered = threading.Event()
        finish = threading.Event()
        native = Mock()

        def session(fd, stop_fd):
            self.assertEqual(fd, 37)
            self.assertGreaterEqual(stop_fd, 0)
            entered.set()
            finish.wait(2)

        native.run_coc_session.side_effect = session
        with patch('ble_lab.coc.socket.socket', return_value=listener), \
             patch('ble_lab.coc._bind_le_public') as bind_le:
            coc = ProductionCoC(native, lambda: True)
            try:
                coc.start('00:11:22:33:44:55')
                self.assertEqual(coc.psm, 0x80)
                bind_le.assert_called_once_with(listener, '00:11:22:33:44:55')
                channel = Channel()
                listener.accepted.put(channel)
                self.assertTrue(entered.wait(1))
                self.assertTrue(coc.active)
                finish.set()
                for _ in range(100):
                    if not coc.active:
                        break
                    time.sleep(0.01)
                self.assertFalse(coc.active)
                self.assertTrue(channel.closed)
            finally:
                finish.set()
                coc.close()
        self.assertFalse(coc.thread.is_alive())

    def test_pairing_peer_excludes_production_session(self):
        listener = Listener()
        native = Mock()
        with patch('ble_lab.coc.socket.socket', return_value=listener), \
             patch('ble_lab.coc._bind_le_public'):
            coc = ProductionCoC(native, lambda: False)
            try:
                coc.start('00:11:22:33:44:55')
                channel = Channel()
                listener.accepted.put(channel)
                for _ in range(100):
                    if channel.closed:
                        break
                    time.sleep(0.01)
                self.assertTrue(channel.closed)
                native.run_coc_session.assert_not_called()
            finally:
                coc.close()

    def test_rejects_non_le_psm(self):
        listener = Listener(0x40)
        with patch('ble_lab.coc.socket.socket', return_value=listener), \
             patch('ble_lab.coc._bind_le_public'):
            coc = ProductionCoC(Mock(), lambda: True)
            try:
                with self.assertRaisesRegex(RuntimeError, 'valid LE PSM'):
                    coc.start('00:11:22:33:44:55')
            finally:
                coc.close()

    def test_bind_uses_linux_le_public_address(self):
        def bind(fd, pointer, length):
            target = ctypes.cast(pointer, ctypes.POINTER(_SockaddrL2)).contents
            self.assertEqual(fd, 53)
            self.assertEqual(length, 14)
            self.assertEqual(target.family, socket.AF_BLUETOOTH)
            self.assertEqual(target.psm, 0)
            self.assertEqual(bytes(target.address), bytes.fromhex('554433221100'))
            self.assertEqual(target.cid, 0)
            self.assertEqual(target.address_type, 1)
            return 0

        libc = Mock()
        libc.bind.side_effect = bind
        with patch('ble_lab.coc.ctypes.CDLL', return_value=libc):
            _bind_le_public(Listener(), '00:11:22:33:44:55')
        libc.bind.assert_called_once()

    def test_bind_rejects_malformed_adapter_address(self):
        with self.assertRaisesRegex(ValueError, 'Invalid Bluetooth'):
            _bind_le_public(Listener(), 'not-a-Bluetooth-address')


if __name__ == '__main__':
    unittest.main()
