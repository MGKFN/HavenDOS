#!/usr/bin/env python3
"""check_console_grid.py -- the console's stride is the GRID, not a constant.

── THE BUG CLASS THIS REFUSES ─────────────────────────────────────────

v0.7.11 made the console's column count dynamic.  v0.7.12 then fixed
FIVE call sites in drivers/vga.c that were still pinned to the
compile-time VGA_WIDTH:

    vga_scroll()   read (y+1)*VGA_WIDTH+x from a 90-column shadow
                   buffer, i.e. ten cells before the start of the
                   source row -- so scrolling smeared the screen
                   diagonally
    vga_putchar()  and vga_puts_at(), the same arithmetic

All five were found BY READING, and that is the point of this check.
The suite that could have found them did not exist until v0.8.8,
because drivers/vga.c had the text window hardcoded at 0xB8000 and a
native suite that exercised the text path died writing to it.  So for
two releases the only thing standing between this file and a stride bug
was somebody noticing.

── AND v0.8.8 MADE IT SIX TIMES MORE LIKELY TO BITE ───────────────────

The grid was capped at 90x60 because that is the largest a VGA CRTC can
be programmed to.  On a framebuffer there is no CRTC, so the cap is now
240x135 -- 32400 cells rather than 5400.  A stride error of ten cells
in a 90-column mode smears the screen; a stride error of 160 cells in a
240-column mode indexes most of the way through a different row, and at
the margins it leaves the visible area entirely.

── WHAT IS REFUSED ────────────────────────────────────────────────────

In drivers/vga.c only, and in code rather than comments:

  1. VGA_WIDTH, VGA_HEIGHT, VGA_COLS_MAX or VGA_ROWS_MAX used as a
     STRIDE -- that is, multiplied by something, or appearing in an
     index expression next to a `+`.  The stride is g_cols, full stop.

  2. A cell-buffer declaration whose size is not VGA_CELLS_MAX.  The
     grid is bounded by AREA and the axes only bound how far an index
     may reach, so a buffer sized VGA_COLS_MAX*VGA_ROWS_MAX would be
     almost twice the budget -- 230 KB of BSS for a grid nothing can
     ask for.

  3. A literal 80, 25, 90 or 60 used as a loop bound over cells.  Each
     is a real number in this file -- they are in the CRTC register
     tables and in the four accepted text modes -- so only the LOOP
     form is refused, which is the form that iterates a grid.

A line that genuinely needs one spells  grid-ok  in a comment on it,
through csource.has_reason(): a bare marker is not enough, the same rule
check_bus_label.py and check_blk_dispatch.py follow.

    python3 tools/check_console_grid.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

TARGET = "drivers/vga.c"
HEADER = "include/vga.h"
OPT_OUT = "grid-ok"

# VGA_WIDTH * something, or something * VGA_WIDTH: a stride.
STRIDE = re.compile(
    r"(?:\bVGA_(?:WIDTH|HEIGHT|COLS_MAX|ROWS_MAX)\s*\*"
    r"|\*\s*VGA_(?:WIDTH|HEIGHT|COLS_MAX|ROWS_MAX)\b)")

# A loop bound that is a bare grid literal: for(...; i < 80; ...)
LOOP_LITERAL = re.compile(r"\bfor\s*\([^;]*;[^;<]*<\s*(80|25|90|60)\s*[;)]")

# uint16_t name[...] -- the cell buffers.
CELL_BUF = re.compile(r"\buint16_t\s+(\w+)\s*\[\s*([^\]]+)\]")
CELL_BUF_NAMES = ("shadow", "fbc_painted")


def window(lines, n):
    """The marker line and the two above it, as the other checks do."""
    lo = max(0, n - 3)
    return "\n".join(lines[lo:n])


def scan(root="."):
    bad = []
    path = os.path.join(root, TARGET)
    if not os.path.exists(path):
        return [(TARGET, 0, "the file is not there", "")]
    text = open(path, encoding="utf-8", errors="replace").read()
    raw = text.splitlines()
    code = csource.blank(text).splitlines()

    for n, line in enumerate(code, 1):
        src = raw[n - 1]
        if csource.has_reason(window(raw, n), OPT_OUT):
            continue

        m = STRIDE.search(line)
        if m:
            # The ONE legitimate multiplication is the buffer size
            # itself, and v0.8.8 replaced those with VGA_CELLS_MAX, so
            # there is nothing left to exempt.
            bad.append((TARGET, n, "a compile-time constant used as a "
                        "stride -- the stride is g_cols", src.strip()))

        m = LOOP_LITERAL.search(line)
        if m:
            bad.append((TARGET, n, "a loop bounded by the literal %s -- "
                        "use vga_cols()/vga_rows() or VGA_CELLS_MAX"
                        % m.group(1), src.strip()))

        for m in CELL_BUF.finditer(line):
            name, size = m.group(1), m.group(2).strip()
            if name not in CELL_BUF_NAMES:
                continue
            if size != "VGA_CELLS_MAX":
                bad.append((TARGET, n, "%s[] is sized %s, not "
                            "VGA_CELLS_MAX" % (name, size), src.strip()))
    return bad


def check_header(root="."):
    """The two maxima have to BE two numbers.

    They were one -- VGA_COLS_MAX was the CRTC's 90 and the framebuffer
    was clamped to it, which is the whole of what 0.8.8 fixed.  If a
    later edit makes the buffer cap equal the text cap again, the
    framebuffer console is silently back where it started and every
    check above still passes.
    """
    bad = []
    path = os.path.join(root, HEADER)
    text = open(path, encoding="utf-8", errors="replace").read()
    vals = {}
    for name in ("VGA_TEXT_COLS_MAX", "VGA_TEXT_ROWS_MAX",
                 "VGA_COLS_MAX", "VGA_ROWS_MAX"):
        m = re.search(r"^#define\s+" + name + r"\s+(\d+)", text, re.M)
        if not m:
            bad.append((HEADER, 0, "%s is not defined" % name, ""))
            continue
        vals[name] = int(m.group(1))
    if len(vals) == 4:
        if vals["VGA_COLS_MAX"] <= vals["VGA_TEXT_COLS_MAX"]:
            bad.append((HEADER, 0, "VGA_COLS_MAX (%d) is not larger than "
                        "the TEXT cap (%d) -- the framebuffer console is "
                        "clamped by CRTC arithmetic again"
                        % (vals["VGA_COLS_MAX"], vals["VGA_TEXT_COLS_MAX"]),
                        ""))
        if vals["VGA_ROWS_MAX"] <= vals["VGA_TEXT_ROWS_MAX"]:
            bad.append((HEADER, 0, "VGA_ROWS_MAX (%d) is not larger than "
                        "the TEXT cap (%d)"
                        % (vals["VGA_ROWS_MAX"], vals["VGA_TEXT_ROWS_MAX"]),
                        ""))
    # ── AND THE BUDGET MUST BE SMALLER THAN THE PRODUCT ──────────────
    #
    # Not an aesthetic preference.  While VGA_CELLS_MAX equalled
    # VGA_COLS_MAX * VGA_ROWS_MAX, no grid could be inside both axis
    # limits and over the budget -- so VGA_MODE_EBUDGET was unreachable
    # and the area clamp in fbc_fit() was dead.  A suite covered both
    # and could not have noticed either being deleted;
    # tools/check_console_suite.py is what found that, by injecting into
    # the distinction and watching 124 checks shrug.
    #
    # So the two limits have to stay genuinely different, and this is
    # the line that keeps them so.
    m = re.search(r"^#define\s+VGA_CELLS_MAX\s+(.+?)\s*$", text, re.M)
    if not m:
        bad.append((HEADER, 0, "VGA_CELLS_MAX is not defined", ""))
    elif len(vals) == 4:
        expr = m.group(1).split("/*")[0].strip()
        if not re.fullmatch(r"[0-9()\*\+\s]+", expr):
            bad.append((HEADER, 0, "VGA_CELLS_MAX is not plain arithmetic: "
                        "%s" % expr, ""))
        else:
            cells = int(eval(expr))          # noqa: S307 -- guarded above
            product = vals["VGA_COLS_MAX"] * vals["VGA_ROWS_MAX"]
            if cells >= product:
                bad.append((HEADER, 0, "VGA_CELLS_MAX (%d) is not smaller "
                            "than VGA_COLS_MAX * VGA_ROWS_MAX (%d) -- the "
                            "area limit and the axis limits have collapsed "
                            "into one, and EBUDGET is unreachable again"
                            % (cells, product), ""))
            if cells < 80 * 25:
                bad.append((HEADER, 0, "VGA_CELLS_MAX (%d) cannot hold the "
                            "stock 80x25 grid" % cells, ""))
    return bad


def main():
    bad = scan() + check_header()
    if not bad:
        print("check_console_grid: ok, %s indexes by the grid and the two "
              "maxima are separate numbers" % TARGET)
        return 0
    print("check_console_grid: FAILED\n")
    for f, n, why, src in bad:
        print("  %s:%d  %s" % (f, n, why))
        if src:
            print("      %s" % src[:100])
    print("""
The shadow buffer's stride is vga_cols(), which changes at runtime.  A
compile-time constant in an index expression was the v0.7.12 bug in five
places, and at 240 columns it reaches further than it did at 90.

If a line genuinely needs a constant, mark it  grid-ok: <reason>""")
    return 1


if __name__ == "__main__":
    sys.exit(main())
