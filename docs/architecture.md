# EdoPro+ Architecture

## Overview

EdoPro+ is a modern Yu-Gi-Oh! simulator built on top of the battle-tested
**ocgcore** rules engine, with a completely rewritten frontend in React/TypeScript.

```
┌──────────────────────────────────────────────────────┐
│              React/TypeScript Frontend                │
│  LobbyPage · DuelPage · DeckBuilderPage              │
│  DuelField · HandDisplay · CardZone · CardPreview    │
│  PhaseBar · ActionLog · TestingMode (rewind UI)      │
└────────────────┬─────────────────────────────────────┘
                 │  OcgClient.ts (cwrap bindings)
┌────────────────▼─────────────────────────────────────┐
│         ocgcore + WASM bridge (C++)                   │
│  wasm_bridge.cpp  — JS-callable exports               │
│  snapshot.cpp     — game state rewind stack           │
│  ocgcore/         — ProjectIgnis rules engine         │
└────────────────┬─────────────────────────────────────┘
                 │  SQLite (via sql.js in browser)
┌────────────────▼─────────────────────────────────────┐
│           Card Database (cards.db / .cdb)             │
│  datas table — stats, type, attribute, level          │
│  texts table — names, card text                       │
└──────────────────────────────────────────────────────┘
```

## Directory Structure

```
EdoPro/
├── engine/
│   ├── CMakeLists.txt        # Builds both native .so and WASM
│   ├── ocgcore/              # git submodule: ProjectIgnis/ocgcore
│   ├── bridge/
│   │   ├── wasm_bridge.cpp   # Emscripten exports
│   │   └── snapshot.cpp      # Rewind / Testing Mode
│   ├── include/
│   │   └── snapshot.h
│   └── scripts/
│       ├── build_native.sh
│       └── build_wasm.sh
├── frontend/
│   ├── src/
│   │   ├── engine/
│   │   │   ├── OcgClient.ts       # WASM client + message parser
│   │   │   ├── SnapshotManager.ts # Testing Mode logic
│   │   │   └── types.ts           # All TypeScript types
│   │   ├── store/
│   │   │   ├── duelStore.ts       # Zustand duel state
│   │   │   └── deckStore.ts       # Zustand deck builder state
│   │   ├── components/
│   │   │   ├── DuelField/         # Main board layout
│   │   │   ├── CardZone/          # Individual card zone
│   │   │   ├── HandDisplay/       # Fan-out hand
│   │   │   ├── CardPreview/       # Sidebar card detail
│   │   │   ├── PhaseBar/          # Phase indicator
│   │   │   ├── ActionLog/         # Duel history log
│   │   │   └── TestingMode/       # Timeline scrubber
│   │   └── pages/
│   │       ├── LobbyPage.tsx
│   │       ├── DuelPage.tsx
│   │       └── DeckBuilderPage.tsx
│   └── public/
│       ├── engine/               # WASM output goes here
│       │   ├── edopro_engine.js
│       │   └── edopro_engine.wasm
│       └── cards/               # Card art (jpg, named by passcode)
│           └── {code}.jpg
└── database/
    └── schema.sql               # SQLite schema (YGOPro-compatible)
```

## Getting Started

### 1. Clone ocgcore

```bash
cd engine
git submodule add https://github.com/edo9300/ygopro-core.git ocgcore
```

### 2. Build the WASM engine

```bash
# Install Emscripten first: https://emscripten.org/docs/getting_started/
source /path/to/emsdk/emsdk_env.sh
chmod +x engine/scripts/build_wasm.sh
./engine/scripts/build_wasm.sh
```

### 3. Add the card database

Download `cards.cdb` from [YGOPRO-DATABASE](https://github.com/purerosefallen/ygopro-database)
and copy it to `frontend/public/cards.db`.

### 4. Add card images

Download card art from a YGOPRO image pack and extract to `frontend/public/cards/`.
Files should be named `{passcode}.jpg`.

### 5. Run the frontend

```bash
cd frontend
npm install
npm run dev
```

## Testing Mode (Rewind)

The snapshot system works as follows:

1. When Testing Mode is enabled, `SnapshotManager.save()` is called before
   each player action. This calls `snap_push()` in the C++ engine.

2. `snap_push()` records the current response log length as a snapshot index.

3. On rewind, `snap_pop()` restores the duel by re-creating it from the
   initial state (seed + deck order) and replaying all responses up to
   the snapshot point. This ensures 100% deterministic restoration.

4. The deck's card order is captured at `snap_init()` time (called once
   before `ocg_start_duel()`), so rewinding a shuffle restores the exact
   pre-shuffle order.

5. The UI timeline in `TestingMode.tsx` shows each snapshot as a chip
   labeled with the turn number and phase. Clicking a chip calls
   `SnapshotManager.jumpTo()` which rewinds step-by-step to that point.

## Card Database

The frontend loads `cards.db` (SQLite) via `sql.js` (SQLite compiled to WASM).
Queries run in a Web Worker to avoid blocking the UI thread.

The schema is fully compatible with YGOPro's `.cdb` format — you can use
any existing YGOPro database file directly by renaming it to `cards.db`.
