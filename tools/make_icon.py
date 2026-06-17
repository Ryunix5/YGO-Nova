#!/usr/bin/env python3
"""Generate the multi-resolution Windows app icon from the YGO Nova logo.

Reads a square PNG logo (transparent background recommended) and writes a
.ico containing the standard icon sizes Windows expects (16-256 px). The
result is embedded into the exe (installer/app_icon.rc) and used by the
installer (SetupIconFile / Add-Remove icon).

Usage:
    python tools/make_icon.py [--src installer/logo.png] [--out installer/YGONova.ico]

Requires Pillow:  pip install pillow
"""
import argparse
import os
import sys

SIZES = [16, 24, 32, 48, 64, 128, 256]


def prepare(img, keep_bg=False):
    """Return a square RGBA image with the outer background knocked out.

    The logo art is a coloured emblem on a flat (usually white) background. We
    flood-fill from the four corners so only the *outer* background turns
    transparent - interior light areas (the eye, gaps inside the ring) are
    walled off by the emblem's outline and preserved. Then we tight-crop to the
    emblem and centre it on a square canvas so the icon is large and uncropped.
    """
    from PIL import Image, ImageDraw
    img = img.convert("RGBA")
    if not keep_bg:
        w, h = img.size
        # Seed colour = the top-left corner (the background). thresh tolerates
        # JPEG-ish noise / anti-aliasing around a near-flat background.
        for seed in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
            ImageDraw.floodfill(img, seed, (0, 0, 0, 0), thresh=40)
    bbox = img.getbbox()            # bounds of everything still opaque
    if bbox:
        img = img.crop(bbox)
    w, h = img.size
    if w != h:                      # centre on a transparent square canvas
        side = max(w, h)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(img, ((side - w) // 2, (side - h) // 2), img)
        img = canvas
    return img


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(root, "installer", "logo.png"),
                    help="source logo PNG (square; transparent bg preferred)")
    ap.add_argument("--out", default=os.path.join(root, "installer", "YGONova.ico"),
                    help="output .ico path")
    ap.add_argument("--keep-bg", action="store_true",
                    help="do not knock out the background (logo already transparent)")
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow is required. Install it with:  pip install pillow")

    if not os.path.isfile(args.src):
        sys.exit(f"Source logo not found: {args.src}\n"
                 f"Save the logo PNG there first.")

    img = prepare(Image.open(args.src), keep_bg=args.keep_bg)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    img.save(args.out, format="ICO",
             sizes=[(s, s) for s in SIZES])
    kb = os.path.getsize(args.out) / 1024.0
    print(f"Wrote {args.out} ({kb:.1f} KB) with sizes {SIZES}")


if __name__ == "__main__":
    main()
