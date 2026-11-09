#!/usr/bin/env python3
"""
make_pwa_icons.py — regenerate the app icons embedded in bootchat2_web.py

    python3 tools/make_pwa_icons.py            # print the base64 block
    python3 tools/make_pwa_icons.py --write    # patch bootchat2_web.py in place

Needs Pillow (pip install pillow). Only needed if you change the icon — the
web client already carries the generated PNGs, so nothing needs Pillow to run.

The icons live inside bootchat2_web.py as base64 rather than as separate
files, so the whole client stays one file you can copy anywhere. All four
sizes together are about 10 KB.
"""

import argparse
import base64
import io
import re
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

BG = (15, 17, 21, 255)          # matches --bg in the page
FG = (78, 161, 255, 255)        # matches --accent


def icon(size, maskable=False):
    """A speech bubble on a rounded dark tile.

    maskable leaves a 10% safety margin, because Android crops a maskable
    icon to whatever shape the launcher uses and art at the edge gets cut.
    """
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = int(size * 0.10) if maskable else 0
    d.rounded_rectangle([pad, pad, size - 1 - pad, size - 1 - pad],
                        radius=int(size * 0.22), fill=BG)

    m = size * 0.20 + pad
    w = size - 2 * m
    by0, by1 = m + w * 0.06, m + w * 0.70
    d.rounded_rectangle([m, by0, m + w, by1], radius=int(w * 0.20), fill=FG)
    d.polygon([(m + w * 0.24, by1 - 2), (m + w * 0.26, by1 + w * 0.24),
               (m + w * 0.52, by1 - 2)], fill=FG)

    cy = (by0 + by1) / 2
    for fx in (0.28, 0.50, 0.72):
        cx, rr = m + w * fx, w * 0.063
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=BG)
    return img


def b64(size, maskable=False):
    buf = io.BytesIO()
    icon(size, maskable).save(buf, "PNG", optimize=True)
    return base64.b64encode(buf.getvalue()).decode()


SPECS = [("ICON192", 192, False), ("ICON512", 512, False),
         ("ICON180", 180, False), ("ICONMASK", 512, True)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="patch tools/bootchat2_web.py in place")
    ap.add_argument("--target", default="tools/bootchat2_web.py")
    a = ap.parse_args()

    block = "".join('%s = "%s"\n' % (n, b64(sz, mk)) for n, sz, mk in SPECS)
    total = sum(len(l) for l in block.splitlines())

    if not a.write:
        sys.stdout.write(block)
        print("\n# %d chars total" % total, file=sys.stderr)
        return 0

    src = open(a.target).read()
    for name, _, _ in SPECS:
        new = [l for l in block.splitlines() if l.startswith(name + " =")][0]
        src, n = re.subn(r'^%s = "[^"]*"$' % name, new.replace("\\", "\\\\"),
                         src, count=1, flags=re.M)
        if n != 1:
            sys.exit("could not find %s in %s" % (name, a.target))
    open(a.target, "w").write(src)
    print("patched %s (%d chars of icon data)" % (a.target, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
