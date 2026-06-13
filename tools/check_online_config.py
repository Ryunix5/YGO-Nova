#!/usr/bin/env python3
# ─── check_online_config.py ──────────────────────────────────────────────────
#
# Pre-flight check for EdoPro+ online relay play. Verifies that the relay
# constants in the client (NetSession.h) and the server (relay_server.py)
# agree, and that a relay server is reachable at a given address.
#
# Usage:
#   python tools/check_online_config.py [--host 127.0.0.1] [--port 7879]
#
import argparse
import os
import re
import socket
import struct
import sys
import time

MAGIC = 0x45444F50
VER   = 1
T_CREATE_ROOM, T_ROOM_CREATED = 101, 102


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def grep1(path, pattern, cast=int):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            txt = f.read()
    except OSError:
        return None
    m = re.search(pattern, txt)
    if not m:
        return None
    try:
        return cast(m.group(1), 0) if cast is int else cast(m.group(1))
    except Exception:
        return m.group(1)


def check_constants():
    root = repo_root()
    ns = os.path.join(root, "src", "NetSession.h")
    rs = os.path.join(root, "tools", "relay_server.py")
    ok = True

    ns_ver = grep1(ns, r"kNetProtocolVersion\s*=\s*(\d+)")
    ns_magic = grep1(ns, r"kNetMagic\s*=\s*(0x[0-9A-Fa-f]+)")
    rs_ver = grep1(rs, r"PROTOCOL_VERSION\s*=\s*(\d+)")
    rs_magic = grep1(rs, r"MAGIC\s*=\s*(0x[0-9A-Fa-f]+)")

    print("client (NetSession.h):  version=%s magic=%s" % (ns_ver, ns_magic))
    print("server (relay_server):  version=%s magic=%s" % (rs_ver, rs_magic))

    if ns_ver is None or rs_ver is None:
        print("[WARN] could not read one of the version constants")
        ok = False
    elif ns_ver != rs_ver:
        print(f"[FAIL] protocol version mismatch: client {ns_ver} != "
              f"server {rs_ver}")
        ok = False
    else:
        print(f"[PASS] protocol version match ({ns_ver})")

    if ns_magic and rs_magic and int(str(ns_magic), 0) == int(str(rs_magic), 0):
        print(f"[PASS] magic match ({ns_magic})")
    else:
        print(f"[WARN] magic mismatch or unreadable "
              f"(client {ns_magic}, server {rs_magic})")
    return ok


def check_reachable(host, port):
    print(f"\nProbing relay at {host}:{port} ...")
    try:
        s = socket.create_connection((host, port), timeout=4)
        s.settimeout(4)
    except OSError as e:
        print(f"[FAIL] cannot connect: {e}")
        print("       Start one with: python tools/relay_server.py")
        return False
    try:
        name = b"ConfigCheck"
        payload = struct.pack("<I", len(name)) + name
        s.sendall(struct.pack("<IIII", MAGIC, VER, T_CREATE_ROOM,
                              len(payload)) + payload)
        hdr = s.recv(16)
        if len(hdr) < 16:
            print("[FAIL] short reply from server")
            return False
        magic, ver, mtype, plen = struct.unpack("<IIII", hdr)
        if magic != MAGIC:
            print(f"[FAIL] bad magic in reply 0x{magic:08X}")
            return False
        if mtype != T_ROOM_CREATED:
            print(f"[WARN] unexpected reply type {mtype}")
            return False
        body = s.recv(plen)
        (n,) = struct.unpack_from("<I", body, 0)
        code = body[4:4 + n].decode("utf-8", "replace")
        print(f"[PASS] relay reachable — test room created: {code}")
        return True
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="EdoPro+ online config check")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7879)
    ap.add_argument("--no-probe", action="store_true",
                    help="only compare constants; don't contact a server")
    args = ap.parse_args()

    ok = check_constants()
    if not args.no_probe:
        ok = check_reachable(args.host, args.port) and ok
    print("\n" + ("RESULT: OK" if ok else "RESULT: problems found"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
