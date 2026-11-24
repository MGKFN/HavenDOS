"""arrowfind.py -- where the mouse pointer is, from two screenshots.

v0.9.7.10: the pointer is an arrow drawn in pixels (drivers/vga.c), not a
character cell with its colours swapped. It spans two or three cells, so
"the one cell that changed" no longer finds it. The arrow's TIP is its
top-left pixel, and the tip is always over the cell a click goes to; so:
diff the screen now against one taken with the pointer parked in the
top-left corner, ignore that corner, and the top-left-most changed pixel
is the tip.

    ref = shot()                     # pointer parked at (0, 0)
    ... move ...
    cell = tip_cell(shot(), ref, x0, y0, cw, ch)
"""
import numpy as np

# drivers/vga.c's ptr_shape: '1' the outline, '2' the fill. Only the top
# eleven rows are matched -- the solid part, and enough that an arrow
# half off the bottom of the screen is still found.
SHAPE = [
    "1           ",
    "11          ",
    "121         ",
    "1221        ",
    "12221       ",
    "122221      ",
    "1222221     ",
    "12222221    ",
    "122222221   ",
    "1222222221  ",
    "12222222221 ",
]
EDGE = (0x13, 0x16, 0x1c)
FILLS = ((0xe8, 0xe4, 0xda), (0xe2, 0x67, 0x3f))   # up, a button down


def _by_shape(a):
    """(x, y) of the arrow's tip found by its shape, or None. a: HxWx3."""
    H, W = a.shape[:2]
    edge = np.all(a == EDGE, axis=2)
    fill = np.zeros((H, W), bool)
    for f in FILLS:
        fill |= np.all(a == f, axis=2)
    for s in (2, 1):
        h, w = len(SHAPE) * s, len(SHAPE[0]) * s
        if H < h or W < w:
            continue
        ok = np.ones((H - h + 1, W - w + 1), bool)
        for ry, row in enumerate(SHAPE):
            for rx, c in enumerate(row):
                if c == " ":
                    continue
                m = edge if c == "1" else fill
                for dy in range(s):
                    for dx in range(s):
                        y, x = ry * s + dy, rx * s + dx
                        ok &= m[y:y + H - h + 1, x:x + W - w + 1]
                if not ok.any():
                    break
            if not ok.any():
                break
        ys, xs = np.nonzero(ok)
        if ys.size:
            return int(xs[0]), int(ys[0])
    return None


def tip_cell(now, ref, x0, y0, cw, ch, skip_cells=2):
    """The (col, row) the arrow's tip is in, or None.

    now, ref -- PIL images of the same size. x0, y0 -- the grid's top-left
    pixel; cw, ch -- a cell. Anything changed within the first skip_cells
    columns and rows is where the parked pointer was, and is ignored."""
    a = np.asarray(now.convert("RGB"), dtype=np.int16)
    # the arrow's own shape first: a screen that changed under the move
    # (a menu opening as the pointer crossed it) fools the diff
    at = _by_shape(a)
    if at is not None:
        return (int((at[0] - x0) // cw), int((at[1] - y0) // ch))
    b = np.asarray(ref.convert("RGB"), dtype=np.int16)
    diff = np.abs(a - b).sum(axis=2) > 24
    diff[: y0 + skip_cells * ch, : x0 + skip_cells * cw] = False
    ys, xs = np.nonzero(diff)
    if ys.size == 0:
        return None
    # the arrow's tip: the topmost changed row, and on it the leftmost
    top = ys.min()
    band = ys <= top + 1
    x = xs[band].min()
    y = top
    return (int((x - x0) // cw), int((y - y0) // ch))
