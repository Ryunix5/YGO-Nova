#!/usr/bin/env python3
# ─── relay_smoke_test.py ─────────────────────────────────────────────────────
#
# End-to-end smoke test for the EdoPro+ relay server using two raw socket
# clients (no GUI, no ocgcore). Verifies the room handshake and that gameplay
# frames are relayed host<->guest verbatim.
#
# It does NOT need the real client — it speaks the wire format directly, so it
# can run in CI / on a headless box.
#
# Usage:
#   1. Start the server:   python tools/relay_server.py --port 7879
#   2. Run this:           python tools/relay_smoke_test.py --port 7879
#
# Or let it auto-spawn the server:
#                          python tools/relay_smoke_test.py --spawn
#
import argparse
import socket
import struct
import subprocess
import sys
import time
import os

MAGIC = 0x45444F50
VER   = 1
T_CREATE_ROOM, T_ROOM_CREATED   = 101, 102
T_JOIN_ROOM,   T_ROOM_JOINED    = 103, 104
T_ROOM_PEER_JOIN                = 105
T_ROOM_ERROR                    = 106
T_LIST_ROOMS,  T_ROOM_LIST      = 108, 109
T_CHAT = 6           # an existing gameplay type we use as a relay probe


def frame(t, payload=b""):
    return struct.pack("<IIII", MAGIC, VER, t, len(payload)) + payload


def pstr(s):
    b = s.encode()
    return struct.pack("<I", len(b)) + b


def rstr(p, off):
    (n,) = struct.unpack_from("<I", p, off); off += 4
    return p[off:off + n].decode("utf-8", "replace"), off + n


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise ConnectionError("closed")
        buf += c
    return buf


def read_frame(s):
    hdr = recv_exact(s, 16)
    magic, ver, t, ln = struct.unpack("<IIII", hdr)
    assert magic == MAGIC, f"bad magic {magic:#x}"
    assert ver == VER, f"bad version {ver}"
    return t, (recv_exact(s, ln) if ln else b"")


def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(5)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7879)
    ap.add_argument("--spawn", action="store_true",
                    help="auto-start a relay server for the test")
    args = ap.parse_args()

    proc = None
    if args.spawn:
        here = os.path.dirname(os.path.abspath(__file__))
        proc = subprocess.Popen(
            [sys.executable, os.path.join(here, "relay_server.py"),
             "--port", str(args.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.8)

    ok = True
    try:
        # ── Host creates a room ──────────────────────────────────────────
        host = connect(args.host, args.port)
        host.sendall(frame(T_CREATE_ROOM, pstr("HostBot")))
        t, p = read_frame(host)
        assert t == T_ROOM_CREATED, f"expected RoomCreated, got {t}"
        code, _ = rstr(p, 0)
        print(f"[PASS] room created: code={code}")

        # ── Guest joins ─────────────────────────────────────────────────
        guest = connect(args.host, args.port)
        guest.sendall(frame(T_JOIN_ROOM, pstr("GuestBot") + pstr(code)))
        t, p = read_frame(guest)
        assert t == T_ROOM_JOINED, f"expected RoomJoined, got {t}"
        is_host = p[0]
        host_name, _ = rstr(p, 1)
        print(f"[PASS] guest joined: isHost={is_host} hostName='{host_name}'")

        # Host should have been told the peer joined.
        t, p = read_frame(host)
        assert t == T_ROOM_PEER_JOIN, f"expected RoomPeerJoined, got {t}"
        guest_name, _ = rstr(p, 0)
        print(f"[PASS] host notified of guest '{guest_name}'")

        # ── Relay a gameplay frame host -> guest ─────────────────────────
        probe = b"hello-from-host"
        host.sendall(frame(T_CHAT, struct.pack("<I", len(probe)) + probe))
        t, p = read_frame(guest)
        assert t == T_CHAT, f"expected relayed Chat, got {t}"
        (n,) = struct.unpack_from("<I", p, 0)
        got = p[4:4 + n]
        assert got == probe, f"payload mismatch: {got!r}"
        print(f"[PASS] host->guest relay verbatim: {got!r}")

        # ── Relay a gameplay frame guest -> host ─────────────────────────
        probe2 = b"choice-from-guest"
        guest.sendall(frame(T_CHAT, struct.pack("<I", len(probe2)) + probe2))
        t, p = read_frame(host)
        assert t == T_CHAT
        (n,) = struct.unpack_from("<I", p, 0)
        assert p[4:4 + n] == probe2
        print(f"[PASS] guest->host relay verbatim: {probe2!r}")

        # ── Join a bogus room -> friendly error ──────────────────────────
        bad = connect(args.host, args.port)
        bad.sendall(frame(T_JOIN_ROOM, pstr("LostBot") + pstr("ZZZZZ")))
        t, p = read_frame(bad)
        assert t == T_ROOM_ERROR, f"expected RoomError, got {t}"
        msg, _ = rstr(p, 0)
        print(f"[PASS] bad room code rejected: '{msg}'")
        bad.close()

        # ── ListRooms returns the open room ──────────────────────────────
        browser = connect(args.host, args.port)
        browser.sendall(frame(T_LIST_ROOMS))
        t, p = read_frame(browser)
        assert t == T_ROOM_LIST, f"expected RoomList, got {t}"
        (count,) = struct.unpack_from("<I", p, 0)
        off = 4
        found = False
        for _ in range(count):
            rc, off = rstr(p, off)
            hn, off = rstr(p, off)
            players = p[off]; state = p[off + 1]; off += 2
            if rc == code:
                found = True
                print(f"[PASS] room listed: code={rc} host={hn} "
                      f"players={players} state={state}")
        assert found, f"created room {code} not in RoomList"
        browser.close()

        print("\nALL CHECKS PASSED")
    except Exception as e:
        ok = False
        print(f"\n[FAIL] {type(e).__name__}: {e}")
    finally:
        if proc:
            proc.terminate()

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
