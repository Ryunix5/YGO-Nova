#!/usr/bin/env python3
# ─── fetch_md_rarity.py ──────────────────────────────────────────────────────
#
# Build the Arcade mode's Master Duel rarity table from the ygoprodeck API.
# One bulk request (misc=yes) → assets/arcade/md_rarity.txt with lines:
#
#   <passcode> <rarity>      rarity: 0=N 1=R 2=SR 3=UR
#
# Cards WITHOUT an md_rarity are not in Master Duel and are simply omitted —
# which doubles as the "is this card openable in packs" filter.
#
# Re-run whenever Master Duel gets new cards, then ship the refreshed file.
import json
import sys
import urllib.request

URL = "https://db.ygoprodeck.com/api/v7/cardinfo.php?misc=yes"
OUT = "assets/arcade/md_rarity.txt"

RARITY = {
    "Common": 0, "Normal": 0,
    "Rare": 1,
    "Super Rare": 2,
    "Ultra Rare": 3,
}

def main():
    print("fetching bulk card data (this is ~100 MB, be patient)...")
    req = urllib.request.Request(URL, headers={"User-Agent": "YGONova-Arcade/1.0"})
    with urllib.request.urlopen(req, timeout=300) as r:
        data = json.load(r)
    rows = []
    for card in data.get("data", []):
        misc = (card.get("misc_info") or [{}])[0]
        md = misc.get("md_rarity")
        if md is None or md not in RARITY:
            continue
        # Main printing id; alt arts share gameplay via alias in cards.cdb.
        rows.append((int(card["id"]), RARITY[md]))
    rows.sort()
    import os
    os.makedirs("assets/arcade", exist_ok=True)
    with open(OUT, "w") as f:
        f.write("# Master Duel rarities (ygoprodeck md_rarity). code rarity\n")
        f.write("# 0=N 1=R 2=SR 3=UR\n")
        for code, r in rows:
            f.write(f"{code} {r}\n")
    counts = [0, 0, 0, 0]
    for _, r in rows:
        counts[r] += 1
    print(f"wrote {len(rows)} cards -> {OUT}")
    print(f"  N={counts[0]}  R={counts[1]}  SR={counts[2]}  UR={counts[3]}")

if __name__ == "__main__":
    sys.exit(main())
