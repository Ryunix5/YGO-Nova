#!/usr/bin/env python3
"""
check_multiplayer_config.py — validate multiplayer settings + run basic
network diagnostics on localhost.

Usage:

    python tools/check_multiplayer_config.py

Reads assets/config/settings.cfg (if present) for the user's preferred
host/port/display-name and verifies:
  * port is in a legal range
  * port is bindable on localhost
  * a TCP listener can accept a local self-connection (round-trip)

No external dependencies. Times out fast so the script can be wired into
CI later.
"""

from __future__ import annotations
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SETTINGS_PATH = ROOT / "assets" / "config" / "settings.cfg"


def load_settings() -> dict[str, str]:
    out: dict[str, str] = {}
    if not SETTINGS_PATH.is_file():
        return out
    for raw in SETTINGS_PATH.read_text(encoding="utf-8",
                                       errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        k, _, v = line.partition("=")
        out[k.strip()] = v.strip()
    return out


def check_port(port: int) -> bool:
    if not (1 <= port <= 65535):
        print(f"  [FAIL] port {port}: out of range (1..65535)")
        return False
    print(f"  [PASS] port {port}: in legal range")
    return True


def check_bind(port: int) -> bool:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", port))
            s.listen(1)
        print(f"  [PASS] bind 127.0.0.1:{port}: succeeded")
        return True
    except OSError as e:
        print(f"  [WARN] bind 127.0.0.1:{port}: {e} "
              f"(port may already be in use)")
        return False


def check_loopback(port: int) -> bool:
    """Spawn a listener thread + client connect to verify round-trip."""
    ok = [False]
    def server() -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("127.0.0.1", port))
                s.listen(1)
                s.settimeout(2.0)
                conn, _ = s.accept()
                with conn:
                    conn.settimeout(2.0)
                    msg = conn.recv(64)
                    if msg == b"HELO":
                        conn.sendall(b"ACK")
                        ok[0] = True
        except OSError:
            pass

    t = threading.Thread(target=server, daemon=True)
    t.start()
    time.sleep(0.1)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as c:
            c.sendall(b"HELO")
            r = c.recv(64)
            if r != b"ACK":
                print(f"  [FAIL] loopback {port}: unexpected reply {r!r}")
                return False
    except OSError as e:
        print(f"  [FAIL] loopback {port}: {e}")
        return False
    t.join(timeout=2.0)
    if ok[0]:
        print(f"  [PASS] loopback {port}: HELO/ACK round-trip OK")
        return True
    print(f"  [WARN] loopback {port}: handshake incomplete")
    return False


def main() -> int:
    print("check_multiplayer_config.py — multiplayer diagnostics\n")
    settings = load_settings()
    if not settings:
        print(f"  (no settings file at {SETTINGS_PATH} — defaults used)\n")

    name = settings.get("mpDisplayName", "Player")
    host = settings.get("mpHostIP", "127.0.0.1")
    try:
        port = int(settings.get("mpPort", "7878"))
    except ValueError:
        port = 7878
    mode = settings.get("mpMode", "offline")
    print(f"  display name : {name}")
    print(f"  host IP      : {host}")
    print(f"  port         : {port}")
    print(f"  mode         : {mode}\n")

    fails = 0
    if not check_port(port):       fails += 1
    if not check_bind(port):       pass            # warn only
    if not check_loopback(port):   pass            # warn only

    # Local hostname resolution (sanity).
    try:
        host_name = socket.gethostname()
        local_ip = socket.gethostbyname(host_name)
        print(f"  [PASS] hostname: {host_name} -> {local_ip}")
    except OSError as e:
        print(f"  [WARN] hostname lookup failed: {e}")

    print(f"\nSummary: {fails} FAIL")
    return fails


if __name__ == "__main__":
    sys.exit(main())
