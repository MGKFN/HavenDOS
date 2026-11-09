#!/usr/bin/env python3
"""make_font8x16.py -- the 8x16 console font, and why there is one.

── THE BUG THIS FIXES ────────────────────────────────────────────────

drivers/vga.c's framebuffer console picks its horizontal and vertical
glyph scale INDEPENDENTLY:

    sx = fb_width  / (cols * 8)
    sy = fb_height / (rows * 8)

With an 8x8 font at 80x25 on a 1280x800 panel that is sx=2, sy=4, so
every character is drawn SIXTEEN PIXELS WIDE AND THIRTY-TWO TALL. On
1920x1080 it is 3 and 5. Text has been stretched to twice its proper
height on every framebuffer machine this has ever run on, and it was
reported as "resolution works but is bugged and small" and "seems a
bit stretched" -- which is exactly what it is.

The scale factors are not the mistake. 80x25 of 8x8 cells is 640x200,
which is an aspect ratio of 16:5; no panel is that shape, so no pair
of equal scale factors fits one. 80x25 of 8x16 cells is 640x400,
which is 16:10 -- and divides 1280x800 by exactly 2 in both axes, and
1920x1200 by 3. That is why every VGA text mode in history used an
8x16 font for 80x25 and an 8x8 one only for 80x50.

So the fix is the font, not the scaler. "Switch to real graphics"
would change nothing: the console already IS a framebuffer, drawing
glyphs pixel by pixel.

── WHERE THE GLYPHS COME FROM ────────────────────────────────────────

ASCII 0x20-0x7E comes from koi8x16.bdf, shipped in X11's
font-misc-cyrillic. Its licence, quoted in full from
/usr/share/doc/xfonts-cyrillic/copyright:

    koi12x24b.bdf, koi8x16b.bdf and koi8x16.bdf:
    --------------------------------------------
    May be distributed and modified without restrictions.

That is the whole of it. There is precedent in this tree:
include/font8x8.h is Daniel Hepper's font8x8, public domain, with its
provenance recorded in the same way. A font was not invented here
because drawing 256 legible glyphs badly is worse than using 256
drawn well, and the licence question -- the thing that actually
needed deciding -- has a written answer.

0x80-0xFF are COMPOSED, not taken from that font: koi8x16 puts
Cyrillic there and HavenDOS puts CP437 accented letters there. They
are built from the 8x16 base letter plus an accent, using the same
table tools/make_font_ext.py uses for the 8x8 font, so a name with an
accent in it renders the same shape at both cell sizes.

0x00-0x1F and 0x7F are blank, as they are in include/font8x8.h.

── WHAT STAYS ON 8x8 ─────────────────────────────────────────────────

The VGA TEXT console. Its 80x50 mode uploads an 8-row font into plane
2 of the character generator, and that hardware wants exactly 8 rows.
include/font8x8.h is still what goes there, and drivers/mode13.c keeps
its own copy for the 320x200 graphics mode. This file is the
FRAMEBUFFER console's font and nothing else's.

Run:  python3 tools/make_font8x16.py            (writes include/font8x16.h)
"""
import os
import struct
import subprocess
import sys

SRC_PCF = "/usr/share/fonts/X11/cyrillic/koi8x16.pcf.gz"
OUT = "include/font8x16.h"
H = 16
FONT_ASCENT = 14          # koi8x16's baseline, 14 rows down a 16-row cell

# ── the accents, two rows each, exactly as tools/make_font_ext.py
#    draws them for the 8x8 font ───────────────────────────────────
DIAERESIS  = (0x00, 0x66)
ACUTE      = (0x0C, 0x18)
GRAVE      = (0x30, 0x18)
CIRCUMFLEX = (0x18, 0x24)
TILDE      = (0x00, 0x36)
RING       = (0x18, 0x18)

# base letter, accent, where it goes -- the same map as the 8x8 table,
# so the two fonts disagree about nothing.
GLYPHS = {
    0x80: ("C", None,       "cedilla"),
    0x81: ("u", DIAERESIS,  "above"),
    0x82: ("e", ACUTE,      "above"),
    0x83: ("a", CIRCUMFLEX, "above"),
    0x84: ("a", DIAERESIS,  "above"),
    0x85: ("a", GRAVE,      "above"),
    0x86: ("a", RING,       "above"),
    0x87: ("c", None,       "cedilla"),
    0x88: ("e", CIRCUMFLEX, "above"),
    0x89: ("e", DIAERESIS,  "above"),
    0x8A: ("e", GRAVE,      "above"),
    0x8B: ("i", DIAERESIS,  "above"),
    0x8C: ("i", CIRCUMFLEX, "above"),
    0x8D: ("i", GRAVE,      "above"),
    0x8E: ("A", DIAERESIS,  "above"),
    0x8F: ("A", RING,       "above"),
    0x90: ("E", ACUTE,      "above"),
    0x93: ("o", CIRCUMFLEX, "above"),
    0x94: ("o", DIAERESIS,  "above"),
    0x95: ("o", GRAVE,      "above"),
    0x96: ("u", CIRCUMFLEX, "above"),
    0x97: ("u", GRAVE,      "above"),
    0x98: ("y", DIAERESIS,  "above"),
    0x99: ("O", DIAERESIS,  "above"),
    0x9A: ("U", DIAERESIS,  "above"),
    0xA0: ("a", ACUTE,      "above"),
    0xA1: ("i", ACUTE,      "above"),
    0xA2: ("o", ACUTE,      "above"),
    0xA3: ("u", ACUTE,      "above"),
    0xA4: ("n", TILDE,      "above"),
    0xA5: ("N", TILDE,      "above"),
}

# ── NO CP437 BOX DRAWING, and that is deliberate ───────────────────
# There was a box() helper here that built line-drawing cells for
# 0xB0-0xDF, and it was deleted unused: shell/ui.c draws every border
# out of '.', '-' and '|', and nothing in the tree emits a CP437 line
# character. A generator that produces glyphs nobody asks for is a
# generator somebody later has to work out the purpose of.
#
# If the UI ever moves to real box drawing, this is where those
# glyphs go, and include/font8x8.h will need the same range filled or
# the two consoles will draw different borders.

def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


# ── a minimal PCF reader ────────────────────────────────────────────
PCF_METRICS = 4
PCF_BITMAPS = 8
PCF_BDF_ENCODINGS = 0x20
FMT_BYTE_BIG = 1 << 2
FMT_BIT_BIG = 1 << 3
FMT_COMPRESSED_METRICS = 0x100


def read_pcf(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x01fcp":
        sys.exit("make_font8x16: %s is not a PCF file" % path)
    (ntab,) = struct.unpack_from("<i", d, 4)
    tabs = {}
    for i in range(ntab):
        t, f, sz, off = struct.unpack_from("<4i", d, 8 + 16 * i)
        tabs[t] = (f, sz, off)

    f, sz, off = tabs[PCF_BDF_ENCODINGS]
    (fmt,) = struct.unpack_from("<i", d, off)
    e = ">" if fmt & FMT_BYTE_BIG else "<"
    minc, maxc, minb, maxb, defc = struct.unpack_from(e + "5h", d, off + 4)
    n = (maxc - minc + 1) * (maxb - minb + 1)
    idx = struct.unpack_from(e + "%dh" % n, d, off + 14)
    enc, k = {}, 0
    for b1 in range(minb, maxb + 1):
        for b2 in range(minc, maxc + 1):
            g = idx[k]
            k += 1
            if g != 0xFFFF:
                enc[((b1 << 8) | b2) if maxb else b2] = g

    f, sz, off = tabs[PCF_METRICS]
    (fmt,) = struct.unpack_from("<i", d, off)
    e = ">" if fmt & FMT_BYTE_BIG else "<"
    metrics = []
    if fmt & FMT_COMPRESSED_METRICS:
        (cnt,) = struct.unpack_from(e + "h", d, off + 4)
        p = off + 6
        for i in range(cnt):
            vals = struct.unpack_from("5B", d, p)
            p += 5
            metrics.append(tuple(v - 0x80 for v in vals))
    else:
        (cnt,) = struct.unpack_from(e + "i", d, off + 4)
        p = off + 8
        for i in range(cnt):
            vals = struct.unpack_from(e + "6h", d, p)
            p += 12
            metrics.append(vals[:5])

    f, sz, off = tabs[PCF_BITMAPS]
    (fmt,) = struct.unpack_from("<i", d, off)
    e = ">" if fmt & FMT_BYTE_BIG else "<"
    (cnt,) = struct.unpack_from(e + "i", d, off + 4)
    offs = struct.unpack_from(e + "%di" % cnt, d, off + 8)
    sizes = struct.unpack_from(e + "4i", d, off + 8 + 4 * cnt)
    data = d[off + 8 + 4 * cnt + 16:][:sizes[fmt & 3]]
    return dict(enc=enc, metrics=metrics, bitmaps=data, offs=offs,
                pad=1 << (fmt & 3), bit_big=bool(fmt & FMT_BIT_BIG))


def glyph(fnt, code):
    """`code`'s 16 rows, MSB = leftmost pixel, or None."""
    gi = fnt["enc"].get(code)
    if gi is None:
        return None
    lsb, rsb, cw, asc, desc = fnt["metrics"][gi]
    gw, gh = rsb - lsb, asc + desc
    rowbytes = max(fnt["pad"],
                   ((gw + 8 * fnt["pad"] - 1) // (8 * fnt["pad"])) * fnt["pad"])
    start = fnt["offs"][gi]
    out = [0] * H
    for r in range(gh):
        p = start + r * rowbytes
        b = fnt["bitmaps"][p] if p < len(fnt["bitmaps"]) else 0
        if not fnt["bit_big"]:
            b = int("{:08b}".format(b)[::-1], 2)
        if lsb > 0:
            b >>= lsb
        elif lsb < 0:
            b = (b << -lsb) & 0xFF
        y = FONT_ASCENT - asc + r
        if 0 <= y < H:
            out[y] = b & 0xFF
    return out


def compose(base_rows, accent, where):
    """A base letter plus an accent, the same way the 8x8 table does."""
    rows = list(base_rows)
    if where == "cedilla":
        # a hook under the baseline; the glyph's own descender space
        for y, bits in ((FONT_ASCENT, 0x18), (FONT_ASCENT + 1, 0x0C)):
            if y < H:
                rows[y] |= bits
        return rows
    # An accent above: find the topmost used row and put the mark two
    # rows above it, so it sits over the letter rather than through it.
    top = next((y for y in range(H) if rows[y]), 4)
    a0 = max(0, top - 3)
    rows[a0] |= accent[0]
    if a0 + 1 < H:
        rows[a0 + 1] |= accent[1]
    return rows


def main():
    src = SRC_PCF
    tmp = "/tmp/_koi8x16.pcf"
    if src.endswith(".gz"):
        if not os.path.exists(src):
            sys.exit("make_font8x16: %s is not installed.\n"
                     "  apt-get install -y xfonts-cyrillic" % src)
        r = sh(["sh", "-c", "gunzip -c %s > %s" % (src, tmp)])
        if r.returncode != 0:
            sys.exit("make_font8x16: could not unpack %s" % src)
        src = tmp

    fnt = read_pcf(src)

    table = [[0] * H for _ in range(256)]
    have = 0
    missing = []

    # ASCII, straight from the font
    for c in range(0x20, 0x7F):
        g = glyph(fnt, c)
        if g is None:
            missing.append(c)
            continue
        table[c] = g
        have += 1
    if missing:
        sys.exit("make_font8x16: koi8x16 has no glyph for %s -- a console "
                 "font with holes in ASCII is not usable"
                 % ", ".join("0x%02X" % c for c in missing))

    # the accented range, composed from those
    composed = 0
    for code, (base, accent, where) in sorted(GLYPHS.items()):
        b = table[ord(base)]
        if not any(b):
            sys.exit("make_font8x16: no base glyph for %r" % base)
        table[code] = compose(b, accent, where)
        composed += 1

    # ── every glyph must fit the cell and not touch the right edge ──
    # An 8-wide cell with ink in bit 0 of every row has no gap between
    # characters, which is what makes a wall of text unreadable. This
    # is a property of the font and is asserted rather than assumed.
    solid = [c for c in range(0x20, 0x7F)
             if all(table[c][y] & 1 for y in range(4, 12))]
    if solid:
        sys.exit("make_font8x16: glyphs %s have ink in the rightmost "
                 "column on every row" % solid)

    blank = [c for c in range(0x21, 0x7F) if not any(table[c])]
    if blank:
        sys.exit("make_font8x16: printable code points came out blank: %s"
                 % ", ".join("0x%02X" % c for c in blank))
    if any(table[0x20]):
        sys.exit("make_font8x16: SPACE is not blank")

    L = []
    a = L.append
    a("/*")
    a(" * font8x16.h - the 8x16 FRAMEBUFFER console font.  v0.8.8.2.")
    a(" *")
    a(" * GENERATED by tools/make_font8x16.py - do not edit by hand.")
    a(" *")
    a(" * WHY 8x16 AND NOT 8x8")
    a(" *")
    a(" * drivers/vga.c scales the glyph independently in each axis, so")
    a(" * an 8x8 font at 80x25 on a 1280x800 panel came out at sx=2,")
    a(" * sy=4 -- every character sixteen pixels wide and THIRTY-TWO")
    a(" * TALL. That is not a scaler bug: 80x25 of 8x8 cells is 640x200,")
    a(" * an aspect ratio of 16:5, and no panel is that shape. 80x25 of")
    a(" * 8x16 cells is 640x400, which is 16:10 and divides 1280x800 by")
    a(" * exactly two in both axes. Every VGA text mode in history used")
    a(" * an 8x16 font for 80x25 for this reason.")
    a(" *")
    a(" * SOURCE AND LICENCE")
    a(" *")
    a(" * 0x20-0x7E are from koi8x16.bdf, in X11's font-misc-cyrillic.")
    a(" * Its licence, quoted in full from the package's copyright file:")
    a(" *")
    a(" *     koi12x24b.bdf, koi8x16b.bdf and koi8x16.bdf:")
    a(" *     --------------------------------------------")
    a(" *     May be distributed and modified without restrictions.")
    a(" *")
    a(" * That is the whole of it. Same arrangement as include/font8x8.h,")
    a(" * which is Daniel Hepper's public-domain font8x8.")
    a(" *")
    a(" * 0x80-0xFF are COMPOSED here from the 8x16 base letter plus an")
    a(" * accent -- koi8x16 puts Cyrillic at those code points and")
    a(" * HavenDOS puts CP437 accented letters there. The accent table is")
    a(" * the one tools/make_font_ext.py uses for the 8x8 font, so the")
    a(" * two sizes draw the same shapes.")
    a(" *")
    a(" * 0x00-0x1F and 0x7F are blank, as in include/font8x8.h.")
    a(" *")
    a(" * THIS IS THE FRAMEBUFFER CONSOLE'S FONT AND NOTHING ELSE'S. The")
    a(" * VGA text console uploads 8 rows into plane 2 of the character")
    a(" * generator and that hardware wants exactly 8; it still uses")
    a(" * include/font8x8.h, and drivers/mode13.c keeps its own copy.")
    a(" */")
    a("#pragma once")
    a('#include "types.h"')
    a("")
    a("#define FONT8X16_GLYPHS 256")
    a("#define FONT8X16_HEIGHT 16")
    a("")
    a("static const uint8_t font8x16[FONT8X16_GLYPHS*FONT8X16_HEIGHT] = {")
    for c in range(256):
        rows = table[c]
        ch = chr(c) if 0x20 < c < 0x7F else ""
        note = ("  /* %3d %s */" % (c, ch)) if ch else ("  /* %3d */" % c)
        a("    " + ", ".join("0x%02X" % b for b in rows) + "," + note)
    a("};")

    with open(OUT, "w") as f:
        f.write("\n".join(L) + "\n")

    print("make_font8x16: %s" % OUT)
    print("  %d ASCII glyphs from koi8x16.bdf (unrestricted licence)" % have)
    print("  %d accented glyphs composed from them" % composed)
    print("  cell 8x16; 80x25 of these is 640x400, which is 16:10")
    return 0


if __name__ == "__main__":
    sys.exit(main())
