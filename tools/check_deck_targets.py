#!/usr/bin/env python3
"""
check_deck_targets.py - verify a .ydk deck contains legal targets for
Vanquish Soul Razen's on-summon search effect.

Razen (#29302858) triggers on Normal/Special Summon and searches:
    1 non-Warrior "Vanquish Soul" monster from the Deck.

The Lua filter (c29302858.lua, s.thfilter) is:
    c:IsSetCard(SET_VANQUISH_SOUL)  -- set code 0x196
    and c:IsMonster()               -- type & 0x1
    and not c:IsRace(RACE_WARRIOR)   -- race & 0x1
    and c:IsAbleToHand()

This tool reads every "Vanquish Soul" monster in the .ydk Main Deck, prints
its metadata (type / race / setcode / source .cdb) and says whether it is a
valid Razen target. It NEVER writes to any database.

Examples:
    python tools/check_deck_targets.py \\
        --ydk build/windows/Release/assets/decks/VSK9.ydk \\
        --db  build/windows/Release/assets/cards.cdb

    python tools/check_deck_targets.py --ydk assets/decks/VSK9.ydk \\
        --db assets/cards.cdb --db-dir BabelCDB-master
"""

import argparse
import glob
import os
import sqlite3
import sys

SET_VANQUISH_SOUL = 0x196
RACE_WARRIOR      = 0x1
TYPE_MONSTER      = 0x1
TYPE_EXTRA        = 0x40 | 0x2000 | 0x800000 | 0x4000000  # Fusion/Synchro/Xyz/Link


def open_dbs(db_paths):
    """Open each .cdb read-only. First = primary, rest = fallbacks."""
    dbs = []
    for path in db_paths:
        if not os.path.isfile(path):
            print(f"  [warn] not found: {path}", file=sys.stderr)
            continue
        try:
            con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
            dbs.append((con, path))
        except sqlite3.Error as e:
            print(f"  [warn] cannot open {path}: {e}", file=sys.stderr)
    return dbs


def lookup(dbs, code):
    """Resolve a card from the primary DB first, then every fallback."""
    for con, path in dbs:
        row = con.execute(
            "SELECT d.id,d.type,d.race,d.attribute,d.setcode,t.name "
            "FROM datas d JOIN texts t ON d.id=t.id WHERE d.id=?",
            (code,)).fetchone()
        if row:
            return {
                "id": row[0], "type": row[1], "race": row[2],
                "attribute": row[3], "setcode": row[4] or 0,
                "name": row[5] or "", "source": os.path.basename(path),
            }
    return None


def setcode_parts(setcode):
    """Unpack the 64-bit packed setcode into its non-zero 16-bit parts."""
    return [(setcode >> (i * 16)) & 0xFFFF for i in range(4)
            if (setcode >> (i * 16)) & 0xFFFF]


def parse_ydk_main(path):
    """Return the list of card codes in the #main section of a .ydk."""
    main = []
    section = None
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#main"):
                section = "main"; continue
            if line.startswith("#extra"):
                section = "extra"; continue
            if line.startswith("!side"):
                section = "side"; continue
            if line.startswith("#") or line.startswith("!"):
                continue
            if line.isdigit() and section == "main":
                main.append(int(line))
    return main


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ydk", required=True, help="path to the .ydk deck file")
    ap.add_argument("--db", action="append", default=[],
                    help="a cards.cdb (repeatable; first = primary)")
    ap.add_argument("--db-dir", action="append", default=[],
                    help="a folder of .cdb files to add as fallbacks")
    args = ap.parse_args()

    db_paths = list(args.db)
    for d in args.db_dir:
        db_paths.extend(sorted(glob.glob(os.path.join(d, "*.cdb"))))
    if not db_paths:
        print("error: pass at least one --db or --db-dir", file=sys.stderr)
        return 2

    dbs = open_dbs(db_paths)
    if not dbs:
        print("error: no usable card database opened", file=sys.stderr)
        return 2

    if not os.path.isfile(args.ydk):
        print(f"error: deck not found: {args.ydk}", file=sys.stderr)
        return 2

    main_codes = parse_ydk_main(args.ydk)
    print(f"Deck: {args.ydk}")
    print(f"Databases: {', '.join(os.path.basename(p) for _, p in dbs)}")
    print(f"Main Deck: {len(main_codes)} cards "
          f"({len(set(main_codes))} distinct)\n")

    valid_total = 0
    rows = []
    for code in sorted(set(main_codes)):
        info = lookup(dbs, code)
        copies = main_codes.count(code)
        if info is None:
            rows.append((code, "(missing from all DBs)", "?", "?", "?",
                         "no", "missing DB data", copies))
            continue
        parts = setcode_parts(info["setcode"])
        is_vs       = SET_VANQUISH_SOUL in parts
        name_says   = "Vanquish Soul" in info["name"]
        if not is_vs and not name_says:
            continue  # not archetype-relevant — skip
        is_monster  = bool(info["type"] & TYPE_MONSTER)
        is_extra    = bool(info["type"] & TYPE_EXTRA)
        is_warrior  = bool(info["race"] & RACE_WARRIOR)

        if not is_vs and name_says:
            verdict, reason = "no", "name says VS but setcode metadata WRONG"
        elif not is_monster:
            verdict, reason = "no", "not a monster"
        elif is_extra:
            verdict, reason = "no", "Extra Deck monster (not in Main Deck)"
        elif is_warrior:
            verdict, reason = "no", "is a Warrior (excluded by Razen)"
        else:
            verdict, reason = "yes", "non-Warrior Vanquish Soul monster"
            valid_total += copies

        rows.append((code, info["name"], f'0x{info["type"]:x}',
                     str(info["race"]), f'0x{info["setcode"]:x}',
                     verdict, reason, copies))

    if not rows:
        print("No 'Vanquish Soul' cards found in the Main Deck.")
    else:
        hdr = ("code", "name", "type", "race", "setcode",
               "Razen target?", "reason", "x")
        print(f'{hdr[0]:<10} {hdr[1]:<28} {hdr[2]:<8} {hdr[3]:<10} '
              f'{hdr[4]:<10} {hdr[5]:<14} {hdr[6]:<42} {hdr[7]}')
        print("-" * 132)
        for r in rows:
            print(f'{r[0]:<10} {r[1]:<28.28} {r[2]:<8} {r[3]:<10} '
                  f'{r[4]:<10} {r[5]:<14} {r[6]:<42} x{r[7]}')

    print()
    if valid_total > 0:
        print(f"RESULT: {valid_total} legal Razen target copy/copies in the "
              f"Main Deck — the search trigger SHOULD be offered after summon.")
        print("If the in-game trace still shows 0 options, the cause is "
              "engine card metadata (setcodes) or rule options, not the deck.")
    else:
        print("RESULT: 0 legal Razen targets in the Main Deck — the search "
              "trigger is LEGALLY unavailable for this deck.")

    for con, _ in dbs:
        con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
