#!/usr/bin/env python3
# ─── fetch_setnames.py ───────────────────────────────────────────────────────
#
# Build the Arcade mode's archetype-name table from Project Ignis' official
# strings.conf ("!setname 0x1aa White Forest" lines). Output:
#
#   assets/arcade/setnames.txt with lines:  <setcode-decimal> <name>
#
# Secret packs are keyed by 16-bit setcodes; this file gives them their real
# archetype names instead of the word-frequency fallback ("Series 0x1AA").
# Re-run alongside fetch_md_rarity.py when new archetypes release.
import sys
import urllib.request

URL = ("https://raw.githubusercontent.com/ProjectIgnis/Distribution/"
       "master/config/strings.conf")
OUT = "assets/arcade/setnames.txt"

def main():
    print("fetching Project Ignis strings.conf ...")
    req = urllib.request.Request(URL, headers={"User-Agent": "YGONova-Arcade/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        text = r.read().decode("utf-8", "replace")
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("!setname "):
            continue
        parts = line.split(None, 2)          # !setname 0x1aa White Forest
        if len(parts) < 3:
            continue
        try:
            code = int(parts[1], 0)
        except ValueError:
            continue
        # Only base 16-bit archetype codes matter for secret-pack keys.
        if not (0 < code <= 0xFFFF):
            continue
        name = parts[2].split("|")[0].strip()   # drop alt-language suffixes
        if name:
            rows.append((code, name))
    rows.sort()
    import os
    os.makedirs("assets/arcade", exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# Archetype names (Project Ignis !setname). setcode name\n")
        for code, name in rows:
            f.write(f"{code} {name}\n")
    print(f"wrote {len(rows)} archetype names -> {OUT}")

if __name__ == "__main__":
    sys.exit(main())
