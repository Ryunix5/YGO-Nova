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

    here = os.path.dirname(os.path.abspath(__file__))
    server = os.path.join(here, "relay_server.py")
    argv = [sys.executable, server, "--port", str(args.port)]
    if args.verbose:
        argv.append("--verbose")
    os.execv(sys.executable, argv)


if __name__ == "__main__":
    main()
