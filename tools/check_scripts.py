#!/usr/bin/env python3
"""
check_scripts.py - prove whether a deck's cards have their Lua effect scripts.

A card's data (name/type/atk/def) comes from cards.cdb, but its EFFECT comes
from a Lua script  scripts/.../cNNNN.lua . If the script is missing the card
behaves as a vanilla card: no on-summon triggers, Spells/Traps only settable.

Usage:
    python tools/check_scripts.py --scripts assets/scripts \\
        --deck "assets/decks/Kewl tune.ydk" --db-dir BabelCDB-master
    python tools/check_scripts.py --scripts build/windows/Release/assets/scripts \\
        --deck "build/windows/Release/assets/decks/Kewl tune.ydk" \\
        --db build/windows/Release/assets/cards.cdb --db-dir BabelCDB-master
"""

import argparse
import glob
import os
import sqlite3
import sys

# Subfolders that ocgcore-style script collections use.
SCRIPT_SUBDIRS = ["", "official", "unofficial", "rush", "skill",
                  "goat", "pre-errata"]
# Procedure / helper scripts ocgcore loads besides per-card cNNNN.lua.
HELPER_SCRIPTS = ["constant.lua", "utility.lua", "proc_unofficial.lua",
                  "proc_normal.lua", "proc_synchro.lua", "proc_fusion.lua",
                  "proc_xyz.lua", "proc_link.lua", "proc_pendulum.lua",
                  "cards_specific_functions.lua"]
EXTRA_DECK_MASK = 0x40 | 0x2000 | 0x800000 | 0x4000000


def find_script(scripts_dir, fname):
    """Return the path of `fname` under any script subfolder, or None."""
    for sub in SCRIPT_SUBDIRS:
        p = os.path.join(scripts_dir, sub, fname)
        if os.path.isfile(p):
            return p
    return None


def collect_dbs(db_args, dir_args):
    paths = []
    for d in db_args or []:
        paths.append(os.path.abspath(d))
    for folder in dir_args or []:
        for p in sorted(glob.glob(os.path.join(folder, "*.cdb"))):
            paths.append(os.path.abspath(p))
    seen, out = set(), []
    for p in paths:
        if p not in seen and os.path.exists(p):
            seen.add(p)
            out.append(p)
    return out


def lookup(dbs, code):
    """Return (name, type, db_basename) for a code, or (None, None, None)."""
    for path in dbs:
        try:
            con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
            r = con.execute(
                "SELECT d.type,t.name FROM datas d JOIN texts t ON d.id=t.id "
                "WHERE d.id=?", (code,)).fetchone()
            con.close()
            if r:
                return r[1], r[0], os.path.basename(path)
        except sqlite3.Error:
            pass
    return None, None, None


def parse_ydk(path):
    out, zone = [], "main"
    for ln in open(path, encoding="utf-8", errors="ignore"):
        ln = ln.strip()
        if ln.startswith("#extra"):
            zone = "extra"; continue
        if ln.startswith("#main"):
            zone = "main"; continue
        if ln.startswith("!side"):
            zone = "side"; continue
        if not ln or ln.startswith("#") or not ln.isdigit():
            continue
        out.append((int(ln), zone))
    return out


def main():
    ap = argparse.ArgumentParser(description="Audit a deck's card scripts.")
    ap.add_argument("--scripts", required=True, help="scripts folder")
    ap.add_argument("--deck", required=True, help="a .ydk deck file")
    ap.add_argument("--db", action="append", help="a .cdb file (repeatable)")
    ap.add_argument("--db-dir", action="append", help="folder of .cdb files")
    args = ap.parse_args()

    scripts = os.path.abspath(args.scripts)
    print(f"scripts folder : {scripts}")
    if not os.path.isdir(scripts):
        print("ERROR: scripts folder does not exist.")
        return 2
    for sub in SCRIPT_SUBDIRS:
        d = os.path.join(scripts, sub)
        if os.path.isdir(d):
            n = len(glob.glob(os.path.join(d, "*.lua")))
            print(f"  {os.path.join('scripts', sub) or 'scripts(root)'}: {n} .lua")

    print("\nHelper / procedure scripts:")
    for h in HELPER_SCRIPTS:
        p = find_script(scripts, h)
        print(f"  {h:30s} {'OK  ' + p if p else 'MISSING'}")

    dbs = collect_dbs(args.db, args.db_dir)
    print(f"\ndatabases: {len(dbs)} file(s)")
    if not os.path.exists(args.deck):
        print(f"ERROR: deck not found: {args.deck}")
        return 2

    cards = parse_ydk(args.deck)
    print(f"\nDeck: {args.deck}  ({len(cards)} cards)")
    print(f"  {'code':>9} {'sect':5} {'card':9} {'script':8} name")
    missing_card, missing_script = 0, 0
    for code, section in cards:
        name, ctype, db = lookup(dbs, code)
        sp = find_script(scripts, f"c{code}.lua")
        cstat = "in-DB" if name else "MISSING"
        # A Normal (vanilla) monster has no effect, so it legitimately needs
        # no script. Only effect monsters / Spells / Traps require one.
        ct = ctype or 0
        vanilla = name and (ct & 0x1) and (ct & 0x10) and not (ct & 0x20)
        if sp:
            sstat = "OK"
        elif vanilla:
            sstat = "n/a"          # vanilla card — no script expected
        else:
            sstat = "MISSING"
        if not name:
            missing_card += 1
        if sstat == "MISSING":
            missing_script += 1
        extra = name and (ct & EXTRA_DECK_MASK)
        route = "EXTRA" if (extra or (not name and section == "extra")) else "MAIN"
        print(f"  {code:>9} {section:5} {cstat:9} {sstat:8} "
              f"{name or '(unknown)'}  [route={route}]")

    print(f"\nSummary: {missing_card} card(s) missing from databases, "
          f"{missing_script} effect card(s) missing a script.")
    if missing_script:
        print("Effect cards with a MISSING script have NO effects in the duel "
              "(no triggers; Spells/Traps only settable, not activatable).")
        print("Obtain those cNNNN.lua scripts and place them under "
              "assets/scripts/unofficial/ (or official/).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
