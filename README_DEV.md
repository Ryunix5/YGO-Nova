# EdoPro+ — Developer Notes

A modern custom UI on top of the ProjectIgnis `ocgcore` engine. Single-player
duels work end-to-end; replay recording / browsing / playback works; a
multiplayer foundation (UI + protocol scaffolding) is in place but actual
LAN sockets are not wired yet.

## Build

The project is CMake-based. From the project root:

```
cmake -S . -B build
cmake --build build --config Release
```

The build pulls `ocgcore` and Dear ImGui from `vendor/`. SDL2 needs to be
discoverable by CMake (system install, or `SDL2_DIR=...` for a local copy).

## Run

The runtime expects to be launched from a directory where the relative
path `assets/...` resolves to this repo's `assets` folder. The simplest
way is to run from the project root:

```
./build/Release/edopro
```

Game logic uses `assets/cards.cdb` + `assets/scripts/*.lua`. Anything missing
is reported by the in-app **Assets** popup (lobby top-right) and by
`tools/check_install.py`.

## Generate procedural SFX

The runtime ships without bundled audio. To synthesise the 19-sound bank
the SFX layer expects, run:

```
python tools/generate_sfx.py
```

This writes WAV files into `assets/sfx/`. The list of expected names is
the canonical bank in `src/AudioManager.cpp::kSfxBank`.

## Where things live

| Path                       | Purpose                                |
|----------------------------|----------------------------------------|
| `assets/cards.cdb`         | Primary card database                  |
| `assets/BabelCDB-master/`  | Optional .cdb fallbacks                |
| `assets/scripts/*.lua`     | ocgcore card scripts                   |
| `assets/decks/*.ydk`       | Decks usable in lobby + deck builder   |
| `assets/sfx/*.wav`         | Procedural SFX (generated)             |
| `assets/replays/*.json`    | Auto-saved match replays               |
| `assets/config/settings.cfg` | Persisted user settings              |
| `tools/`                   | Python regression scripts              |

The Settings popup (lobby top-right) lets the user toggle every persisted
preference; changes save immediately.

## Multiplayer (LAN, real)

LAN multiplayer is live. TCP sockets via WinSock2 on Windows and BSD
sockets elsewhere, with a dedicated worker thread per session so the
render loop never blocks on network I/O.

* `src/NetSession.h` — protocol versioning, message types
  (`Hello`/`DeckInfo`/`Ready`/`StartDuel`/`EngineResponse`/`Chat`/
  `Disconnect`/`Ping`/`Pong`/`Error`), `NetMode`/`NetState` enums,
  framed wire format (`[u32 magic 0x45444F50][u32 version][u32 type][u32 len][payload]`),
  the `NetSession` class.
* `src/NetSession.cpp` — socket worker, blocking recv with
  `SO_RCVTIMEO` so the worker can still notice shutdown requests, frame
  parser, thread-safe outbox/inbox.
* `Screen::Multiplayer` in `UI.cpp` — Host/Join, deck picker that
  auto-announces on selection, Ready toggle, host-only Start Duel
  button enabled when both peers have decks + ready.

### Design contract

* **Host owns the seed.** When the host clicks Start Duel, both peers
  receive a `StartDuel` packet carrying the seed and both deck lists.
  Both call `m_dm.setForcedSeed(seed); m_dm.startDuel(...)` with the
  same data → byte-identical ocgcore state.
* **Player responses are the wire unit.** Local clicks generate
  response bytes via the existing `respond*` helpers, which call
  `submitResponse`, which calls our recorder. In MP mode the recorder
  pushes those same bytes into `NetSession::send(EngineResponse)`. The
  peer receives the packet, sets `m_mpFeedingRemote = true` (so the
  recorder doesn't echo), then calls `m_dm.respond(...)` to feed the
  bytes into the engine.
* **Local player index**: Host → 0 (P1), Client → 1 (P2). Input gating
  via `isLocalPromptOwner()` — only the seat owner answers. A
  "Waiting for opponent..." overlay appears mid-screen when the engine
  is paused on the remote seat's prompt.

### Host a game

1. Open Multiplayer from the main menu.
2. Set display name + port (default 7878) → Host Game.
3. Choose deck (auto-announced after handshake) → Ready.
4. Once the client connects, sends their deck, and toggles Ready, the
   Start Duel button enables. Click it to seed and broadcast.

### Join a game

1. Open Multiplayer.
2. Enter host IP + port + display name → Join Game.
3. Choose deck → Ready.
4. Wait for the host to click Start Duel.

## Online play (relay server)

LAN play needs both peers on the same network. **Online mode** adds a small
relay server so players can connect over the internet without port-forwarding
on the *client* side — both peers connect *out* to the relay.

### Architecture — the relay is transport only

* The relay runs **no ocgcore** and never interprets gameplay. The host stays
  fully authoritative: it sends `FieldSnapshot`/`PromptSnapshot`, the client
  sends `ClientChoice`, exactly as in LAN.
* The relay parses only the room-control message types (`CreateRoom`,
  `JoinRoom` → `RoomCreated`/`RoomJoined`/`RoomPeerJoined`/`RoomError`/
  `RoomClosed`, types 101–107 in `NetSession.h` / `relay_server.py`). **Every
  other frame it forwards verbatim** to the room's other peer, so the entire
  host-authoritative protocol is unchanged byte-for-byte.
* Client side: `NetSession::joinRelay(addr, port, name, createRoom, code)`
  connects to the relay and sets `NetMode::Host` (room creator) or
  `NetMode::Client` (joiner). From there `localPlayerIndex()` and all routing
  are identical to LAN — only the transport differs. The relay worker does
  **not** auto-send `Hello`; `UI::mpKickoffHandshake()` drives the existing
  Hello/deck/ready handshake once the room is formed (on `RoomPeerJoined` for
  the host, `RoomJoined` for the guest).

### Run the relay

```
python tools/run_relay_server.py            # prints LAN/loopback addresses
# or directly:
python tools/relay_server.py --port 7879 [--verbose]
```

Check config + reachability, and smoke-test the relay end-to-end:

```
python tools/check_online_config.py --host 127.0.0.1 --port 7879
python tools/relay_smoke_test.py --spawn      # auto-starts a server, runs 2 clients
```

### Play online

1. Start the relay (host PC, or any reachable box; forward the relay's TCP
   port if it's behind a router for internet play).
2. Both players: Multiplayer → **Online (relay)** → set the relay address/port.
3. Host: **Create Room** → share the shown room code (Copy room code).
4. Guest: type the code → **Join Room**.
5. Both pick a deck + Ready; the host clicks **Start Duel**.
6. Gameplay routes through the relay using the same host-authoritative
   snapshots/choices as LAN. A peer drop shows a friendly "Opponent left"
   notice; the duel pauses (no further snapshots/choices arrive).

Chat is relayed transparently (the `Chat` frame is < 101, so the server
forwards it like any gameplay frame).

## Replay system

* Every duel auto-saves a JSON replay to `assets/replays/` when it ends,
  unless `autoSaveReplays` is off in Settings.
* The Replays screen lists files newest-first; selecting one shows
  metadata, decks, outcome, and a step-by-step event timeline.
* `Play Replay` seeds a new duel with the recorded seed + decks and feeds
  the recorded response bytes back into ocgcore. Live input is locked
  while in replay mode.
* `tools/check_replay_files.py` audits the `.json` files on disk.

## Diagnostic exports

* **Assets popup** — startup health summary (PASS/WARN lines), file paths,
  `Copy diagnostics` for clipboard.
* **Debug popup** — engine state, audio status, log toggle flags,
  `Copy debug report` (compact) and `Copy Full Diagnostics` (full report
  including the last 100 debug log lines, suitable for bug reports).

## Troubleshooting

| Symptom                                  | Likely fix                                                 |
|------------------------------------------|------------------------------------------------------------|
| No sound                                 | Run `python tools/generate_sfx.py`; check Audio popup mute |
| Card not found / "missing from CDB"      | Place `cards.cdb` in `assets/`; check Assets popup         |
| "Script missing" warnings on a card      | Add the matching `c<code>.lua` to `assets/scripts/`        |
| Replay desync — engine waits, no bytes   | Replay was recorded against different scripts/CDB; reload  |
| Replay says "engine awaits a response …" | Use `Step (try once)` in the desync modal; or restart      |
| Multiplayer "connect failed"             | Verify host is running + firewall allows the chosen port   |
| Multiplayer "bind failed"                | Port is in use; pick another (run `check_multiplayer_config.py`) |
| Multiplayer desync                       | Verify both peers run the same scripts + CDB version       |
| Multiplayer "Connection lost"            | Save replay from the modal, then reconnect from MP screen  |
| Online "cannot connect" to relay         | Start it: `python tools/run_relay_server.py`; check the port + firewall |
| Online "No room with code …"             | Code is case-insensitive but must be exact; ask host to re-share |
| Online protocol mismatch                 | Run `python tools/check_online_config.py` — client/server versions must match |
| App crashes on launch                    | Run `python tools/check_install.py` and report the output  |

## Regression scripts

| Tool                                | What it checks                                |
|-------------------------------------|-----------------------------------------------|
| `tools/check_install.py`            | Full install audit (CDB, scripts, SFX, decks) |
| `tools/check_deck_compat.py <.ydk>` | Every card resolves; section + copy limits    |
| `tools/check_replay_files.py`       | JSON parse + metadata sanity for replays      |
| `tools/check_multiplayer_config.py` | Port range, bind, loopback round-trip         |

Each exits with the number of FAIL items so they can be wired into CI.

## Contributing — guard rails

* `ocgcore/` and `vendor/imgui/` are vendored upstream — do not modify.
* `DuelManager` exposes a public API; new features should live in UI / new
  modules instead.
* Settings keys: add the field to `Settings.h`, the writer line in
  `save()`, and the parser case in `applyKV()`.
* New screens go through `enum class Screen` in `UI.h` + a `drawXxx`
  method + a `switch` case in `UI::draw`.
