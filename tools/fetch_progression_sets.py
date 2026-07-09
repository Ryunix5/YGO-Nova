#!/usr/bin/env python3
# ─── fetch_progression_sets.py ───────────────────────────────────────────────
#
# Build the data the Arcade "Progression Series" mode needs: the chronological
# list of Yu-Gi-Oh print sets and which cards each set contains. Two files:
#
#   assets/arcade/sets.txt
#       One line per set, sorted oldest-first:
#           <tcg_date>\t<num_openable>\t<set_name>
#       tcg_date is ISO (YYYY-MM-DD); num_openable = cards we could map to a
#       passcode. Progression walks these in order (LOB → MRD → …), and OTS /
#       tournament packs are just sets in here too, so "an OTS pack released
#       before the current set" is a simple date compare.
#
#   assets/arcade/set_cards.txt
#       One line per set:
#           <set_name>\t<code>,<code>,<code>,...
#       The pool a pack from that set draws from.
#
# Data source: ygoprodeck. /cardsets.php gives set metadata (tcg_date); the
# bulk /cardinfo.php carries card_sets[] per card (which sets it was printed
# in). We only keep sets that have a date AND at least a few mappable cards.
#
# Re-run when new sets release, then ship the refreshed files. The API host is
# different from the image CDN, so this can work even where card art is blocked.
import json
import os
import sys
import urllib.request

SETS_URL  = "https://db.ygoprodeck.com/api/v7/cardsets.php"
CARDS_URL = "https://db.ygoprodeck.com/api/v7/cardinfo.php"
OUT_SETS  = "assets/arcade/sets.txt"
OUT_CARDS = "assets/arcade/set_cards.txt"
MIN_CARDS = 12            # skip tiny promo sets that can't fill packs


def get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "YGONova-Arcade/1.0"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.load(r)


def main():
    print("fetching set list...")
    set_date = {}                      # set_name -> tcg_date (ISO)
    for s in get_json(SETS_URL):
        name = s.get("set_name")
        date = s.get("tcg_date")       # some sets have no TCG date; skip those
        if name and date:
            set_date[name] = date

    print("fetching bulk card data (~100 MB, be patient)...")
    cards = get_json(CARDS_URL).get("data", [])
    set_cards = {}                     # set_name -> set(passcodes)
    for c in cards:
        cid = c.get("id")
        if not cid:
            continue
        for cs in (c.get("card_sets") or []):
            name = cs.get("set_name")
            if name in set_date:       # only sets we have a date for
                set_cards.setdefault(name, set()).add(int(cid))

    # Keep sets that can actually fill packs, sort chronologically.
    usable = [(set_date[n], sorted(codes), n)
              for n, codes in set_cards.items()
              if len(codes) >= MIN_CARDS]
    usable.sort(key=lambda t: (t[0], t[2]))

    os.makedirs("assets/arcade", exist_ok=True)
    with open(OUT_SETS, "w", encoding="utf-8") as f:
        f.write("# Progression Series sets, oldest first.\n")
        f.write("# tcg_date<TAB>num_cards<TAB>set_name\n")
        for date, codes, name in usable:
            f.write(f"{date}\t{len(codes)}\t{name}\n")
    with open(OUT_CARDS, "w", encoding="utf-8") as f:
        f.write("# set_name<TAB>comma-separated passcodes\n")
        for _date, codes, name in usable:
            f.write(name + "\t" + ",".join(str(x) for x in codes) + "\n")

    print(f"wrote {len(usable)} sets -> {OUT_SETS}, {OUT_CARDS}")
    if usable:
        print(f"  oldest: {usable[0][2]} ({usable[0][0]})")
        print(f"  newest: {usable[-1][2]} ({usable[-1][0]})")


if __name__ == "__main__":
    sys.exit(main())
