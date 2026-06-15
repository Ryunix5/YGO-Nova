#!/usr/bin/env python3
# ─── relay_server.py ─────────────────────────────────────────────────────────
#
# EdoPro+ online relay / room server.
#
# This is a TRANSPORT-ONLY relay. It runs NO ocgcore and never interprets
# gameplay. The host remains fully authoritative: it sends FieldSnapshot /
# PromptSnapshot, the client sends ClientChoice, and the server simply
# forwards those frames between the two peers in a room. This solves NAT /
# port-forwarding for internet play while preserving the existing
# host-authoritative protocol byte-for-byte.
#
# Wire format (identical to NetSession.h — every frame is):
#   [4B magic 0x45444F50 LE][4B version LE][4B type LE][4B len LE][N payload]
# Integers little-endian. Strings are [4B len LE][bytes]. The relay only
# parses a handful of CONTROL message types (room setup); all other frames
# are forwarded verbatim, so the relay never needs to understand or inspect
# hidden card data.
#
# Room flow:
#   1. Host connects, sends CreateRoom(name)            → RoomCreated(code)
#   2. Guest connects, sends JoinRoom(name, code)       → RoomJoined(...)
#      and the host is told RoomPeerJoined(guestName).
#   3. From then on every non-control frame from one peer is relayed to the
#      other. The existing Hello/DeckInfo/Ready/StartDuel + FieldSnapshot /
#      PromptSnapshot / ClientChoice handshake runs end-to-end through the
#      relay unchanged.
#
# Usage:
#   python tools/relay_server.py [--host 0.0.0.0] [--port 7879] [--verbose]
#
# No accounts, no passwords, no matchmaking — guest names only, by design.
#
import argparse
import os
import signal
import socket
import struct
import threading
import time
import random
import string
import sys

# ── Protocol constants (must match NetSession.h) ─────────────────────────────
MAGIC            = 0x45444F50          # 'EDOP'
PROTOCOL_VERSION = 1
HEADER_SIZE      = 16
MAX_PAYLOAD      = 16 * 1024 * 1024     # 16 MiB — matches the client cap

# Gameplay message types (1..16) are forwarded verbatim; we never parse them.
# Relay-control types (100+) are consumed by the server. Ping/Pong (8/9) are
# answered locally for heartbeat and not forwarded.
T_PING            = 8
T_PONG            = 9
T_CREATE_ROOM     = 101
T_ROOM_CREATED    = 102
T_JOIN_ROOM       = 103
T_ROOM_JOINED     = 104
T_ROOM_PEER_JOIN  = 105
T_ROOM_ERROR      = 106
T_ROOM_CLOSED     = 107
T_LIST_ROOMS      = 108   # client -> server: request the open-room list
T_ROOM_LIST       = 109   # server -> client: [u32 N][ code, host, u8 players, u8 state ]*

CONTROL_TYPES = {T_CREATE_ROOM, T_JOIN_ROOM, T_LIST_ROOMS}

# Room state codes sent in the room list.
STATE_WAITING  = 0   # has a host, no guest — joinable
STATE_READY    = 1   # both present, not started
STATE_IN_DUEL  = 2

# Room-code alphabet excludes ambiguous glyphs (0/O, 1/I) so codes read aloud
# cleanly. 5 chars → ~28 million combinations, plenty for casual play.
CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
CODE_LEN      = 5

HEARTBEAT_TIMEOUT = 45.0   # seconds without any frame → drop the connection


# ── Frame encode / decode ────────────────────────────────────────────────────
def pack_frame(msg_type: int, payload: bytes = b"") -> bytes:
    return struct.pack("<IIII", MAGIC, PROTOCOL_VERSION, msg_type,
                       len(payload)) + payload


def put_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<I", len(b)) + b


def put_u8(v: int) -> bytes:
    return struct.pack("<B", v & 0xFF)


def read_str(payload: bytes, off: int):
    if off + 4 > len(payload):
        return "", off
    (n,) = struct.unpack_from("<I", payload, off)
    off += 4
    if off + n > len(payload):
        return "", off
    s = payload[off:off + n].decode("utf-8", "replace")
    return s, off + n


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes or raise ConnectionError on close."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return bytes(buf)


def read_frame(sock: socket.socket):
    """Return (msg_type, payload) or raise on error/close."""
    hdr = recv_exact(sock, HEADER_SIZE)
    magic, version, mtype, plen = struct.unpack("<IIII", hdr)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08X}")
    if version != PROTOCOL_VERSION:
        raise ValueError(f"version mismatch (got {version})")
    if plen > MAX_PAYLOAD:
        raise ValueError(f"payload too large ({plen})")
    payload = recv_exact(sock, plen) if plen else b""
    return mtype, payload


# ── Room + connection model ──────────────────────────────────────────────────
class Conn:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.name = "Player"
        self.room = None          # Room
        self.role = None          # "host" | "guest"
        self.last_seen = time.time()
        self.send_lock = threading.Lock()
        self.relayed = 0

    def send(self, msg_type, payload=b""):
        try:
            with self.send_lock:
                self.sock.sendall(pack_frame(msg_type, payload))
            return True
        except OSError:
            return False


class Room:
    def __init__(self, code):
        self.code = code
        self.host = None          # Conn
        self.guest = None         # Conn
        self.created = time.time()
        self.started = False      # set once a StartDuel frame is relayed

    def state_code(self):
        if self.host and self.guest:
            return STATE_IN_DUEL if self.started else STATE_READY
        return STATE_WAITING

    def players(self):
        return (1 if self.host else 0) + (1 if self.guest else 0)

    def peer_of(self, conn):
        if conn is self.host:
            return self.guest
        if conn is self.guest:
            return self.host
        return None

    def empty(self):
        return self.host is None and self.guest is None


class RelayServer:
    def __init__(self, verbose=False):
        self.rooms = {}                  # code -> Room
        self.lock = threading.Lock()
        self.verbose = verbose
        self.total_relayed = 0

    # ── Logging ──────────────────────────────────────────────────────────
    def log(self, *a):
        print(f"[{time.strftime('%H:%M:%S')}]", *a, flush=True)

    def vlog(self, *a):
        if self.verbose:
            self.log(*a)

    # ── Room helpers ─────────────────────────────────────────────────────
    def _new_code(self):
        for _ in range(40):
            code = "".join(random.choice(CODE_ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms:
                return code
        # Extremely unlikely fallback.
        return "".join(random.choice(CODE_ALPHABET) for _ in range(CODE_LEN + 2))

    def _unique_name(self, room, name):
        """Avoid a guest sharing the host's exact display name."""
        name = (name or "Player").strip()[:32] or "Player"
        existing = []
        if room.host and room.host.name:
            existing.append(room.host.name)
        if room.guest and room.guest.name:
            existing.append(room.guest.name)
        if name in existing:
            name = f"{name} (2)"
        return name

    # ── Control handlers ─────────────────────────────────────────────────
    def handle_create(self, conn, payload):
        name, _ = read_str(payload, 0)
        with self.lock:
            code = self._new_code()
            room = Room(code)
            conn.name = self._unique_name(room, name)
            room.host = conn
            conn.room = room
            conn.role = "host"
            self.rooms[code] = room
        conn.send(T_ROOM_CREATED, put_str(code))
        self.log(f"room {code} created by host '{conn.name}' {conn.addr}")

    def handle_join(self, conn, payload):
        name, off = read_str(payload, 0)
        code, off = read_str(payload, off)
        code = code.strip().upper()
        with self.lock:
            room = self.rooms.get(code)
            if room is None:
                conn.send(T_ROOM_ERROR, put_str(f"No room with code {code}"))
                self.log(f"join failed: unknown room {code} from {conn.addr}")
                return
            if room.guest is not None:
                conn.send(T_ROOM_ERROR, put_str("Room is full"))
                self.log(f"join failed: room {code} full")
                return
            conn.name = self._unique_name(room, name)
            room.guest = conn
            conn.room = room
            conn.role = "guest"
            host = room.host
        # Tell the guest it joined (carries the host's name + isHost=0).
        host_name = host.name if host else ""
        jp = struct.pack("<B", 0) + put_str(host_name)
        conn.send(T_ROOM_JOINED, jp)
        # Tell the host a guest arrived (carries the guest's name).
        if host:
            host.send(T_ROOM_PEER_JOIN, put_str(conn.name))
        self.log(f"room {code}: guest '{conn.name}' joined "
                 f"(host '{host_name}')")

    def handle_list(self, conn):
        # Snapshot the rooms that have a host (browsable). Waiting rooms come
        # first so the joinable ones are at the top of the client's list.
        with self.lock:
            rooms = [r for r in self.rooms.values() if r.host is not None]
        rooms.sort(key=lambda r: (r.state_code(), r.created))
        body = struct.pack("<I", len(rooms))
        for r in rooms:
            host_name = r.host.name if r.host else "?"
            body += put_str(r.code) + put_str(host_name)
            body += put_u8(r.players()) + put_u8(r.state_code())
        conn.send(T_ROOM_LIST, body)
        self.vlog(f"room list -> {conn.addr}: {len(rooms)} room(s)")

    # ── Connection lifecycle ─────────────────────────────────────────────
    def serve_conn(self, conn):
        sock = conn.sock
        sock.settimeout(HEARTBEAT_TIMEOUT)
        try:
            while True:
                try:
                    mtype, payload = read_frame(sock)
                except socket.timeout:
                    self.log(f"timeout {conn.addr} - dropping")
                    break
                conn.last_seen = time.time()

                if mtype == T_PING:
                    conn.send(T_PONG)
                    continue
                if mtype == T_CREATE_ROOM:
                    if conn.room is not None:
                        conn.send(T_ROOM_ERROR, put_str("Already in a room"))
                    else:
                        self.handle_create(conn, payload)
                    continue
                if mtype == T_JOIN_ROOM:
                    if conn.room is not None:
                        conn.send(T_ROOM_ERROR, put_str("Already in a room"))
                    else:
                        self.handle_join(conn, payload)
                    continue
                if mtype == T_LIST_ROOMS:
                    self.handle_list(conn)
                    continue
                # Anything else = gameplay frame → forward to the peer.
                self.forward_raw(conn, mtype, payload)
        except (ConnectionError, OSError, ValueError) as e:
            self.vlog(f"conn {conn.addr} closed: {e}")
        finally:
            self.drop_conn(conn)

    def forward_raw(self, conn, msg_type, payload):
        room = conn.room
        if room is None:
            return
        # A StartDuel frame (gameplay type 4) flips the room to "in duel" so
        # the room list shows it as non-joinable.
        if msg_type == 4 and not room.started:
            room.started = True
            self.log(f"room {room.code}: duel started")
        peer = room.peer_of(conn)
        if peer is None:
            # Peer not present (left / not joined yet). Silently drop — the
            # host keeps running authoritatively and will resync on rejoin.
            return
        if peer.send(msg_type, payload):
            conn.relayed += 1
            self.total_relayed += 1
            if self.verbose and conn.relayed % 100 == 0:
                self.vlog(f"room {room.code}: {conn.relayed} frames from "
                          f"{conn.role}")

    def drop_conn(self, conn):
        try:
            conn.sock.close()
        except OSError:
            pass
        with self.lock:
            room = conn.room
            if room is None:
                return
            peer = room.peer_of(conn)
            if conn is room.host:
                room.host = None
            elif conn is room.guest:
                room.guest = None
            # Notify the surviving peer, then close the room if empty.
            if peer is not None:
                peer.send(T_ROOM_CLOSED,
                          put_str(f"{conn.name} disconnected"))
            if room.empty():
                self.rooms.pop(room.code, None)
                self.log(f"room {room.code} closed (empty); "
                         f"relayed {self.total_relayed} frames total")
            else:
                self.log(f"room {room.code}: {conn.role} '{conn.name}' left")

    # ── Accept loop ──────────────────────────────────────────────────────
    def run(self, host, port):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(64)
        # A short accept timeout lets the loop notice a SIGTERM-set stop flag
        # (containers/systemd stop with SIGTERM, not Ctrl+C/SIGINT).
        srv.settimeout(1.0)
        self._stop = False

        def _on_term(signum, _frame):
            self.log(f"received signal {signum} — shutting down")
            self._stop = True
        try:
            signal.signal(signal.SIGTERM, _on_term)
        except (ValueError, OSError):
            pass   # not on the main thread / unsupported — ignore

        self.log(f"EdoPro+ relay server started on {host}:{port} "
                 f"(protocol v{PROTOCOL_VERSION})")
        self.log("waiting for connections - Ctrl+C or SIGTERM to stop")
        try:
            while not self._stop:
                try:
                    csock, addr = srv.accept()
                except socket.timeout:
                    continue
                csock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                conn = Conn(csock, addr)
                self.vlog(f"connection from {addr}")
                threading.Thread(target=self.serve_conn, args=(conn,),
                                 daemon=True).start()
        except KeyboardInterrupt:
            self.log("shutting down (Ctrl+C)")
        finally:
            srv.close()


def main():
    # Env vars are the natural config surface for containers / systemd; CLI
    # flags override them. EDOPRO_RELAY_HOST / EDOPRO_RELAY_PORT / *_VERBOSE.
    env_host = os.environ.get("EDOPRO_RELAY_HOST", "0.0.0.0")
    env_port = int(os.environ.get("EDOPRO_RELAY_PORT", "7879"))
    env_verbose = os.environ.get("EDOPRO_RELAY_VERBOSE", "0") not in ("0", "", "false", "False")

    ap = argparse.ArgumentParser(description="EdoPro+ online relay server")
    ap.add_argument("--host", default=env_host,
                    help="bind address (default 0.0.0.0 / $EDOPRO_RELAY_HOST)")
    ap.add_argument("--port", type=int, default=env_port,
                    help="listen port (default 7879 / $EDOPRO_RELAY_PORT)")
    ap.add_argument("--verbose", action="store_true", default=env_verbose,
                    help="log every connection + periodic relay counts")
    args = ap.parse_args()
    RelayServer(verbose=args.verbose).run(args.host, args.port)


if __name__ == "__main__":
    main()
