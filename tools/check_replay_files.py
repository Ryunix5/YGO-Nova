#!/usr/bin/env python3
"""
check_replay_files.py — scan assets/replays and report status of each
.json replay.

Usage:

    python tools/check_replay_files.py

Validates that:
  * the file parses as JSON
  * required top-level keys exist (version, seed, deck1, deck2)
  * deck arrays are populated
  * response and event arrays are present

Exit code = number of FAIL items.
"""

from __future__ import annotations
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPLAYS = ROOT / "assets" / "replays"

REQUIRED = ["version", "seed", "deck1", "deck2", "responses", "events"]


def main() -> int:
    print(f"check_replay_files.py — {REPLAYS}\n")
    if not REPLAYS.is_dir():
        print("(no replays folder yet — run a duel to create one)")
        return 0
    files = sorted(REPLAYS.glob("*.json"))
    if not files:
        print("(no replay files)")
        return 0
    fails = 0
    warns = 0
    for f in files:
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            print(f"  [FAIL] {f.name}  JSON parse error: {e.msg} (line {e.lineno})")
            fails += 1
            continue
        missing = [k for k in REQUIRED if k not in data]
        if missing:
            print(f"  [FAIL] {f.name}  missing keys: {', '.join(missing)}")
            fails += 1
            continue
        d1 = data.get("deck1", {}) or {}
        d2 = data.get("deck2", {}) or {}
        m1, m2 = d1.get("main", []), d2.get("main", [])
        nr = len(data.get("responses", []))
        ne = len(data.get("events", []))
        winner = data.get("winner", -2)
        if not m1 or not m2:
            print(f"  [WARN] {f.name}  empty deck data — playback will refuse")
            warns += 1
        if nr == 0:
            print(f"  [WARN] {f.name}  no recorded responses — duel ended at start?")
            warns += 1
        date = data.get("timestamp", "?")
        print(f"  [OK]   {f.name}  {date}  decks {len(m1)}/{len(m2)}  "
              f"responses={nr}  events={ne}  winner={winner}")
    print("")
    print(f"Summary: {len(files)} files  {fails} FAIL  {warns} WARN")
    return fails


if __name__ == "__main__":
    sys.exit(main())
