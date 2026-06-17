#!/usr/bin/env python3
"""
sync_projectignis_assets.py - merge ProjectIgnis Lua scripts into the EdoPro+
canonical assets folder.

Reads from a ProjectIgnis source tree (e.g. C:\\ProjectIgnis) - recursively
finds every .lua script, classifies it by path component (official /
unofficial / rush / skill / goat / pre-errata, or the helper-script root) -
and merges it into <dst>/scripts/ keeping that structure. Dry-run by default;
nothing on disk changes until --apply.

Examples
    # Dry run - just preview the plan:
    python tools/sync_projectignis_assets.py \\
        --src C:\\ProjectIgnis --dst C:\\path\\to\\YGO-Nova\\assets

    # Apply the sync (backs up any existing scripts first):
    python tools/sync_projectignis_assets.py \\
        --src C:\\ProjectIgnis --dst C:\\path\\to\\YGO-Nova\\assets --apply

    # Also mirror into the build's Release/assets so a stale Release copy
    # cannot mask the fresh project-root scripts:
    python tools/sync_projectignis_assets.py \\
        --src C:\\ProjectIgnis --dst C:\\path\\to\\YGO-Nova\\assets --apply \\
        --mirror-release C:\\path\\to\\YGO-Nova\\build\\windows\\Release\\assets

The tool NEVER writes inside --src (ProjectIgnis stays untouched), never
modifies ocgcore/ or vendor/, never touches cards.cdb (scripts only).
"""

import argparse
import filecmp
import os
import shutil
import sys
import time
from pathlib import Path

# Folder names that, when found anywhere in a script's source path, decide its
# destination subfolder under <dst>/scripts/. Order matters only for logging.
CATEGORY_DIRS = ("official", "unofficial", "rush", "skill", "goat",
                 "pre-errata")

# Scripts whose presence is verified after sync (and asked about in the spec).
CRITICAL = [
    ("c81439173.lua", "Foolish Burial",          "official"),
    ("c24094653.lua", "Polymerization",          "official"),
    ("c35618217.lua", "Lunalight Kaleido Chick", "official"),
    ("c29302858.lua", "Vanquish Soul Razen",     "official"),
    ("utility.lua",                  "core helper",  ""),
    ("constant.lua",                 "core helper",  ""),
    ("archetype_setcode_constants.lua", "core helper", ""),
    ("proc_unofficial.lua",          "unofficial helper", ""),
]


def categorise(src_path: Path) -> str:
    """Return the destination subfolder ('' for the scripts/ root) for one
    .lua file based on the folder names along its source path."""
    parts = [p.lower() for p in src_path.parts]
    for cat in CATEGORY_DIRS:
        if cat in parts:
            return cat
    return ""


def is_card_script(name: str) -> bool:
    """c<digits>.lua  -- a per-card script."""
    if not name.startswith("c") or not name.endswith(".lua"):
        return False
    stem = name[1:-4]
    return stem.isdigit() and len(stem) > 0


def find_lua_scripts(src: Path):
    """Yield (src_file, category, dst_relative_name). Skips a few obvious
    non-script things (release notes, READMEs, *.lua in build/test outputs)."""
    skip_dirs = {"build", ".git", "tests", "test", "__pycache__", "node_modules"}
    for root, dirs, files in os.walk(src):
        # don't descend into obviously-not-scripts dirs
        dirs[:] = [d for d in dirs if d.lower() not in skip_dirs]
        for fn in files:
            if not fn.endswith(".lua"):
                continue
            sp = Path(root) / fn
            cat = categorise(sp)
            yield sp, cat, fn


def detected_source_roots(scripts):
    """For logging: the set of distinct top-level folders inside src that hold
    any Lua scripts, with counts."""
    roots = {}
    for sp, cat, _ in scripts:
        try:
            rel = sp.relative_to(SRC_ROOT)
        except ValueError:
            rel = sp
        top = rel.parts[0] if len(rel.parts) >= 1 else "(src root)"
        roots.setdefault(top, [0, 0])[0] += 1
        if cat:
            roots[top][1] += 1
    return roots


def fmt_ts(t):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t))


def fmt_size(n):
    return f"{n:,}".replace(",", " ")


SRC_ROOT = Path()  # set in main


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="ProjectIgnis root (e.g. C:\\ProjectIgnis)")
    ap.add_argument("--dst", required=True,
                    help="EdoPro+ assets root (e.g. C:\\path\\to\\YGO-Nova\\assets)")
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--dry-run", action="store_true",
                     help="preview only (default)")
    grp.add_argument("--apply", action="store_true",
                     help="actually copy files, backing up any existing scripts/")
    ap.add_argument("--mirror-release", metavar="DIR", default=None,
                    help="after applying, also copy <dst>/scripts/ into "
                         "<DIR>/scripts/ (typically the build Release/assets) "
                         "so a stale Release copy cannot mask the synced "
                         "project-root scripts.")
    args = ap.parse_args()

    src = Path(args.src).resolve()
    dst_assets = Path(args.dst).resolve()
    dst_scripts = dst_assets / "scripts"
    apply = bool(args.apply)
    if not apply:
        print("(no --apply: DRY RUN — nothing will be written.)\n")
    if not src.is_dir():
        print(f"error: --src not a directory: {src}", file=sys.stderr)
        return 2
    if not dst_assets.exists() and apply:
        dst_assets.mkdir(parents=True, exist_ok=True)

    global SRC_ROOT
    SRC_ROOT = src

    print(f"source  : {src}")
    print(f"target  : {dst_scripts}")
    if args.mirror_release:
        print(f"mirror  : {Path(args.mirror_release).resolve() / 'scripts'}")
    print()

    # ── scan source ──────────────────────────────────────────────────────────
    print("[scan] searching for .lua scripts under source ...")
    scripts = list(find_lua_scripts(src))
    if not scripts:
        print("error: no .lua files found in --src. Pass the ProjectIgnis "
              "root (the folder that contains script/ or CardScripts).",
              file=sys.stderr)
        return 1

    roots = detected_source_roots(scripts)
    print(f"[scan] {len(scripts)} .lua files found under {src}")
    print("[scan] top-level src folders containing scripts:")
    for k, (total, in_cat) in sorted(roots.items(),
                                     key=lambda x: -x[1][0])[:10]:
        print(f"  {k:<30}  {total:>6} script(s)  "
              f"({in_cat} in category subfolders)")
    by_cat = {}
    for _, cat, _ in scripts:
        by_cat[cat or "(root)"] = by_cat.get(cat or "(root)", 0) + 1
    print("[scan] classified by category:")
    for k in sorted(by_cat):
        print(f"  scripts/{k:<14}  {by_cat[k]:>6}")
    print()

    # ── pre-flight: backup existing scripts ──────────────────────────────────
    backup_dir = None
    if dst_scripts.exists():
        stamp = time.strftime("%Y%m%d_%H%M%S")
        backup_dir = dst_assets / f"scripts_backup_{stamp}"
        print(f"[backup] existing {dst_scripts} -> {backup_dir}")
        if apply:
            shutil.copytree(dst_scripts, backup_dir)
            print(f"[backup] done.")
        else:
            print("[backup] (skipped in dry-run)")
    else:
        print(f"[backup] no existing {dst_scripts} to back up.")
    print()

    # ── plan / apply the merge ───────────────────────────────────────────────
    stats = dict(copied=0, identical=0, skipped_older=0, overwritten=0,
                 new_files=0, errors=0)
    conflicts = []
    # de-duplicate: if the same basename appears under multiple src paths and
    # the same destination, keep the newest/largest src.
    candidates = {}  # dst_path -> (src_path, mtime, size)
    for sp, cat, fn in scripts:
        dp = dst_scripts / cat / fn if cat else dst_scripts / fn
        try:
            stt = sp.stat()
        except OSError:
            continue
        cur = candidates.get(dp)
        if cur is None:
            candidates[dp] = (sp, stt.st_mtime, stt.st_size)
        else:
            _, cur_mt, cur_sz = cur
            if stt.st_mtime > cur_mt or stt.st_size > cur_sz:
                candidates[dp] = (sp, stt.st_mtime, stt.st_size)

    print(f"[plan] {len(candidates)} distinct destination file(s).")

    for dp, (sp, mt, sz) in candidates.items():
        try:
            if dp.exists():
                dst_stat = dp.stat()
                if filecmp.cmp(sp, dp, shallow=False):
                    stats["identical"] += 1
                    continue
                # different content -> conflict
                src_newer = mt > dst_stat.st_mtime
                src_larger = sz > dst_stat.st_size
                pick_src = src_newer or src_larger
                conflicts.append({
                    "file": dp.name,
                    "category": dp.parent.name if dp.parent != dst_scripts
                                                else "(root)",
                    "src": str(sp),
                    "dst": str(dp),
                    "src_mtime": fmt_ts(mt),  "dst_mtime": fmt_ts(dst_stat.st_mtime),
                    "src_size":  fmt_size(sz), "dst_size":  fmt_size(dst_stat.st_size),
                    "chose":     "src" if pick_src else "dst",
                })
                if pick_src:
                    if apply:
                        shutil.copy2(sp, dp)
                    stats["overwritten"] += 1
                else:
                    stats["skipped_older"] += 1
            else:
                if apply:
                    dp.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(sp, dp)
                stats["new_files"] += 1
                stats["copied"] += 1
        except OSError as e:
            print(f"  [error] {dp}: {e}", file=sys.stderr)
            stats["errors"] += 1

    # ── conflict log ─────────────────────────────────────────────────────────
    if conflicts:
        print()
        print(f"[conflicts] {len(conflicts)} file(s) differed between src and dst:")
        # show at most 25 in stdout; write the full list to a file
        for c in conflicts[:25]:
            print(f"  {c['file']:<24} [{c['category']}] "
                  f"src({c['src_size']}B {c['src_mtime']}) vs "
                  f"dst({c['dst_size']}B {c['dst_mtime']}) "
                  f"-> chose {c['chose']}")
        if len(conflicts) > 25:
            print(f"  ... and {len(conflicts) - 25} more "
                  f"(written to sync_conflicts.log)")
        try:
            with open("sync_conflicts.log", "w", encoding="utf-8") as f:
                for c in conflicts:
                    f.write(
                        f"{c['file']}\t[{c['category']}]\t"
                        f"src={c['src']}\tdst={c['dst']}\t"
                        f"src_mtime={c['src_mtime']}\tdst_mtime={c['dst_mtime']}\t"
                        f"src_size={c['src_size']}\tdst_size={c['dst_size']}\t"
                        f"chose={c['chose']}\n")
        except OSError:
            pass

    print()
    print("[stats]")
    print(f"  new files     : {stats['new_files']}")
    print(f"  overwritten   : {stats['overwritten']}")
    print(f"  identical     : {stats['identical']}")
    print(f"  kept dst (newer): {stats['skipped_older']}")
    print(f"  conflicts     : {len(conflicts)}")
    print(f"  errors        : {stats['errors']}")
    if not apply:
        print()
        print("(dry run — re-run with --apply to actually write.)")

    # ── critical-script check (post-state) ───────────────────────────────────
    print()
    print("[verify] critical scripts after sync:")
    final_root = dst_scripts                       # the live dst
    for fname, label, cat in CRITICAL:
        # planned final location after this run
        planned = (final_root / cat / fname) if cat else (final_root / fname)
        # if dry-run, decide by what WOULD be in candidates plus what's already there
        exists_now = planned.exists()
        will_exist = exists_now or planned in candidates
        if apply:
            mark = "PRESENT" if planned.exists() else "MISSING"
        else:
            mark = "PRESENT" if exists_now else \
                   ("would be ADDED" if planned in candidates else "MISSING (not in src)")
        # if not present at the planned location, also check root in case
        # ProjectIgnis layout differs
        if mark.startswith("MISSING") and cat:
            alt = final_root / fname
            if alt.exists() or alt in candidates:
                mark = mark + f"   (found at scripts/ root: {alt.name})"
        print(f"  {fname:<36} [{label:<23}]  {mark}")

    # ── optional mirror to Release ───────────────────────────────────────────
    if args.mirror_release:
        rel_assets = Path(args.mirror_release).resolve()
        rel_scripts = rel_assets / "scripts"
        print()
        print(f"[mirror] {dst_scripts} -> {rel_scripts}")
        if apply:
            if rel_scripts.exists():
                stamp = time.strftime("%Y%m%d_%H%M%S")
                bk = rel_assets / f"scripts_backup_{stamp}"
                shutil.move(str(rel_scripts), str(bk))
                print(f"[mirror] existing release scripts moved to {bk}")
            shutil.copytree(dst_scripts, rel_scripts)
            print(f"[mirror] done.")
        else:
            print("[mirror] (skipped in dry-run)")

    print()
    if apply:
        print("DONE. Next steps:")
        print("  1. (re)build the app so CMake's POST_BUILD step copies the")
        print("     freshly-synced assets into build/.../assets, OR use")
        print("     --mirror-release to do it now without rebuilding.")
        print("  2. Run: python tools/check_card_compat.py --code 81439173 "
              "--db <cards.cdb> --scripts <dst>/scripts")
        print("     and confirm 'script present + Normal/Special-Summon trigger'.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
