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


# ── v0.9.7.10: THE SMOOTH FONT ─────────────────────────────────────────
# drivers/vga.c draws cells of 12x24 and up from include/font_aa.h's 32x64
# master, shrunk to the cell by averaging and blended between ink and
# background. Such a cell is read by estimating, per pixel, how much of it
# is ink -- its position between the cell's two extreme colours -- and
# taking the glyph whose template, made the kernel's way from the same
# master (harness/font_aa.bin), is nearest. Both ways round, so a
# selection bar's inverted text reads too.
AA_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_aa.bin")


@functools.lru_cache(maxsize=8)
def _aa_templates(cw, ch):
    import numpy as np
    raw = np.frombuffer(open(AA_BIN, "rb").read(), dtype=np.uint8)
    nib = np.empty(raw.size * 2, dtype=np.int32)
    nib[0::2] = raw >> 4
    nib[1::2] = raw & 15
    m = nib.reshape(256, 64, 32)
    out = np.empty((256, ch, cw), dtype=np.float32)
    for y in range(ch):
        y0 = y * 64 // ch
        y1 = max((y + 1) * 64 // ch, y0 + 1)
        for x in range(cw):
            x0 = x * 32 // cw
            x1 = max((x + 1) * 32 // cw, x0 + 1)
            blk = m[:, y0:y1, x0:x1]
            n = (y1 - y0) * (x1 - x0)
            out[:, y, x] = (blk.sum(axis=(1, 2)) * 17 + n // 2) // n
    return (out / 255.0).reshape(256, ch * cw)


def _decode_smooth(im, cols, rows, cw, ch, ox, oy):
    import numpy as np
    a = np.asarray(im, dtype=np.float32)
    T = _aa_templates(cw, ch)                         # (256, P)
    order = list(range(32, 127)) + [c for c in range(256) if not 32 <= c < 127]
    T = T[order]
    tt = (T * T).sum(axis=1)                          # |t|^2
    cells = a[oy:oy + rows * ch, ox:ox + cols * cw].reshape(rows, ch, cols, cw, 3)
    cells = cells.transpose(0, 2, 1, 3, 4).reshape(rows * cols, ch * cw, 3)
    lum = cells @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    lo = lum.argmin(axis=1)
    hi = lum.argmax(axis=1)
    idx = np.arange(rows * cols)
    c_lo = cells[idx, lo]                             # (N, 3)
    c_hi = cells[idx, hi]
    d = c_hi - c_lo
    dd = (d * d).sum(axis=1)
    blank = dd < 600.0                                # about 14 levels apart
    dd[blank] = 1.0
    A = ((cells - c_lo[:, None, :]) * d[:, None, :]).sum(axis=2) / dd[:, None]
    A = np.clip(A, 0.0, 1.0)                          # ink = the light colour
    B = 1.0 - A                                       # ink = the dark colour
    best_err = np.full(rows * cols, np.inf, dtype=np.float32)
    best_g = np.zeros(rows * cols, dtype=np.int32)
    for X in (A, B):
        err = (X * X).sum(axis=1)[:, None] + tt[None, :] - 2.0 * (X @ T.T)
        g = err.argmin(axis=1)
        e = err[idx, g]
        better = e < best_err
        best_err[better] = e[better]
        best_g[better] = g[better]
    P = ch * cw
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            k = r * cols + c
            if blank[k]:
                line.append(" ")
                continue
            code = order[best_g[k]]
            # mean squared error per pixel; a clean glyph is near 0, a
            # pointer or a shadow over one stays well under 0.05
            ch_ = chr(code) if 32 <= code < 127 else ("?" if code > 127 else " ")
            line.append(ch_ if best_err[k] / P < 0.06 else "?")
        out.append("".join(line).rstrip())
    return out


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
    if cw >= 12 and ch >= 24 and os.path.exists(AA_BIN):
        out = _decode_smooth(im, cols, rows, cw, ch, ox, oy)
        # A screen drawn in the classic font -- `font classic`, or a
        # release before 0.9.7.10 (migrate_fat32 installs with 0.9.7.7) --
        # matches the smooth templates badly. Mostly '?' means read it the
        # old way.
        txt = "".join(out).replace(" ", "")
        if not txt or txt.count("?") * 4 < len(txt):
            return out
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
            # v0.9.7.8: the Studio theme draws background 0 as a fade, one
            # colour per pixel ROW. A cell whose every row is one colour is
            # blank, however many rows there are; and the ink is the one
            # colour that is NOT the row's background, so each seen colour
            # is also tried as the foreground.
            if all(len(set(row)) == 1 for row in samp):
                line.append(" ")
                continue
            best, bestd = 32, 999
            # Background first, as before: one exact match ends the cell.
            # Only a cell that did not match exactly -- in practice one on
            # the fade -- pays for trying every colour as the ink, so the
            # reader stays as fast as it was on a flat background (a harness
            # watching the boot splash polls for lines shown for a moment).
            tries = [("bg", c) for c in seen[:4]]
            if len(seen) > 2:
                tries += [("fg", c) for c in seen[:24]]
            for kind, col in tries:
                if bestd == 0:
                    break
                if kind == "bg":
                    bits = tuple(sum((1 << (FONT_W - 1 - i)) for i in range(FONT_W)
                                     if samp[j][i] != col) for j in range(FONT_H))
                else:
                    bits = tuple(sum((1 << (FONT_W - 1 - i)) for i in range(FONT_W)
                                     if samp[j][i] == col) for j in range(FONT_H))
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
