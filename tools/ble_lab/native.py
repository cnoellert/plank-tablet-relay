# SPDX-License-Identifier: GPL-3.0-or-later
"""Typed access to the existing C pairing and Noise implementation."""
import ctypes as C
import os
import time


def now_ms():
    return time.monotonic_ns() // 1_000_000


class ProtocolError(Exception):
    pass


class Native:
    def __init__(self, library, directory):
        self.lib = C.CDLL(str(library))
        signatures = {
            'create': ([C.c_char_p], C.c_void_p),
            'destroy': ([C.c_void_p], None),
            'open_pairing': ([C.c_void_p, C.c_uint64, C.c_uint64], C.c_int),
            'disconnect': ([C.c_void_p, C.c_uint64], None),
            'receive': ([C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t),
                         C.c_uint64, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            'key': ([C.c_void_p, C.c_uint8, C.c_uint64, C.c_void_p,
                     C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            'tablet': ([C.c_void_p, C.c_int], None),
            'button': ([C.c_void_p, C.c_uint16, C.c_int, C.c_uint64, C.c_uint64,
                        C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            'tick': ([C.c_void_p, C.c_uint64, C.c_void_p,
                      C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            'observing': ([C.c_void_p], C.c_int),
            'approval_pending': ([C.c_void_p], C.c_int),
            'pairing_busy': ([C.c_void_p], C.c_int),
            'sample': ([C.c_void_p, C.c_void_p, C.c_size_t,
                        C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)], C.c_int),
            'run_coc_session': ([C.c_void_p, C.c_int, C.c_int], C.c_int),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.lib, 'pltr_ble_lab_' + name)
            function.argtypes, function.restype = arguments, result
        self.handle = self.lib.pltr_ble_lab_create(os.fsencode(directory))
        if not self.handle:
            raise ProtocolError('Identity store unavailable; require an owned 0700 directory.')

    def output(self, name, *arguments):
        output, written = (C.c_uint8 * 8448)(), C.c_size_t()
        result = getattr(self.lib, 'pltr_ble_lab_' + name)(
            self.handle, *arguments, output, len(output), C.byref(written))
        if result < 0 or written.value > len(output):
            raise ProtocolError('Protocol operation %s rejected or timed out.' % name)
        return bytes(output[:written.value])

    def receive(self, data):
        replies = []
        while data:
            consumed = C.c_size_t()
            buffer = C.create_string_buffer(data)
            reply = self.output('receive', buffer, len(data), C.byref(consumed), now_ms())
            if not 0 < consumed.value <= len(data):
                raise ProtocolError('Invalid stream consumption.')
            data = data[consumed.value:]
            if reply:
                replies.append(reply)
        return replies

    def open_pairing(self):
        if self.lib.pltr_ble_lab_open_pairing(self.handle, int(time.time()), now_ms()):
            raise ProtocolError('Pairing is active or locked; no window opened.')

    def key(self, digit):
        return self.output('key', digit, now_ms())

    def tablet(self, attached):
        self.lib.pltr_ble_lab_tablet(self.handle, bool(attached))

    def button(self, code, value):
        return self.output('button', code, value, int(time.time()), now_ms())

    def tick(self):
        return self.output('tick', now_ms())

    @property
    def observing(self):
        return bool(self.lib.pltr_ble_lab_observing(self.handle))

    @property
    def approval_pending(self):
        return self.lib.pltr_ble_lab_approval_pending(self.handle)

    @property
    def pairing_busy(self):
        return bool(self.lib.pltr_ble_lab_pairing_busy(self.handle))

    def sample(self, payload):
        return self.output('sample', C.create_string_buffer(payload), len(payload))

    def disconnect(self):
        self.lib.pltr_ble_lab_disconnect(self.handle, now_ms())

    def run_coc_session(self, coc_fd, stop_fd):
        return self.lib.pltr_ble_lab_run_coc_session(self.handle, coc_fd, stop_fd)

    def close(self):
        if self.handle:
            self.lib.pltr_ble_lab_destroy(self.handle)
            self.handle = None
