#!/usr/bin/env python3
r"""A stand-in for the Discord app's local IPC endpoint, for testing the Discord status
(src/runtime_discord.c) without the player's Discord: it answers the handshake as the app does
(READY), answers SET_ACTIVITY, and prints each status it gets with the seconds since it started.

    python tools/fake_discord.py minustwo-fake-discord --seconds 120

A name is \\.\pipe\<name> on Windows and $TMPDIR/<name> (or /tmp) elsewhere; a path is used as
given. Start the game with MT_DISCORD_PIPE=<the same name> and MT_DISCORD_APP_ID=<any id> (a scripted
run needs the id to turn the status on). --refuse answers the handshake as Discord does an unknown
application (a CLOSE frame).
"""
import argparse
import json
import os
import struct
import sys
import time

OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG = range(5)


class WindowsPipe:
    def __init__(self, path):
        import ctypes
        from ctypes import wintypes
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.CreateNamedPipeW.restype = wintypes.HANDLE
        self.ctypes, self.wintypes = ctypes, wintypes
        PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_WAIT = 3, 0, 0
        self.h = self.k.CreateNamedPipeW(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
                                         None)
        if self.h in (None, wintypes.HANDLE(-1).value):
            raise OSError(f'CreateNamedPipe failed: {ctypes.get_last_error()}')

    def accept(self):
        ok = self.k.ConnectNamedPipe(self.h, None)
        if not ok and self.ctypes.get_last_error() != 535:  # ERROR_PIPE_CONNECTED
            raise OSError(f'ConnectNamedPipe failed: {self.ctypes.get_last_error()}')

    def read(self, n):
        out = b''
        while len(out) < n:
            buf = self.ctypes.create_string_buffer(n - len(out))
            got = self.wintypes.DWORD()
            if not self.k.ReadFile(self.h, buf, n - len(out), self.ctypes.byref(got), None) or not got.value:
                raise EOFError
            out += buf.raw[:got.value]
        return out

    def write(self, data):
        got = self.wintypes.DWORD()
        if not self.k.WriteFile(self.h, data, len(data), self.ctypes.byref(got), None):
            raise EOFError

    def close(self):
        self.k.CloseHandle(self.h)


class UnixSocket:
    def __init__(self, path):
        import socket
        if os.path.exists(path):
            os.unlink(path)
        self.path = path
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(path)
        self.server.listen(1)

    def accept(self):
        self.conn, _ = self.server.accept()

    def read(self, n):
        out = b''
        while len(out) < n:
            chunk = self.conn.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def write(self, data):
        self.conn.sendall(data)

    def close(self):
        self.server.close()
        os.unlink(self.path)


def frame(op, payload):
    data = json.dumps(payload, separators=(',', ':')).encode()  # compact, as the Discord app writes it
    return struct.pack('<II', op, len(data)) + data


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('path')
    p.add_argument('--seconds', type=float, default=0, help='stop after this long (0: when the game disconnects)')
    p.add_argument('--refuse', action='store_true', help='turn the application away, as Discord does an unknown one')
    a = p.parse_args()
    if '/' not in a.path and '\\' not in a.path:
        a.path = (f'\\\\.\\pipe\\{a.path}' if sys.platform == 'win32' else
                  os.path.join(os.environ.get('TMPDIR') or '/tmp', a.path))
    endpoint = WindowsPipe(a.path) if sys.platform == 'win32' else UnixSocket(a.path)
    start = time.monotonic()
    say = lambda text: print(f'{time.monotonic() - start:7.1f} {text}', flush=True)
    say(f'listening on {a.path}')
    endpoint.accept()
    say('the game connected')
    try:
        while not a.seconds or time.monotonic() - start < a.seconds:
            op, length = struct.unpack('<II', endpoint.read(8))
            body = json.loads(endpoint.read(length) or b'{}')
            if op == OP_HANDSHAKE:
                say(f'handshake: {body}')
                if a.refuse:
                    endpoint.write(frame(OP_CLOSE, {'code': 4000, 'message': 'Invalid Client ID'}))
                    break
                endpoint.write(frame(OP_FRAME, {'cmd': 'DISPATCH', 'evt': 'READY',
                                                'data': {'v': 1, 'user': {'username': 'stand-in'}}}))
            elif op == OP_FRAME and body.get('cmd') == 'SET_ACTIVITY':
                activity = body.get('args', {}).get('activity')
                say(f'status: {json.dumps(activity) if activity else "cleared"}')
                endpoint.write(frame(OP_FRAME, {'cmd': 'SET_ACTIVITY', 'data': activity, 'evt': None,
                                                'nonce': body.get('nonce')}))
            elif op == OP_PING:
                endpoint.write(frame(OP_PONG, body))
            elif op == OP_CLOSE:
                say('the game closed the connection')
                break
            else:
                say(f'frame {op}: {body}')
    except EOFError:
        say('the game disconnected')
    finally:
        endpoint.close()


if __name__ == '__main__':
    main()
