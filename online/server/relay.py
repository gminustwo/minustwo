"""Match traffic relay for players who cannot reach each other directly (strict NAT on both sides).

The port tries a direct connection first (hole punching); after a few seconds without an answer it
sends that opponent's traffic here instead. Both players register the same token (derived from their
room and member numbers); the relay then passes every packet of one to the other. One token holds at
most two players, and tokens nobody used for a minute are forgotten. Python standard library only.

    python relay.py [port]      (UDP 31316 by default)

Packet: "MTRL", a type (1 register, 2 registered, 3 data), a 16-byte token, then for data the
port's own packet, which the relay forwards whole.
"""
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 31316
IDLE = 60.0
REGISTER, REGISTERED, DATA = 1, 2, 3


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", PORT))
    print(f"relay listening on UDP {PORT}", flush=True)
    pairs = {}  # token -> [[address, last seen], ...] (at most two)
    last_sweep = time.monotonic()
    while True:
        data, address = s.recvfrom(16384)
        now = time.monotonic()
        if now - last_sweep > IDLE:
            for token in [t for t, ends in pairs.items() if all(now - e[1] > IDLE for e in ends)]:
                del pairs[token]
            last_sweep = now
        if len(data) < 21 or data[:4] != b"MTRL":
            continue
        kind, token = data[4], data[5:21]
        ends = [e for e in pairs.get(token, []) if now - e[1] <= IDLE]
        me = next((e for e in ends if e[0] == address), None)
        if me is None:
            if len(ends) >= 2:
                continue  # the pair is complete: a third party is not let in
            me = [address, now]
            ends.append(me)
            print(f"{address[0]}:{address[1]} joined a pair ({len(ends)}/2)", flush=True)
        me[1] = now
        pairs[token] = ends
        if kind == REGISTER:
            s.sendto(b"MTRL" + bytes([REGISTERED]) + token, address)
        elif kind == DATA:
            for other in ends:
                if other[0] != address:
                    s.sendto(data, other[0])


if __name__ == "__main__":
    main()
