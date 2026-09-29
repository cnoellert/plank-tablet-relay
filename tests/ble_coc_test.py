# SPDX-License-Identifier: GPL-3.0-or-later
import queue
import socket
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ble_lab.coc import ProductionCoC


class Listener:
    def __init__(self, psm=0x81):
        self.psm = psm
        self.accepted = queue.Queue()
        self.bound = None

    def bind(self, value):
        self.bound = value

    def getsockname(self):
        return ('00:11:22:33:44:55', self.psm, 0, 1)

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
        with patch('ble_lab.coc.socket.socket', return_value=listener):
            coc = ProductionCoC(native, lambda: True)
            try:
                coc.start('00:11:22:33:44:55')
                self.assertEqual(coc.psm, 0x81)
                self.assertEqual(listener.bound[:2], ('00:11:22:33:44:55', 0))
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
        with patch('ble_lab.coc.socket.socket', return_value=listener):
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
        with patch('ble_lab.coc.socket.socket', return_value=listener):
            coc = ProductionCoC(Mock(), lambda: True)
            try:
                with self.assertRaisesRegex(RuntimeError, 'valid LE PSM'):
                    coc.start('00:11:22:33:44:55')
            finally:
                coc.close()


if __name__ == '__main__':
    unittest.main()
