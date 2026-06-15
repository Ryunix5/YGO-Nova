#!/usr/bin/env python3
"""
lookup_card_cdb.py - diagnose card lookups across one or many YGOPro/EDOPro
cards.cdb databases (e.g. a stale runtime cards.cdb plus BabelCDB-master).

Examples:
    python tools/lookup_card_cdb.py --db assets/cards.cdb --query "Kewl Tune Crackle"
    python tools/lookup_card_cdb.py --db-dir BabelCDB-master --code 39576656
    python tools/lookup_card_cdb.py --db-dir BabelCDB-master --code 17209452
    python tools/lookup_card_cdb.py --scan-deck "assets/decks/Kewl tune.ydk" \\
        --db build/windows/Release/assets/cards.cdb --db-dir BabelCDB-master \\
        --scripts build/windows/Release/assets/scripts

It searches the `datas` + `texts` tables, decodes type flags, supports
case-insensitive partial search with a local "Kewl Tune" <-> "Killer Tune"
alias, reports which database file contains each card, and never writes to
any .cdb.
"""

import argparse
import glob
import os
import sqlite3
import sys

TYPE_FLAGS = [
    (0x1, "MONSTER"), (0x2, "SPELL"), (0x4, "TRAP"), (0x10, "NORMAL"),
    (0x20, "EFFECT"), (0x40, "FUSION"), (0x80, "RITUAL"), (0x100, "TRAPMONSTER"),
    (0x200, "SPIRIT"), (0x400, "UNION"), (0x800, "GEMINI"), (0x1000, "TUNER"),
    (0x2000, "SYNCHRO"), (0x4000, "TOKEN"), (0x8000, "MAXIMUM"),
    (0x10000, "QUICKPLAY"), (0x20000, "CONTINUOUS"), (0x40000, "EQUIP"),
    (0x80000, "FIELD"), (0x100000, "COUNTER"), (0x200000, "FLIP"),
    (0x400000, "TOON"), (0x800000, "XYZ"), (0x1000000, "PENDULUM"),
    (0x2000000, "SPSUMMON"), (0x4000000, "LINK"),
]
ATTR_FLAGS = [
    (0x1, "EARTH"), (0x2, "WATER"), (0x4, "FIRE"), (0x8, "WIND"),
    (0x10, "LIGHT"), (0x20, "DARK"), (0x40, "DIVINE"),
]
EXTRA_DECK_MASK = 0x40 | 0x2000 | 0x800000 | 0x4000000  # FUSION/SYNCHRO/XYZ/LINK


def decode(value, table):
    return " | ".join(n for b, n in table if value & b) or "(none)"


def alias_terms(term):
    out = {term}
    low = term.lower()
    if "kewl tune" in low:
        out.add(low.replace("kewl tune", "killer tune"))
    if "killer tune" in low:
        out.add(low.replace("killer tune", "kewl tune"))
    return out


def collect_dbs(db_args, dir_args):
    """Return [(label, absolute_path)] for every .cdb to search."""
    paths = []
    for d in db_args or []:
        paths.append(os.path.abspath(d))
    for folder in dir_args or []:
        for p in sorted(glob.glob(os.path.join(folder, "*.cdb"))):
            paths.append(os.path.abspath(p))
    # de-dupe, keep order
    seen, out = set(), []
    for p in paths:
        if p not in seen:
            seen.add(p)
            out.append((os.path.basename(p), p))
    return out


def open_db(path):
    if not os.path.exists(path):
        return None, "file does not exist"
    try:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        cols = [r[1] for r in con.execute("PRAGMA table_info(datas)")]
        tcols = [r[1] for r in con.execute("PRAGMA table_info(texts)")]
        if not cols or not tcols:
            return None, "unexpected schema (no datas/texts table)"
        return con, None
    except sqlite3.Error as e:
        return None, f"sqlite error: {e}"


def card_row(con, cid):
    d = con.execute(
        "SELECT id,alias,type,atk,def,level,race,attribute FROM datas WHERE id=?",
        (cid,)).fetchone()
    if not d:
        return None
    t = con.execute("SELECT name,desc FROM texts WHERE id=?", (cid,)).fetchone()
    return d, (t or ("(no text)", ""))


def print_card(label, d, t):
    print(f"  FOUND in [{label}]")
    print(f"     id={d[0]}  name={t[0]!r}")
    if d[1]:
        print(f"     alias={d[1]}")
    extra = bool(d[2] & EXTRA_DECK_MASK)
    print(f"     type=0x{d[2] & 0xffffffff:x}  -> {decode(d[2], TYPE_FLAGS)}")
    print(f"     EXTRA-DECK card: {'YES' if extra else 'no'}")
    print(f"     atk={d[3]} def={d[4]} level/rank=0x{d[5] & 0xffffffff:x}"
          f" attribute={decode(d[7], ATTR_FLAGS)}")
    if t[1]:
        desc = t[1].replace("\n", " ")
        print(f"     desc={desc[:150]}{'...' if len(desc) > 150 else ''}")


def find_card(dbs, cid):
    """Return list of (label, d, t) where the card code is present."""
    hits = []
    for label, path in dbs:
        con, err = open_db(path)
        if not con:
            continue
        r = card_row(con, cid)
        if r:
            hits.append((label, r[0], r[1]))
        con.close()
    return hits


def do_code(dbs, code):
    print(f"=== Code lookup: {code} ===")
    hits = find_card(dbs, code)
    if not hits:
        print(f"  code {code} is MISSING from all {len(dbs)} scanned database(s).")
        return
    for label, d, t in hits:
        print_card(label, d, t)


def do_query(dbs, query):
    terms = alias_terms(query)
    print(f"=== Name search: {query!r}  (aliases: {sorted(terms)}) ===")
    seen, total = set(), 0
    for label, path in dbs:
        con, err = open_db(path)
        if not con:
            print(f"  [{label}] skipped: {err}")
            continue
        for term in terms:
            rows = con.execute(
                "SELECT d.id FROM datas d JOIN texts t ON d.id=t.id "
                "WHERE LOWER(t.name) LIKE '%'||LOWER(?)||'%' "
                "   OR LOWER(t.desc) LIKE '%'||LOWER(?)||'%'",
                (term, term)).fetchall()
            for (cid,) in rows:
                if cid in seen:
                    continue
                seen.add(cid)
                r = card_row(con, cid)
                if r:
                    total += 1
                    print_card(label, r[0], r[1])
        con.close()
    print(f"  total unique results: {total}")
    if total == 0:
        print("  -> not found in ANY scanned database (card genuinely missing).")


def parse_ydk(path):
    """Return list of (code, section) in file order."""
    out, zone = [], "main"
    for ln in open(path, encoding="utf-8", errors="ignore"):
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#extra"):
            zone = "extra"; continue
        if ln.startswith("#main"):
            zone = "main"; continue
        if ln.startswith("!side"):
            zone = "side"; continue
        if ln.startswith("#") or not ln.isdigit():
            continue
        out.append((int(ln), zone))
    return out


def do_scan_deck(dbs, deck_path, scripts_dir):
    print(f"=== Deck scan: {deck_path} ===")
    if not os.path.exists(deck_path):
        print("  deck file not found.")
        return
    cards = parse_ydk(deck_path)
    print(f"  {len(cards)} card entries ({len(dbs)} database(s) loaded)")
    print(f"  {'code':>9} {'sect':5} {'route':5} {'script':7} db / name / warning")
    bad = 0
    for code, section in cards:
        hits = find_card(dbs, code)
        scr = "-"
        if scripts_dir:
            for sub in ("", "official"):
                if os.path.exists(os.path.join(scripts_dir, sub, f"c{code}.lua")):
                    scr = "found"; break
            else:
                scr = "MISSING"
        if hits:
            label, d, t = hits[0]
            extra = bool(d[2] & EXTRA_DECK_MASK)
            route = "EXTRA" if extra else "MAIN"
            warn = ""
            if section == "main" and extra:
                warn = "  *** Extra-Deck card in #main section!"
                bad += 1
            if section == "extra" and not extra:
                warn = "  (non-Extra card in #extra -> routes MAIN)"
            print(f"  {code:>9} {section:5} {route:5} {scr:7} "
                  f"[{label}] {t[0]}{warn}")
        else:
            route = "EXTRA" if section == "extra" else "MAIN"
            print(f"  {code:>9} {section:5} {route:5} {scr:7} "
                  f"*** MISSING from all databases — routed by .ydk section")
            bad += 1
    print(f"  scan complete: {bad} card(s) need attention "
          f"(missing from DB, or mis-sectioned).")


def main():
    ap = argparse.ArgumentParser(description="Diagnose cards.cdb lookups.")
    ap.add_argument("--db", action="append", help="a .cdb file (repeatable)")
    ap.add_argument("--db-dir", action="append",
                    help="a folder of .cdb files (repeatable)")
    ap.add_argument("--query", action="append", help="name search (repeatable)")
    ap.add_argument("--code", action="append", type=int,
                    help="exact card code (repeatable)")
    ap.add_argument("--scan-deck", help="a .ydk deck file to audit")
    ap.add_argument("--scripts", help="scripts folder, to check cNNNN.lua files")
    args = ap.parse_args()

    dbs = collect_dbs(args.db, args.db_dir)
    if not dbs:
        print("No databases given. Use --db <file> and/or --db-dir <folder>.")
        return 2
    print(f"Scanning {len(dbs)} database file(s):")
    for label, path in dbs:
        con, err = open_db(path)
        if con:
            n = con.execute("SELECT COUNT(*) FROM datas").fetchone()[0]
            print(f"  [{label}] {path}  ({os.path.getsize(path)} bytes, {n} cards)")
            con.close()
        else:
            print(f"  [{label}] {path}  -- UNUSABLE: {err}")
    print("-" * 64)

    for code in args.code or []:
        do_code(dbs, code)
        print()
    for query in args.query or []:
        do_query(dbs, query)
        print()
    if args.scan_deck:
        do_scan_deck(dbs, args.scan_deck, args.scripts)

    if not (args.code or args.query or args.scan_deck):
        print("Nothing to do - pass --query, --code and/or --scan-deck.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
