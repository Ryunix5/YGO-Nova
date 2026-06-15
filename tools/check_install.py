#!/usr/bin/env python3
"""
check_install.py — sanity-check an EdoPro+ install.

Run from the project root:

    python tools/check_install.py

Reports PASS / WARN / FAIL for the assets the runtime needs. Exit code is
the number of failures so the script is CI-friendly.

Only standard library — no downloads.
"""

from __future__ import annotations
import os
import sys
import sqlite3
from pathlib import Path

# Treat the script's parent's parent as project root so it works from
# any cwd: tools/check_install.py -> project root.
ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"

# Canonical SFX bank — must stay in sync with AudioManager::expectedSfx
# and tools/generate_sfx.py.
EXPECTED_SFX = [
    "click", "hover", "confirm", "cancel", "error",
    "draw", "shuffle", "summon", "special_summon", "set",
    "activate", "chain", "send_gy", "banish", "attack",
    "damage", "victory", "defeat", "duel_start",
]


class Tally:
    def __init__(self) -> None:
        self.passes = 0
        self.warns = 0
        self.fails = 0

    def line(self, tag: str, ok: bool, msg: str, *, warn_only: bool = False) -> None:
        if ok:
            self.passes += 1
            print(f"  [PASS] {tag:<14} {msg}")
        elif warn_only:
            self.warns += 1
            print(f"  [WARN] {tag:<14} {msg}")
        else:
            self.fails += 1
            print(f"  [FAIL] {tag:<14} {msg}")


def check_cards_cdb(t: Tally) -> None:
    cdb = ASSETS / "cards.cdb"
    if not cdb.is_file():
        t.line("cards.cdb", False, f"missing — expected at {cdb}")
        return
    try:
        with sqlite3.connect(str(cdb)) as conn:
            rows = conn.execute("SELECT COUNT(*) FROM datas").fetchone()[0]
        t.line("cards.cdb", True, f"{cdb} ({rows} cards)")
    except sqlite3.Error as e:
        t.line("cards.cdb", False, f"unreadable: {e}")


def check_cdb_fallbacks(t: Tally) -> None:
    babel = ASSETS / "BabelCDB-master"
    if not babel.is_dir():
        t.line("BabelCDB", True, "(none — optional fallback)", warn_only=False)
        return
    extras = list(babel.rglob("*.cdb"))
    t.line("BabelCDB", True, f"{len(extras)} fallback .cdb files")


def check_scripts(t: Tally) -> None:
    scripts = ASSETS / "scripts"
    if not scripts.is_dir():
        t.line("scripts", False, f"missing folder: {scripts}")
        return
    luas = list(scripts.rglob("*.lua"))
    n = len(luas)
    if n < 50:
        t.line("scripts", False, f"{n} .lua files (expected hundreds)")
    else:
        t.line("scripts", True, f"{n} .lua files")


def check_decks(t: Tally) -> None:
    decks = ASSETS / "decks"
    if not decks.is_dir():
        t.line("decks", False, f"missing folder: {decks}")
        return
    ydks = list(decks.glob("*.ydk"))
    t.line("decks", True, f"{len(ydks)} .ydk files at {decks}")


def check_sfx(t: Tally) -> None:
    sfx = ASSETS / "sfx"
    if not sfx.is_dir():
        t.line("sfx", False, "missing folder — run tools/generate_sfx.py")
        return
    have = {p.stem for p in sfx.glob("*.wav")}
    missing = [n for n in EXPECTED_SFX if n not in have]
    if missing:
        t.line("sfx", False,
               f"{len(have)}/{len(EXPECTED_SFX)} loaded "
               f"(missing: {', '.join(missing)})",
               warn_only=False)
    else:
        t.line("sfx", True, f"{len(have)}/{len(EXPECTED_SFX)} present")


def check_card_back(t: Tally) -> None:
    back = ASSETS / "card_back.png"
    if back.is_file():
        t.line("card_back", True, str(back))
    else:
        t.line("card_back", True,
               "(missing — procedural fallback in use)",
               warn_only=True)


def check_replays(t: Tally) -> None:
    rep = ASSETS / "replays"
    if not rep.is_dir():
        # Not a fail — the dir is created on first save.
        t.line("replays", True, "(folder will be created on first save)")
        return
    files = list(rep.glob("*.json"))
    t.line("replays", True, f"{len(files)} replay file(s) at {rep}")


def check_settings(t: Tally) -> None:
    cfg = ASSETS / "config" / "settings.cfg"
    if cfg.is_file():
        t.line("settings", True, str(cfg))
    else:
        t.line("settings", True,
               "(file will be created on first save)", warn_only=False)


def check_write_perms(t: Tally) -> None:
    for sub in ["config", "decks", "replays"]:
        d = ASSETS / sub
        try:
            d.mkdir(parents=True, exist_ok=True)
            probe = d / ".write_probe"
            probe.write_text("ok", encoding="utf-8")
            probe.unlink()
            t.line(f"write {sub}", True, f"OK ({d})")
        except OSError as e:
            t.line(f"write {sub}", False, f"{e}")


def main() -> int:
    print(f"check_install.py — EdoPro+ asset audit")
    print(f"project root: {ROOT}\n")
    t = Tally()
    check_cards_cdb(t)
    check_cdb_fallbacks(t)
    check_scripts(t)
    check_decks(t)
    check_sfx(t)
    check_card_back(t)
    check_replays(t)
    check_settings(t)
    check_write_perms(t)
    print("")
    print(f"  Summary: {t.passes} PASS  {t.warns} WARN  {t.fails} FAIL")
    return t.fails


if __name__ == "__main__":
    sys.exit(main())
