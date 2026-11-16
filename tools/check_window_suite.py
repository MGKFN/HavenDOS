#!/usr/bin/env python3
"""check_window_suite.py -- prove tests/test_window.c can fail.

v0.9.6.5 makes every app a window (shell/chrome.c): a title bar with
minimize, full screen and close, and the taskbar on the last row. The
layout decides where the app's box goes and what a click hits; a wrong
layout overlaps the app with its own chrome, or turns a click on X into
a click on full screen.

Run:  python3 tools/check_window_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "shell/chrome.c"
TEST = "tests/test_window.c"
CC = ["gcc", "-g", "-Iinclude", "-Itests", "-Wno-builtin-declaration-mismatch",
      "-fsanitize=address,undefined"]

BUGS = [
    ("an any-size app starts on row 0, under its own title bar",
     "Settings' first line is drawn over by the title bar",
     """            L->vx = 0; L->vy = 1; L->vw = pw; L->vh = ph - 2;""",
     """            L->vx = 0; L->vy = 0; L->vw = pw; L->vh = ph - 2;"""),

    ("the app's box runs into the taskbar",
     "the app's last line is drawn over by the taskbar",
     """            L->vx = 0; L->vy = 1; L->vw = pw; L->vh = ph - 2;""",
     """            L->vx = 0; L->vy = 1; L->vw = pw; L->vh = ph - 1;"""),

    ("a window on a grid too small for one",
     "an 80x25 screen gives an app 23 rows; the apps lay out for 25",
     """        if(!full && pw >= 80 && ph - 2 >= 25){""",
     """        if(!full && pw >= 80){"""),

    ("full screen keeps the taskbar",
     "the button does nothing a person can see",
     """        if(!full && pw >= 80 && ph - 2 >= 25){""",
     """        if(pw >= 80 && ph - 2 >= 25){"""),

    ("a boxed app's title row is inside its box",
     "the title bar covers the app's first line",
     """        L->title_y = top; L->vy = top + 1; L->bar_y = ph - 1;""",
     """        L->title_y = top; L->vy = top; L->bar_y = ph - 1;"""),

    ("the buttons swapped: X is full screen",
     "a person reaching for X gets a full-screen app",
     """                return i == 0 ? CH_MIN : i == 1 ? CH_FULL : CH_CLOSE;""",
     """                return i == 0 ? CH_MIN : i == 1 ? CH_CLOSE : CH_FULL;"""),

    ("the buttons one cell further right",
     "X's last cell is the frame; a click on the title's end closes",
     """    return L->title_x1 - (3 - which) * (BTN_W + 1) + 1;""",
     """    return L->title_x1 - (3 - which) * (BTN_W + 1) + 2;"""),

    ("chips counted from the Start button",
     "a click on the first app's chip opens the second",
     """            if(chip) *chip = (x - CHIP_X0) / CHIP_W;""",
     """            if(chip) *chip = x / CHIP_W;"""),

    ("the taskbar's clock is a chip",
     "a click on the clock opens whatever app is listed last",
     """        if(x >= CHIP_X0 && x < pw - 7){""",
     """        if(x >= CHIP_X0){"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the layout changed",
    """/* ── the layout, pure ────────────────────────────────────────────────── */""",
    """/* ── the layout, pure, and tested ─────────────────────────────────────── */""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "window", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_window.c can fail."))
