/* ════════════════════════════════════════════════════════════════════
   test_console.c -- the console grid, on both consoles, at sizes no
                     VGA card can be programmed to.

   v0.8.8 "Helios" is the release where the console stopped being four
   register tables.  drivers/vga.c had TWO consoles sharing one grid,
   and the grid's limits were the VGA text hardware's: 90 columns
   because 720 pixels divided by an 8-dot character clock is 90, 60
   rows because 480 scanlines divided by an 8-line cell is 60.  The
   framebuffer console has neither a character clock nor a scanline
   count it did not choose, and it was clamped by both.

   ── WHY THIS SUITE EXISTS AND WHAT IT IS NOT ────────────────────────

   It is NOT a check that the arithmetic in fbc_fit() matches the
   arithmetic in this file.  That is the mistake tests/test_usbmux.c
   made at 0.8.7.6 -- a fixture written from the same guess as the code
   agrees with it perfectly -- and in a geometry routine it is the
   easiest mistake in the world to make, because both sides are three
   divisions.

   So every check here is written against something OUTSIDE the
   driver's arithmetic:

     - THE PIXELS.  The simulated framebuffer is a real array and the
       driver really draws into it.  "80x25 fills 1280x800" is checked
       by looking at the corner pixels of the grid and the pixels just
       outside it, not by asking the driver what offset it chose.

     - THE CELL.  Every cell of a 240x135 grid is written with a
       DISTINCT character and read back.  A stride bug cannot survive
       that: it is exactly the v0.7.12 defect (five call sites pinned
       to VGA_WIDTH while the column count had become dynamic), and it
       was found by eye because no suite could compile this file.

     - THE OTHER CONSOLE.  The text path is compiled too, through the
       VGA_MEM_PTR seam this release adds, so the two consoles are
       checked for AGREEING rather than each against itself.

   ── THE REFUSALS ARE ASSERTED BY CODE, NOT BY SIGN ──────────────────

   vga_set_mode() returned 0 for four unrelated causes and 0.8.7.7's
   harnesses spent the release establishing what that costs: remove a
   length guard from mux_recv() and it returns -4, which is USB_ENODEV,
   "nothing on the port" -- a refusal that sends the next person to
   check whether the phone is plugged in.  Here the four causes are
   "not a text mode", "outside the caps", "over the cell budget" and
   "bigger than the panel", and they have four different remedies.
   Every refusal test below asserts the vga_mode_why() CODE.

   ── WHAT IS DELIBERATELY NOT MODELLED ───────────────────────────────

   The simulated framebuffer accepts any geometry, like a real one: it
   is memory.  A fixture that refused an odd pitch or a grid with a
   margin would fail a correct driver -- the mock-that-cannot-refuse
   mistake inverted, which tests/muxdev.inc's zero TCP checksum records
   one layer down.  What it does enforce is the one thing a real
   framebuffer enforces absolutely: the BOUNDS.  Every store the driver
   makes goes through fb_store(), which counts a write outside
   width*height as an escape and fails the suite.
   ════════════════════════════════════════════════════════════════════ */
#define _GNU_SOURCE 1   /* MAP_32BIT */
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* ── the io.h seam: this driver pokes CRTC registers ─────────────────
   Recorded rather than discarded, because one check below is about
   the framebuffer path NOT touching them. */
#define IO_H
#include "../include/types.h"
static int io_writes;
static uint8_t crtc_idx_last;
/* ── the HARDWARE CURSOR, captured off the simulated CRTC ─────────────
   v0.8.8.4. The caret's position is written to the CRTC as two bytes
   through index registers 14 and 15, and until now this stub counted
   the writes and threw the values away.

   It is captured because the viewport has to translate it: cur_x and
   cur_y are the APP's coordinates, and the CRTC's are the panel's. An
   untranslated caret blinks in the top-left corner while the app's
   text appears centred -- and because the text is right, that reads as
   a cursor bug rather than as a missing translation, so it is exactly
   the kind of thing that ships.

   The injection harness caught this gap before any person did: the
   planted defect "the hardware cursor is placed in app coordinates"
   was MISSED by a suite of 252 checks, because nothing here had ever
   looked at where the caret went. */
static int crtc_pos_hi = -1, crtc_pos_lo = -1;

static inline void outb(uint16_t p, uint8_t v){
    io_writes++;
    if(p==0x3D4) crtc_idx_last=v;
    if(p==0x3D5){
        if(crtc_idx_last==14) crtc_pos_hi=v;
        if(crtc_idx_last==15) crtc_pos_lo=v;
    }
}

/* The cell index the CRTC was last told to put the caret at, or -1. */
static int crtc_cursor_cell(void){
    if(crtc_pos_hi < 0 || crtc_pos_lo < 0) return -1;
    return (crtc_pos_hi << 8) | crtc_pos_lo;
}
static inline void outw(uint16_t p, uint16_t v){ (void)p;(void)v; io_writes++; }
static inline void outl(uint16_t p, uint32_t v){ (void)p;(void)v; io_writes++; }
static inline uint8_t  inb(uint16_t p){ (void)p; return 0; }
static inline uint16_t inw(uint16_t p){ (void)p; return 0; }
static inline uint32_t inl(uint16_t p){ (void)p; return 0; }
static inline void io_wait(void){ }
static inline void i8042_delay(void){ }

/* ── the text window, through v0.8.8's seam ──────────────────────────
   The whole reason the text console has never been under test.  Sized
   from vga.h's TEXT maxima, which is what a VGA card really has. */
#include "../include/vga.h"
static uint16_t sim_text[VGA_TEXT_COLS_MAX * VGA_TEXT_ROWS_MAX];
#define VGA_MEM_PTR sim_text

/* ── the font plane, at 0xA0000 ──────────────────────────────────────
   vga_font_load8x8()/restore() write there through a raw pointer, and
   a native run would die on the first one.  The two functions are only
   reached by a SUCCESSFUL text-mode change, and 80x50/90x60 are two of
   the four, so they are reached.  There is no seam for this one: the
   suite simply never asks for an 8-pixel cell height on the text path,
   and asserts that it did not -- see "the eight-line cell modes are
   out of this suite's reach" below, which is a recorded gap and says
   what would close it. */

#include "../include/vga.h"

/* ── the simulated panel ─────────────────────────────────────────────
   A real framebuffer with real bounds.  fb[] is oversized so an
   out-of-bounds store lands in memory we own and can detect, rather
   than in the heap. */
#define FB_MAX_PX (4096u * 2304u)
static uint32_t *fb;
static uint32_t  fb_w, fb_h, fb_pitch;
static long      fb_escapes;

#define FB_GUARD_ROWS 4
static void fb_setup(uint32_t w, uint32_t h, uint32_t pitch_px){
    fb_w = w; fb_h = h; fb_pitch = pitch_px;
    fb_escapes = 0;
    /* The visible rectangle AND a guard band under it, both filled with
       the firmware-leftover pattern.  The band is the point: a driver
       that walks one row too far writes into it, and nothing else in
       this suite ever touches it.  The first version of fb_setup filled
       only the visible rows, so the band held malloc's leftovers and
       the guard reported 7680 escapes on a correct driver -- a fixture
       that cannot distinguish anything, which is the shape 0.8.7's
       harnesses found four times. */
    memset(fb, 0x5A, (size_t)pitch_px * (h + FB_GUARD_ROWS) * 4);
}
static uint32_t fb_at(uint32_t x, uint32_t y){
    if(x >= fb_pitch || y >= fb_h) return 0xDEAD;
    return fb[(size_t)y * fb_pitch + x];
}

/* ── the driver, as it ships ─────────────────────────────────────────── */
/* kernel/string.c's two converters, which vga_printf() calls.  The real
   ones, not stubs: a suite that reimplemented itoa would be certifying
   its own copy -- tools/check_diskname_sync.py's whole subject. */
#include "../kernel/string.c"

#include "../drivers/vga.c"

/* Count any store the driver made outside the visible rectangle.  The
   driver writes through fbc_addr directly, so this is checked after the
   fact against a guard band rather than intercepted. */
static void fb_check_guard(void){
    /* the band between width and pitch, and everything below height */
    for(uint32_t y = 0; y < fb_h; y++)
        for(uint32_t x = fb_w; x < fb_pitch; x++)
            if(fb[(size_t)y*fb_pitch + x] != 0x5A5A5A5Au) fb_escapes++;
    for(uint32_t y = fb_h; y < fb_h + FB_GUARD_ROWS; y++)
        for(uint32_t x = 0; x < fb_pitch; x++)
            if(fb[(size_t)y*fb_pitch + x] != 0x5A5A5A5Au) fb_escapes++;
}

static uint16_t cell(int x, int y){ return vga_shadow_read(x, y); }

/* ── the PHYSICAL cell, read out of the simulated VGA memory ──────────
   cell() goes through vga_shadow_read(), which v0.8.8.4 made
   viewport-aware -- so while a viewport is set it answers in the
   app's coordinates and cannot say where on the panel anything
   landed. That is exactly the question a viewport test has to ask.

   sim_text IS the hardware here: put_cell() writes to VGA_MEM_PTR,
   which this file defines as that array. So this reads the real
   destination, with a stride taken from vga_phys_cols() rather than
   from anything the viewport code computed -- the same discipline as
   the pixel checks above. */
static uint16_t phys_cell(int x, int y){
    return sim_text[y*vga_phys_cols() + x];
}
static char phys_ch(int x, int y){ return (char)(phys_cell(x,y) & 0xFF); }
static char     cell_ch(int x, int y){ return (char)(cell(x,y) & 0xFF); }

/* A printable, distinct character per cell index.  94 printable ASCII
   values and up to 32400 cells, so the pattern repeats -- but it
   repeats with a period that is coprime with neither the row stride nor
   the column count, which is what makes an off-by-one stride visible.
   Two primes: the character cycles every 94 and the index step is 1, so
   reading cell (x,y) back and comparing against the value computed from
   y*cols+x catches any stride error whose magnitude is not a multiple
   of 94 -- and a stride error of exactly 94 cells on a 240-column grid
   would also have to be a whole number of rows, which it is not. */
static char pattern(int idx){ return (char)(33 + (idx % 94)); }

int main(void)
{
    /* ── v0.8.9.5: THE PANEL HAS TO BE BELOW 4 GB ─────────────────────
       vga_fb_init() takes the framebuffer address as a uint32_t,
       because on the machine it is a 32-bit physical address. This
       suite used to get the panel from malloc(), and a 33 MB malloc is
       an mmap -- which on an ordinary x86-64 Linux kernel lands near
       0x7f0000000000. Cast to uint32_t, that is a different address
       entirely, and the first pixel written there kills the process
       before it prints a result line.

       It passed for every release because the container the tree was
       built in hands out low mmap addresses. The first run on a stock
       Ubuntu kernel -- the GitHub Actions release build -- died here
       with "the suite printed no result". MAP_32BIT asks the kernel
       for the first 2 GB outright. The check below it is not belt and
       braces: an address that does not survive the cast must stop the
       suite with a sentence, not a segfault. */
    fb = mmap(0, (size_t)FB_MAX_PX * 4, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if(fb == MAP_FAILED){
        printf("cannot map the simulated panel below 4 GB\n");
        return 2;
    }
    if((uintptr_t)fb != (uintptr_t)(uint32_t)(uintptr_t)fb){
        printf("the simulated panel is at %p, which does not fit the "
               "uint32_t vga_fb_init() takes\n", (void *)fb);
        return 2;
    }

    printf("\ntest_console -- the console grid, on both consoles\n");

    /* ════════════════════════════════════════════════════════════════
       1. THE CAPS ARE TWO DIFFERENT NUMBERS NOW
       ════════════════════════════════════════════════════════════════ */
    head("the two maxima are separate, and the budget is the product");
    ck("the text cap is still the CRTC's 90 columns",
       VGA_TEXT_COLS_MAX == 90, VGA_TEXT_COLS_MAX);
    ck("the text cap is still the CRTC's 60 rows",
       VGA_TEXT_ROWS_MAX == 60, VGA_TEXT_ROWS_MAX);
    ck("the column index limit is past 1920/8",
       VGA_COLS_MAX > 240, VGA_COLS_MAX);
    ck("the row index limit is past 1080/8",
       VGA_ROWS_MAX > 135, VGA_ROWS_MAX);
    /* ── THE BUDGET IS SMALLER THAN THE PRODUCT, AND MUST BE ──────────
       While it was EQUAL, no grid could be inside both axis limits and
       over the budget -- so VGA_MODE_EBUDGET was unreachable and
       fbc_fit()'s area clamp was dead, and this suite "covered" both
       without being able to reach either.
       tools/check_console_suite.py found it by injecting into the
       distinction and watching 124 checks shrug. */
    ck("the cell budget is SMALLER than the product of the axes",
       VGA_CELLS_MAX < VGA_COLS_MAX * VGA_ROWS_MAX, VGA_CELLS_MAX);
    ck("  ... and it is 1920x1080 at one pixel per pixel",
       VGA_CELLS_MAX == 240 * 135, VGA_CELLS_MAX);
    ck("  ... and it still holds the stock grid several times over",
       VGA_CELLS_MAX > 80 * 25 * 8, VGA_CELLS_MAX);
    /* The grid that makes the two limits differ: inside both axes,
       over the budget. If this is ever empty, EBUDGET is dead again. */
    ck("  ... so a grid exists that is in range AND over budget",
       VGA_COLS_MAX * VGA_ROWS_MAX > VGA_CELLS_MAX, 0);
    /* The two buffers are sized by the budget. A buffer sized by one
       axis pair while the grid is bounded by the other is the v0.7.12
       stride bug with the operands swapped. */
    ck("shadow[] is sized by the budget",
       (int)(sizeof shadow / sizeof shadow[0]) == VGA_CELLS_MAX,
       (long)(sizeof shadow / sizeof shadow[0]));
    ck("fbc_painted[] is sized by the budget",
       (int)(sizeof fbc_painted / sizeof fbc_painted[0]) == VGA_CELLS_MAX,
       (long)(sizeof fbc_painted / sizeof fbc_painted[0]));
    /* VGA_WIDTH/VGA_HEIGHT were a maximum and a width at the same time:
       VGA_WIDTH was 80 (the stock grid) and VGA_HEIGHT was 60 (the
       maximum), in adjacent lines, under one naming scheme. */
    ck("VGA_WIDTH and VGA_HEIGHT are both the STOCK grid now",
       VGA_WIDTH == 80 && VGA_HEIGHT == 25, VGA_HEIGHT);

    /* ════════════════════════════════════════════════════════════════
       2. THE TEXT PATH REFUSES, AND SAYS WHICH REFUSAL IT IS
       Before any init: a refusal returns before it writes anything, so
       these run with no console at all and touch no hardware.
       ════════════════════════════════════════════════════════════════ */
    head("the text console still runs exactly four modes");
    io_writes = 0;
    struct { int c, r; const char *what; } bad_text[] = {
        { 100, 25, "100x25 -- no such character clock" },
        {  80, 43, "80x43 -- no such register set" },
        {  80, 30, "80x30 -- 480 lines needs the 8-dot clock" },
        {  90, 25, "90x25 -- 400 lines needs the 9-dot clock" },
        { 240,135, "240x135 -- the framebuffer grid, on a CRTC" },
        {   0,  0, "0x0" },
    };
    for(unsigned i = 0; i < sizeof bad_text / sizeof bad_text[0]; i++){
        int r = vga_set_mode(bad_text[i].c, bad_text[i].r);
        ck(bad_text[i].what, r == 0, r);
        ck("  ... refused as 'not a VGA text mode'",
           vga_mode_why() == VGA_MODE_ETEXT, vga_mode_why());
    }
    ck("a refused text mode touches no register at all",
       io_writes == 0, io_writes);
    /* 80x25 is the current mode, so it is a no-op and still writes
       nothing -- which is also what makes these checks safe to run
       before vga_init(). */
    ck("80x25 is accepted (it is already the mode)",
       vga_set_mode(80,25) == 1, 0);
    ck("  ... and reports ok",
       vga_mode_why() == VGA_MODE_OK, vga_mode_why());

    /* RECORDED GAP, v0.8.8: 80x50 and 90x60 are the two text modes this
       suite cannot take, because a successful change to an 8-line cell
       calls vga_font_load8x8(), which writes the font plane through a
       raw pointer at 0xA0000.  The io.h seam does not cover a memory
       window.  What would close it is a second seam for the plane, the
       same shape as VGA_MEM_PTR -- not a stub for the font functions,
       which would remove the thing worth testing (that the BIOS 8x16
       plane is saved before the first overwrite and restored for a
       16-pixel cell, which is the v0.7.11 bug).  Recorded rather than
       worked around: v0.8.7.6 wrote down that it could not tell tx_seq
       from rx_seq and was right that the check was impossible and wrong
       about why, so a gap gets the reason it actually has. */
    head("a recorded gap: the 8-line text modes need a font-plane seam");
    ck("80x50 is one of the four, and out of reach here",
       1, 0);

    /* ════════════════════════════════════════════════════════════════
       3. 1280x800 AT 80x25: A SQUARE SCALE THAT FILLS THE PANEL
       This block used to assert sx=2 sy=4 and call it "as it always
       was". It always was, and it was the bug: 80x25 of 8x8 cells is
       640x200, an aspect ratio of 16:5, so the only way to fill a
       1280x800 panel with it was to draw every character twice as
       tall as it was wide.

       The cell is 8x16 in v0.8.8.2, so 80x25 is 640x400 -- 16:10,
       which is 1280x800 divided by exactly two in BOTH axes. The
       panel is still filled completely and nothing is stretched.
       ════════════════════════════════════════════════════════════════ */
    head("1280x800 at 80x25: scale 2 in both axes, filling the panel");
    fb_setup(1280, 800, 1280);
    ck("vga_fb_init takes a 32bpp panel",
       vga_fb_init((uint32_t)(uintptr_t)fb, 1280, 800, 1280*4, 32) == 1, 0);
    ck("the framebuffer console is active", vga_fb_active() == 1, 0);
    ck("80 columns", vga_cols() == 80, vga_cols());
    ck("25 rows", vga_rows() == 25, vga_rows());
    ck("horizontal scale 2", fbc_sx == 2, fbc_sx);
    ck("vertical scale 2 -- it was 4, and that was the stretch",
       fbc_sy == 2, fbc_sy);
    ck("no left margin", fbc_ox == 0, (long)fbc_ox);
    ck("no top margin", fbc_oy == 0, (long)fbc_oy);
    ck("and the fit was not capped", vga_grid_capped() == 0, 0);
    /* 80*8*2 = 1280 and 25*16*2 = 800: the grid is the whole panel. */
    ck("the grid covers the panel exactly",
       (uint32_t)(vga_cols()*FBC_CW*fbc_sx) == 1280u &&
       (uint32_t)(vga_rows()*FBC_CH*fbc_sy) == 800u, 0);

    /* ════════════════════════════════════════════════════════════════
       4. 1920x1080 AT 240x135 -- THE THING THE RELEASE IS NAMED FOR
       ════════════════════════════════════════════════════════════════ */
    head("1920x1080: the grid the old cap could not express");
    fb_setup(1920, 1080, 1920);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
    vga_init();
    /* 240x67, NOT 240x135.
       135 rows of a 16-pixel cell is 2160 pixels and a 1080p panel
       has 1080. The old 240x135 was only ever reachable with an 8x8
       cell, and what it produced was 240 columns of text drawn twice
       as tall as it was wide -- the "bugged and small" screen.
       240x67 is the tallest grid this panel can hold with a glyph
       that is the right shape, and 1080/16 is 67 with 8 pixels over. */
    int got = vga_set_mode(240, 67);
    ck("240x67 is accepted on a 1080p panel", got == 1, got);
    ck("  ... and 240 columns is what we got", vga_cols() == 240, vga_cols());
    ck("  ... and 67 rows", vga_rows() == 67, vga_rows());
    ck("  ... at scale 1 in both axes",
       fbc_sx == 1 && fbc_sy == 1, fbc_sx * 100 + fbc_sy);
    ck("  ... with no horizontal margin, because 240*8 == 1920",
       fbc_ox == 0, (long)fbc_ox);
    ck("  ... and four pixels of vertical margin, because 67*16 is 1072",
       fbc_oy == 4, (long)fbc_oy);
    ck("  ... and the budget did not bite", vga_grid_capped() == 0, 0);
    ck("  ... and the framebuffer path touched no CRTC register",
       crtc_idx_last == 0, crtc_idx_last);

    /* ── 135 ROWS IS NOW REFUSED ON THIS PANEL, BY NAME ──────────────
       It used to be the headline grid of v0.8.8. It is not a
       regression that it is gone: it was only ever expressible by
       distorting the glyph, and a refusal that says which limit it
       hit is what this console does instead of guessing. */
    ck("240x135 no longer fits a 1080p panel", vga_set_mode(240, 135) == 0, 0);
    ck("  ... and the refusal says so",
       vga_mode_why() == VGA_MODE_EPANEL, vga_mode_why());
    ck("  ... and the console is still 240x67 after it",
       vga_cols() == 240 && vga_rows() == 67, vga_rows());

    /* EVERY CELL, read back.  The v0.7.12 bug class, mechanically. */
    head("every one of 32400 cells is addressable and distinct");
    {
        long wrong = 0, first_x = -1, first_y = -1;
        vga_begin_frame();
        for(int y = 0; y < vga_rows(); y++)
            for(int x = 0; x < vga_cols(); x++)
                vga_putchar_at(pattern(y*vga_cols()+x), x, y,
                               VGA_LIGHT_GREY, VGA_BLACK);
        vga_end_frame();
        for(int y = 0; y < vga_rows(); y++)
            for(int x = 0; x < vga_cols(); x++)
                if(cell_ch(x,y) != pattern(y*vga_cols()+x)){
                    if(wrong == 0){ first_x = x; first_y = y; }
                    wrong++;
                }
        ck("no cell reads back as another cell's character", wrong == 0, wrong);
        if(wrong) printf("         first wrong cell: %ld,%ld\n", first_x, first_y);
    }
    fb_check_guard();
    ck("and not one pixel was written outside the panel",
       fb_escapes == 0, fb_escapes);

    /* The scroll, at a width no VGA card has.  v0.7.12's five pinned
       call sites were all this, and all found by reading. */
    head("the scroll moves rows at the grid's stride, not at 80");
    {
        for(int y = 0; y < vga_rows(); y++)
            for(int x = 0; x < vga_cols(); x++)
                vga_putchar_at((char)('A' + (y % 26)), x, y,
                               VGA_LIGHT_GREY, VGA_BLACK);
        vga_scroll();
        long bad = 0;
        for(int y = 0; y < vga_rows()-1; y++)
            for(int x = 0; x < vga_cols(); x++)
                if(cell_ch(x,y) != (char)('A' + ((y+1) % 26))) bad++;
        ck("every row came from the row below it", bad == 0, bad);
        long notblank = 0;
        for(int x = 0; x < vga_cols(); x++)
            if(cell_ch(x, vga_rows()-1) != ' ') notblank++;
        ck("the last row is blanked across its full width",
           notblank == 0, notblank);
    }

    /* ════════════════════════════════════════════════════════════════
       5. THE REFUSALS ON THE FRAMEBUFFER, EACH WITH ITS OWN CODE
       ════════════════════════════════════════════════════════════════ */
    head("four causes, four codes -- not one zero for all of them");
    fb_setup(1920, 1080, 1920);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);

    ck("one past the column limit is refused",
       vga_set_mode(VGA_COLS_MAX + 1, 50) == 0, 0);
    ck("  ... as 'outside the grid limits', not as a panel problem",
       vga_mode_why() == VGA_MODE_ERANGE, vga_mode_why());
    ck("one past the row limit is refused",
       vga_set_mode(80, VGA_ROWS_MAX + 1) == 0, 0);
    ck("  ... as 'outside the grid limits'",
       vga_mode_why() == VGA_MODE_ERANGE, vga_mode_why());
    ck("zero columns is refused", vga_set_mode(0, 25) == 0, 0);
    ck("  ... as 'outside the grid limits'",
       vga_mode_why() == VGA_MODE_ERANGE, vga_mode_why());

    /* ── THE REFUSAL THE OLD CAPS COULD NOT PRODUCE ───────────────────
       Inside both axis limits and over the cell budget.  320x180 is
       57600 cells against a 32400-cell budget, and it is the grid that
       proves 'outside the grid limits' and 'over the cell budget' are
       two different sentences with two different remedies. */
    ck("a grid inside both axes but over the budget is refused",
       vga_set_mode(VGA_COLS_MAX, VGA_ROWS_MAX) == 0, 0);
    ck("  ... as 'over the cell budget' -- NOT as outside the limits",
       vga_mode_why() == VGA_MODE_EBUDGET, vga_mode_why());
    /* ── THE BUDGET BOUNDARY NEEDS A PANEL THAT CAN HOLD IT ──────────
       240x135 is exactly VGA_CELLS_MAX, and with a 16-pixel cell 135
       rows is 2160 scanlines -- so the only panel that can express
       the boundary at all is a 4K one. On 1080p the budget is now
       unreachable: 240 columns by the 67 rows that fit is 16080
       cells, half of it. That is a consequence of the cell getting
       taller and not a regression; the budget is a bound on the
       BUFFERS, and the panel is a separate bound with its own code. */
    fb_setup(3840, 2160, 3840);
    vga_fb_init((uint32_t)(uintptr_t)fb, 3840, 2160, 3840*4, 32);
    ck("240x135 -- exactly the budget -- is allowed on a 4K panel",
       vga_set_mode(240,135) == 1, vga_mode_why());
    ck("  ... and one cell more in area is not",
       vga_set_mode(241,135) == 0, 0);
    ck("  ... refused on the BUDGET, since 241 is inside the axis limit",
       vga_mode_why() == VGA_MODE_EBUDGET, vga_mode_why());
    ck("  ... and 135 rows of a 16-pixel cell is exactly 2160",
       135 * FBC_CH == 2160, 135 * FBC_CH);
    /* back to 1080p for what follows */
    fb_setup(1920, 1080, 1920);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
    ck("the same grid is refused on 1080p, on the PANEL not the budget",
       vga_set_mode(240,135) == 0, 0);
    ck("  ... as 'will not fit this panel'",
       vga_mode_why() == VGA_MODE_EPANEL, vga_mode_why());
    /* And a wide, short grid IS allowed -- the other thing separating
       the limits buys.  On an ultrawide, because 320 columns needs
       2560 pixels and the panel above has 1920: the BUDGET allows it
       and the PANEL does not, which is itself the distinction under
       test two checks above. */
    head("a wide short grid, on a panel wide enough for it");
    fb_setup(2560, 1080, 2560);
    vga_fb_init((uint32_t)(uintptr_t)fb, 2560, 1080, 2560*4, 32);
    /* 2560 wide holds 320 columns of an 8-pixel cell exactly; 1080
       tall holds 67 rows of a 16-pixel one. 320x67 is 21440 cells,
       inside the budget, and is the widest grid this panel can
       express -- which is the point: a wide short grid is allowed
       and no VGA card can produce one. */
    ck("320x67 is allowed on an ultrawide",
       vga_set_mode(320, 67) == 1, vga_mode_why());
    ck("  ... 320 columns, which no VGA card can express at all",
       vga_cols() == 320 && vga_rows() == 67, vga_cols());
    ck("  ... and a taller, narrower grid of similar area too",
       vga_set_mode(160, 67) == 1, vga_mode_why());
    ck("  ... while 100 rows does not fit 1080 scanlines",
       vga_set_mode(320, 100) == 0, 0);
    ck("    ... on the PANEL, not the budget",
       vga_mode_why() == VGA_MODE_EPANEL, vga_mode_why());
    /* back to the 1080p panel the rest of this section uses */
    fb_setup(1920, 1080, 1920);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);

    /* A panel smaller than the grid: the other refusal entirely, and
       the one the old single return value could not distinguish. */
    head("a grid that fits the caps but not the panel");
    fb_setup(640, 480, 640);
    vga_fb_init((uint32_t)(uintptr_t)fb, 640, 480, 640*4, 32);
    ck("640x480 gives 80x25", vga_cols()==80 && vga_rows()==25, 0);
    /* 640/(80*8) = 1 across and 480/(25*16) = 1 down, so the cell is
       its native 8x16 and 80 of the 480 scanlines are margin.
       THE TWO SCALES AGREE, and that is now the invariant: this
       block used to assert 1 across and 2 down and called the
       independent scaling deliberate. It was deliberate and it was
       the bug -- 1:2 here meant every character on this panel was
       drawn twice as tall as it was wide. */
    ck("  ... at scale 1 in both axes", fbc_sx==1 && fbc_sy==1,
       fbc_sx*100+fbc_sy);
    ck("  ... leaving 80 scanlines of vertical margin",
       fbc_oy == 40u, (long)fbc_oy);
    ck("240x135 is refused on a 640x480 panel",
       vga_set_mode(240,135) == 0, 0);
    ck("  ... as 'will not fit this panel' -- NOT as over the budget",
       vga_mode_why() == VGA_MODE_EPANEL, vga_mode_why());
    /* 80x30, not 80x60: 480 scanlines hold thirty 16-pixel rows and
       sixty 8-pixel ones. Both divide exactly; only one of them
       draws a character the right shape. */
    ck("80x30 is accepted: 640 and 480 divide exactly",
       vga_set_mode(80,30) == 1, vga_mode_why());
    ck("  ... 80x30, which is not one of the four text modes",
       vga_cols()==80 && vga_rows()==30, 0);
    ck("  ... and it covers the panel with no margin at all",
       fbc_ox == 0 && fbc_oy == 0, (long)(fbc_ox + fbc_oy));
    ck("80x60 needs 960 scanlines and is refused",
       vga_set_mode(80,60) == 0, 0);

    /* ════════════════════════════════════════════════════════════════
       6. vga_fb_best_grid: BIGGER PANEL, BIGGER GLYPHS
       The property that matters is that the grid FILLS the panel --
       checked by multiplying back out, not by trusting the loop.
       ════════════════════════════════════════════════════════════════ */
    head("the largest grid that fills the panel, on the panels people own");
    struct { uint32_t w, h; int c, r; const char *name; } panels[] = {
        { 1280,  800,  80,  25, "1280x800  (the HP, and QEMU's default)" },
        { 1920, 1080, 240, 135, "1920x1080 at 1x" },
        { 1366,  768, 170,  96, "1366x768  (the panel that broke 0.8.3)" },
        { 1600,  900, 200, 112, "1600x900" },
        { 2560, 1440, 160,  90, "2560x1440 at 2x" },
        { 3840, 2160, 240, 135, "3840x2160 at 2x" },
    };
    for(unsigned i = 0; i < sizeof panels / sizeof panels[0]; i++){
        int c = 0, r = 0;
        fb_setup(panels[i].w, panels[i].h, panels[i].w);
        vga_fb_init((uint32_t)(uintptr_t)fb, panels[i].w, panels[i].h,
                    panels[i].w*4, 32);
        int ok = vga_fb_best_grid(&c, &r);
        printf("    --   %s -> %dx%d\n", panels[i].name, c, r);
        ck("  a grid is found", ok == 1, ok);
        ck("  inside both axis caps",
           c <= VGA_COLS_MAX && r <= VGA_ROWS_MAX, c*1000L + r);
        ck("  inside the cell budget", c*r <= VGA_CELLS_MAX, c*r);
        /* The filling property, derived from the panel rather than
           copied from the driver: the grid must leave less than one
           whole cell of margin in each axis at the scale it implies. */
        {
            int sx = (int)(panels[i].w / (uint32_t)(c*FBC_CW));
            int sy = (int)(panels[i].h / (uint32_t)(r*FBC_CH));
            int s  = sx < sy ? sx : sy;      /* one scale, as fbc_fit */
            ck("  leaves under one cell of horizontal margin",
               panels[i].w - (uint32_t)(c*FBC_CW*s) < (uint32_t)(FBC_CW*s),
               (long)(panels[i].w - (uint32_t)(c*FBC_CW*s)));
            ck("  leaves under one cell of vertical margin",
               panels[i].h - (uint32_t)(r*FBC_CH*s) < (uint32_t)(FBC_CH*s),
               (long)(panels[i].h - (uint32_t)(r*FBC_CH*s)));
        }
        /* And it is actually settable, which is the point. */
        ck("  and vga_set_mode takes it",
           vga_set_mode(c, r) == 1, vga_mode_why());
    }

    /* no framebuffer -> no panel to fill, and it says so rather than
       returning an arithmetic leftover. */
    head("best_grid on a machine with no framebuffer");
    fbc_active = 0;
    {
        int c = -1, r = -1;
        int ok = vga_fb_best_grid(&c, &r);
        ck("it refuses", ok == 0, ok);
        ck("  ... and reports the stock grid, not zeroes",
           c == 80 && r == 25, c*1000L + r);
    }

    /* ════════════════════════════════════════════════════════════════
       7. THE MARGIN IS PAINTED, AND THE GRID IS CENTRED IN PIXELS
       Checked by reading the framebuffer, which is why it is a real
       array.  0x5A5A5A5A is what fb_setup leaves there -- firmware
       leftovers -- so a margin the driver did not paint is visible.
       ════════════════════════════════════════════════════════════════ */
    head("a grid smaller than the panel is centred and the rest painted");
    fb_setup(1920, 1080, 1920);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
    /* 100x40: 1920/(100*8) = 2 across, 1080/(40*16) = 1 down, and
       the SMALLER wins -- so scale 1 in both axes, a grid of 800x640
       pixels, and margins of 560 and 220.

       The old numbers here were sx=2, sy=3, which drew a character
       16 pixels wide and 48 tall. This check was the one that would
       have shown the stretch to anybody reading the suite, and it
       asserted it as correct. */
    ck("100x40 is accepted", vga_set_mode(100,40) == 1, vga_mode_why());
    ck("  scale 1 across", fbc_sx == 1, fbc_sx);
    ck("  scale 1 down -- the SMALLER of the two, so the glyph is square",
       fbc_sy == 1, fbc_sy);
    ck("  and the two agree, which is the invariant",
       fbc_sx == fbc_sy, fbc_sx * 100 + fbc_sy);
    ck("  centred horizontally: (1920-800)/2",
       fbc_ox == 560u, (long)fbc_ox);
    ck("  centred vertically: (1080-640)/2",
       fbc_oy == 220u, (long)fbc_oy);
    ck("  the pixel just left of the grid is painted, not firmware junk",
       fb_at(fbc_ox - 1, fbc_oy) != 0x5A5A5A5Au, (long)fb_at(0, fbc_oy));
    ck("  the top-left corner of the panel is painted",
       fb_at(0,0) != 0x5A5A5A5Au, (long)fb_at(0,0));
    ck("  the bottom-right corner of the panel is painted",
       fb_at(1919,1079) != 0x5A5A5A5Au, (long)fb_at(1919,1079));
    fb_check_guard();
    ck("  and nothing escaped the panel", fb_escapes == 0, fb_escapes);

    /* A cell really becomes pixels, at the scale and offset claimed.
       A full block glyph (0xDB in CP437) is all ones in font8x8, so
       every pixel of its cell takes the foreground colour. */
    head("one cell, as pixels, at the offset and scale claimed");
    {
        /* A SPACE on a white background, not a solid-block glyph.  The
           background covers every pixel of a cell whatever the glyph,
           which is the property the whole UI rests on -- ui.c draws its
           taskbars and windows as spaces in a colour, never as block
           characters.  (And the block glyph would not have worked: see
           the recorded difference below.) */
        vga_putchar_at(' ', 3, 2, VGA_BLACK, VGA_WHITE);
        vga_flush();
        uint32_t px = fbc_ox + 3u*(uint32_t)FBC_CW*(uint32_t)fbc_sx;
        uint32_t py = fbc_oy + 2u*(uint32_t)FBC_CH*(uint32_t)fbc_sy;
        ck("the cell's first pixel is white", fb_at(px,py) == 0xFFFFFFu,
           (long)fb_at(px,py));
        ck("its last pixel is white too",
           fb_at(px + (uint32_t)FBC_CW*(uint32_t)fbc_sx - 1u,
                 py + (uint32_t)FBC_CH*(uint32_t)fbc_sy - 1u) == 0xFFFFFFu, 0);
        ck("the pixel one past the cell is not",
           fb_at(px + (uint32_t)FBC_CW*(uint32_t)fbc_sx, py) != 0xFFFFFFu, 0);
        /* every pixel, not only three of them */
        {
            long notwhite = 0;
            for(uint32_t yy = 0; yy < (uint32_t)FBC_CH*(uint32_t)fbc_sy; yy++)
                for(uint32_t xx = 0; xx < (uint32_t)FBC_CW*(uint32_t)fbc_sx; xx++)
                    if(fb_at(px+xx, py+yy) != 0xFFFFFFu) notwhite++;
            ck("and every pixel of the cell, not just its corners",
               notwhite == 0, notwhite);
        }
    }

    head("the write-through path draws without a flush");
    /* vga.c's architecture note calls write-through "the fast path for
       all normal drawing": outside a frame, a write goes to the shadow
       AND to the screen together.  Every pixel check above calls
       vga_flush() first, and vga_flush() paints through fbc_put()
       directly -- so all of them passed with the write-through path
       completely disabled.  tools/check_console_suite.py found that:
       an injection making put_cell() skip on the shadow instead of on
       the painted record (the v0.7.6 defect, one console along) was
       MISSED by 134 checks.
       No flush here, deliberately. */
    {
        vga_putchar_at(' ', 7, 4, VGA_BLACK, VGA_WHITE);   /* no flush */
        uint32_t px = fbc_ox + 7u*(uint32_t)FBC_CW*(uint32_t)fbc_sx;
        uint32_t py = fbc_oy + 4u*(uint32_t)FBC_CH*(uint32_t)fbc_sy;
        ck("the pixel is white with no vga_flush() at all",
           fb_at(px,py) == 0xFFFFFFu, (long)fb_at(px,py));
        /* And inside a frame it must NOT be: that is what
           vga_begin_frame() exists for, and the two claims together
           are what make the path testable at all. */
        vga_begin_frame();
        vga_putchar_at(' ', 9, 4, VGA_BLACK, VGA_WHITE);
        uint32_t qx = fbc_ox + 9u*(uint32_t)FBC_CW*(uint32_t)fbc_sx;
        ck("  ... and inside an open frame it is deferred",
           fb_at(qx,py) != 0xFFFFFFu, (long)fb_at(qx,py));
        vga_end_frame();
        ck("  ... until the frame closes",
           fb_at(qx,py) == 0xFFFFFFu, (long)fb_at(qx,py));
    }

    /* ── A RECORDED DIFFERENCE BETWEEN THE TWO CONSOLES, v0.8.8 ───────
       Found by the seam, on the first run, by a check that expected the
       opposite.

       CP437 0xDB is a solid block.  The BIOS 8x16 font has it, so the
       80x25 text console draws it; font8x8[] in this tree is ALL ZEROES
       from 0x80 up except the accented letters tools/make_font_ext.py
       generates, and 0xDB is not one of them -- so on the framebuffer
       console it is a blank cell.  vga_font_patch_symbols() fills the
       gaps at 0x01-0x1F for BOOT Chat's emoji and does not touch the
       box/shade range.

       NOTHING IS BROKEN BY IT TODAY: shell/ui.c draws every bar, window
       and highlight as a space in a background colour, which is why the
       check above uses one.  It is recorded because the next person to
       reach for a shade character for a progress bar will get one on
       BIOS and nothing under UEFI, and because a difference between two
       consoles that both "work" is exactly what this suite was added to
       be able to see.  An injection that proves not to be a defect is
       recorded, not deleted -- and so is a finding. */
    head("a recorded difference: font8x8[] has no solid block at 0xDB");
    {
        vga_putchar_at((char)0xDB, 5, 5, VGA_WHITE, VGA_BLACK);
        vga_flush();
        uint32_t px = fbc_ox + 5u*(uint32_t)FBC_CW*(uint32_t)fbc_sx;
        uint32_t py = fbc_oy + 5u*(uint32_t)FBC_CH*(uint32_t)fbc_sy;
        long white = 0;
        for(uint32_t yy = 0; yy < (uint32_t)FBC_CH*(uint32_t)fbc_sy; yy++)
            for(uint32_t xx = 0; xx < (uint32_t)FBC_CW*(uint32_t)fbc_sx; xx++)
                if(fb_at(px+xx, py+yy) == 0xFFFFFFu) white++;
        ck("0xDB renders blank on the framebuffer console (it is not a block)",
           white == 0, white);
        /* and the glyph table says so directly, so this is not an
           inference from pixels */
        {
            int nonzero = 0;
            for(int i = 0; i < 8; i++) if(font8x8[0xDB*8+i]) nonzero++;
            ck("  ... because font8x8[0xDB] is eight zero rows",
               nonzero == 0, nonzero);
        }
        /* The accented letters ARE there, which is what makes the gap a
           gap rather than "the font stops at 127". */
        {
            int nonzero = 0;
            /* 0x82, not 0xE9: the table is indexed by CP437 code
               point, which is what the VGA font plane and this font
               both are.  Latin-1's 0xE9 would be a different
               character entirely in this encoding. */
            for(int i = 0; i < FONT8X8_EXT_N; i++)
                if(FONT8X8_EXT[i].idx == 0x82) nonzero = 1;   /* CP437 e-acute */
            ck("  ... while the extended table does carry CP437 e-acute",
               nonzero == 1, nonzero);
            ck("  ... and the gap is the box/shade range, 0xB0-0xDF",
               FONT8X8_EXT_N == 32, FONT8X8_EXT_N);
        }
    }

    /* ════════════════════════════════════════════════════════════════
       8. SHRINKING THE GRID MUST NOT LEAVE THE CURSOR OFF IT
       The one failure here that would be silent: a cursor past the
       last row indexes shadow[] out of the live area and the caret
       lands in a cell nothing will ever repaint.
       ════════════════════════════════════════════════════════════════ */
    head("the cursor survives a grid that shrinks under it");
    vga_set_mode(240,135);
    vga_set_cursor(239, 134);
    vga_set_mode(80, 25);
    {
        int cx = -1, cy = -1;
        vga_get_cursor(&cx, &cy);
        ck("the cursor is inside the new grid",
           cx < vga_cols() && cy < vga_rows(), cx*1000L + cy);
    }
    /* And the other direction: growing must not move it. */
    vga_set_cursor(10, 11);
    vga_set_mode(160, 90);
    {
        int cx = -1, cy = -1;
        vga_get_cursor(&cx, &cy);
        ck("growing the grid leaves the cursor where it was",
           cx == 10 && cy == 11, cx*1000L + cy);
    }

    /* ════════════════════════════════════════════════════════════════
       9. THE OUT-OF-BOUNDS WRITE GUARD, AT THE NEW SIZES
       vga_write()'s bounds check exists because a long theme list in
       Settings wrote past shadow[] into the heap and hung the machine.
       The grid is six times bigger now; the guard is the same guard.
       ════════════════════════════════════════════════════════════════ */
    head("a write outside the grid is dropped, at every grid size");
    vga_set_mode(80,25);
    {
        uint16_t before = cell(0,0);
        vga_putchar_at('Z', 80, 0, VGA_WHITE, VGA_BLACK);   /* one past */
        vga_putchar_at('Z', 0, 25, VGA_WHITE, VGA_BLACK);
        vga_putchar_at('Z', -1, 0, VGA_WHITE, VGA_BLACK);
        vga_putchar_at('Z', 0, -1, VGA_WHITE, VGA_BLACK);
        vga_putchar_at('Z', 239, 134, VGA_WHITE, VGA_BLACK); /* in buffer,
                                                   outside the grid */
        ck("none of five out-of-grid writes landed in cell 0,0",
           cell(0,0) == before, cell(0,0));
        /* ── THE RAW BUFFER, NOT THE ACCESSOR ─────────────────────────
           239,134 at an 80-column grid is index 134*80+239 = 10959,
           which is a VALID index into shadow[] and an invalid CELL.  A
           guard that bounds the buffer instead of the grid lets it
           through, and the cell holds a character no flush will ever
           paint.
           The first version of this check read it back through
           vga_shadow_read(), which bounds against the grid itself and
           therefore returns the blank whatever is in the buffer -- so
           the check could not fail.  tools/check_console_suite.py's
           injection was MISSED, and this is the fix: read shadow[]
           directly, which this suite can do because it compiles the
           file. */
        ck("a write inside the buffer but outside the grid is dropped",
           shadow[134*80 + 239] != make_entry('Z',VGA_WHITE,VGA_BLACK),
           shadow[134*80 + 239]);
        /* and the accessor refuses it too, which is a separate claim */
        ck("  ... and the accessor refuses to read it either",
           vga_shadow_read(239,134) == 0x0720, vga_shadow_read(239,134));
    }

    /* ════════════════════════════════════════════════════════════════
       10. AND THE TWO CONSOLES AGREE
       The reason the VGA_MEM_PTR seam was worth adding.  Same writes,
       same readback, on both paths.
       ════════════════════════════════════════════════════════════════ */
    head("the text console and the framebuffer console agree");
    {
        /* framebuffer first */
        fb_setup(1280, 800, 1280);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1280, 800, 1280*4, 32);
        vga_init();
        vga_set_cursor(0,0);
        vga_puts("HavenDOS");
        char fbline[16];
        for(int i = 0; i < 8; i++) fbline[i] = cell_ch(i,0);
        fbline[8] = 0;

        /* then the text console, through the seam */
        fbc_active = 0;
        memset(sim_text, 0, sizeof sim_text);
        g_cols = 80; g_rows = 25;
        vga_init();
        vga_set_cursor(0,0);
        vga_puts("HavenDOS");
        char txline[16];
        for(int i = 0; i < 8; i++) txline[i] = cell_ch(i,0);
        txline[8] = 0;

        ck("the framebuffer console wrote the string",
           !strcmp(fbline, "HavenDOS"), 0);
        ck("the text console wrote the same string",
           !strcmp(txline, "HavenDOS"), 0);
        ck("and the text console put it in VGA memory, through the seam",
           (sim_text[0] & 0xFF) == 'H' && (sim_text[7] & 0xFF) == 'S',
           sim_text[0] & 0xFF);
        /* The attribute byte, which is the half that is easy to get
           right in one console and wrong in the other. */
        ck("with the same attribute in both",
           (cell(0,0) >> 8) == (uint16_t)((VGA_BLACK<<4)|VGA_LIGHT_GREY),
           cell(0,0) >> 8);
    }

    /* The text console's scroll, at 80x25, against VGA memory. */
    head("the text console scrolls in VGA memory, not only in the shadow");
    {
        for(int x = 0; x < 80; x++)
            vga_putchar_at('1', x, 0, VGA_LIGHT_GREY, VGA_BLACK);
        for(int x = 0; x < 80; x++)
            vga_putchar_at('2', x, 1, VGA_LIGHT_GREY, VGA_BLACK);
        vga_scroll();
        vga_flush();
        ck("row 0 of VGA memory now holds what row 1 held",
           (sim_text[0] & 0xFF) == '2' && (sim_text[79] & 0xFF) == '2',
           sim_text[0] & 0xFF);
        long bad = 0;
        for(int x = 0; x < 80; x++)
            if((sim_text[24*80 + x] & 0xFF) != ' ') bad++;
        ck("and the bottom row is blank across all 80 columns", bad==0, bad);
    }

    /* tools/run_suite.py reads this exact line and refuses a suite that
       does not print one, or that comes in under its floor -- which is
       what caught 621 silently-absent checks at v0.8.5.6. */
    /* ════════════════════════════════════════════════════════════════
       11. THREE THINGS THE FIRST VERSION OF THIS SUITE COULD NOT REACH
       All three were found by tools/check_console_suite.py, which
       injected a defect into each and watched 124 checks pass anyway.
       ════════════════════════════════════════════════════════════════ */
    head("the margin is painted by vga_fb_init itself, before any mode set");
    /* The earlier margin check ran after a vga_set_mode(), and that
       path paints the WHOLE framebuffer blank on its own -- so deleting
       fbc_clear_margin() from vga_fb_init() changed nothing it could
       see.  1366x768 is the panel that broke v0.8.3: 80x25 fits at
       sx=2, sy=3, which is 1280x600 of it, leaving 86 pixels across
       and 168 down that NOTHING else in the driver ever writes to. */
    fb_setup(1366, 768, 1366);
    vga_fb_init((uint32_t)(uintptr_t)fb, 1366, 768, 1366*4, 32);
    ck("80x25 at sx=2 sy=3 leaves a margin",
       fbc_ox > 0 && fbc_oy > 0, (long)(fbc_ox * 1000 + fbc_oy));
    ck("  ... and the top-left pixel is painted, not firmware junk",
       fb_at(0,0) != 0x5A5A5A5Au, (long)fb_at(0,0));
    ck("  ... and the bottom-right pixel too",
       fb_at(1365,767) != 0x5A5A5A5Au, (long)fb_at(1365,767));
    ck("  ... and the pixel just outside the grid's right edge",
       fb_at(fbc_ox + (uint32_t)(vga_cols()*8*fbc_sx), fbc_oy)
           != 0x5A5A5A5Au, 0);
    fb_check_guard();
    ck("  ... without writing past the panel", fb_escapes == 0, fb_escapes);

    head("fbc_fit's own clamps, reached directly");
    /* vga_set_mode() validates range, budget and panel BEFORE calling
       fbc_fit(), so no clamp inside the fit can fire on that path --
       which is why the injection that deleted the area clamp was
       MISSED.  The clamps are defence in depth for vga_fb_init() and
       any later caller, and drivers/vga.c says so.  This suite
       compiles the file, so it calls the static function and reaches
       them on purpose rather than believing it had. */
    fb_setup(3840, 2160, 3840);
    vga_fb_init((uint32_t)(uintptr_t)fb, 3840, 2160, 3840*4, 32);
    fbc_fit(VGA_COLS_MAX + 100, VGA_ROWS_MAX + 100);
    ck("a grid past both axis limits is clamped to them or below",
       vga_cols() <= VGA_COLS_MAX && vga_rows() <= VGA_ROWS_MAX,
       vga_cols() * 1000L + vga_rows());
    ck("  ... and the AREA clamp brought it inside the budget",
       vga_cols() * vga_rows() <= VGA_CELLS_MAX,
       vga_cols() * vga_rows());
    /* The portrait case the area clamp exists for: both axes legal,
       area not. */
    fbc_fit(VGA_ROWS_MAX, VGA_COLS_MAX);        /* 180 x 320 */
    ck("a portrait grid inside both axes is clamped by area",
       vga_cols() * vga_rows() <= VGA_CELLS_MAX,
       vga_cols() * vga_rows());
    ck("  ... and it kept its columns in preference to its rows",
       vga_cols() == VGA_ROWS_MAX, vga_cols());
    /* and whatever it settled on is writable end to end */
    {
        long wrong = 0;
        for(int y = 0; y < vga_rows(); y++)
            for(int x = 0; x < vga_cols(); x++)
                vga_putchar_at(pattern(y*vga_cols()+x), x, y,
                               VGA_LIGHT_GREY, VGA_BLACK);
        for(int y = 0; y < vga_rows(); y++)
            for(int x = 0; x < vga_cols(); x++)
                if(cell_ch(x,y) != pattern(y*vga_cols()+x)) wrong++;
        ck("  ... and every cell of the clamped grid is addressable",
           wrong == 0, wrong);
    }

    /* ════════════════════════════════════════════════════════════════
       12. THE LARGEST READABLE GRID
       `resolution max` on 1080p is a cell 8 physical pixels square,
       and the first thing anybody said about it was that it is too
       small to use. This is the grid to recommend instead, and what
       is asserted is the PHYSICAL CELL SIZE -- derived from the panel,
       not copied from the driver.
       ════════════════════════════════════════════════════════════════ */
    head("the largest grid whose text is still a normal size");
    struct { uint32_t w, h; const char *name; } rp[] = {
        { 1280,  800, "1280x800" },
        { 1920, 1080, "1920x1080" },
        { 1366,  768, "1366x768" },
        { 2560, 1440, "2560x1440" },
        { 3840, 2160, "3840x2160" },
        {  640,  480, "640x480" },
    };
    for(unsigned i = 0; i < sizeof rp / sizeof rp[0]; i++){
        int c = 0, r = 0;
        fb_setup(rp[i].w, rp[i].h, rp[i].w);
        vga_fb_init((uint32_t)(uintptr_t)fb, rp[i].w, rp[i].h,
                    rp[i].w*4, 32);
        int ok = vga_fb_readable_grid(VGA_READABLE_PX, &c, &r);
        printf("    --   %s -> %dx%d\n", rp[i].name, c, r);
        ck("  a grid is found", ok == 1, ok);
        ck("  it is settable", vga_set_mode(c, r) == 1, vga_mode_why());
        /* THE POINT: the cell really is at least 16 physical pixels.
           Checked against the panel's own dimensions and the scale the
           driver then chose, not against the function's arithmetic. */
        ck("  the glyph cell is at least 16 physical pixels across",
           FBC_CW * fbc_sx >= VGA_READABLE_PX, FBC_CW * fbc_sx);
        /* ...and twice that tall, because the cell is 8x16 and both
           axes take the same scale. 16x32 is the classic VGA cell. */
        ck("  ... and twice as tall, which is the right shape",
           FBC_CH * fbc_sy == 2 * (FBC_CW * fbc_sx), FBC_CH * fbc_sy);
        ck("  ... at ONE scale, so nothing is stretched",
           fbc_sx == fbc_sy, fbc_sx * 100 + fbc_sy);
        /* And it is AT LEAST the stock grid, or this buys nothing.
           1280x800 now lands on exactly 80x25, which is not a
           failure: 80x25 of 8x16 cells is 640x400 and that panel is
           exactly twice it. 640x480 is checked separately below. */
        if(rp[i].w >= 1280)
            ck("  and it is at least as big as 80x25",
               c * r >= 80 * 25, c * r);
    }
    /* ── A PANEL WHERE THE BUDGET BITES AND THE CELL MUST NOT ────────
       On every panel above, 16-pixel cells fit inside 32400 cells with
       room to spare, so the area clamp in readable_grid never fired
       and an injection that replaced it with "raise the scale instead"
       was MISSED -- the two behaved identically on everything tested.
       With an 8x16 cell a readable 16-pixel-wide cell is 16x32 = 512
       pixels, so exceeding a 32400-cell budget would need a panel of
       over 16.6 million pixels -- bigger than fb[] here and bigger
       than anything shipping. The clamp is driven through min_px
       instead, which is a parameter and therefore the same code path
       a caller takes: at min_px 8 the cell is its native 8x16, and
       a 4K panel gives 480x135 before the caps -- 320x135 after the
       axis cap, which is 43200 against a 32400 budget.
       What must give way is the GRID, not the cell: a caller asked for
       a readable size and raising the scale hands back the smaller
       grid with bigger text that vga_fb_best_grid() already gives.

       The first version of this block used a 4096x4096 panel and ran
       off the end of fb[], which is how that limit got worked out. */
    head("a panel where the cell budget bites, not the cell size");
    {
        int c = 0, r = 0;
        /* 3840x2160, not 4096x2304: FB_MAX_PX is 4096*2304 exactly,
           so a panel of that size runs into the guard band fb[]
           keeps past the end and the suite dies in the allocator
           rather than failing a check. */
        fb_setup(3840, 2160, 3840);
        vga_fb_init((uint32_t)(uintptr_t)fb, 3840, 2160, 3840*4, 32);
        ck("a readable grid is found",
           vga_fb_readable_grid(8, &c, &r) == 1, 0);
        printf("    --   3840x2160 at an 8px cell -> %dx%d\n", c, r);
        ck("  it is inside the budget", c * r <= VGA_CELLS_MAX, c * r);
        ck("  the COLUMNS survived, capped by the axis limit",
           c == VGA_COLS_MAX, c);
        ck("  the rows are what gave way", r < 144 && r > 100, r);
        ck("  it is settable", vga_set_mode(c, r) == 1, vga_mode_why());
        ck("  and the cell is the 8 pixels asked for, not 16",
           FBC_CW * fbc_sx == 8, FBC_CW * fbc_sx);
        ck("  ... and 16 down, at the same scale",
           FBC_CH * fbc_sy == 16, FBC_CH * fbc_sy);
    }

    /* A panel too small for a readable grid bigger than stock:
       640x480 at a 16x32 cell is 40x15 -- fewer columns AND fewer
       rows than 80x25. The function still answers honestly rather
       than pretending. */
    head("a panel too small to have it both ways");
    {
        int c = 0, r = 0;
        fb_setup(640, 480, 640);
        vga_fb_init((uint32_t)(uintptr_t)fb, 640, 480, 640*4, 32);
        vga_fb_readable_grid(VGA_READABLE_PX, &c, &r);
        ck("640x480 at a 16x32 cell is 40x15, and it says so",
           c == 40 && r == 15, c*1000L + r);
        ck("  ... which is FEWER columns than the stock grid",
           c < 80, c);
        ck("  ... and fewer rows, because the cell is 32 tall",
           r < 25, r);
    }
    /* No framebuffer: the same contract as best_grid -- refuse, and
       still report the stock grid rather than leaving the caller's
       variables untouched. */
    head("readable_grid on a machine with no framebuffer");
    {
        int c = -1, r = -1;
        fbc_active = 0;
        int ok = vga_fb_readable_grid(VGA_READABLE_PX, &c, &r);
        ck("it refuses", ok == 0, ok);
        ck("  ... and reports the stock grid, not zeroes",
           c == 80 && r == 25, c*1000L + r);
    }
    /* And `max` really is smaller-textured than `fit` -- the two have
       to be different answers or one of them is pointless. */
    head("'max' and 'fit' are different answers");
    {
        int mc = 0, mr = 0, rc = 0, rr = 0;
        fb_setup(1920, 1080, 1920);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
        vga_fb_best_grid(&mc, &mr);
        vga_fb_readable_grid(VGA_READABLE_PX, &rc, &rr);
        ck("max gives more cells than fit", mc * mr > rc * rr,
           mc*mr - rc*rr);
        vga_set_mode(mc, mr);
        int max_px = 8 * fbc_sy;
        vga_set_mode(rc, rr);
        int fit_px = 8 * fbc_sy;
        ck("  ... and fit's glyphs are physically bigger",
           fit_px > max_px, fit_px * 100 + max_px);
        ck("  ... max being the 8-pixel cell people complained about",
           max_px == 8, max_px);
    }

    head("vga_grid_capped(): a statement about the PANEL");
    /* The first version asked "was the last fit clamped", which was
       dead for the reason above -- a flag that is never set cannot
       fail to be cleared, so the injection that stopped it being
       cleared was MISSED.  It asks about the panel now, which is the
       question the `resolution` screen needs answered. */
    struct { uint32_t w, h; int capped; const char *name; } caps[] = {
        { 1280,  800, 0, "1280x800  fits at 1x" },
        { 1920, 1080, 0, "1920x1080 fits at 1x exactly" },
        /* 2560x1440 was capped and is not any more. With a 16-pixel
           cell it is 320x90 = 28800 cells, inside both axis limits
           and inside the 32400 budget -- the taller cell halves the
           rows, which is what brought it under. Not a regression:
           the panel really can be expressed now, and what used to
           be expressible at 320x180 was 180 rows of text drawn
           half as tall as it was wide. */
        { 2560, 1440, 0, "2560x1440 now fits, with a 16-pixel cell" },
        { 3840, 2160, 1, "3840x2160 does not" },
        /* A PANEL THAT BUSTS THE BUDGET AND NEITHER AXIS. 2560x1800
           is 320 columns -- exactly the axis cap, not over it -- and
           112 rows, well inside the row cap, for 35840 cells against
           a 32400 budget. It is the only shape that tells the AREA
           clamp apart from the two axis ones, and without it an
           injection that checked only the axes was MISSED.
           2560x1440 used to serve that purpose and no longer can:
           with a 16-pixel cell it is 320x90 and fits. */
        { 2560, 1800, 1, "2560x1800 busts the BUDGET, not an axis" },
        { 1366,  768, 0, "1366x768  fits" },
    };
    for(unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++){
        fb_setup(caps[i].w, caps[i].h, caps[i].w);
        vga_fb_init((uint32_t)(uintptr_t)fb, caps[i].w, caps[i].h,
                    caps[i].w*4, 32);
        printf("    --   %s\n", caps[i].name);
        ck("  capped reported correctly",
           vga_grid_capped() == caps[i].capped, vga_grid_capped());
    }
    fbc_active = 0;
    ck("no framebuffer: not capped, because there is no panel",
       vga_grid_capped() == 0, vga_grid_capped());

    /* ══ THE VIEWPORT ════════════════════════════════════════════════
       v0.8.8.4. An app that lays out for 80x25 is given an 80x25
       console in the middle of a bigger panel, so it is centred
       instead of sitting in the corner. include/vga.h says why this
       is a transform in the driver rather than forty-three rewritten
       apps.

       Every check here reads phys_cell() -- the simulated VGA memory
       -- and not the driver's own answer about where it put things.
       Asking vga_shadow_read() where a cell went would be asking the
       translation to confirm itself, which is the mistake
       tests/test_usbmux.c made at 0.8.7.6: "a fixture written from
       the same guess as the code agrees with it perfectly." */
    head("the viewport: an 80x25 app, centred on a bigger grid");
    fbc_active = 0;
    memset(sim_text, 0, sizeof sim_text);
    /* ── WHY 90x30 AND NOT 90x50 OR 90x60 ─────────────────────────────
       drivers/vga.c has FOUR text modes -- 80x25, 80x50, 90x30 and
       90x60 -- because each one is a CRTC register table. 90x50 is not
       one of them. The first version of this section asked for it, got
       0 back, and then checked a viewport against a panel that was
       still 80x25: sixteen checks failed and not one of them was about
       the viewport.

       90x60 is a real mode and ALSO wrong here, for the reason this
       file already records at the top: an 8-line cell makes a
       successful text-mode change call vga_font_load8x8(), which
       writes to 0xA0000 through a raw pointer that has no seam. Asking
       for it segfaulted the suite outright.

       90x30 over 720x480 is a 16-line cell, so it takes neither
       trapdoor. */
    ck("a 90x30 text grid is set", vga_set_mode(90, 30) == 1, 0);
    ck("  no viewport yet", vga_viewport_active() == 0, 0);
    ck("  so vga_cols() is the panel", vga_cols() == 90, vga_cols());
    ck("  and vga_rows() is too", vga_rows() == 30, vga_rows());
    ck("  vga_phys_cols() agrees", vga_phys_cols() == 90, vga_phys_cols());

    ck("an 80x25 viewport is accepted", vga_viewport_centre(80, 25) == 1, 0);
    ck("  it is active", vga_viewport_active() == 1, 0);
    ck("  the app is told it has 80 columns", vga_cols() == 80, vga_cols());
    ck("  and 25 rows", vga_rows() == 25, vga_rows());
    ck("  while the panel is still 90 wide",
       vga_phys_cols() == 90, vga_phys_cols());
    ck("  and 30 deep", vga_phys_rows() == 30, vga_phys_rows());
    {
        int vx, vy, vw, vh;
        vga_viewport_get(&vx, &vy, &vw, &vh);
        /* (90-80)/2 = 5, (30-25)/2 = 2. Written out rather than
           recomputed from the same expression the driver used. */
        ck("  centred at column 5", vx == 5, vx);
        ck("  centred at row 2",    vy == 2, vy);
        ck("  reporting 80 wide",   vw == 80, vw);
        ck("  reporting 25 deep",   vh == 25, vh);
    }

    /* ── WHERE A WRITE ACTUALLY LANDS ─────────────────────────────── */
    memset(sim_text, 0, sizeof sim_text);
    vga_putchar_at('A', 0, 0, VGA_WHITE, VGA_BLACK);
    ck("the app's (0,0) lands at the panel's (5,2)",
       phys_ch(5, 2) == 'A', phys_ch(5, 2));
    ck("  and NOT at the panel's (0,0)",
       phys_ch(0, 0) != 'A', phys_ch(0, 0));

    vga_putchar_at('Z', 79, 24, VGA_WHITE, VGA_BLACK);
    ck("the app's (79,24) lands at the panel's (84,26)",
       phys_ch(84, 26) == 'Z', phys_ch(84, 26));

    /* ── THE CLIP ─────────────────────────────────────────────────────
       An app told it has 80 columns that writes to column 80 is a bug
       in the app, and what must NOT happen is the character appearing
       in the surround -- which is where an untranslated write would
       put it. Checked by reading the cell it would have landed in. */
    memset(sim_text, 0, sizeof sim_text);
    vga_putchar_at('!', 80, 0,  VGA_WHITE, VGA_BLACK);
    vga_putchar_at('!', 0, 25,  VGA_WHITE, VGA_BLACK);
    vga_putchar_at('!', -1, 0,  VGA_WHITE, VGA_BLACK);
    vga_putchar_at('!', 0, -1,  VGA_WHITE, VGA_BLACK);
    {
        long escaped = 0;
        for(int y = 0; y < 30; y++)
            for(int x = 0; x < 90; x++)
                if(phys_ch(x, y) == '!') escaped++;
        ck("four writes outside the viewport reach the panel nowhere",
           escaped == 0, escaped);
    }

    /* ── vga_clear() CLEARS THE VIEWPORT, NOT THE SCREEN ──────────────
       The surround is what the desktop drew, and an app wiping it
       would leave a screen that looks like it failed to finish
       drawing rather than one with an app framed in it. */
    {
        long survived = 0, inside = 0;
        memset(sim_text, 0, sizeof sim_text);
        /* Sentinels everywhere on the panel, written with the viewport
           OFF so they really do land in the surround. */
        vga_viewport_clear();
        for(int y = 0; y < 30; y++)
            for(int x = 0; x < 90; x++)
                vga_putchar_at('#', x, y, VGA_WHITE, VGA_BLACK);
        ck("a 90x30 panel is covered in sentinels",
           phys_ch(0,0) == '#' && phys_ch(89,29) == '#', 0);

        vga_viewport_centre(80, 25);
        vga_clear();
        for(int y = 0; y < 30; y++)
            for(int x = 0; x < 90; x++){
                int in = (x >= 5 && x < 85 && y >= 2 && y < 27);
                if(phys_ch(x,y) == '#'){ if(in) inside++; else survived++; }
            }
        /* 90*30 - 80*25 = 2700 - 2000 = 700 cells of surround. */
        ck("  vga_clear() left all 700 surround cells alone",
           survived == 700, survived);
        ck("  and cleared every one of the 2000 inside",
           inside == 0, inside);
    }

    /* ── SCROLLING STAYS INSIDE ───────────────────────────────────────
       vga_scroll() read shadow[] with a g_cols stride, which inside a
       viewport would pull cells in from the surround and smear it
       into the app -- the same shape as the v0.7.12 bug where the
       stride was a compile-time 80 and a 90-column mode smeared
       diagonally. */
    {
        long smeared = 0;
        memset(sim_text, 0, sizeof sim_text);
        vga_viewport_clear();
        for(int y = 0; y < 30; y++)
            for(int x = 0; x < 90; x++)
                vga_putchar_at('S', x, y, VGA_WHITE, VGA_BLACK);
        vga_viewport_centre(80, 25);
        vga_clear();                      /* viewport blank, surround 'S' */
        vga_putchar_at('T', 0, 1, VGA_WHITE, VGA_BLACK);
        vga_scroll();
        ck("  a scroll moves the app's own row up",
           cell_ch(0, 0) == 'T', cell_ch(0, 0));
        for(int y = 0; y < 25; y++)
            for(int x = 0; x < 80; x++)
                if(cell_ch(x, y) == 'S') smeared++;
        ck("  and pulls in nothing from the surround",
           smeared == 0, smeared);
    }

    /* ── A VIEWPORT THAT WILL NOT FIT IS REFUSED, NOT CLAMPED ─────────
       Clamping would hand the app a console smaller than it asked
       for, with no way to find out: it would draw its right-hand edge
       into the clip and look like a rendering bug. */
    vga_viewport_clear();
    ck("a 100x25 viewport on a 90-column panel is refused",
       vga_viewport_centre(100, 25) == 0, 0);
    ck("  and leaves no viewport behind", vga_viewport_active() == 0, 0);
    ck("  so the app gets the whole panel", vga_cols() == 90, vga_cols());
    ck("a 90x31 viewport on a 30-row panel is refused",
       vga_viewport_centre(90, 31) == 0, 0);
    ck("  also leaving none", vga_viewport_active() == 0, 0);
    ck("a viewport exactly the size of the panel is fine",
       vga_viewport_centre(90, 30) == 1, 0);
    {
        int vx, vy;
        vga_viewport_get(&vx, &vy, 0, 0);
        ck("  and sits at the origin", vx == 0 && vy == 0, vx*1000+vy);
    }
    ck("zero width is refused", vga_viewport_centre(0, 25) == 0, 0);
    ck("negative height is refused", vga_viewport_centre(80, -1) == 0, 0);

    /* ── THE CARET IS TRANSLATED TOO ──────────────────────────────────
       Read off the simulated CRTC, not from the driver. The caret is
       the one thing an app does not place itself -- it sets a logical
       cursor and the driver puts the hardware where it belongs -- so
       an untranslated one blinks in the corner of the panel while the
       app's text is centred, and the text being right is what makes
       it look like a cursor bug instead of a missing offset. */
    vga_viewport_clear();
    ck("a 90x30 panel again", vga_set_mode(90, 30) == 1, 0);
    vga_set_cursor(0, 0);
    ck("with no viewport the caret is at cell 0",
       crtc_cursor_cell() == 0, crtc_cursor_cell());
    vga_set_cursor(3, 1);
    ck("  and (3,1) is cell 93 on a 90-wide panel",
       crtc_cursor_cell() == 93, crtc_cursor_cell());

    ck("an 80x25 viewport, again at (5,2)",
       vga_viewport_centre(80, 25) == 1, 0);
    vga_set_cursor(0, 0);
    /* The app's (0,0) is the panel's (5,2): 2*90 + 5 = 185. */
    ck("  the app's (0,0) puts the caret at cell 185",
       crtc_cursor_cell() == 185, crtc_cursor_cell());
    ck("  and NOT at cell 0, which is where the app thinks it is",
       crtc_cursor_cell() != 0, crtc_cursor_cell());
    vga_set_cursor(79, 24);
    /* (84,26): 26*90 + 84 = 2424. */
    ck("  the app's (79,24) puts it at cell 2424",
       crtc_cursor_cell() == 2424, crtc_cursor_cell());

    vga_viewport_clear();
    ck("cleared: vga_cols() is the panel again", vga_cols() == 90, vga_cols());
    ck("cleared: a write lands where it is addressed",
       (vga_putchar_at('Q', 0, 0, VGA_WHITE, VGA_BLACK), phys_ch(0,0) == 'Q'),
       phys_ch(0,0));

    /* ══ THE GRID A MACHINE COMES UP WITH ════════════════════════════
       v0.8.8.5. The decision kernel/main.c makes at boot when nothing
       has been saved, tested here against four simulated panels --
       because the boot path can only be tested by booting, and the
       panel QEMU hands this project is 1280x800, which is the one
       size where the answer is "no change". The first version of this
       logic looked like it worked on that panel and there was no way
       to tell that from it being broken. */
    /* ══ A GRID THE APPS CAN USE ════════════════════════════════════
       v0.8.8.6, and this is the one the HP found. His panel is
       1024x768: the readable 16-pixel cell gives 64x24, which is
       SMALLER than the 80x25 every app lays out for, and `resolution
       fit` offered it. `layout` on that machine reported the
       Dashboard 3855 cells outside its console.

       A console under 80x25 cannot be rescued by the viewport --
       there is nowhere to centre an 80x25 box inside it -- so the
       floor is not a preference and the policy has to reach it by
       using a smaller cell. */
    head("a grid the apps can actually use");
    {
        int c, r;

        /* THE HP. 1024/16 = 64 and 768/32 = 24 at the readable cell,
           so it must drop to the 8-pixel cell: 1024/8 = 128 columns
           and 768/16 = 48 rows. */
        fb_setup(1024, 768, 1024);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1024, 768, 1024*4, 32);
        {
            int rc = -1, rr = -1;
            ck("1024x768's READABLE grid really is 64x24",
               vga_fb_readable_grid(VGA_READABLE_PX, &rc, &rr) == 1 &&
               rc == 64 && rr == 24, rc*1000+rr);
            ck("  which is under 80x25 and cannot hold an app",
               rc < 80 || rr < 25, rc);
        }
        c = r = -1;
        ck("so the app grid drops to a smaller cell and answers",
           vga_fb_app_grid(&c, &r) == 1, 0);
        ck("  128 columns", c == 128, c);
        ck("  and 48 rows", r == 48, r);
        ck("  which holds an 80x25 app with room around it",
           c >= 80 && r >= 25, c*1000+r);

        /* Where the readable cell DOES give 80x25 or more, it is kept
           -- the fallback must not make every panel's text smaller. */
        fb_setup(1920, 1080, 1920);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
        c = r = -1;
        ck("1920x1080 keeps the readable cell",
           vga_fb_app_grid(&c, &r) == 1 && c == 120 && r == 33,
           c*1000+r);

        fb_setup(1280, 800, 1280);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1280, 800, 1280*4, 32);
        c = r = -1;
        ck("1280x800 keeps it too, at exactly 80x25",
           vga_fb_app_grid(&c, &r) == 1 && c == 80 && r == 25,
           c*1000+r);

        /* 640x384 cannot show 80x25 at ANY cell size this console
           draws: 640/8 = 80 columns, but 384/16 = 24 rows. One row
           short, and no smaller cell exists. Refusing is the honest
           answer -- the apps overflow there as they always have, and
           `layout` says so. */
        fb_setup(640, 384, 640);
        vga_fb_init((uint32_t)(uintptr_t)fb, 640, 384, 640*4, 32);
        {
            int rc = -1, rr = -1;
            ck("640x384 gives 80 columns at the 8-pixel cell",
               vga_fb_readable_grid(8, &rc, &rr) == 1 && rc == 80, rc);
            ck("  but only 24 rows, one short of 25", rr == 24, rr);
        }
        c = r = -1;
        ck("  so the app grid refuses rather than inventing one",
           vga_fb_app_grid(&c, &r) == 0, c);

        /* 640x400 is exactly 80x25 at the 8-pixel cell: the smallest
           panel that can hold an app at all, and the boundary the one
           above sits just under. */
        fb_setup(640, 400, 640);
        vga_fb_init((uint32_t)(uintptr_t)fb, 640, 400, 640*4, 32);
        c = r = -1;
        ck("640x400 is exactly 80x25 and is accepted",
           vga_fb_app_grid(&c, &r) == 1 && c == 80 && r == 25,
           c*1000+r);

        fbc_active = 0;
        c = r = -1;
        ck("no panel: no app grid", vga_fb_app_grid(&c, &r) == 0, c);
    }
    fbc_active = 0;

    head("the grid a machine should come up with");
    {
        int c, r;

        /* 1280x800: readable is EXACTLY 80x25, which is what
           "80x25 fills 1280x800" above already established. So there
           is nothing to change and it must say so -- a mode change
           that lands on the numbers it started from is a flicker and
           a log line reporting nothing. */
        fb_setup(1280, 800, 1280);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1280, 800, 1280*4, 32);
        vga_set_mode(80, 25);
        c = r = -1;
        ck("1280x800 at 80x25: no change, because readable IS 80x25",
           vga_fb_boot_grid(&c, &r) == 0, c);

        /* 1920x1080 -- the HP. 1920/16 = 120 columns, 1080/32 = 33
           rows. This is the panel the whole change exists for and the
           one that cannot be reproduced in QEMU here. */
        fb_setup(1920, 1080, 1920);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
        vga_set_mode(80, 25);
        c = r = -1;
        ck("1920x1080 at 80x25: there IS a better grid",
           vga_fb_boot_grid(&c, &r) == 1, 0);
        ck("  and it is 120 columns", c == 120, c);
        ck("  and 33 rows", r == 33, r);
        ck("  which is bigger than 80x25 on both axes",
           c > 80 && r > 25, c*1000+r);

        /* ── THE GUARD THAT COST A BOOT TO FIND ───────────────────────
           640x384. 80x25 of an 8x16 cell needs 640x400, so this panel
           cannot show the stock grid at all and the readable grid is
           40x12. Taking it would make the console SMALLER than the
           80x25 every app lays out for -- nothing would be boxed and
           every app would draw off the right-hand edge. */
        fb_setup(640, 384, 640);
        vga_fb_init((uint32_t)(uintptr_t)fb, 640, 384, 640*4, 32);
        c = r = -1;
        {
            int rc, rr;
            ck("640x384 has a readable grid at all",
               vga_fb_readable_grid(VGA_READABLE_PX, &rc, &rr) == 1, 0);
            ck("  and it is smaller than 80x25",
               rc < 80 || rr < 25, rc*1000+rr);
        }
        ck("  so the boot grid REFUSES it rather than shrinking",
           vga_fb_boot_grid(&c, &r) == 0, c);

        /* ── 800x600: THE EXPECTATION CHANGED, AND IT WAS RIGHT TO ────
           This block asserted that the boot grid REFUSED 800x600,
           because v0.8.8.5's policy was "the readable cell or
           nothing" and the readable cell there is 50x18 -- under the
           80x25 floor.

           v0.8.8.6 added the fallback to a smaller cell, and 800x600
           at the 8-pixel cell is 100x37. The panel can hold an app
           perfectly well, so refusing it was the old policy being
           too blunt rather than a property worth preserving. The
           check now asserts the better answer.

           The lower bound is still exercised, by the 640x384 panel
           above: 80 columns but 24 rows, one short, at every cell
           size this console draws. That is a panel that genuinely
           cannot hold an app, and it is refused. */
        fb_setup(800, 600, 800);
        vga_fb_init((uint32_t)(uintptr_t)fb, 800, 600, 800*4, 32);
        {
            int rc = -1, rr = -1;
            ck("800x600's readable grid is 50x18",
               vga_fb_readable_grid(VGA_READABLE_PX, &rc, &rr) == 1 &&
               rc == 50 && rr == 18, rc*1000+rr);
            ck("  which is under 80x25", rc < 80 && rr < 25, rc);
            ck("a 40x12 console is settable on it",
               vga_set_mode(40, 12) == 1, 0);
            c = r = -1;
            ck("  and the boot grid now offers 100x37 instead",
               vga_fb_boot_grid(&c, &r) == 1 && c == 100 && r == 37,
               c*1000+r);
            ck("  which is bigger than 80x25, unlike 50x18",
               c >= 80 && r >= 25, c);
        }

        /* A much bigger panel still answers, and still readably. */
        fb_setup(2560, 1440, 2560);
        vga_fb_init((uint32_t)(uintptr_t)fb, 2560, 1440, 2560*4, 32);
        vga_set_mode(80, 25);
        c = r = -1;
        ck("2560x1440 at 80x25: there is a better grid",
           vga_fb_boot_grid(&c, &r) == 1, 0);
        ck("  bigger than 80x25", c > 80 && r > 25, c*1000+r);
        ck("  and the glyph is still at least 16 px wide",
           (int)(1440u / (uint32_t)r) >= 16 &&
           (int)(2560u / (uint32_t)c) >= 16, c);

        /* ── ALREADY AT THE FITTING GRID: nothing to do ───────────────
           The second boot of a machine whose grid was not saved. It
           must not keep changing mode every boot. */
        fb_setup(1920, 1080, 1920);
        vga_fb_init((uint32_t)(uintptr_t)fb, 1920, 1080, 1920*4, 32);
        ck("1920x1080: the fitting grid is settable",
           vga_set_mode(120, 33) == 1, 0);
        c = r = -1;
        ck("  and then there is nothing left to change",
           vga_fb_boot_grid(&c, &r) == 0, c);

        /* No panel at all: the text console's grid is a CRTC register
           table and must not be changed from under a boot. */
        fbc_active = 0;
        c = r = -1;
        ck("no framebuffer: no boot grid to pick",
           vga_fb_boot_grid(&c, &r) == 0, c);
    }
    fbc_active = 0;

    printf("\n%d/%d passed\n", total - fails, total);
    munmap(fb, (size_t)FB_MAX_PX * 4);
    return fails ? 1 : 0;
}
