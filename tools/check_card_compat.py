#!/usr/bin/env python3
"""
check_card_compat.py - per-card compatibility diagnostic for EDOPro-style
content. Answers: "this card exists in my database but shows no effects in a
duel - why?"

It cross-checks one card against:
  * every loaded .cdb (primary + BabelCDB fallbacks) - including duplicates,
  * every script folder,
  * the helper/procedure scripts the card script depends on,
  * the cards that share its archetype set code.

Typical cause of a "newer card has no trigger": the card row is present in an
updated cards.cdb, but the matching c<code>.lua script was never added to the
script collection - so ocgcore loads the card as a vanilla body with no
effects.

Examples:
    python tools/check_card_compat.py --name "Lunalight Gold Leo" \\
        --db build/windows/Release/assets/cards.cdb \\
        --db-dir BabelCDB-master \\
        --scripts build/windows/Release/assets/scripts

    python tools/check_card_compat.py --code 89392810 \\
        --db build/windows/Release/assets/cards.cdb --db-dir BabelCDB-master \\
        --scripts build/windows/Release/assets/scripts

This tool is read-only: it never writes to any .cdb or script.
"""

import argparse
import glob
import os
import sqlite3
import sys

# ─── metadata decode tables ──────────────────────────────────────────────────
TYPE_FLAGS = [
    (0x1, "Monster"), (0x2, "Spell"), (0x4, "Trap"), (0x10, "Normal"),
    (0x20, "Effect"), (0x40, "Fusion"), (0x80, "Ritual"),
    (0x100, "TrapMonster"), (0x200, "Spirit"), (0x400, "Union"),
    (0x800, "Gemini"), (0x1000, "Tuner"), (0x2000, "Synchro"),
    (0x4000, "Token"), (0x10000, "QuickPlay"), (0x20000, "Continuous"),
    (0x40000, "Equip"), (0x80000, "Field"), (0x100000, "Counter"),
    (0x200000, "Flip"), (0x400000, "Toon"), (0x800000, "Xyz"),
    (0x1000000, "Pendulum"), (0x2000000, "SpecialSummon"), (0x4000000, "Link"),
]
RACE_FLAGS = {
    0x1: "Warrior", 0x2: "Spellcaster", 0x4: "Fairy", 0x8: "Fiend",
    0x10: "Zombie", 0x20: "Machine", 0x40: "Aqua", 0x80: "Pyro",
    0x100: "Rock", 0x200: "WingedBeast", 0x400: "Plant", 0x800: "Insect",
    0x1000: "Thunder", 0x2000: "Dragon", 0x4000: "Beast",
    0x8000: "BeastWarrior", 0x10000: "Dinosaur", 0x20000: "Fish",
    0x40000: "SeaSerpent", 0x80000: "Reptile", 0x100000: "Psychic",
    0x200000: "Divine", 0x400000: "CreatorGod", 0x800000: "Wyrm",
    0x1000000: "Cyberse", 0x2000000: "Illusion",
}
ATTR_FLAGS = {
    0x1: "EARTH", 0x2: "WATER", 0x4: "FIRE", 0x8: "WIND",
    0x10: "LIGHT", 0x20: "DARK", 0x40: "DIVINE",
}
OT_NAMES = {1: "OCG", 2: "TCG", 3: "OCG+TCG (legal)",
            4: "anime/illegal", 8: "custom/pre-release"}

# Trigger / timing events worth highlighting in a script.
TRIGGER_EVENTS = [
    "EVENT_SUMMON_SUCCESS", "EVENT_SPSUMMON_SUCCESS", "EVENT_FLIP_SUMMON_SUCCESS",
    "EVENT_TO_HAND", "EVENT_TO_GRAVE", "EVENT_TO_DECK", "EVENT_LEAVE_FIELD",
    "EVENT_DESTROYED", "EVENT_CHAINING", "EVENT_CHAIN_SOLVING",
    "EVENT_FREE_CHAIN", "EVENT_PHASE", "EVENT_BATTLED", "EVENT_DAMAGE",
    "EVENT_ATTACK_ANNOUNCE", "EVENT_BE_BATTLE_TARGET", "EVENT_DRAW",
]
# The standard init/helper scripts every card script may pull functions from.
HELPER_SCRIPTS = [
    "constant.lua", "utility.lua", "archetype_setcode_constants.lua",
    "card_counter_constants.lua", "cards_specific_functions.lua",
    "proc_normal.lua", "proc_fusion.lua", "proc_synchro.lua", "proc_xyz.lua",
    "proc_link.lua", "proc_pendulum.lua", "proc_ritual.lua",
]
SCRIPT_SUBDIRS = ["", "official", "unofficial", "rush", "skill", "goat",
                  "pre-errata"]


def decode_type(t):
    parts = [name for bit, name in TYPE_FLAGS if t & bit]
    return "/".join(parts) if parts else "(none)"


def decode_race(r):
    parts = [name for bit, name in RACE_FLAGS.items() if r & bit]
    return "/".join(parts) if parts else "(none)"


def decode_attr(a):
    parts = [name for bit, name in ATTR_FLAGS.items() if a & bit]
    return "/".join(parts) if parts else "(none)"


def decode_setcodes(setcode):
    out = []
    for i in range(4):
        part = (setcode >> (i * 16)) & 0xFFFF
        if part:
            out.append(part)
    return out


def open_dbs(db_paths):
    dbs = []
    for path in db_paths:
        if not os.path.isfile(path):
            print(f"  [warn] db not found: {path}", file=sys.stderr)
            continue
        try:
            con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
            dbs.append((con, path))
        except sqlite3.Error as e:
            print(f"  [warn] cannot open {path}: {e}", file=sys.stderr)
    return dbs


def row_for(con, code):
    return con.execute(
        "SELECT d.id,d.alias,d.type,d.atk,d.def,d.level,d.race,d.attribute,"
        "d.setcode,d.category,d.ot,t.name FROM datas d JOIN texts t "
        "ON d.id=t.id WHERE d.id=?", (code,)).fetchone()


def find_codes(dbs, name=None, code=None):
    """Resolve the set of card codes to inspect."""
    if code is not None:
        return [code]
    codes = []
    for con, _ in dbs:
        for r in con.execute("SELECT d.id FROM datas d JOIN texts t "
                             "ON d.id=t.id WHERE t.name=?", (name,)):
            if r[0] not in codes:
                codes.append(r[0])
    if codes:
        return codes
    # fall back to a partial match
    for con, _ in dbs:
        for r in con.execute("SELECT d.id,t.name FROM datas d JOIN texts t "
                             "ON d.id=t.id WHERE t.name LIKE ?",
                             (f"%{name}%",)):
            if r[0] not in codes:
                codes.append(r[0])
    return codes


def find_script(script_roots, code):
    """Return (path, mtime, size) for c<code>.lua, or (None, None, None)."""
    fn = f"c{code}.lua"
    for root in script_roots:
        for sub in SCRIPT_SUBDIRS:
            p = os.path.join(root, sub, fn) if sub else os.path.join(root, fn)
            if os.path.isfile(p):
                st = os.stat(p)
                return p, st.st_mtime, st.st_size
    return None, None, None


def scan_script(path):
    """Read a card script and summarise its effect registration."""
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    info = {
        "create_effect": text.count("Effect.CreateEffect"),
        "register_effect": text.count(":RegisterEffect"),
        "events": [e for e in TRIGGER_EVENTS if e in text],
        "has_condition": ":SetCondition" in text,
        "has_target": ":SetTarget" in text,
        "has_operation": ":SetOperation" in text,
        "has_cost": ":SetCost" in text,
        "uses_aux": "aux." in text or "Auxiliary." in text,
        "listed_series": "listed_series" in text,
        "stringids": sorted(set(_stringids(text))),
    }
    return info


def _stringids(text):
    """Pull the N from aux.Stringid(id,N) / Stringid(id,N) occurrences."""
    out = []
    for token in ("Stringid(id,", "Stringid(id ,"):
        start = 0
        while True:
            i = text.find(token, start)
            if i < 0:
                break
            j = i + len(token)
            num = ""
            while j < len(text) and text[j] in " 0123456789":
                if text[j].isdigit():
                    num += text[j]
                j += 1
            if num:
                out.append(int(num))
            start = i + len(token)
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--name", help="card name (exact, else partial match)")
    g.add_argument("--code", type=int, help="card passcode")
    ap.add_argument("--db", action="append", default=[],
                    help="a cards.cdb (repeatable; first = primary)")
    ap.add_argument("--db-dir", action="append", default=[],
                    help="folder of .cdb files to add as fallbacks")
    ap.add_argument("--scripts", action="append", default=[],
                    help="script root folder (repeatable). Subfolders "
                         "official/ unofficial/ rush/ skill/ goat/ "
                         "pre-errata/ are searched automatically.")
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
    script_roots = args.scripts or []
    if not script_roots:
        print("  [warn] no --scripts root given; script checks skipped",
              file=sys.stderr)

    codes = find_codes(dbs, name=args.name, code=args.code)
    if not codes:
        print(f"No card matched {args.name or args.code!r} in any database.")
        return 1

    for code in codes:
        print("=" * 78)
        print(f"CARD COMPATIBILITY CHECK  -  code {code}")
        print("=" * 78)

        # ── all CDB hits (duplicates included) ───────────────────────────────
        hits = []
        for con, path in dbs:
            r = row_for(con, code)
            if r:
                hits.append((path, r))
        if not hits:
            print(f"  card {code} not found in ANY database.")
            continue

        print(f"\n[CDB] resolved name : {hits[0][1][11]!r}")
        print(f"[CDB] present in    : {len(hits)} database(s)"
              + ("   <-- DUPLICATE across CDBs" if len(hits) > 1 else ""))
        for path, r in hits:
            (_id, alias, typ, atk, dfn, lvl, race, attr, setc, cat,
             ot, name) = r
            print(f"  - {os.path.basename(path)}")
            print(f"      name={name!r} alias={alias} type=0x{typ:x}"
                  f" ({decode_type(typ)})")
            print(f"      atk={atk} def={dfn} level/rank/link={lvl & 0xff}"
                  f" race=0x{race:x} ({decode_race(race)})"
                  f" attribute=0x{attr:x} ({decode_attr(attr)})")
            sc = decode_setcodes(setc)
            print(f"      setcode=0x{setc:x} -> {[hex(s) for s in sc]}"
                  f" category=0x{cat:x} ot={ot}"
                  f" ({OT_NAMES.get(ot, 'non-standard')})")

        # warn on metadata divergence between CDBs
        if len(hits) > 1:
            base = hits[0][1]
            for path, r in hits[1:]:
                diffs = []
                for idx, label in ((2, "type"), (6, "race"),
                                   (7, "attribute"), (8, "setcode"),
                                   (1, "alias"), (10, "ot")):
                    if r[idx] != base[idx]:
                        diffs.append(f"{label} "
                                     f"({base[idx]:#x} vs {r[idx]:#x})")
                if diffs:
                    print(f"  [!] {os.path.basename(path)} DIVERGES from "
                          f"primary: {', '.join(diffs)}")

        primary = hits[0][1]
        alias = primary[1]

        # ── script presence ──────────────────────────────────────────────────
        print()
        if not script_roots:
            print("[SCRIPT] (skipped - no --scripts root supplied)")
        else:
            spath, mtime, size = find_script(script_roots, code)
            expected = f"c{code}.lua"
            if spath is None:
                print(f"[SCRIPT] expected {expected}  ->  *** MISSING ***")
                print("         the card has NO script: it loads as a vanilla "
                      "body and cannot register any trigger or activated "
                      "effect.")
                if alias:
                    apath, _, _ = find_script(script_roots, alias)
                    print(f"         alias={alias}: alias script "
                          + ("found at " + apath if apath
                             else f"c{alias}.lua ALSO missing"))
            else:
                import time
                print(f"[SCRIPT] {expected}  ->  {spath}")
                print(f"         size={size} bytes  modified="
                      f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(mtime))}")
                s = scan_script(spath)
                print(f"         Effect.CreateEffect x{s['create_effect']}"
                      f"   :RegisterEffect x{s['register_effect']}")
                print(f"         SetCondition={s['has_condition']}"
                      f" SetTarget={s['has_target']}"
                      f" SetOperation={s['has_operation']}"
                      f" SetCost={s['has_cost']}")
                print(f"         trigger/timing events: "
                      + (", ".join(s["events"]) if s["events"]
                         else "(none found)"))
                if s["stringids"]:
                    print(f"         effect description indices used: "
                          f"{s['stringids']}  (Stringid(id,N) -> texts.str(N+1))")
                summon_trig = any(e in s["events"] for e in
                                  ("EVENT_SUMMON_SUCCESS",
                                   "EVENT_SPSUMMON_SUCCESS"))
                print(f"         registers a Normal/Special-Summon trigger: "
                      f"{'YES' if summon_trig else 'no'}")

        # ── helper scripts ───────────────────────────────────────────────────
        if script_roots:
            print()
            missing_helpers = []
            for h in HELPER_SCRIPTS:
                hp, _, _ = None, None, None
                for root in script_roots:
                    cand = os.path.join(root, h)
                    if os.path.isfile(cand):
                        hp = cand
                        break
                if hp is None:
                    missing_helpers.append(h)
            if missing_helpers:
                print(f"[HELPERS] MISSING: {', '.join(missing_helpers)}")
                print("          a card script that calls into a missing "
                      "helper will fail to load.")
            else:
                print(f"[HELPERS] all {len(HELPER_SCRIPTS)} standard "
                      f"helper/procedure scripts present.")

        # ── related archetype cards ──────────────────────────────────────────
        sc = decode_setcodes(primary[8])
        if sc:
            related = set()
            for con, _ in dbs:
                for s in sc:
                    for r in con.execute("SELECT id FROM datas WHERE "
                                         "(setcode&0xffff)=? OR "
                                         "((setcode>>16)&0xffff)=? OR "
                                         "((setcode>>32)&0xffff)=? OR "
                                         "((setcode>>48)&0xffff)=?",
                                         (s, s, s, s)):
                        related.add(r[0])
            print()
            print(f"[ARCHETYPE] set code(s) {[hex(s) for s in sc]}: "
                  f"{len(related)} card(s) share this series across all CDBs "
                  f"(search/target pool).")

        # ── verdict ──────────────────────────────────────────────────────────
        print()
        if script_roots:
            spath, _, _ = find_script(script_roots, code)
            if spath is None:
                print("VERDICT: SCRIPT MISSING. The card database knows this "
                      "card but the script collection does not. Add "
                      f"c{code}.lua (matching the cards.cdb ProjectIgnis "
                      "snapshot) to the assets/scripts folder.")
            else:
                print("VERDICT: script present. If the effect still does not "
                      "appear, check that its trigger condition is met "
                      "(targets/related cards) and that metadata above is "
                      "correct.")
        print()

    for con, _ in dbs:
        con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
