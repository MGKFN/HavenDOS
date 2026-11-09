#pragma once
#include "types.h"

/* ── v0.8.8 "Helios": TWO maxima, because they are two different things ──
 *
 * These were one number, and the number was the VGA text hardware's.
 * VGA_COLS_MAX was 90 and VGA_ROWS_MAX 60 because 720x480 with an 8-dot
 * character clock and an 8-line cell is the largest grid a CRTC can be
 * programmed to, and for a long time that was the only console there was.
 *
 * A FRAMEBUFFER has no CRTC character clock and no font plane.  A cell is
 * however many pixels the renderer draws, so the grid a panel can hold is
 * its pixels divided by the glyph -- 1920x1080 with an unscaled 8x8 font
 * is 240x135 cells, nearly seven times the area the text cap allows.
 *
 * fbc_fit() nevertheless clamped the framebuffer grid to the TEXT maxima,
 * and vga_set_mode() refused anything that was not one of the four text
 * modes BEFORE it ever looked at which console was active.  So on a
 * machine with no VGA text memory at all, the thing that decided how much
 * of the screen HavenDOS could use was a register table that machine
 * cannot execute.  That is the whole of "the console cannot express a
 * 1080p grid": not a missing feature, a cap imported from the wrong
 * hardware.
 *
 *   VGA_TEXT_COLS_MAX / VGA_TEXT_ROWS_MAX   what a CRTC can be told
 *   VGA_COLS_MAX      / VGA_ROWS_MAX        what the buffers can hold
 *
 * WHY THE BUDGET IS 1080p AND NOT THE BIGGEST PANEL MADE.  Two cell
 * buffers are allocated at the budget -- shadow[] and fbc_painted[] -- so
 * the budget is BSS, 2 bytes a cell each.  32400 cells costs 127 KB for
 * the pair.  4K at scale 1 would be 480x270 -- 129600 cells, 506 KB --
 * for glyphs 8 physical pixels tall on a 32-inch panel, which nobody can
 * read.  A larger panel gets the same number of cells at a larger SCALE
 * instead, which is what vga_fb_best_grid() computes: 2560x1440 and
 * 3840x2160 both divide exactly by 16 into 160x90 and 240x135, so
 * neither letterboxes.
 *
 * ── AND THE AREA IS A SEPARATE LIMIT FROM THE AXES, ON PURPOSE ────────
 *
 * VGA_CELLS_MAX is deliberately SMALLER than VGA_COLS_MAX * VGA_ROWS_MAX,
 * and that is not an oversight.  The buffers are bounded by AREA; the
 * axes are bounded only by how far an index may reach.  Keeping them
 * separate buys two things:
 *
 *   - a grid that is wide and short, or narrow and tall, is allowed.
 *     320x100 is 32000 cells and fits; a 2560x1080 ultrawide can have
 *     it, and a panel rotated into portrait can have 120x180.
 *   - "over the cell budget" and "outside the grid limits" become two
 *     REACHABLE refusals with two different remedies.  While the budget
 *     was exactly the product of the axes, no grid could be inside both
 *     axes and over the budget, so VGA_MODE_EBUDGET was dead code and
 *     the suite that "covered" it could not reach it.  That was found by
 *     tools/check_console_suite.py, which injected a defect into the
 *     distinction and watched the suite not notice.
 */
#define VGA_WIDTH  80           /* the stock text grid, not a maximum */
#define VGA_HEIGHT 25           /* v0.8.8: was 60, i.e. was a maximum */
#define VGA_TEXT_COLS_MAX 90    /* 720 pixels / 8-dot characters */
#define VGA_TEXT_ROWS_MAX 60    /* 480 scanlines / 8-line cells   */
#define VGA_COLS_MAX 320        /* how far a column index may reach */
#define VGA_ROWS_MAX 180        /* how far a row index may reach      */
/* The buffers' real bound, and SMALLER than the product above --
   see the note on the two limits. 240x135 is 1920x1080 at one
   pixel per pixel, and it is the shape that costs the most cells
   of any grid a panel in this century will ask for. */
#define VGA_CELLS_MAX (240 * 135)

typedef enum {
    VGA_BLACK=0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
    VGA_YELLOW, VGA_WHITE
} vga_color_t;
/* ── the framebuffer console ───────────────────────────────────────────
   Call vga_fb_init() with a linear 32bpp framebuffer BEFORE vga_init() and
   every character the OS draws is rendered as pixels instead of being poked
   into VGA text memory.  Nothing above this driver changes: the desktop,
   the apps, the shell and the boot log all come along, because they all go
   through the same shadow buffer either way.  Returns 1 if it took. */
int      vga_fb_init(uint32_t addr, uint32_t w, uint32_t h,
                     uint32_t pitch_bytes, uint32_t bpp);
int      vga_fb_active(void);

/* ── v0.8.8: the grid the panel can actually hold ───────────────────────
 * The largest grid that FILLS this framebuffer without exceeding
 * VGA_CELLS_MAX, and the integer glyph scale it implies.  It is the
 * smallest scale s for which fbw/(8s) and fbh/(8s) are both inside the
 * cap, so a bigger panel gets bigger GLYPHS rather than a letterboxed
 * grid: 1920x1080 -> 240x135 at 1x, 2560x1440 -> 160x90 at 2x,
 * 3840x2160 -> 240x135 at 2x.  Returns 1 on success; on EVERY refusal it
 * returns 0 AND reports the stock 80x25, so a caller that ignores the
 * return value does not hand an uninitialised grid to vga_set_mode(). */
int      vga_fb_best_grid(int *cols, int *rows);

/* ── v0.8.8.1: the largest grid that is still READABLE ───────────────
 * The most cells whose glyph is at least `min_px` physical pixels in
 * each axis. VGA_READABLE_PX is the default and is the size a console
 * font is drawn at on every other system.
 *
 * `resolution max` gives the most cells there are; on 1080p that is a
 * cell 8 pixels square, which is too small to read. This is the one to
 * recommend. It does NOT substitute for a real 8x16 glyph table --
 * see the note in drivers/vga.c. */
#define VGA_READABLE_PX 16
int      vga_fb_readable_grid(int min_px, int *cols, int *rows);
/* 1 if THIS PANEL is larger than the console can express at one pixel
   per pixel -- that is, if its pixels divided by the glyph exceed an
   axis limit or the cell budget.  A screen that reports the grid has to
   be able to say this, or a user on a 4K panel reads 240x135 as a
   driver failure rather than as the budget.

   v0.8.8, SECOND VERSION.  The first asked "was the LAST FIT clamped",
   which was dead: vga_set_mode() validates the range, the budget and
   the panel BEFORE calling fbc_fit(), so by the time the fit runs no
   clamp inside it can fire.  The flag was therefore always zero and the
   `resolution` screen could never print the one sentence it existed
   for.  Found by tools/check_console_suite.py: the injection that
   stopped the flag being cleared was MISSED, because a flag that is
   never set cannot fail to be cleared. */
int      vga_grid_capped(void);
uint32_t vga_fb_width(void);
uint32_t vga_fb_height(void);

void vga_init(void);
void vga_clear(void);
void vga_flush(void);
/* Commit a pending frame before stopping to wait; no-op inside a frame. */
void vga_present(void);
/* ── the LOGICAL grid ────────────────────────────────────────────────
   What the code drawing right now has to work with. While a viewport
   is set (below) these report ITS size, not the panel's, so an app
   laid out for 80x25 is told it has 80x25. */
int  vga_rows(void);          /* v0.7.8: active row count (25 or 50) */
int  vga_cols(void);

/* ── the PHYSICAL grid ───────────────────────────────────────────────
   The real console. Needed only by code that owns the whole screen --
   the desktop deciding where to centre an app, a diagnostic reporting
   the mode. Drawing code wants vga_rows()/vga_cols(); using these to
   place text is how something ends up drawing outside its own box. */
int  vga_phys_rows(void);
int  vga_phys_cols(void);

/* ── THE VIEWPORT ────────────────────────────────────────────────────

   A coordinate transform with a clip, so that an app which lays out
   for 80x25 lands centred on a 160x50 panel instead of in its corner.

   v0.7.18 made the desktop's chrome derive from the real grid and its
   header recorded what it could not reach: "Apps launched FROM the
   desktop still lay themselves out for 80x25 and render in the
   top-left of a larger screen -- converting those is a separate pass
   and is not claimed here." There are forty-three of them, placing
   text at absolute columns and rows. This is that pass, done once in
   the driver rather than forty-three times by hand.

   While a viewport is set:
     - vga_rows()/vga_cols() report the viewport's size,
     - every coordinate passed to a drawing call is relative to it,
     - anything outside it is clipped and cannot be drawn,
     - vga_clear() clears the viewport and not the screen.

   IT IS NOT A WINDOW SYSTEM. One viewport, no nesting, no stacking:
   one app runs at a time, so there is nothing to clip against.

   vga_viewport_centre() returns 0 and sets nothing if the box will not
   fit the panel. It refuses rather than clamping, because an app told
   it has 80 columns inside a 70-column box would draw its right-hand
   edge into the clip and never find out. A caller that gets 0 should
   run the app full-screen, which is what happened before this
   existed. */
/* ── WHAT TRIED TO DRAW OUTSIDE, AND HOW FAR ─────────────────────────
   Every write that lands outside the grid, or outside the viewport an
   app has been given, is clipped by vga_write() -- and counted here
   with the worst coordinate it saw.

   This exists because "some apps go off the screen" is a report about
   forty-three apps, and guessing which three is how a release fixes
   the wrong ones. shell/desktop.c resets it before launching an app
   and logs it afterwards, so `dmesg` names the app and the overflow.

   maxx/maxy are -1 when nothing was clipped. They are the worst
   coordinate SEEN, not a bounding box: an app that writes one
   character at row 40 and a thousand at row 25 reports 40, which is
   what you want to know first. */
void vga_clip_reset(void);
void vga_clip_stats(uint32_t *n, int *maxx, int *maxy);

/* ── the grid a machine should come up with ───────────────────────────
   1 and fills cols/rows when the panel can do better than the grid it
   is showing; 0 when it cannot, and then nothing should change.

   Never returns a grid smaller than 80x25, and never one no bigger
   than the current. drivers/vga.c has both reasons at length; the
   short form is that a console smaller than 80x25 boxes nothing, so
   every app would draw off the edge of it -- which is the complaint
   this exists to fix, backwards.

   A function rather than an `if` in the boot path because a boot-path
   `if` can only be tested by booting, on a panel of the right size,
   and the panel QEMU hands this project is the one size where the
   answer is "no change". */
/* ── a grid the apps can actually use ────────────────────────────────
   A readable cell if the grid is still at least 80x25, and a smaller
   cell if that is what it takes to get there. 0 if the panel cannot
   show 80x25 at any cell size this console draws.

   THE 80x25 FLOOR IS NOT A PREFERENCE. Every app in the registry lays
   out for it, and a console smaller than that cannot be rescued by
   the viewport: there is nowhere to centre an 80x25 box inside a
   64x24 screen. Found on a 1024x768 panel, where the readable cell
   gives exactly 64x24 and `resolution fit` offered it. */
int  vga_fb_app_grid(int *cols, int *rows);

int  vga_fb_boot_grid(int *cols, int *rows);

int  vga_viewport_centre(int w, int h);
void vga_viewport_clear(void);
int  vga_viewport_active(void);
void vga_viewport_get(int *x, int *y, int *w, int *h);
int  vga_set_rows(int rows);
/* On VGA text memory: one of 80x25, 80x50, 90x30, 90x60 and nothing else,
   because each is a complete CRTC/sequencer/attribute register set.  On a
   FRAMEBUFFER: any grid inside the caps that fits the panel, because there
   are no registers involved -- only the glyph scale and the margin.
   Returns 1 if the mode was taken, 0 if it was refused.

   ── v0.8.8: WHY THE REFUSAL HAS A NAME NOW ──────────────────────────
   This returned 0 for four unrelated causes and the shell printed one
   sentence naming the four text modes for all of them.  On a 1080p panel
   `resolution 300x40` and `resolution 200x200` are refused for completely
   different reasons -- one is over the column cap, the other is over the
   cell budget -- and both were answered with advice about 90x60, a mode
   that machine cannot run at all.  That is x_control() returning (3) for
   six causes, in a screen instead of a driver.  vga_mode_why() is read
   AFTER a refusal; it is not an error return, so the three existing
   callers of this function are unchanged. */
int  vga_set_mode(int cols, int rows);
#define VGA_MODE_OK      0
#define VGA_MODE_ETEXT   1   /* text console: not one of the four modes  */
#define VGA_MODE_ERANGE  2   /* outside 1..VGA_COLS_MAX/VGA_ROWS_MAX     */
#define VGA_MODE_EBUDGET 3   /* cols*rows over VGA_CELLS_MAX             */
#define VGA_MODE_EPANEL  4   /* will not fit this panel even at scale 1  */
int         vga_mode_why(void);
const char *vga_mode_why_name(void);
void vga_force_text_mode(void);          /* blind recovery to stock 80x25 */
void vga_begin_frame(void);   /* v0.7.6: atomic frame — see drivers/vga.c */
void vga_end_frame(void);
void vga_scroll(void);
void vga_putchar(char c);
void vga_puts(const char *s);
void vga_printf(const char *fmt, ...);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_set_cursor(int x, int y);
void vga_get_cursor(int *x, int *y);
void vga_putchar_at(char c, int x, int y, vga_color_t fg, vga_color_t bg);
void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg);
void vga_fill_row(int y, char c, vga_color_t fg, vga_color_t bg);
void vga_getchar_at(int x, int y, uint8_t *ch, uint8_t *attr);
void vga_putraw_at(uint8_t ch, uint8_t attr, int x, int y);
void vga_shadow_write(int x, int y, uint16_t val);
uint16_t vga_shadow_read(int x, int y);
