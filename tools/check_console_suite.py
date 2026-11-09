#!/usr/bin/env python3
"""check_console_suite.py -- prove tests/test_console.c can fail.

tests/test_console.c passed 124 of 124 on its first run.  By this
tree's own rule that is the least trustworthy result available, and
0.8.7's three new suites all did the same before their harnesses found
sixteen holes between them.

A GEOMETRY ROUTINE IS THE WORST CASE FOR A SUITE THAT AGREES WITH THE
CODE.  Both sides are three divisions, and a fixture written from the
same arithmetic as the driver reproduces the driver's mistakes
perfectly -- which is tests/test_usbmux.c at v0.8.7.6, 79 of 79
against a sixteen-byte header the phone has never used.  So most of
the injections below are not hypothetical: each is either a defect
drivers/vga.c actually had, or the v0.8.8 change undone.

  - "the fit clamps to the TEXT maxima" is the v0.8.8 bug verbatim: a
    framebuffer grid bounded by what a CRTC can be programmed to, on a
    machine with no CRTC.
  - "the scroll stride is VGA_WIDTH" is one of the five call sites
    v0.7.12 fixed, all of them found by reading.
  - "the text-mode table gates the framebuffer" is the ORDER this
    release inverted, and the subtlest one here: everything still
    works at 80x25, which is every screenshot anybody has.
  - "the refusals share one code" is x_control() returning (3) for six
    causes, in a geometry routine.

tools/check_console_grid.py is declared as part of this harness's
coverage: the suite checks the LOGIC and the build check covers the
TEXT -- a constant typed into an index expression -- which is the
pairing tools/check_diskname_sync.py established.

── ONE INJECTION THAT PROVED NOT TO BE A DEFECT ──────────────────────

"the boot grid answers on a machine with no panel" -- removing

    if(!fbc_active) return 0;

from vga_fb_boot_grid(). Not caught, and correctly: the very next
line calls vga_fb_readable_grid(), which starts

    if (!fbc_active || !fbc_w || !fbc_h) return 0;

so the guard is redundant and its removal changes no behaviour.

Recorded here and NOT in the list below. The line stays in the driver
with a note: it states the precondition where the decision is made
rather than two functions away, and the day readable_grid grows an
answer for a machine with no panel it is what stops a CRTC register
table being rewritten at boot with nobody watching.

── AND A GAP IT FOUND IN A CHECK THAT LOOKED FINE ────────────────────

"the boot grid accepts one smaller than 80x25" was MISSED at first.
The suite tested it on a 640x384 panel, where the readable grid is
40x12 -- but 40x12 is also no bigger than the 80x25 console, so the
"only upwards" guard refused it either way and the lower bound was
never exercised. Two guards and a check that could not tell them
apart. It is tested on 800x600 at a 40x12 console now, where 50x18 IS
an increase and only the lower bound can refuse it.

Run:  python3 tools/check_console_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "drivers/vga.c"
HDR  = "include/vga.h"
TEST = "tests/test_console.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "-Wno-int-to-pointer-cast"]

CHECKS = [["python3", "tools/check_console_grid.py"]]

BUGS = [
    ("the fit clamps to the TEXT maxima again",
     "the v0.8.8 bug verbatim. A 1080p panel asked for 240x135 gets "
     "90x60 back, centred, with a quarter of the screen black down "
     "each side -- bounded by what a CRTC can be programmed to, on a "
     "machine that has no text memory at all",
     """    if (g_cols > VGA_COLS_MAX) g_cols = VGA_COLS_MAX;
    if (g_rows > VGA_ROWS_MAX) g_rows = VGA_ROWS_MAX;""",
     """    if (g_cols > VGA_TEXT_COLS_MAX) g_cols = VGA_TEXT_COLS_MAX;
    if (g_rows > VGA_TEXT_ROWS_MAX) g_rows = VGA_TEXT_ROWS_MAX;"""),

    ("the text-mode table gates the framebuffer path again",
     "the ORDER this release inverted, and the subtlest defect in this "
     "list: 80x25 still works, so every screenshot anybody has looks "
     "identical. Only a grid that is not one of the four register sets "
     "is refused, on a console with no registers",
     """int vga_set_mode(int cols, int rows){
    g_mode_why = VGA_MODE_OK;

    if(fbc_active){""",
     """int vga_set_mode(int cols, int rows){
    g_mode_why = VGA_MODE_OK;
    if(cols!=80 && cols!=90){ g_mode_why = VGA_MODE_ETEXT; return 0; }
    if(rows!=25 && rows!=50 && rows!=30 && rows!=60){
        g_mode_why = VGA_MODE_ETEXT; return 0; }

    if(fbc_active){"""),

    ("the scroll reads at the compile-time width",
     "one of the five call sites v0.7.12 fixed, all found by reading "
     "because no suite could compile this file. At 90 columns it "
     "smeared the screen diagonally; at 240 it reads most of the way "
     "through a different row",
     # Re-anchored at v0.8.8.4: vga_scroll() reads through
     # vga_shadow_read() now, so the viewport's clip applies. The
     # defect is the same one -- a compile-time stride -- expressed
     # against the function as it actually stands. NOT deleted: the
     # bug it describes is still available to be written.
     """    int w = vga_cols(), h = vga_rows();
    for(int y=0;y<h-1;y++)
        for(int x=0;x<w;x++)
            vga_write(x,y,vga_shadow_read(x,y+1));""",
     """    int w = vga_cols(), h = vga_rows();
    for(int y=0;y<h-1;y++)
        for(int x=0;x<w;x++)
            vga_write(x,y,shadow[(y+1)*VGA_WIDTH+x]);"""),

    ("the area clamp is gone",
     "the per-axis caps bound 240 and 135 INDIVIDUALLY; the buffers "
     "are bounded by the product. A portrait panel asking for 135x240 "
     "passes both axis checks and writes 32400 cells past the end of "
     "shadow[] -- which is the bug vga_write()'s bounds check was "
     "added for in v0.7.9, one layer up",
     """    while (g_rows > 1 && g_cols * g_rows > VGA_CELLS_MAX) g_rows--;
    while (g_cols > 1 && g_cols * g_rows > VGA_CELLS_MAX) g_cols--;""",
     """    /* area clamp removed */"""),

    ("the budget refusal is reported as a panel problem",
     "two different remedies. 'over the cell budget' means ask for "
     "fewer cells at a larger scale; 'will not fit this panel' means "
     "those pixels do not exist. One code for both is x_control() "
     "returning (3) for six causes, in a geometry routine",
     """        if(cols * rows > VGA_CELLS_MAX){
            g_mode_why = VGA_MODE_EBUDGET; return 0;
        }""",
     """        if(cols * rows > VGA_CELLS_MAX){
            g_mode_why = VGA_MODE_EPANEL; return 0;
        }"""),

    ("the panel check is dropped",
     "a grid bigger than the framebuffer. fbc_fit() shrinks it, so "
     "nothing crashes -- the caller is simply told 1 for a mode it did "
     "not get, which is the whole class of bug fbc_fit()'s header is "
     "about: honouring a request and then reporting a different grid",
     """        if((uint32_t)(cols * FBC_CW) > fbc_w ||
           (uint32_t)(rows * FBC_CH) > fbc_h){
            g_mode_why = VGA_MODE_EPANEL; return 0;
        }""",
     """        /* panel check removed */"""),

    ("best_grid takes the first scale without checking the caps",
     "on a 4K panel that is 480x270 -- 129600 cells into a 32400-cell "
     "buffer. It is also the value `resolution max` would then hand "
     "straight to vga_set_mode(), so the refusal path would be the "
     "only thing standing between this and a 254 KB overrun",
     """        if (c > VGA_COLS_MAX || r > VGA_ROWS_MAX) continue;
        if (c * r > VGA_CELLS_MAX) continue;""",
     """        /* caps not checked */"""),

    ("best_grid fills nothing when it refuses",
     "the defect this function actually shipped with for an hour. The "
     "early return left the caller's variables untouched while the "
     "overflow path at the bottom filled 80x25 -- two meanings for one "
     "refusal, and a caller that ignores the return value then hands "
     "an uninitialised grid to vga_set_mode()",
     """    if (!fbc_active || !fbc_w || !fbc_h) {
        if (cols) *cols = 80;
        if (rows) *rows = 25;
        return 0;
    }""",
     """    if (!fbc_active || !fbc_w || !fbc_h) return 0;"""),

    ("the write bounds check tests the BUFFER, not the grid",
     "239,134 is a valid index into shadow[] and an invalid cell at "
     "80x25. A guard that checks the buffer lets it through, and the "
     "cell holds a character no flush will ever paint -- which is the "
     "v0.7.6 stale-shadow bug with the operands swapped",
     # Re-anchored at v0.8.8.4: the bounds check moved out of
     # vga_write() into vp_xlate(), which is where the viewport
     # translation happens and therefore where the clip has to be.
     # The defect is unchanged -- a guard against the BUFFER rather
     # than the GRID -- and is now written against vp_xlate().
     """    if(*x < 0 || *x >= g_cols || *y < 0 || *y >= g_rows) return 0;
    return 1;
}""",
     """    if((*y)*g_cols+(*x) >= VGA_CELLS_MAX || *x < 0 || *y < 0) return 0;
    return 1;
}"""),

    ("the margin is never painted",
     "nothing else writes outside the grid, so whatever the firmware "
     "left in the framebuffer stays on screen for ever -- a GRUB menu "
     "fragment framing the console",
     """    fbc_fit(80, 25);
    fbc_clear_margin();
    return 1;""",
     """    fbc_fit(80, 25);
    return 1;"""),

    ("the capped report ignores the cell budget",
     "it tests the axis limits and not the area, so a 2560x1440 panel "
     "-- 320x180 at one pixel per pixel, inside both axes and nearly "
     "twice the budget -- reports NOT capped. The `resolution` screen "
     "then prints 160x90 with no explanation, and the one sentence "
     "that tells a user the budget from a driver failure is the "
     "sentence that goes missing",
     """    if (c * r > (uint32_t)VGA_CELLS_MAX) return 1;
    return 0;""",
     """    return 0;"""),

    ("the capped report answers for the last fit instead of the panel",
     "the defect this function actually shipped with. vga_set_mode() "
     "validates range, budget and panel BEFORE calling fbc_fit(), so "
     "no clamp inside the fit can fire -- the flag was always zero and "
     "the screen could never print its one sentence. A flag that is "
     "never set cannot fail to be cleared, which is why the first "
     "version of this harness MISSED it",
     """    if (c > (uint32_t)VGA_COLS_MAX) return 1;
    if (r > (uint32_t)VGA_ROWS_MAX) return 1;
    if (c * r > (uint32_t)VGA_CELLS_MAX) return 1;
    return 0;""",
     """    (void)c; (void)r;
    return 0;"""),

    ("the readable grid ignores the pixel size it was asked for",
     "the whole point of it. It would then be vga_fb_best_grid() under "
     "another name, and `resolution fit` would hand back the 8-pixel "
     "cell that people said was too small to read",
     """    int sx = (min_px + FBC_CW - 1) / FBC_CW;
    int sy = sx;""",
     """    int sx = 1;
    int sy = 1;"""),

    ("the readable grid raises the SCALE when it overflows the budget",
     "a caller asked for cells of a given size, and raising the scale "
     "gives them back a smaller grid with bigger text -- which is "
     "exactly what vga_fb_best_grid() already does. Two functions with "
     "one behaviour, and the one people were told to use is the one "
     "that stopped answering their question",
     """    while (r > 1 && c * r > VGA_CELLS_MAX) r--;
    while (c > 1 && c * r > VGA_CELLS_MAX) c--;

    if (cols) *cols = c;
    if (rows) *rows = r;
    return 1;
}

int vga_fb_best_grid(int *cols, int *rows)""",
     """    while (c * r > VGA_CELLS_MAX) { sx++; sy++;
        c = (int)(fbc_w / (uint32_t)(8 * sx));
        r = (int)(fbc_h / (uint32_t)(8 * sy));
        if (c < 1 || r < 1) return 0; }

    if (cols) *cols = c;
    if (rows) *rows = r;
    return 1;
}

int vga_fb_best_grid(int *cols, int *rows)"""),

    ("the readable grid fills nothing when it refuses",
     "the same two-meanings-for-one-refusal defect vga_fb_best_grid() "
     "shipped with for an hour: a caller that ignores the return value "
     "hands an uninitialised grid to vga_set_mode()",
     """    if (min_px < 8) min_px = 8;
    if (cols) *cols = 80;
    if (rows) *rows = 25;
    if (!fbc_active || !fbc_w || !fbc_h) return 0;""",
     """    if (min_px < 8) min_px = 8;
    if (!fbc_active || !fbc_w || !fbc_h) return 0;"""),

    ("the cursor is not pulled back onto a shrinking grid",
     "the one failure here that would be silent. A cursor past the "
     "last row indexes the live area out of bounds, and the caret "
     "lands in a cell nothing will ever repaint",
     """        for(int i=0;i<g_rows*g_cols;i++) fbc_put(i%g_cols,i/g_cols,blank_fb);
        if(cur_y>=g_rows) cur_y=g_rows-1;
        if(cur_x>=g_cols) cur_x=g_cols-1;
        return 1;""",
     """        for(int i=0;i<g_rows*g_cols;i++) fbc_put(i%g_cols,i/g_cols,blank_fb);
        return 1;"""),

    ("the write-through path skips on the SHADOW, not on the screen",
     "the v0.7.6 defect exactly, one console along. vga_write() stores "
     "into shadow[] and then calls put_cell(), so a dirty check "
     "against the shadow is always true and NOTHING is ever drawn. "
     "(The first version of this injection made the flush repaint "
     "unconditionally, which is slower and perfectly correct -- an "
     "injection that was not a defect, and the harness said so)",
     """        if (fbc_painted[idx] == entry) return;      /* already on screen */""",
     """        if (shadow[idx] == entry) return;           /* already on screen */"""),
    # ══ THE VIEWPORT, v0.8.8.4 ══════════════════════════════════════
    #
    # A coordinate transform in the one function every cell goes
    # through. Every way it can be wrong is a way that an app looks
    # almost right -- which is the worst kind, because "almost right"
    # does not get reported.

    ("the viewport offset is never applied",
     "the whole point. vga_cols() tells the app 80 and the write "
     "lands at the panel's own (0,0), so a centred app draws in the "
     "corner again -- which is the exact symptom v0.7.18 documented "
     "and this release set out to fix. It would look like the "
     "viewport had simply not been turned on",
     """    if(g_vp_w > 0){
        if(*x < 0 || *x >= g_vp_w || *y < 0 || *y >= g_vp_h) return 0;
        *x += g_vp_x;
        *y += g_vp_y;
    }""",
     """    if(g_vp_w > 0){
        if(*x < 0 || *x >= g_vp_w || *y < 0 || *y >= g_vp_h) return 0;
    }"""),

    ("the clip is dropped, so an app can draw into the surround",
     "the clip is load-bearing and not tidiness: this function's own "
     "note records that an out-of-bounds y from a long theme list in "
     "Settings wrote past shadow[] into kernel heap and stack and "
     "hung the machine. Inside a viewport the same write now lands "
     "in whatever the desktop drew around the app",
     """        if(*x < 0 || *x >= g_vp_w || *y < 0 || *y >= g_vp_h) return 0;
        *x += g_vp_x;""",
     """        *x += g_vp_x;"""),

    ("the accessors report the panel instead of the viewport",
     "an app told it has 90 columns inside an 80-column box draws its "
     "right-hand ten columns into the clip and never finds out. "
     "Everything looks fine except that text is missing down one "
     "edge -- and only on grids bigger than the app",
     """int  vga_rows(void){ return (g_vp_h > 0) ? g_vp_h : g_rows; }
int  vga_cols(void){ return (g_vp_w > 0) ? g_vp_w : g_cols; }""",
     """int  vga_rows(void){ return g_rows; }
int  vga_cols(void){ return g_cols; }"""),

    ("a viewport too big for the panel is clamped instead of refused",
     "the caller gets a console smaller than it asked for with no way "
     "to learn that, because the clip absorbs the difference "
     "silently. Refusing hands back 0 and the app runs full-screen, "
     "which is what happened before the viewport existed and is "
     "always correct",
     """    if(w > g_cols || h > g_rows){ vga_viewport_clear(); return 0; }""",
     """    if(w > g_cols) w = g_cols;
    if(h > g_rows) h = g_rows;"""),

    ("vga_clear() clears the panel instead of the viewport",
     "an app clearing its own console wipes the surround the desktop "
     "drew, so a centred app reads as a screen that failed to finish "
     "drawing rather than one with an app framed in it",
     """    if(g_vp_w > 0){
        for(int y=0;y<g_vp_h;y++)
            for(int x=0;x<g_vp_w;x++) vga_write(x,y,blank);
    } else {
        for(int i=0;i<g_rows*g_cols;i++) shadow[i]=blank;
    }""",
     """    for(int i=0;i<g_rows*g_cols;i++) shadow[i]=blank;"""),

    ("the scroll reads shadow[] directly instead of through the clip",
     "a g_cols stride inside a viewport pulls cells in from the "
     "surround and smears it into the app on every scrolled line. "
     "Same shape as the v0.7.12 bug this function already carries a "
     "note about, where the stride was a compile-time 80 and "
     "scrolling a 90-column mode smeared the screen diagonally",
     """            vga_write(x,y,vga_shadow_read(x,y+1));""",
     """            vga_write(x,y,shadow[(y+1)*g_cols+x]);"""),

    ("the hardware cursor is placed in app coordinates",
     "the caret of an app in a centred viewport blinks in the "
     "top-left of the panel while its text appears in the middle. The "
     "text is right, so it reads as a cursor bug rather than as a "
     "missing translation",
     """    int cpx=cur_x, cpy=cur_y;
    uint16_t pos;
    if(!vp_xlate(&cpx,&cpy)){ cpx=0; cpy=0; }""",
     """    int cpx=cur_x, cpy=cur_y;
    uint16_t pos;"""),

    ("clearing the viewport leaves its width set",
     "vga_viewport_clear() is what hands the screen back. Leaving "
     "g_vp_w set means the desktop itself then draws inside the last "
     "app's box: the taskbar lands in the middle of the screen and "
     "everything outside it is frozen at whatever was there",
     """void vga_viewport_clear(void){ g_vp_x = g_vp_y = g_vp_w = g_vp_h = 0; }""",
     """void vga_viewport_clear(void){ g_vp_x = g_vp_y = g_vp_h = 0; }"""),
    # ══ THE BOOT GRID, v0.8.8.5 ═════════════════════════════════════

    # ══ THE APP GRID, v0.8.8.6 ══════════════════════════════════════
    #
    # Found on the HP: a 1024x768 panel, a readable cell of 64x24, and
    # `layout` reporting the Dashboard 3855 cells outside its console.

    ("the app grid accepts one smaller than 80x25",
     "THE DEFECT THE HP SHIPPED WITH. Every app lays out for 80x25, "
     "and a console smaller than that cannot be rescued by the "
     "viewport -- there is nowhere to centre an 80x25 box inside a "
     "64x24 screen. The apps draw off the edge with only the grid to "
     "clip them, which is exactly what `layout` measured on his "
     "machine",
     """    if(vga_fb_readable_grid(VGA_READABLE_PX, &c, &r) && c >= 80 && r >= 25){""",
     """    if(vga_fb_readable_grid(VGA_READABLE_PX, &c, &r)){"""),

    ("the app grid never falls back to a smaller cell",
     "this is what v0.8.8.5 did, and it is why the HP had a 64x24 "
     "console. On a 1024x768 panel the readable cell gives 64x24 and "
     "nothing else is tried, so the policy either offers a console no "
     "app fits in or refuses a panel that can perfectly well show "
     "128x48",
     """    if(vga_fb_readable_grid(8, &c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }

    return 0;""",
     """    return 0;"""),

    ("the fallback cell is used even when the readable one fits",
     "every panel's text becomes half the size it should be. 1920x1080 "
     "goes from 120x33 with a 16-pixel cell to 240x67 with an 8-pixel "
     "one -- which is `resolution max`, the setting people complained "
     "about, arrived at by accident",
     """    if(vga_fb_readable_grid(VGA_READABLE_PX, &c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }""",
     """    if(0){ }"""),

    ("the fallback has no floor of its own",
     "640x384 is 80 columns at the 8-pixel cell and only 24 rows -- "
     "one short, with no smaller cell to try. Accepting it hands back "
     "a console an app still does not fit in, having gone to the "
     "trouble of looking for one that would",
     """    if(vga_fb_readable_grid(8, &c, &r) && c >= 80 && r >= 25){""",
     """    if(vga_fb_readable_grid(8, &c, &r)){"""),

    ("the boot grid changes mode when there is nothing to change",
     "on a 1280x800 panel the readable grid IS 80x25, so a machine "
     "already showing it would switch to the same numbers on every "
     "boot -- a flicker, and a log line reporting that nothing "
     "happened. It is also the panel QEMU hands this project, so the "
     "noise would be in every single test run",
     """    if(c <= g_cols && r <= g_rows) return 0;   /* only upwards */""",
     """    if(c == 0 || r == 0) return 0;"""),

    # Re-anchored at v0.8.8.6: the readable-cell call moved out of
    # vga_fb_boot_grid() into vga_fb_app_grid(), where the policy now
    # lives. The defect is unchanged -- picking the biggest grid
    # rather than the readable one -- and is written against the
    # function that makes that choice today. NOT deleted: `resolution
    # max` is still a thing somebody could wire in here by mistake.
    ("the app grid picks the biggest instead of the readable one",
     "vga_fb_best_grid() is the 8-pixel cell people complained about. "
     "On a 1920x1080 panel it is 240x67 and the glyphs are half the "
     "size they were, which is a worse answer than the letterboxed "
     "80x25 it replaced",
     """    if(vga_fb_readable_grid(VGA_READABLE_PX, &c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }

    /* Otherwise the smallest cell this console draws, which is the""",
     """    if(vga_fb_best_grid(&c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }

    /* Otherwise the smallest cell this console draws, which is the"""),

]

# Must NOT be caught. A comment reworded: no behaviour changes, so a
# catch here means the loop is restoring badly or the suite is
# asserting something that is not behaviour. See tools/inject.py.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """    if (c > (uint32_t)VGA_COLS_MAX) return 1;""",
    """    /* the axis limits, for a panel bigger than the buffers */
    if (c > (uint32_t)VGA_COLS_MAX) return 1;""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "console", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        files=(SRC, HDR, TEST), checks=CHECKS,
        preamble="Proving tests/test_console.c can fail.\n"
                 "It passed 124 of 124 first time, and a geometry suite "
                 "written from the same three divisions as the driver "
                 "agrees with it perfectly."))
