"""Smoke test for the GODZILLA shadNet server: two players register, sign in as the game,
start matchmaking, and one finds the other's VS room and its UDP endpoint.

Usage: python -I smoke_test.py [host] [port]
"""
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 31313
UDP_PORT = 31314
TITLE_ID, TITLE_NAME = "CUSA02046", "GODZILLA"

ERRORS = {0: "NoError", 1: "Malformed", 2: "Invalid", 3: "InvalidInput", 5: "LoginError",
          6: "LoginAlreadyLoggedIn", 8: "LoginInvalidPassword", 10: "CreationError",
          11: "CreationExistingUsername"}


# ---- minimal protobuf encoding/decoding ----
def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def f_str(field, value):
    data = value.encode() if isinstance(value, str) else value
    return varint(field << 3 | 2) + varint(len(data)) + data


def f_int(field, value):
    return varint(field << 3) + varint(value) if value else b""


def decode(buf):
    """Decode a protobuf message into {field: [values]}; nested messages stay as bytes."""
    out, i = {}, 0
    while i < len(buf):
        key, i = read_varint(buf, i)
        field, wire = key >> 3, key & 7
        if wire == 0:
            val, i = read_varint(buf, i)
        elif wire == 2:
            n, i = read_varint(buf, i)
            val, i = buf[i:i + n], i + n
        elif wire == 1:
            val, i = struct.unpack_from("<Q", buf, i)[0], i + 8
        elif wire == 5:
            val, i = struct.unpack_from("<I", buf, i)[0], i + 4
        else:
            raise ValueError(f"wire type {wire}")
        out.setdefault(field, []).append(val)
    return out


def read_varint(buf, i):
    n = shift = 0
    while True:
        b = buf[i]
        i += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, i


# ---- shadNet framing ----
class Client:
    def __init__(self, name):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.packet_id = 0
        kind, _, size, _ = struct.unpack("<BHIQ", self.recv(15))
        info = self.recv(size - 15)
        assert kind == 3, f"expected ServerInfo, got packet type {kind}"
        print(f"[{name}] connected, server protocol version {struct.unpack('<I', info)[0]}")

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("server closed the connection")
            data += chunk
        return data

    def request(self, command, proto=b"", label=""):
        self.packet_id += 1
        payload = struct.pack("<I", len(proto)) + proto
        self.sock.sendall(struct.pack("<BHIQ", 0, command, 15 + len(payload), self.packet_id) + payload)
        while True:
            kind, cmd, size, pid = struct.unpack("<BHIQ", self.recv(15))
            body = self.recv(size - 15)
            if kind == 1 and pid == self.packet_id:
                error = body[0]
                blob = b""
                if len(body) >= 5:
                    n = struct.unpack_from("<I", body, 1)[0]
                    blob = body[5:5 + n]
                status = ERRORS.get(error, f"error {error}")
                print(f"[{self.name}] {label or command}: {status}")
                return error, blob
            if kind == 2:
                print(f"[{self.name}]   (notification {cmd}, {len(body)} bytes)")


def stun_ping(npid):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    packet = bytes.fromhex("FF83FFFFFFFF") + b"\x01" + npid.encode().ljust(16, b"\0") + socket.inet_aton("127.0.0.1")
    s.sendto(packet, (HOST, UDP_PORT))
    reply, _ = s.recvfrom(64)
    ip, port = socket.inet_ntoa(reply[6:10]), struct.unpack(">H", reply[10:12])[0]
    print(f"[{npid}] STUN: server sees this player at {ip}:{port}")
    return s


def sign_in(npid):
    c = Client(npid)
    err, _ = c.request(2, f_str(1, npid) + f_str(2, "testpass123") + f_str(4, f"{npid.lower()}@test.local"),
                       "register")
    assert err in (0, 11), "registration failed"
    err, blob = c.request(0, f_str(1, npid) + f_str(2, "testpass123") + f_str(4, TITLE_ID) + f_str(5, TITLE_NAME),
                          "login as GODZILLA")
    assert err == 0, "login failed"
    print(f"[{npid}]   account id {decode(blob).get(2, ['?'])[0]}")
    c.request(13, f_str(1, "MinusTwo GODZILLA smoke test"), "report client version")
    err, _ = c.request(100, f_int(1, 1), "matchmaking context start")
    assert err == 0, "context start failed"
    return c


def main():
    tag = str(int(time.time()) % 100000)
    host_name, joiner_name = f"GodzHost{tag}", f"GodzJoin{tag}"

    host = sign_in(host_name)
    stun = stun_ping(host_name)
    err, blob = host.request(111, f_int(1, 1), "world list")
    worlds = [decode(w) for w in decode(blob).get(1, [])]
    print(f"[{host_name}]   worlds: {[w.get(1, [0])[0] for w in worlds]}")
    assert worlds, "no worlds configured for GODZILLA"

    err, blob = host.request(101, f_int(1, 1) + f_int(2, 2) + f_int(4, 1), "create VS room (2 slots, world 1)")
    assert err == 0, "room creation failed"
    room = decode(blob)
    room_id = room[1][0]
    print(f"[{host_name}]   room {room_id:#x}, member id {room.get(5, [0])[0]}")

    joiner = sign_in(joiner_name)
    err, blob = joiner.request(104, f_int(1, 1) + f_int(3, 1) + f_int(4, 20), "search rooms in world 1")
    rooms = [decode(r) for r in decode(blob).get(1, [])]
    found = [r for r in rooms if r[7][0] == room_id]
    owner = found[0][18][0].decode() if found else None
    print(f"[{joiner_name}]   {len(rooms)} room(s) found; host's room present: {bool(found)}, owner {owner}")
    assert found, "joiner could not see the host's room"

    err, blob = joiner.request(105, f_str(1, host_name), "look up host's UDP endpoint")
    info = decode(blob)
    print(f"[{joiner_name}]   host reachable at {info[2][0].decode()}:{info[3][0]}")

    stun.close()
    print("\nPASS: register, sign-in, matchmaking, room search and peer lookup all work.")


if __name__ == "__main__":
    main()
