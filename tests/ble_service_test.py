# SPDX-License-Identifier: GPL-3.0-or-later
import configparser
import os
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ble_lab.capture import Capture, SAMPLE
from ble_lab.config import read_settings
from ble_lab.controller import clear_advertisements
from ble_lab.notify import ready
from ble_lab.bluez import Server, PSMCharacteristic, PROPERTIES


class ServiceTests(unittest.TestCase):
    def test_psm_characteristic_is_little_endian_and_requires_listener(self):
        characteristic = PSMCharacteristic.__new__(PSMCharacteristic)
        characteristic.server = MagicMock()
        characteristic.server.coc.psm = 0x81
        self.assertEqual(list(characteristic.ReadValue({})), [0x81, 0])
        characteristic.server.coc.psm = 0
        with self.assertRaisesRegex(Exception, 'unavailable'):
            characteristic.ReadValue({})

    def read(self, content):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'relay.conf'
            path.write_text(content)
            return read_settings(path)

    def test_default_configuration_does_not_take_over_controller(self):
        settings = self.read('[relay]\n')
        self.assertFalse(settings.exclusive_adapter)
        self.assertFalse(settings.disable_controller_address_resolution)
        self.assertEqual(settings.tablet, '')

    def test_opt_in_and_tablet_selection(self):
        settings = self.read('[relay]\nadapter=hci2\ntablet=AA:BB:CC:DD:EE:FF\n'
                             'exclusive_adapter=yes\ndisable_controller_address_resolution=true\n')
        self.assertTrue(settings.exclusive_adapter)
        self.assertTrue(settings.disable_controller_address_resolution)
        self.assertEqual(settings.adapter, 'hci2')
        self.assertEqual(settings.tablet, 'AA:BB:CC:DD:EE:FF')

    def test_rejects_typos_and_invalid_values(self):
        for content in ('', '[other]\n', '[DEFAULT]\nadapter=hci0\n[relay]\n',
                        '[relay]\n[other]\n', '[relay]\nexclusive_adaptor=yes\n',
                        '[relay]\nadapter=hci-1\n', '[relay]\nadapter=hci١\n',
                        '[relay]\nexclusive_adapter=maybe\n', '[relay]\ntablet=/dev/input/event0\n',
                        '[relay]\nname=' + 'é' * 14, '[relay]\nname=\n',
                        '[relay]\nadapter=hci0\nadapter=hci1\n'):
            with self.subTest(content=content), self.assertRaises((ValueError, configparser.Error)):
                self.read(content)

    def test_readiness_notification_reaches_systemd_socket(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / 'notify')
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as receiver:
                receiver.bind(path)
                receiver.settimeout(1)
                with patch.dict(os.environ, {'NOTIFY_SOCKET': path}):
                    ready()
                self.assertIn(b'READY=1\n', receiver.recv(256))

    def test_ambiguous_tablets_stay_offline_without_opening_either(self):
        capture = Capture()
        candidates = {('bluetooth:a', '', 5): [], ('bluetooth:b', '', 5): []}
        with patch('ble_lab.capture.candidates', return_value=candidates), \
                patch('ble_lab.capture.os.open') as opener:
            capture.discover()
            self.assertFalse(capture.attached)
            self.assertIsNone(capture.identity)
            opener.assert_not_called()
        capture.close()

    def test_sleep_clears_readings_but_retains_physical_identity(self):
        capture = Capture()
        capture.identity = ('bluetooth:a', 'local-controller', 5)
        capture.axes = {0: 99, 1: 80, 24: 4000}
        capture.ranges = {0: (0, 100), 1: (0, 100), 24: (0, 8191)}
        capture.keys = {320, 330}
        capture.contacts = {0: True}
        capture.pad_mask = 511
        capture.close_nodes()
        snapshot = SAMPLE.unpack(capture.sample())
        self.assertEqual(snapshot[1:3], (0, 0))
        self.assertEqual(snapshot[5:17], (0,) * 12)
        self.assertEqual(capture.identity, ('bluetooth:a', 'local-controller', 5))
        capture.close()

    def test_bluetooth_daemon_loss_and_adapter_removal_require_restart(self):
        for event in ('daemon', 'power', 'removed'):
            server = MagicMock()
            server.adapter = '/org/bluez/hci0'
            server.failure = None
            if event == 'daemon':
                Server.bluez_changed(server, 'org.bluez', ':1.10', '')
            elif event == 'power':
                Server.adapter_changed(server, 'org.bluez.Adapter1', {'Powered': False}, [])
            else:
                Server.removed(server, server.adapter, ['org.bluez.Adapter1'])
            self.assertIsNotNone(server.failure)
            server.loop.quit.assert_called_once()

    def test_busy_adapter_is_rejected_before_controller_changes(self):
        server = MagicMock()
        server.exclusive_adapter = True
        properties = MagicMock()
        properties.Get.return_value = True  # Discovery already running.
        with patch('ble_lab.bluez.dbus.Interface', return_value=properties), \
                patch('ble_lab.bluez.clear_advertisements') as clear, \
                patch('ble_lab.bluez.disable_address_resolution') as workaround:
            with self.assertRaisesRegex(RuntimeError, 'no scan'):
                Server.run(server)
            properties.Set.assert_not_called()
            clear.assert_not_called()
            workaround.assert_not_called()

    def test_ready_only_after_both_registrations_succeed(self):
        for advertise_success in (False, True):
            with self.subTest(advertise_success=advertise_success):
                server = MagicMock()
                server.adapter = '/org/bluez/hci0'
                server.exclusive_adapter = server.controller_workaround = False
                server.native = server.capture = None
                server.failure = None
                server.notify_systemd = True
                server.advertising = server.gatt_registered = False
                gatt, advertising, properties = MagicMock(), MagicMock(), MagicMock()
                properties.Get.return_value = True
                def register_ad(*args, **kwargs):
                    self.assertTrue(server.gatt_registered)
                    if advertise_success:
                        kwargs['reply_handler']()
                    else:
                        kwargs['error_handler'](RuntimeError('no advertising slots'))
                advertising.RegisterAdvertisement.side_effect = register_ad
                def run_loop():
                    gatt.RegisterApplication.call_args.kwargs['reply_handler']()
                server.loop.run.side_effect = run_loop
                interfaces = {'org.bluez.GattManager1': gatt,
                              'org.bluez.LEAdvertisingManager1': advertising,
                              PROPERTIES: properties}
                with patch('ble_lab.bluez.dbus.Interface', side_effect=lambda _, kind: interfaces[kind]), \
                        patch('ble_lab.bluez.signal.signal'), patch('ble_lab.bluez.GLib.timeout_add'), \
                        patch('ble_lab.bluez.ready') as notify:
                    if advertise_success:
                        Server.run(server)
                        notify.assert_called_once()
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'registration failed'):
                            Server.run(server)
                        notify.assert_not_called()
                    gatt.UnregisterApplication.assert_called_once()


if __name__ == '__main__':
    unittest.main()
