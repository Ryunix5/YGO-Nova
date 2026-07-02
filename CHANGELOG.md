# Changelog

All notable changes to YGO: Nova are documented here.

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
