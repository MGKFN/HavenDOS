#!/usr/bin/env python3
"""
Draw the GRUB background at the resolution the menu actually runs in.

The old background was 800x600 and GRUB scaled it to fill a 1280x800 screen,
which blurred the wordmark and dragged it down into the menu rows.  Drawing it
at the target size instead keeps it sharp, and the wordmark is placed as a
watermark above the menu rather than behind them.

Everything here is geometry and gradients - no text that could go stale.  The
version label is a theme label, written by tools/make_version.py.
"""
import sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1280, 800
BG        = (6, 10, 14)
GRID      = (12, 20, 26)
GLOW      = (34, 211, 211)
SUBTITLE  = (26, 120, 128)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "isodir", "boot", "grub", "themes", "havendos",
                   "background.png")


def font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def main():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)

    # a faint grid, so the field is not flat black
    for x in range(0, W, 32):
        d.line([(x, 0), (x, H)], fill=GRID)
    for y in range(0, H, 32):
        d.line([(0, y), (W, y)], fill=GRID)

    # the wordmark, drawn on its own layer so it can be blurred into a glow
    # and then laid back over itself at low opacity
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    f = font(104)
    text = "HAVENDOS"
    bbox = gd.textbbox((0, 0), text, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx, ty = (W - tw) // 2, 84
    gd.text((tx - bbox[0], ty - bbox[1]), text, font=f, fill=GLOW)

    sf = font(22)
    sub = "TechHaven Studios"
    sb = gd.textbbox((0, 0), sub, font=sf)
    gd.text(((W - (sb[2] - sb[0])) // 2 - sb[0], ty + th + 26 - sb[1]),
            sub, font=sf, fill=SUBTITLE)

    blurred = glow.filter(ImageFilter.GaussianBlur(18))
    im = Image.blend(im, Image.blend(im, blurred, 0.55), 1.0)
    im = Image.blend(im, Image.blend(im, glow, 0.22), 1.0)

    # a hairline under the wordmark, and one above the footer
    d = ImageDraw.Draw(im)
    d.line([(W * 0.12, ty + th + 68), (W * 0.88, ty + th + 68)], fill=(18, 56, 60))
    d.line([(0, int(H * 0.90)), (W, int(H * 0.90))], fill=(16, 24, 30))

    im.save(os.path.normpath(OUT))
    print("make_grub_bg: %s  %dx%d" % (os.path.normpath(OUT), W, H))


if __name__ == "__main__":
    main()
