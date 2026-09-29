# SPDX-License-Identifier: GPL-3.0-or-later
"""BlueZ peripheral for the authenticated PLANK input-observer lab."""
import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib
import signal
import time

from .capture import Capture
from .coc import ProductionCoC
from .controller import clear_advertisements, disable_address_resolution
from .notify import ready
from .native import Native, ProtocolError
from .transport import EchoChannel, Indications, write_peer

SERVICE_UUID = '462f3a10-7a31-4ab3-9e7f-c36af495ecf0'
RX_UUID = '462f3a11-7a31-4ab3-9e7f-c36af495ecf0'
TX_UUID = '462f3a12-7a31-4ab3-9e7f-c36af495ecf0'
ECHO_RX_UUID = '462f3a13-7a31-4ab3-9e7f-c36af495ecf0'
ECHO_TX_UUID = '462f3a14-7a31-4ab3-9e7f-c36af495ecf0'
PSM_UUID = 'abdd3056-28fa-441d-a470-55a75a52553a'
PROPERTIES = 'org.freedesktop.DBus.Properties'
OBJECTS = 'org.freedesktop.DBus.ObjectManager'
GATT = 'org.bluez.GattCharacteristic1'
SERVICE = 'org.bluez.GattService1'
ADVERTISEMENT = 'org.bluez.LEAdvertisement1'
BASE = '/la/instinctual/plank/tablet_lab'


class Rejected(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.NotPermitted'


class Object(dbus.service.Object):
    def __init__(self, bus, path, interface, properties):
        super().__init__(bus, path)
        self.path, self.interface, self.properties = path, interface, properties

    @dbus.service.method(PROPERTIES, in_signature='s', out_signature='a{sv}')
    def GetAll(self, interface):
        if interface != self.interface:
            raise Rejected('Unknown interface')
        return self.properties

    @dbus.service.method(PROPERTIES, in_signature='ss', out_signature='v')
    def Get(self, interface, name):
        if interface != self.interface or name not in self.properties:
            raise Rejected('Unknown property')
        return self.properties[name]

    @dbus.service.signal(PROPERTIES, signature='sa{sv}as')
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class Advertisement(Object):
    def __init__(self, bus, name):
        super().__init__(bus, BASE + '/advertisement', ADVERTISEMENT, {
            'Type': 'peripheral', 'ServiceUUIDs': dbus.Array([SERVICE_UUID], signature='s'),
            'LocalName': name, 'Discoverable': dbus.Boolean(True)})

    @dbus.service.method(ADVERTISEMENT)
    def Release(self):
        pass


class Characteristic(Object):
    def __init__(self, server, transmit, diagnostic=False):
        self.server, self.transmit, self.diagnostic = server, transmit, diagnostic
        self.endpoint = server.echo if diagnostic else server
        path = BASE + '/service/' + ('echo_' if diagnostic else '') + ('tx' if transmit else 'rx')
        uuid = (ECHO_TX_UUID if transmit else ECHO_RX_UUID) if diagnostic else (TX_UUID if transmit else RX_UUID)
        super().__init__(server.bus, path, GATT, {
            'UUID': uuid,
            'Service': dbus.ObjectPath(BASE + '/service'),
            'Flags': dbus.Array(['indicate'] if transmit else ['write'], signature='s'),
            **({'Notifying': dbus.Boolean(False)} if transmit else {})})

    @dbus.service.method(GATT, in_signature='aya{sv}')
    def WriteValue(self, value, options):
        if self.transmit:
            raise Rejected('Write to RX')
        if self.diagnostic:
            self.server.receive_echo(bytes(value), options)
        else:
            self.server.receive(bytes(value), options)

    @dbus.service.method(GATT)
    def StartNotify(self):
        if not self.transmit:
            raise Rejected('Subscribe to TX')
        self.endpoint.notifying = True
        print('Bluetooth test replies subscribed.' if self.diagnostic else 'Relay replies subscribed.', flush=True)
        self.properties['Notifying'] = dbus.Boolean(True)
        self.PropertiesChanged(GATT, {'Notifying': dbus.Boolean(True)}, [])

    @dbus.service.method(GATT)
    def StopNotify(self):
        if self.transmit:
            self.endpoint.notifying = False
            self.properties['Notifying'] = dbus.Boolean(False)
            self.endpoint.disconnect()
            self.PropertiesChanged(GATT, {'Notifying': dbus.Boolean(False)}, [])

    @dbus.service.method(GATT)
    def Confirm(self):
        if self.transmit:
            self.endpoint.queue.confirm()


class PSMCharacteristic(Object):
    def __init__(self, server):
        self.server = server
        super().__init__(server.bus, BASE + '/service/psm', GATT, {
            'UUID': PSM_UUID,
            'Service': dbus.ObjectPath(BASE + '/service'),
            'Flags': dbus.Array(['read'], signature='s')})

    @dbus.service.method(GATT, in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, options):
        if not self.server.coc or not self.server.coc.psm:
            raise Rejected('Production Bluetooth channel unavailable.')
        psm = self.server.coc.psm
        return dbus.Array([psm & 0xff, psm >> 8], signature='y')


class Server(dbus.service.Object):
    def __init__(self, args):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SystemBus()
        super().__init__(self.bus, BASE)
        self.loop = GLib.MainLoop()
        self.adapter = '/org/bluez/' + args.adapter
        self.controller_workaround = args.disable_controller_address_resolution
        self.exclusive_adapter = getattr(args, 'exclusive_adapter', False)
        self.notify_systemd = getattr(args, 'notify_systemd', False)
        self.native = None if args.transport_only else Native(args.library, args.state_dir)
        self.coc = ProductionCoC(self.native, lambda: not self.peer) if self.native else None
        self.capture = None if args.transport_only else Capture(args.tablet, self.button)
        self.peer = None
        self.notifying = False
        self.queue = Indications(self.emit)
        self.echo = EchoChannel(self.emit_echo, self.close_peer)
        self.last_sample = 0
        self.was_observing = False
        self.failure = None
        self.service = Object(self.bus, BASE + '/service', SERVICE,
            {'UUID': SERVICE_UUID, 'Primary': dbus.Boolean(True)})
        self.rx = Characteristic(self, False) if self.native else None
        self.tx = Characteristic(self, True) if self.native else None
        self.echo_rx, self.echo_tx = Characteristic(self, False, True), Characteristic(self, True, True)
        self.psm_characteristic = PSMCharacteristic(self) if self.coc else None
        self.advertisement = Advertisement(self.bus, getattr(args, 'name', 'PLANK Relay Lab'))
        self.gatt_registered = self.advertising = False
        self.bus.add_signal_receiver(self.device_changed, dbus_interface=PROPERTIES,
            signal_name='PropertiesChanged', path_keyword='path', arg0='org.bluez.Device1')
        self.bus.add_signal_receiver(self.removed, dbus_interface=OBJECTS,
            signal_name='InterfacesRemoved')
        self.bus.add_signal_receiver(self.adapter_changed, dbus_interface=PROPERTIES,
            signal_name='PropertiesChanged', path=self.adapter, arg0='org.bluez.Adapter1')
        self.bus.add_signal_receiver(self.bluez_changed, dbus_interface='org.freedesktop.DBus',
            signal_name='NameOwnerChanged', arg0='org.bluez')

    @dbus.service.method(OBJECTS, out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {dbus.ObjectPath(obj.path): {obj.interface: obj.properties}
                for obj in (self.service, self.rx, self.tx, self.echo_rx, self.echo_tx,
                            self.psm_characteristic) if obj is not None}

    def emit_echo(self, data):
        if not self.echo.peer or not self.echo.notifying:
            raise ProtocolError('Bluetooth test subscription ended.')
        self.echo_tx.PropertiesChanged(GATT, {'Value': dbus.Array(data, signature='y')}, [])

    def receive_echo(self, data, options):
        try:
            self.echo.receive(data, options, self.adapter, self.peer)
        except ValueError as error:
            raise Rejected(str(error))
        except (ProtocolError, BufferError, TimeoutError) as error:
            self.echo.disconnect()
            raise Rejected(str(error))

    def emit(self, data):
        if not self.peer or not self.notifying:
            raise ProtocolError('Bluetooth subscription ended.')
        self.tx.PropertiesChanged(GATT, {'Value': dbus.Array(data, signature='y')}, [])

    def receive(self, data, options):
        if self.coc:
            with self.coc.gate:
                if self.coc.active:
                    raise Rejected('Bluetooth data channel is active; pairing is unavailable.')
                return self._receive_pairing(data, options)
        return self._receive_pairing(data, options)

    def _receive_pairing(self, data, options):
        if not self.native or self.echo.peer:
            raise Rejected('Bluetooth transport test is active; pairing is unavailable.')
        try:
            peer = write_peer(data, options, self.adapter, self.notifying, self.peer)
        except ValueError as error:
            print('Relay write rejected before protocol processing.', flush=True)
            raise Rejected(str(error))
        self.queue.mtu_payload = max(20, min(512, int(options.get('mtu', 23)) - 3))
        try:
            # Drain before every fragment, including the one completing START.
            # Earlier physical events must not become approval for a new request.
            self.capture.poll()
            self.native.tablet(self.capture.attached)
            if not self.peer:
                self.peer = peer
                print('Headset transport connected; authenticating.', flush=True)
            was_pending = self.native.approval_pending
            for reply in self.native.receive(data):
                self.queue.append(reply)
            if not was_pending and self.native.approval_pending:
                kind = 'Existing' if self.native.approval_pending == 2 else 'New'
                print(kind + ' headset pairing request; awaiting three tablet-button presses.', flush=True)
                if not self.capture.attached:
                    print('Pairing is waiting for the selected tablet to connect.', flush=True)
        except (ProtocolError, BufferError, TimeoutError) as error:
            print('Relay write failed: ' + str(error), flush=True)
            self.disconnect()
            raise Rejected('Protocol rejected; existing trust retained.')

    def button(self, code, value):
        self.native.tablet(self.capture.attached)
        self.queue.append(self.native.button(code, value))

    def disconnect(self):
        peer, self.peer = self.peer, None
        self.queue.clear()
        if self.native:
            self.native.disconnect()
        self.was_observing = False
        if peer:
            print('Headset link closed; existing trust retained.', flush=True)
            self.close_peer(peer)

    def close_peer(self, peer):
        try:
            dbus.Interface(self.bus.get_object('org.bluez', peer), 'org.bluez.Device1').Disconnect(
                reply_handler=lambda: None, error_handler=lambda error: None)
        except dbus.exceptions.DBusException:
            pass  # The adapter or BlueZ may already have disappeared.

    def device_changed(self, interface, changed, invalidated, path):
        if str(path) == self.peer and 'Connected' in changed and not changed['Connected']:
            self.disconnect()
        if str(path) == self.echo.peer and 'Connected' in changed and not changed['Connected']:
            self.echo.disconnect()

    def removed(self, path, interfaces):
        if str(path) == self.adapter and 'org.bluez.Adapter1' in interfaces:
            self.failure = 'Bluetooth adapter removed; service will retry.'
            self.loop.quit()
        if str(path) == self.peer and 'org.bluez.Device1' in interfaces:
            self.disconnect()
        if str(path) == self.echo.peer and 'org.bluez.Device1' in interfaces:
            self.echo.disconnect()

    def adapter_changed(self, interface, changed, invalidated):
        if 'Powered' in changed and not changed['Powered']:
            self.failure = 'Bluetooth controller powered off; service will retry.'
            self.loop.quit()

    def bluez_changed(self, name, previous, current):
        if previous and previous != current:
            self.failure = 'BlueZ restarted; service will register again.'
            self.loop.quit()

    def tick(self):
        if self.coc and self.coc.failure:
            self.failure = self.coc.failure
            self.loop.quit()
            return False
        try:
            self.echo.tick()
        except (ProtocolError, BufferError, TimeoutError) as error:
            print(str(error), flush=True)
            self.echo.disconnect()
        if not self.native:
            return True
        try:
            self.capture.poll()
            self.native.tablet(self.capture.attached)
            self.queue.check_timeout()
            self.queue.append(self.native.tick())
            observing = self.native.observing
            if observing and not self.was_observing:
                self.capture.dirty = True
                print('Authenticated input observer started.', flush=True)
            self.was_observing = observing
            now = time.monotonic()
            if (observing and not self.queue.busy and now - self.last_sample >= 0.05 and
                (self.capture.dirty or now - self.last_sample >= 1)):
                self.queue.append(self.native.sample(self.capture.sample()))
                self.last_sample = now
        except (ProtocolError, BufferError, TimeoutError) as error:
            print(str(error), flush=True)
            self.disconnect()
        except (OSError, RuntimeError, ValueError) as error:
            self.failure = str(error)
            self.loop.quit()
            return False
        return True

    def run(self):
        adapter = self.bus.get_object('org.bluez', self.adapter)
        gatt = dbus.Interface(adapter, 'org.bluez.GattManager1')
        advertising = dbus.Interface(adapter, 'org.bluez.LEAdvertisingManager1')
        properties = dbus.Interface(adapter, PROPERTIES)
        if self.controller_workaround or self.exclusive_adapter:
            if (properties.Get('org.bluez.Adapter1', 'Discovering') or
                    properties.Get('org.bluez.LEAdvertisingManager1', 'ActiveInstances')):
                raise RuntimeError('Adapter setup requires no scan or advertisement active.')
        if self.exclusive_adapter:
            properties.Set('org.bluez.Adapter1', 'Powered', dbus.Boolean(True))
            properties.Set('org.bluez.Adapter1', 'Pairable', dbus.Boolean(False))
        if not properties.Get('org.bluez.Adapter1', 'Powered'):
            raise RuntimeError('Bluetooth adapter is powered off.')
        if self.exclusive_adapter:
            clear_advertisements(self.adapter.rsplit('/', 1)[1])
        if self.controller_workaround:
            disable_address_resolution(self.adapter.rsplit('/', 1)[1])
            print('Controller address-resolution workaround applied.', flush=True)

        if self.coc:
            address = str(properties.Get('org.bluez.Adapter1', 'Address'))
            self.coc.start(address)
            print(f'Production Bluetooth channel ready on PSM {self.coc.psm}.', flush=True)

        def failed(error):
            self.failure = 'BlueZ registration failed: ' + str(error)
            self.loop.quit()

        def advertised():
            self.advertising = True
            print('Bluetooth relay is advertising. Use the app to discover it.', flush=True)
            if self.notify_systemd:
                try:
                    ready()
                except OSError as error:
                    failed(error)

        def registered():
            self.gatt_registered = True
            advertising.RegisterAdvertisement(self.advertisement.path, {},
                reply_handler=advertised, error_handler=failed)

        signal.signal(signal.SIGINT, lambda *_: self.loop.quit())
        signal.signal(signal.SIGTERM, lambda *_: self.loop.quit())
        if self.capture:
            self.capture.discover()
        else:
            print('Transport-only mode: no tablet capture, pairing or identity store is opened.', flush=True)
        gatt.RegisterApplication(BASE, {}, reply_handler=registered, error_handler=failed)
        GLib.timeout_add(10, self.tick)
        try:
            self.loop.run()
        finally:
            if self.coc:
                self.coc.close()
            self.disconnect()
            self.echo.disconnect()
            for enabled, function, path in (
                (self.advertising, advertising.UnregisterAdvertisement, self.advertisement.path),
                (self.gatt_registered, gatt.UnregisterApplication, BASE)):
                if enabled:
                    try:
                        function(path)
                    except dbus.exceptions.DBusException:
                        pass
            if self.capture:
                self.capture.close()
            if self.native:
                self.native.close()
        if self.failure:
            raise RuntimeError(self.failure)
