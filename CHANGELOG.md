# Changelog

All notable changes to YGO: Nova are documented here.

## 1.0.5

### Added — Arcade (Master Saga) overhaul
- **Shared campaigns with invite codes** — "Invite Friend & Duel" creates a
  PIN-locked room and one invite code (`ROOMC-PIN`); a friend pastes it (no
  save needed on their side) and the campaign syncs to their machine
  automatically. Joining makes you a permanent member.
- **The cycle** — open 10 Master + 10 Secret Packs → duel → BOTH players
  spin the prize wheel → winner +5 leaderboard points → repeat (packs
  restock to 10/10; secret-pack keys reset each cycle).
- **New wheel** — craft credits (1SR+1NR / 2SR+2NR / 1SR+3NR) or 5 bonus
  Secret Packs. Old random-card/free-key rewards removed; token system
  removed.
- **Crafting** — spend wheel credits on exact cards from the secret packs
  you unlock **and open** in the current cycle (new Craft tab).
- **Leaderboard** — member points live in the save and merge whenever two
  members connect; cycle-stage strip shows where the group is.

### Added — Online
- **Optional room passwords** — set one when hosting; locked rooms show a
  padlock in the lobby and prompt joiners; Quick Match skips them.

### Changed — Deck builder rework
- EdoPro-style deck area: full-width section bars (Deck/Extra/Side counts +
  Monster/Spell/Trap and Fusion/Xyz/Synchro/Link breakdowns), tight
  full-art tiles (no plates/borders/copy badges), Extra/Side always one
  row, everything fits on screen with no scrolling.
- Master Duel-style search: results are a horizontal card-art grid;
  double-click adds, right-click picks the section, drag places anywhere.
- Format/banlist + per-deck sleeve moved into a top-bar "Format" popup;
  unsaved/violation indicators moved into the Deck bar.
- Wider deck column; deadbanded integer tile sizing (no jitter).

### Changed — UI reorganisation
- Solo setup: two-column modal (decks | rules + practice modes, each with
  a one-line explainer).
- Puzzles: card-style rows with difficulty stripe + BOARD BREAK / SOLVE IT
  chips, double-click to play.

### Fixed
- **Crisp card text at small sizes** — mipmaps are now built gamma-correct
  (linear-light) with a sharper LOD bias; downscaled card text no longer
  looks pixelated. Anisotropic filtering raised to 8x.
- Arcade "Invite Friend & Duel" no longer fails with relay error 10061
  (empty relay-address buffer fell back to 127.0.0.1).
- ImGui boundary assert in the online room's opponent panel.

## 1.0.4

### Fixed (image quality — the big one)
- **Crisp app on scaled displays** — the app is now per-monitor DPI-aware;
  previously Windows bitmap-stretched the whole window on 125%/150% display
  scaling (the "blurry / low-res / old" look). Fonts rasterize at native
  physical resolution with oversampling.
- **Sharp card art** — the bundled image pack was 177×254 thumbnails; art
  smaller than full size now auto-re-downloads at 813×1185 from the CDN and
  hot-swaps in as you play. Mipmaps are also now generated correctly on the
  core-profile GL context (trilinear + anisotropic filtering).

### Added
- **On-demand card scripts** — decks are scanned at duel start and any
  missing card scripts are fetched automatically (a few KB per new card), so
  cards newer than your install just work. Toggleable in Settings.
- **Hand-Trap Gauntlet** — you go first; the AI opens with a hand of meta
  hand traps (Ash, Imperm, Droll…) and fires them at your combo.
- **Crash reporter** — fatal errors write a dump + report and offer a
  one-click pre-filled GitHub issue.
- **UI theme packs** — Crimson / Midnight / Emerald / Mono.
- **Per-deck sleeves** and **custom field mats** (assets/mats/).
- **Duel & menu music playlists** — drop .wav files into assets/music/.
- **Discord Rich Presence** (needs a free Discord Application ID in Settings).
- Deck builder layout: card info on the left, search on the right.

## 1.0.3

### Fixed (important)
- **"Crashes on start" on a clean PC** — the VC++ 2015–2022 runtime DLLs
  (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) are now bundled
  next to the exe, so the app runs without the Microsoft Visual C++
  Redistributable installed. This was the cause of immediate crashes on
  fresh machines.

### Added
- **Startup splash** — fades the sponsor logo + "Sponsored by Dark Side",
  then "Made by Ryunix"; click/Space/Enter/Esc skips.
- **Deck archetype detector** — names a deck's dominant archetypes by real
  setcode (ranked most-mentioned first); generic staples like Mulcharmy and
  non-archetype cards are excluded.
- **Chain stack visualizer** — numbered links during a chain, with a live
  resolution sweep (resolved links dim/strike, the active link pulses).
- **Deck builder**: drag cards from search into the deck; "+ Add" picks
  Main/Extra/Side; copy-count badges; attribute/level + effect-text search;
  Tidy (remove over-limit); ydke:// copy; signature/boss card; favorites +
  filter; custom banlist/format editor.
- **Replay viewer**: step-back + seek (rewind/jump), rename/delete.
- **Audio mixer**: master/SFX/music volumes + "mute UI sounds"; background
  music; single-channel SFX (no pile-up).
- **Settings**: UI scale, colorblind-safe legality colors, frame cap,
  rebindable duel keys.
- **Stats dashboard**: win streaks, going-first/second win rates, avg turns.
- **Your-turn taskbar flash** when the window is unfocused.
- Card-art prefetch + loading indicator; testing "add card to hand";
  best-of-3 already supported.

## 1.0.2
- Background music + music volume slider; cinematic dark main-menu remake
  with subtle Egyptian glyphs; board-break puzzles; AI per-action pacing;
  phase-transition fixes; right-click = No / pass; crisper card art.

## 1.0.1
- Versioning, in-app update check, and release packaging groundwork.

## 1.0.0
- First beta: offline duels vs. a heuristic AI, deck builder, replays,
  testing mode, online relay multiplayer (foundation), and LAN scaffolding.
