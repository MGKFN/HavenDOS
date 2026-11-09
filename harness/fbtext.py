"""
Read text back off a HavenDOS framebuffer screenshot.

The framebuffer console draws 8x16 glyphs scaled by an integer factor, so a
screenshot can be decoded back to the exact characters rather than guessed at
with OCR: each cell is sampled at one pixel per glyph pixel and matched
against the font table the kernel compiled in.

── v0.8.8.2: 8x16, AND ONE SCALE FOR BOTH AXES ─────────────────────────
This read include/font8x8.h and computed a scale per axis, because that is
what drivers/vga.c did. Both changed in v0.8.8.2 and this stopped decoding
anything -- the screen came back as rows of "?" -- which is the right kind
of failure: the decoder is pinned to the kernel's font and geometry, so it
breaks loudly when they move instead of quietly reading the wrong pixels.

The font is 8 wide and 16 tall now because 80x25 of 8x8 cells is 640x200,
an aspect ratio no panel has, and the per-axis scaling that followed from
it drew every character twice as tall as it was wide. The scale is now the
MINIMUM of the two axes, so the glyph keeps its shape and the grid is
centred in whatever is left over.

Ambiguity is resolved by nearest Hamming distance, which costs nothing at this
size and keeps a single stray pixel from turning a glyph into "unknown".
"""
import re, os, functools
from PIL import Image

FONT_W = 8
FONT_H = 16
SRC = "include/font8x16.h"


@functools.lru_cache(maxsize=1)
def _font():
    body = open(SRC).read().split("FONT8X16_HEIGHT] = {", 1)[1].split("};", 1)[0]
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    # bit 7 is the leftmost pixel, so a glyph is 16 rows of 8 bits in
    # reading order.
    glyphs = {}
    for g in range(256):
        rows = tuple(vals[g * FONT_H:(g + 1) * FONT_H])
        glyphs.setdefault(rows, []).append(g)
    return vals, glyphs


def _printable(cands):
    for c in cands:
        if 32 <= c < 127:
            return c
    return cands[0]


def decode(path, cols=80, rows=25):
    """Return the screen as a list of `rows` strings, `cols` wide.

    ── v0.8.8: THE CELL SIZE IS THE SCALE, NOT THE SCREEN DIVIDED ──────

    This used to compute the cell as W//cols by H//rows, which assumes
    the grid fills the screen exactly.  drivers/vga.c does not do that
    and never did: it picks an INTEGER glyph scale in each axis and
    CENTRES the result, because an 8x8 font cannot give 80x25 on an
    arbitrary panel without stretching.

    On 1280x800 the two agree -- 1280/80 is 16 and 16/8 is 2 -- which
    is the only panel QEMU has ever been run at here, and so the error
    never showed.  On 1366x768 the old arithmetic gives a 17-pixel cell
    against the driver's 16 and no 43-pixel left margin, so the sampling
    point drifts a pixel per column and the decode turns to noise about
    a third of the way across.

    It showed up the moment v0.8.8 made a non-80x25 grid possible:
    `resolution 100x50` on a 1280x800 panel is a scale of 1x2 with a
    240-pixel margin down each side, and the harness read the resulting
    screen as ASCII art.

    This reproduces fbc_fit()'s arithmetic, which is normally the thing
    this tree refuses -- a fixture written from the same guess as the
    code agrees with it perfectly.  It is acceptable here because this
    is a READER and not a judge: nothing in any harness asserts the
    geometry through it.  The geometry is tested in
    tests/test_console.c, against the pixels of a simulated panel, and
    that suite derives its expectations from the panel rather than from
    the driver.
    """
    im = Image.open(path).convert("RGB")
    W, H = im.size
    sx, sy = W // (cols * FONT_W), H // (rows * FONT_H)
    if sx < 1 or sy < 1:
        raise ValueError("%dx%d does not hold %dx%d cells of %dx%d"
                         % (W, H, cols, rows, FONT_W, FONT_H))
    # ONE SCALE, the minimum -- fbc_fit() takes the smaller of the two so
    # the glyph is not distorted, and reading with two would sample the
    # wrong pixels on any panel where they differ.
    s_ = sx if sx < sy else sy
    sx = sy = s_
    cw, ch = FONT_W * sx, FONT_H * sy
    ox, oy = (W - cols * cw) // 2, (H - rows * ch) // 2
    px = im.load()
    vals, glyphs = _font()

    # A cell holds at most two colours.  Which is the background cannot be
    # decided by counting pixels - a dense glyph like '#' or 'W' lights more
    # than half the cell and would read inverted - so both readings are tried
    # and the one that actually matches a glyph wins.
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            x0, y0 = ox + c * cw, oy + r * ch
            samp = [[px[x0 + i * sx, y0 + j * sy] for i in range(FONT_W)]
                    for j in range(FONT_H)]
            seen = []
            for row in samp:
                for p_ in row:
                    if p_ not in seen:
                        seen.append(p_)
            if len(seen) == 1:
                line.append(" ")
                continue
            best, bestd = 32, 999
            for bg in seen[:4]:
                bits = tuple(sum((1 << (FONT_W - 1 - i)) for i in range(FONT_W)
                                 if samp[j][i] != bg) for j in range(FONT_H))
                if bits in glyphs:
                    g, d = _printable(glyphs[bits]), 0
                else:
                    g, d = 32, 999
                    for cand in range(32, 127):
                        dd = sum(bin(bits[j] ^ vals[cand * FONT_H + j]).count("1")
                                 for j in range(FONT_H))
                        if dd < d:
                            g, d = cand, dd
                if d < bestd:
                    best, bestd = g, d
            # The tolerance scales with the glyph: 8 wrong pixels out of
            # 64 was the old threshold, and the cell is 128 pixels now.
            line.append(chr(best) if bestd <= 16 else "?")
        out.append("".join(line).rstrip())
    return out


def text(path, cols=80, rows=25):
    return "\n".join(decode(path, cols, rows))


if __name__ == "__main__":
    import sys
    print(text(sys.argv[1]))
