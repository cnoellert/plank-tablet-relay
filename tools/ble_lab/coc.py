# SPDX-License-Identifier: GPL-3.0-or-later
"""Production LE Credit Based L2CAP listener using the lab's saved identity."""
import ctypes
import os
import socket
import threading


class _SockaddrL2(ctypes.Structure):
    # Linux bluetooth/l2cap.h. Python's socket.bind() exposes only the
    # BR/EDR two-field address, so it cannot select an LE address type.
    _fields_ = [('family', ctypes.c_ushort), ('psm', ctypes.c_ushort),
                ('address', ctypes.c_ubyte * 6), ('cid', ctypes.c_ushort),
                ('address_type', ctypes.c_ubyte)]


def _bind_le_public(listener, adapter_address):
    octets = adapter_address.split(':')
    if len(octets) != 6 or any(len(octet) != 2 for octet in octets):
        raise ValueError('Invalid Bluetooth adapter address.')
    address = bytes(int(octet, 16) for octet in octets)
    target = _SockaddrL2()
    target.family = socket.AF_BLUETOOTH
    target.psm = 0  # Linux allocates an LE dynamic PSM.
    target.address[:] = address[::-1]
    target.address_type = 1  # BDADDR_LE_PUBLIC in bluetooth/bluetooth.h.
    if ctypes.sizeof(target) != 14:
        raise RuntimeError('Unexpected Linux L2CAP socket address layout.')
    bind = ctypes.CDLL(None, use_errno=True).bind
    bind.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_uint)
    bind.restype = ctypes.c_int
    if bind(listener.fileno(), ctypes.byref(target), ctypes.sizeof(target)) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


class ProductionCoC:
    def __init__(self, native, can_accept):
        self.native = native
        self.can_accept = can_accept
        self.listener = None
        self.channel = None
        self.thread = None
        self.psm = 0
        self.active = False
        self.stopping = False
        self.failure = None
        self.gate = threading.Lock()
        self.stop_read, self.stop_write = os.pipe()
        os.set_blocking(self.stop_write, False)

    def start(self, adapter_address):
        if self.listener:
            raise RuntimeError('Production Bluetooth listener already started.')
        listener = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET,
                                 socket.BTPROTO_L2CAP)
        try:
            _bind_le_public(listener, str(adapter_address))
            self.psm = listener.getsockname()[1]
            if not 0x80 <= self.psm <= 0xff:
                raise RuntimeError('Linux did not allocate a valid LE PSM.')
            listener.listen(1)
            listener.settimeout(0.5)
            self.listener = listener
            self.thread = threading.Thread(target=self._serve, name='plank-ble-coc',
                                           daemon=True)
            self.thread.start()
        except BaseException:
            listener.close()
            raise

    def _serve(self):
        while not self.stopping:
            try:
                channel, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if self.stopping:
                    break
                self.failure = 'Production Bluetooth listener stopped unexpectedly.'
                break
            with channel:
                with self.gate:
                    if self.stopping or not self.can_accept():
                        continue
                    self.channel = channel
                    self.active = True
                try:
                    # The C++ runner validates the saved Client key with the
                    # BLE-specific Noise prologue before starting raw HID.
                    self.native.run_coc_session(channel.fileno(), self.stop_read)
                except Exception as error:
                    self.failure = 'Production Bluetooth session failed: ' + str(error)
                    break
                finally:
                    with self.gate:
                        self.active = False
                        self.channel = None

    def close(self):
        self.stopping = True
        try:
            os.write(self.stop_write, b'x')
        except BlockingIOError:
            pass
        if self.listener:
            self.listener.close()
        if self.channel:
            try:
                self.channel.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=5)
            if self.thread.is_alive():
                raise RuntimeError('Bluetooth session did not stop before identity cleanup.')
        os.close(self.stop_read)
        os.close(self.stop_write)
