# EdoPro+

A modern, single-binary **Yu-Gi-Oh! duel simulator** with a clean ImGui
interface, offline practice duels, replays, a deck builder, and online play
through a lightweight relay server.

> Fan-made, non-commercial project. Not affiliated with or endorsed by Konami.
> "Yu-Gi-Oh!" and all related names are trademarks of their respective owners.

---

## Features

- **Testing (single-player)** — practice duels against a basic auto-opponent,
  with a coin toss for who goes first, full per-phase pacing, and animations.
- **Online rooms** — a live, auto-refreshing lobby. Create or join rooms on a
  relay server, with Quick Match, join-by-code, surrender, and a shared result
  screen.
- **LAN multiplayer** — direct host/join on your local network.
- **Deck Builder** — build and edit `.ydk` decks against the bundled card DB.
- **Replays** — every duel can be auto-saved and played back move-by-move.

---

## Requirements

- **Windows 10/11 (x64).**
- **Microsoft Visual C++ 2015–2022 Redistributable (x64)** — required if the
  app fails to start with a missing `vcruntime140.dll` / `msvcp140.dll`.
  Download: <https://aka.ms/vs/17/release/vc_redist.x64.exe>
- **Internet connection** (first run) — to keep the download small, the release
  ships **without** the card-image pack. Card art is fetched from a public CDN
  the first time each card is shown and cached to `assets/cards/`, so it loads
  locally forever after. Disable in Settings → "Download missing card art on
  demand" if you prefer local-only (cards then show a placeholder).
- For **online play** you (or a friend) need a reachable **relay server**
  (see below). Python 3.9+ is required only to *host* a relay.

---

## Running

1. Unzip the release anywhere.
2. Keep `EdoProPlus.exe` and the `assets/` folder **together** — the app loads
   the card database, scripts, fonts, and sound effects from `assets/`.
3. Run `EdoProPlus.exe`.

From the main menu:

- **ONLINE ROOMS** — opens the live lobby (needs a relay server address).
- **TESTING** — single-player practice duel; pick two decks and start.
- **DECK BUILDER**, **REPLAYS**, **LAN MULTIPLAYER** — as labelled.

---

## Online play (relay server)

Both players connect *out* to a shared relay server, so no port forwarding is
needed on the player side — only the relay host needs a reachable address/port.

**To host a relay** (one machine, the default port is `7879`):

```sh
python tools/run_relay_server.py
```

Leave it running. Share its **IP address** and **port** with players.

**To play:**

1. Main menu → **ONLINE ROOMS**.
2. Settings → set the **Server address** to the relay's IP and the **Port**.
3. **Quick Match** to auto-join the first open room (or host one if none),
   **Create Room** to host, or paste a room **code** to join a specific room.

For a quick local test, run the relay on the same machine and use
`127.0.0.1` as the server address in two app instances.

**Hosting a public relay** (so friends/strangers can play without your laptop
on): see [`deploy/HOSTING.md`](deploy/HOSTING.md) for step-by-step setup on a
free Oracle Cloud VM, a cheap VPS (systemd service), or Docker/Fly.io — plus
how to bake your server address in as the app's default.

---

## Settings worth knowing

- **Developer mode** (Settings → Developer) unlocks the Debug panel, layout
  guides, verbose logs, and Testing-Mode timeline rewind. Off by default.
- **Coin toss** (Settings → Gameplay UI) decides who takes the first turn in
  offline duels; turn it off for "Player 1 always first".
- **Auto-save replays** writes a `.json` to `assets/replays/` at duel end.

---

## Building from source

See [`README_DEV.md`](README_DEV.md) for the full developer setup. In short
(Windows, with Visual Studio 2022 + CMake 3.26+ + vcpkg):

```bat
build.bat
```

To produce a release zip after building:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version 1.0.0
```

---

## Third-party components & licensing

EdoPro+ bundles community and open-source components. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for attributions (ocgcore,
Dear ImGui, SDL2, Lua, SQLite, stb, and the card database/scripts).

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| App won't start, missing `vcruntime140.dll` | Install the VC++ x64 Redistributable (link above). |
| "Cannot reach relay server" | Confirm the relay is running and the address/port match; check firewall. |
| Cards show a purple placeholder instead of art | First-run art download — check your internet connection, or it's a card the CDN doesn't have. Effects still work. |
| Effects don't work / no cards load at all | Ensure `assets/` (incl. `cards.cdb` and `scripts/`) shipped next to the exe. |
| No sound | Settings → unmute, or check the `assets/sfx/` folder is present. |
"# YGO-Nova" 
