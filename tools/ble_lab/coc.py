# SPDX-License-Identifier: GPL-3.0-or-later
"""Production LE Credit Based L2CAP listener using the lab's saved identity."""
import os
import socket
import threading


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
            # PSM zero asks Linux to allocate an unused dynamic LE PSM.
            # The last tuple field selects LE Public rather than BR/EDR.
            listener.bind((str(adapter_address), 0, 0,
                           getattr(socket, 'BDADDR_LE_PUBLIC', 1)))
            self.psm = listener.getsockname()[1]
            if not 0x80 <= self.psm <= 0xff or not self.psm & 1:
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
