#!/usr/bin/env python3
"""netplay_server.py - the meeting point and relay for netplay over the Internet.

    python3 netplay_server.py [--port 47200] [--log]

One UDP port, no state on disk, standard library only. A host asks for a
room and gets a four-digit code; a joiner gives the code and both are told
each other's public address, so they can reach each other directly (each
sends first, which opens its own router for the other - "punch-through").
When that does not get through (strict routers, carrier networks), they send
their game packets here wrapped in RELAY and this forwards them.

Packets (little-endian), all starting with the magic "LGRV" (u32 0x5652474C),
then a type byte:
  1 HOST     host -> server   [code 4 bytes, zeros for a new room]   every 2 s while open
  2 CODE     server -> host   code 4
  3 JOIN     joiner -> server code 4                                  every 0.5 s until PEER
  4 PEER     server -> both   ipv4 4, port u16 (the other side's public address), role u8 (0 you host, 1 you join)
  5 RELAY    either way       code 4, role u8 (the sender's), payload (a netplay packet)
  6 ERROR    server -> sender text
A room lives 10 minutes without a HOST or RELAY packet. Rates are capped per
room (2000 packets/s) so it cannot be used to flood anyone.
"""
import argparse, random, socket, string, struct, sys, time

MAGIC = 0x5652474C
HOST, CODE, JOIN, PEER, RELAY, ERROR = 1, 2, 3, 4, 5, 6
LETTERS = "0123456789"                  # four digits: easy to enter on the headset panel


def addr_bytes(a):
    return socket.inet_aton(a[0]) + struct.pack("<H", a[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47200)
    ap.add_argument("--log", action="store_true")
    o = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", o.port))
    rooms = {}            # code -> {"host": addr, "join": addr|None, "seen": t, "n": packets this second, "sec": int}
    print(f"netplay server on UDP {o.port}", flush=True)

    def log(*a):
        if o.log:
            print(time.strftime("%H:%M:%S"), *a, flush=True)

    def send(t, body, to):
        s.sendto(struct.pack("<IB", MAGIC, t) + body, to)

    last_sweep = time.time()
    while True:
        try:
            b, a = s.recvfrom(2048)
        except ConnectionResetError:
            continue
        now = time.time()
        if now - last_sweep > 30:
            for c in [c for c, r in rooms.items() if now - r["seen"] > 600]:
                log("room", c, "closed (idle)")
                del rooms[c]
            last_sweep = now
        if len(b) < 5 or struct.unpack_from("<I", b)[0] != MAGIC:
            continue
        t = b[4]
        if t == HOST and len(b) >= 9:
            code = b[5:9].decode("ascii", "replace")
            r = rooms.get(code)
            if not r or r["host"] != a:
                if r and r["host"] != a and now - r["seen"] < 30:
                    code = ""                       # someone else's live room: a new one
                if code not in rooms or not code.strip("\0"):
                    code = "".join(random.choice(LETTERS) for _ in range(4))
                    while code in rooms:
                        code = "".join(random.choice(LETTERS) for _ in range(4))
                    rooms[code] = {"host": a, "join": None, "seen": now, "n": 0, "sec": 0}
                    log("room", code, "opened by", a)
            rooms[code]["seen"] = now
            send(CODE, code.encode(), a)
            j = rooms[code]["join"]
            if j:                                   # tell the host again (its PEER may have been lost)
                send(PEER, addr_bytes(j) + b"\x00", a)
        elif t == JOIN and len(b) >= 9:
            code = b[5:9].decode("ascii", "replace").upper()
            r = rooms.get(code)
            if not r:
                send(ERROR, b"no such room", a)
                continue
            if r["join"] != a:
                log("room", code, "joined by", a)
            r["join"] = a
            r["seen"] = now
            send(PEER, addr_bytes(r["host"]) + b"\x01", a)
            send(PEER, addr_bytes(a) + b"\x00", r["host"])
        elif t == RELAY and len(b) >= 10:
            code = b[5:9].decode("ascii", "replace")
            r = rooms.get(code)
            if not r:
                continue
            role = b[9]
            if role == 0 and a == r["host"]:
                to = r["join"]
            elif role == 1:
                if r["join"] != a:                  # a joiner that came straight to the relay
                    r["join"] = a
                to = r["host"]
            else:
                continue
            sec = int(now)
            if r["sec"] != sec:
                r["sec"], r["n"] = sec, 0
            r["n"] += 1
            if to and r["n"] <= 2000:
                s.sendto(b, to)
            r["seen"] = now


if __name__ == "__main__":
    main()
