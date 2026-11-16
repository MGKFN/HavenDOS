#!/usr/bin/env python3
"""check_uimouse_suite.py -- prove tests/test_uimouse.c can fail.

v0.9.6.3 gives every app the mouse by turning clicks, the wheel and the
right button into the keys the app already reads (shell/uimouse.c).
What can go wrong is a click that sends the WRONG keys: a row counted
from the wrong place moves the selection somewhere else and the next
click activates it; a hint read too generously turns ordinary text into
an Enter; a pointer restored over an app's own drawing leaves a stale
cell; a task behind writing into the front's table.

Run:  python3 tools/check_uimouse_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "shell/uimouse.c"
TEST = "tests/test_uimouse.c"
CC = ["gcc", "-g", "-Iinclude", "-Itests", "-Wno-builtin-declaration-mismatch",
      "-fsanitize=address,undefined"]

BUGS = [
    ("rows counted from the top, not from the selected row",
     "every click moves the selection by the row's number, not the gap",
     """                for(k = s; k < d && n < max; k++) out[n++] = KEY_DOWN;""",
     """                for(k = 0; k < d && n < max; k++) out[n++] = KEY_DOWN;"""),

    ("a click on any row is Enter",
     "one click opens whatever is selected -- not what was clicked",
     """                if(d == s && !np){ out[n++] = '\\n'; return n; }""",
     """                if(!np){ out[n++] = '\\n'; return n; }"""),

    ("the prefix is dropped",
     "Settings with its focus on the sidebar: the arrows move the SECTION",
     """                for(k = 0; k < np && n < max; k++) out[n++] = t->pre[g][k];""",
     """                (void)np;"""),

    ("a key needs no words after it",
     "any word on the bottom rows that names a key is pressed when clicked",
     """    if(i > b) return 0;
    if(!((line[i] >= 'a'""",
     """    if(i > b) return first ? first : tok_key(t, n);
    if(!((line[i] >= 'a'"""),

    ("Up/Down hints send Up",
     "an Up/Down choose hint clicked moves the selection",
     """            else if(opposite(first, key)) return 0;""",
     """            else (void)opposite;"""),

    ("hints are read on every row",
     "a paragraph saying Enter your name is a button",
     """    if((t->mode & UM_HINTS) && line && y >= vga_rows() - 2){""",
     """    if((t->mode & UM_HINTS) && line){"""),

    ("the pointer is restored whatever the cell holds now",
     "an app that drew over the pointer gets the old cell back",
     """    if(vga_shadow_read(p_x, p_y) == p_drawn) vga_shadow_write(p_x, p_y, p_saved);""",
     """    vga_shadow_write(p_x, p_y, p_saved);"""),

    ("a real key leaves the pointer drawn",
     "the app redraws around a cyan cell and the restore later undoes it",
     """    if(key != -1){ pointer_hide(); return key; }""",
     """    if(key != -1){ return key; }"""),

    ("the clicks are read whatever the mode",
     "the desktop's own clicks are eaten before it sees them",
     """    clicks = (t->mode & (UM_ZONES | UM_HINTS | UM_RIGHT)) ? mouse_get_clicks() : 0;""",
     """    clicks = mouse_get_clicks();"""),

    ("the wheel is read even when the mode says no",
     "a game's arrows driven by a brushed trackpad",
     """    if(t->mode & UM_WHEEL){""",
     """    if(1){"""),

    ("the wheel is not bounded",
     "a spun wheel scrolls a list for seconds after it stops",
     """        if(w > 5) w = 5;""",
     """        if(w > 50) w = 50;"""),

    ("one table for every task",
     "a clock redrawing behind the front app wipes the front's rows",
     """    int s = g_slot_fn ? g_slot_fn() : 0;""",
     """    int s = 0;"""),

    ("a reused slot keeps the old task's table",
     "a new app inherits the rows and mode of the one that closed",
     """    if(g_s[s].id != id){""",
     """    if(0){"""),

    ("the clear hook forgets nothing",
     "rows of the screen before stay clickable on the screen after",
     """static void on_clear(void){ um_frame(); p_x = p_y = -1; }""",
     """static void on_clear(void){ p_x = p_y = -1; }"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the layer changed",
    """/* ── the pointer ───────────────────────────────────────────────────── */""",
    """/* ── the pointer, drawn and hidden ─────────────────────────────────── */""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "uimouse", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_uimouse.c can fail."))
