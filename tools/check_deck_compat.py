#!/usr/bin/env python3
"""
check_deck_compat.py — validate a .ydk deck against the installed CardDB
and script folder.

Usage:

    python tools/check_deck_compat.py assets/decks/MyDeck.ydk

Reports per-card whether the code exists in cards.cdb, whether a script
exists (when the card has the EFFECT/script flags set), and whether the
card was filed into the right section (Main vs Extra) based on its type.

Exit code = number of FAIL items.
"""

from __future__ import annotations
import sys
import sqlite3
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"

# Bit flags from ocgcore — only the ones we need for extra-deck routing.
TYPE_FUSION  = 0x40
TYPE_SYNCHRO = 0x2000
TYPE_XYZ     = 0x800000
TYPE_LINK    = 0x4000000
EXTRA_MASK = TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK


def parse_ydk(path: Path) -> tuple[list[int], list[int], list[int]]:
    """Parse a .ydk file into (main, extra, side) lists of card codes."""
    main: list[int] = []
    extra: list[int] = []
    side: list[int] = []
    section: list[int] | None = None
    if not path.is_file():
        raise FileNotFoundError(f"deck not found: {path}")
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        low = line.lower()
        if low.startswith("#main"):
            section = main
        elif low.startswith("#extra"):
            section = extra
        elif low.startswith("!side") or low.startswith("#side"):
            section = side
        elif line[0] == "#" or line[0] == "!":
            continue
        else:
            try:
                code = int(line)
            except ValueError:
                continue
            if section is None:
                section = main
            section.append(code)
    return main, extra, side


def load_card(conn: sqlite3.Connection, code: int) -> tuple[str, int] | None:
    """Return (name, type) tuple or None when the card is unknown."""
    row = conn.execute(
        "SELECT name FROM texts WHERE id=?", (code,)
    ).fetchone()
    name = row[0] if row else None
    row = conn.execute(
        "SELECT type FROM datas WHERE id=?", (code,)
    ).fetchone()
    type_ = row[0] if row else None
    if name is None and type_ is None:
        return None
    return (name or "(no name)", type_ or 0)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_deck_compat.py <path/to/deck.ydk>", file=sys.stderr)
        return 2
    deck = Path(sys.argv[1])
    cdb = ASSETS / "cards.cdb"
    if not cdb.is_file():
        print(f"FAIL: cards.cdb not found at {cdb}", file=sys.stderr)
        return 3
    print(f"check_deck_compat.py — {deck}\n")
    try:
        main_codes, extra_codes, side_codes = parse_ydk(deck)
    except FileNotFoundError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 4
    fails = 0
    warns = 0
    with sqlite3.connect(str(cdb)) as conn:
        scripts_dir = ASSETS / "scripts"
        def check_section(label: str, codes: list[int], expected_extra: bool) -> None:
            nonlocal fails, warns
            print(f"== {label} ({len(codes)} cards) ==")
            seen: dict[int, int] = {}
            for c in codes:
                seen[c] = seen.get(c, 0) + 1
            for code, count in seen.items():
                info = load_card(conn, code)
                if info is None:
                    print(f"  [FAIL] {code}  missing from cards.cdb")
                    fails += 1
                    continue
                name, type_ = info
                is_extra = (type_ & EXTRA_MASK) != 0
                section_tag = "OK"
                if expected_extra and not is_extra:
                    section_tag = "WRONG (should be in MAIN)"
                    fails += 1
                elif (not expected_extra) and is_extra:
                    section_tag = "WRONG (should be in EXTRA)"
                    fails += 1
                # Best-effort script presence — many older cards don't have
                # a per-card script and that's normal.
                script = scripts_dir / f"c{code}.lua"
                script_tag = "script present" if script.is_file() else "no script"
                if count > 3:
                    print(f"  [FAIL] {code}  x{count}  {name}  (>3 copies)")
                    fails += 1
                else:
                    print(f"  [PASS] {code}  x{count}  {name}  ({section_tag}, {script_tag})")
        check_section("MAIN",  main_codes,  expected_extra=False)
        check_section("EXTRA", extra_codes, expected_extra=True)
        check_section("SIDE",  side_codes,  expected_extra=False)
        # Count guidance (warning only).
        n_main = len(main_codes)
        if n_main < 40 or n_main > 60:
            print(f"\n[WARN] Main deck size {n_main} (legal range 40..60)")
            warns += 1
        n_extra = len(extra_codes)
        if n_extra > 15:
            print(f"[WARN] Extra deck size {n_extra} (>15)")
            warns += 1
        n_side = len(side_codes)
        if n_side > 15:
            print(f"[WARN] Side deck size {n_side} (>15)")
            warns += 1
    print(f"\nSummary: {fails} FAIL  {warns} WARN")
    return fails


if __name__ == "__main__":
    sys.exit(main())
