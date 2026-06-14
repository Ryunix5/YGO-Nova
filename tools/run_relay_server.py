#!/usr/bin/env python3
# ─── run_relay_server.py ─────────────────────────────────────────────────────
#
# Convenience launcher for the EdoPro+ relay server. Prints the LAN addresses
# players should use, then starts relay_server.py with sensible defaults.
#
# Usage:
#   python tools/run_relay_server.py [--port 7879] [--verbose]
#
import argparse
import os
import socket
import sys


def local_ips():
    ips = set()
    try:
        host = socket.gethostname()
        for info in socket.getaddrinfo(host, None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    # Best-effort outbound-route IP (no packets actually sent).
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    ips.discard("127.0.0.1")
    return sorted(ips)


def main():
    ap = argparse.ArgumentParser(description="Launch the EdoPro+ relay server")
    ap.add_argument("--port", type=int, default=7879)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    print("=" * 60)
    print(" EdoPro+ Relay Server")
    print("=" * 60)
    print(f" Port: {args.port}")
    print(" Players on THIS machine connect to: 127.0.0.1")
    lan = local_ips()
    if lan:
        print(" Players on your LAN connect to one of:")
        for ip in lan:
            print(f"     {ip}")
    print(" Players over the internet connect to your PUBLIC IP")
    print(f"   (forward TCP port {args.port} on your router to this PC).")
    print("=" * 60)
    print()

    # Run the server IN-PROCESS. The previous os.execv() approach broke when
    # sys.executable lived under a path with a space (e.g. "C:\Program Files\
    # Python312\python.exe") — Windows re-parsed it and the launch failed with
    # "C:\Program: can't open file ...". Importing + calling main() avoids any
    # path/exec quirk entirely.
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    try:
        import relay_server
    except ImportError as e:
        print(f"[error] could not import relay_server.py: {e}")
        print(f"[error] expected it next to this script in: {here}")
        sys.exit(1)
    # Hand argv to relay_server's argparse, then run its server loop here.
    sys.argv = ["relay_server.py", "--port", str(args.port)]
    if args.verbose:
        sys.argv.append("--verbose")
    relay_server.main()


if __name__ == "__main__":
    main()
