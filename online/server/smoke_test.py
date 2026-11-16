"""Smoke test for the GODZILLA shadNet server: two players sign in as the game does and go through
Random VS Play's matchmaking: one creates a VS room carrying the port's match key, the other finds it
by that key (and does not with another group's), joins it, and the host hears the join; the joiner
looks up the host's UDP endpoint, and the relay passes packets between the two both ways. Both
leave the room at the end.

    python -I smoke_test.py [host] [port] [--key KEY] [--accounts FILE] [--group NAME]

--key       the server's registration key (RegistrationSecretKey), needed on an invite-only server.
--accounts  a JSON file for two test accounts, smoketest-host and smoketest-join: created on the
            first run with random passwords kept in that file, signed in again on later runs. For a
            server people use. Without it, two new throwaway accounts are registered every run (with
            random passwords no one knows).
--group     the launcher's Match group to test (default: empty, the public pool).

Exits 0 when everything passed.
"""
import argparse
import json
import os
import secrets
import socket
import struct
import sys
import time

TITLE_ID, TITLE_NAME = "CUSA02046", "GODZILLA"
UDP_PORT, RELAY_PORT = 31314, 31316
MT_ONLINE_PROTOCOL = 3  # src/runtime.h: the port's online protocol, part of the match key
GAME_VERSION = "01.01"   # the game's APP_VER with its update, also part of the match key
MATCH_KEY_ATTR = 0x53   # src/runtime_np.c: the searchable int attribute that carries the match key
CMD = {"login": 0, "create_account": 2, "client_version": 13, "context_start": 100, "create_room": 101,
       "join_room": 102, "leave_room": 103, "search_room": 104, "signaling_info": 105, "context_stop": 106,
       "world_list": 111}
NOTIFY_ROOM_EVENT, ROOM_EVENT_MEMBER_JOINED = 10, 0x1101
ERRORS = {0: "NoError", 1: "Malformed", 2: "Invalid", 3: "InvalidInput", 5: "LoginError",
          6: "LoginAlreadyLoggedIn", 8: "LoginInvalidPassword", 10: "CreationError",
          11: "CreationExistingUsername", 14: "RoomMissing", 16: "RoomFull"}


class Failed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failed(message)


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


def match_key(group="", version=GAME_VERSION):
    """runtime_np.c match_key() of a copy with exact math: FNV-1a of
    "minustwo-online/<protocol>/<game version>/<group>", never 0."""
    h = 2166136261
    for byte in f"minustwo-online/{MT_ONLINE_PROTOCOL}/{version}/{group}".encode():
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return (h & 0x7FFFFFFF) | 1


# ---- shadNet framing ----
class Client:
    def __init__(self, host, port, name):
        self.name = name
        self.sock = socket.create_connection((host, port), timeout=10)
        self.packet_id = 0
        self.notifications = []
        kind, _, size, _ = struct.unpack("<BHIQ", self.recv(15))
        info = self.recv(size - 15)
        check(kind == 3, f"expected the server's greeting, got packet type {kind}")
        print(f"[{name}] connected, server protocol version {struct.unpack('<I', info)[0]}")

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise Failed(f"[{self.name}] the server closed the connection")
            data += chunk
        return data

    def request(self, command, proto=b"", label=""):
        self.packet_id += 1
        payload = struct.pack("<I", len(proto)) + proto
        self.sock.sendall(struct.pack("<BHIQ", 0, CMD[command], 15 + len(payload), self.packet_id) + payload)
        while True:
            kind, cmd, size, pid = struct.unpack("<BHIQ", self.recv(15))
            body = self.recv(size - 15)
            if kind == 1 and pid == self.packet_id:
                error = body[0]
                blob = b""
                if len(body) >= 5:
                    n = struct.unpack_from("<I", body, 1)[0]
                    blob = body[5:5 + n]
                print(f"[{self.name}] {label or command}: {ERRORS.get(error, f'error {error}')}")
                return error, blob
            if kind == 2:
                self.notifications.append((cmd, body))

    def close(self):
        self.sock.close()


def stun_ping(host, npid):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    packet = bytes.fromhex("FF83FFFFFFFF") + b"\x01" + npid.encode().ljust(16, b"\0") + socket.inet_aton("127.0.0.1")
    s.sendto(packet, (host, UDP_PORT))
    try:
        reply, _ = s.recvfrom(64)
    except socket.timeout:
        raise Failed(f"no STUN answer on UDP {UDP_PORT}: firewall?")
    ip, port = socket.inet_ntoa(reply[6:10]), struct.unpack(">H", reply[10:12])[0]
    print(f"[{npid}] STUN: the server sees this player at {ip}:{port}")
    return s


def sign_in(args, name, password):
    c = Client(args.host, args.port, name)
    err, blob = c.request("login", f_str(1, name) + f_str(2, password) + f_str(4, TITLE_ID) + f_str(5, TITLE_NAME),
                          "sign in as GODZILLA")
    if err == 8:  # no such account yet (or a wrong password): register it, as the launcher's first sign-in does
        proto = f_str(1, name) + f_str(2, password) + f_str(4, f"{name}@smoketest.invalid")
        if args.key:
            proto += f_str(5, args.key)
        err, _ = c.request("create_account", proto, "register")
        check(err == 0, "registration refused" + (": wrong --key?" if args.key else ": invite-only server? pass --key")
              if err != 11 else f"{name} exists with another password (the accounts file was lost?)")
        err, blob = c.request("login", f_str(1, name) + f_str(2, password) + f_str(4, TITLE_ID) + f_str(5, TITLE_NAME),
                              "sign in as GODZILLA")
    check(err == 0, f"{name} could not sign in")
    print(f"[{name}]   account id {decode(blob).get(2, ['?'])[0]}")
    c.request("client_version", f_str(1, "MinusTwo GODZILLA smoke test"), "report client version")
    err, _ = c.request("context_start", f_int(1, 1), "matchmaking context start")
    check(err == 0, "matchmaking refused (Matching2Enabled, or the client version rules)")
    return c


def accounts(args):
    """(host name, password), (joiner name, password)."""
    if not args.accounts:
        tag = str(int(time.time()) % 100000)
        return (f"GodzHost{tag}", secrets.token_urlsafe(18)), (f"GodzJoin{tag}", secrets.token_urlsafe(18))
    saved = {}
    if os.path.exists(args.accounts):
        with open(args.accounts, encoding="utf-8") as f:
            saved = json.load(f)
    changed = False
    for name in ("smoketest-host", "smoketest-join"):
        if name not in saved:
            saved[name] = secrets.token_urlsafe(18)
            changed = True
    if changed:
        with open(args.accounts, "w", encoding="utf-8") as f:
            json.dump(saved, f, indent=2)
        print(f"test account passwords saved in {args.accounts}")
    return ("smoketest-host", saved["smoketest-host"]), ("smoketest-join", saved["smoketest-join"])


def search(client, key, label):
    """The rooms Random VS Play's search finds: world 1, filtered on the match key like runtime_np.c."""
    key_filter = f_str(7, f_int(1, 1) + f_int(2, MATCH_KEY_ATTR) + f_int(3, key))  # op 1: equal
    err, blob = client.request("search_room", f_int(1, 1) + f_int(3, 1) + f_int(4, 20) + key_filter, label)
    check(err == 0, "room search failed")
    return [decode(r) for r in decode(blob).get(1, [])]


def relay_check(host, label_a, label_b):
    token = secrets.token_bytes(16)
    a, b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for s, label in ((a, label_a), (b, label_b)):
        s.settimeout(5)
        s.sendto(b"MTRL" + bytes([1]) + token, (host, RELAY_PORT))
        try:
            reply, _ = s.recvfrom(64)
        except socket.timeout:
            raise Failed(f"no relay answer on UDP {RELAY_PORT}: is relay.py running, and the port open?")
        check(reply == b"MTRL" + bytes([2]) + token, "the relay answered something unexpected")
        print(f"[{label}] relay: registered")
    for sender, receiver, label in ((a, b, f"{label_a} -> {label_b}"), (b, a, f"{label_b} -> {label_a}")):
        payload = secrets.token_bytes(24)
        sender.sendto(b"MTRL" + bytes([3]) + token + payload, (host, RELAY_PORT))
        try:
            got, _ = receiver.recvfrom(256)
        except socket.timeout:
            raise Failed(f"relay: nothing came through {label}")
        check(got[21:] == payload, f"relay: {label} arrived changed")
        print(f"relay: a packet passed {label}")
    a.close()
    b.close()


def run(args):
    (host_name, host_password), (joiner_name, joiner_password) = accounts(args)
    key, other_key = match_key(args.group), match_key(args.group + "-not-this-group")
    print(f"match group '{args.group or 'public'}': match key {key:#x}\n")

    host = sign_in(args, host_name, host_password)
    stun = stun_ping(args.host, host_name)
    err, blob = host.request("world_list", f_int(1, 1), "world list")
    worlds = [decode(w).get(1, [0])[0] for w in decode(blob).get(1, [])]
    print(f"[{host_name}]   worlds: {worlds}")
    check(1 in worlds, "no world 1 for GODZILLA (worlds.cfg)")

    room_attr = f_str(11, f_int(1, MATCH_KEY_ATTR) + f_int(2, key))  # the room's match key, as the game sets it
    err, blob = host.request("create_room", f_int(1, 1) + f_int(2, 2) + f_int(4, 1) + room_attr,
                             "create a 2-player VS room in world 1")
    check(err == 0, "room creation failed")
    room_id = decode(blob)[1][0]
    print(f"[{host_name}]   room {room_id:#x}")

    joiner = sign_in(args, joiner_name, joiner_password)
    stun_ping(args.host, joiner_name).close()
    rooms = search(joiner, key, "search with this match key")
    found = [r for r in rooms if r.get(7, [None])[0] == room_id]
    print(f"[{joiner_name}]   {len(rooms)} room(s) found; the host's room among them: {bool(found)}")
    check(found, "the joiner's search did not find the host's room")
    others = [r for r in search(joiner, other_key, "search with another group's key") if r.get(7, [None])[0] == room_id]
    check(not others, "a search with another match key found the room: the match key does not filter")
    print(f"[{joiner_name}]   not found with another group's key: right")

    err, _ = joiner.request("join_room", f_int(1, room_id) + f_int(2, 2), "join the host's room")
    check(err == 0, "joining the room failed")
    host.request("world_list", f_int(1, 1), "(host: anything, to read its notifications)")
    joined = [body for cmd, body in host.notifications if cmd == NOTIFY_ROOM_EVENT]
    events = [decode(body[4:] if len(body) >= 4 and struct.unpack_from("<I", body)[0] == len(body) - 4 else body)
              for body in joined]
    check(any(e.get(3, [0])[0] == ROOM_EVENT_MEMBER_JOINED for e in events), "the host did not hear the join")
    print(f"[{host_name}]   heard: a member joined the room")

    err, blob = joiner.request("signaling_info", f_str(1, host_name), "look up the host's UDP endpoint")
    info = decode(blob)
    check(err == 0 and 2 in info, "no UDP endpoint for the host")
    print(f"[{joiner_name}]   host reachable at {info[2][0].decode()}:{info[3][0]}")

    relay_check(args.host, host_name, joiner_name)

    joiner.request("leave_room", f_int(1, room_id) + f_int(2, 3), "leave the room")
    host.request("leave_room", f_int(1, room_id) + f_int(2, 4), "leave the room")
    for c in (joiner, host):
        c.request("context_stop", f_int(1, 1), "matchmaking context stop")
        c.close()
    stun.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("host", nargs="?", default="127.0.0.1")
    parser.add_argument("port", nargs="?", type=int, default=31313)
    parser.add_argument("--key", default="", help="the server's registration key")
    parser.add_argument("--accounts", help="JSON file keeping two test accounts' passwords")
    parser.add_argument("--group", default="", help="the Match group to test (default: public)")
    args = parser.parse_args()
    try:
        run(args)
    except (Failed, OSError) as e:
        print(f"\nFAIL: {e}")
        sys.exit(1)
    print("\nPASS: sign-in, matchmaking with the match key, joining, room events, peer lookup and the relay "
          "all work.")


if __name__ == "__main__":
    main()
