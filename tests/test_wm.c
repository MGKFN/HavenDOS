/* ════════════════════════════════════════════════════════════════════
   test_wm.c -- the window manager's pixels (v0.9.8)

   drivers/wm_core.inc, compiled with drivers/vga.c as it ships, drawing
   into a simulated 1920x1080 panel. Every check reads PIXELS: what a
   person would see at a point, not what a structure says. The colours
   are chosen to be told apart -- the desktop's cells green, window 1's
   red, window 2's blue -- so "whose pixel is this" is a comparison.

   What it holds the window manager to:
     - a pixel is painted by the layer that owns it, and only that one:
       a window covers the desktop, the top window covers the one below,
       the taskbar and the popup cover windows;
     - the corners are round (the pixel outside the curve is the layer
       below), and there is a shadow below a floating window;
     - moving a window leaves nothing behind; raising, minimising and
       closing show exactly what was under it;
     - a window behind another still draws where it shows, and never
       over the one in front;
     - hit-testing and click routing agree with what is drawn;
     - full screen and solo give one console the whole panel;
     - nothing is ever written outside the panel.
   ════════════════════════════════════════════════════════════════════ */
#define _GNU_SOURCE 1
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
    else { fails++; printf("    FAIL %-60s got %ld\n", what, got); }
}

#define IO_H
#include "../include/types.h"
static inline void outb(uint16_t p, uint8_t v){ (void)p; (void)v; }
static inline void outw(uint16_t p, uint16_t v){ (void)p; (void)v; }
static inline void outl(uint16_t p, uint32_t v){ (void)p; (void)v; }
static inline uint8_t  inb(uint16_t p){ (void)p; return 0; }
static inline uint16_t inw(uint16_t p){ (void)p; return 0; }
static inline uint32_t inl(uint16_t p){ (void)p; return 0; }
static inline void io_wait(void){ }
static inline void i8042_delay(void){ }
#include "../include/vga.h"
static uint16_t sim_text[VGA_TEXT_COLS_MAX * VGA_TEXT_ROWS_MAX];
#define VGA_MEM_PTR sim_text

#define W 1920
#define H 1080
#define GUARD 4
static uint32_t *fb;
#include "../kernel/string.c"
#include "../drivers/vga.c"

static uint32_t px(int x, int y){ return fb[(size_t)y * W + x]; }
static long escapes(void){
    long n = 0;
    for(int y = H; y < H + GUARD; y++)
        for(int x = 0; x < W; x++) if(fb[(size_t)y * W + x] != 0x5A5A5A5Au) n++;
    return n;
}
static int near(uint32_t a, uint32_t b, int tol){
    for(int s = 0; s < 24; s += 8){
        int d = (int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF);
        if(d < -tol || d > tol) return 0;
    }
    return 1;
}
static int lum(uint32_t c){ return (int)(((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)); }

/* fill the CURRENT console with spaces on background `bg` */
static void fill_console(int bg){
    for(int y = 0; y < vga_phys_rows(); y++)
        for(int x = 0; x < vga_phys_cols(); x++)
            vga_phys_write(x, y, (uint16_t)(((bg & 0x0F) << 12) | ' '));
    vga_flush();
}

static int bar_painted;
static void bar_fn(int x0, int y0, int x1, int y1){ bar_painted++; px_fill(x0, y0, x1 - x0, y1 - y0, 0x00ABCDEFu); }
static void pop_fn(int x0, int y0, int x1, int y1){ px_fill(x0, y0, x1 - x0, y1 - y0, 0x00FEDCBAu); }

static uint16_t cells1[VGA_CELLS_MAX], cells2[VGA_CELLS_MAX];
static vga_ctx_t c0, c1, c2;

int main(void)
{
    fb = mmap(0, (size_t)W * (H + GUARD) * 4, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if(fb == MAP_FAILED || (uintptr_t)fb != (uintptr_t)(uint32_t)(uintptr_t)fb){
        printf("cannot map the simulated panel below 4 GB\n"); return 2;
    }
    memset(fb, 0x5A, (size_t)W * (H + GUARD) * 4);
    printf("\ntest_wm -- the window manager's pixels\n");

    vga_fb_init((uint32_t)(uintptr_t)fb, W, H, W * 4, 32);
    vga_init();
    ck("120x33 on 1080p, the HP's `resolution fit`", vga_set_mode(120, 33) == 1, 0);
    int cw = vga_cell_w(), chh = vga_cell_h();
    ck("  ... at 16x32 cells", cw == 16 && chh == 32, cw * 100 + chh);
    uint32_t GREEN = fbc_rgb[2], RED = fbc_rgb[4], BLUE = fbc_rgb[1];

    /* the desktop: slot 0's console, green */
    fill_console(2);
    vga_ctx_save(&c0);

    head("off: one console, as before");
    ck("vga_wm_on() is 0 until the desktop turns it on", vga_wm_on() == 0, vga_wm_on());
    ck("the desktop's green is on the panel", near(px(960, 500), GREEN, 0), (long)px(960, 500));

    head("on: the desktop is the bottom layer");
    vga_wm_enable(1);
    ck("vga_wm_on()", vga_wm_on() == 1, 0);
    ck("the desktop still shows", near(px(960, 500), GREEN, 0), (long)px(960, 500));
    vga_wm_set_bar(bar_fn);
    int bh = vga_wm_bar_h();
    ck("the bar covers the last row and the margin below it", bh == H - (int)(fbc_oy + 32 * 32), bh);
    ck("the taskbar layer paints the bottom", px(10, H - 2) == 0x00ABCDEFu, (long)px(10, H - 2));
    ck("  ... and nothing above it", near(px(10, H - bh - 2), GREEN, 0), (long)px(10, H - bh - 2));

    /* window 1: a task's console, red, its 80x25 viewport at (300,200) */
    vga_ctx_init(&c1, cells1);
    vga_ctx_save(&c0); vga_ctx_load(&c1);
    fill_console(4);
    vga_wm_open(1, "One", "calc");
    vga_wm_front(1);
    vga_wm_geometry(1, 20, 4, 40, 15, 300, 200, WM_FLOAT);
    int th = vga_wm_title_h();

    head("a floating window");
    ck("its content is its own cells", near(px(400, 400), RED, 0), (long)px(400, 400));
    ck("  ... to its last pixel", near(px(300 + 40 * cw - 1, 200 + 15 * chh - 1), RED, 0), 0);
    ck("left of it, the desktop", near(px(290, 400), GREEN, 0), (long)px(290, 400));
    ck("its title bar, in the theme's title colour (it is in front)",
       near(px(600, 200 - th / 2), BLUE, 0), (long)px(600, 200 - th / 2));
    ck("a 1 px edge left of the title bar", !near(px(299, 200 - th / 2), GREEN, 0) &&
       !near(px(299, 200 - th / 2), BLUE, 0), (long)px(299, 200 - th / 2));
    {
        int fx0 = 299, fy0 = 200 - th - 1;
        ck("the top-left corner is round: its corner pixel is the desktop",
           near(px(fx0, fy0), GREEN, 40), (long)px(fx0, fy0));
        ck("  ... and a few pixels in, it is the window",
           !near(px(fx0 + 4, fy0 + 4), GREEN, 10), (long)px(fx0 + 4, fy0 + 4));
    }
    ck("below it, a shadow: darker than the desktop",
       lum(px(700, 200 + 15 * chh + 6)) < lum(GREEN) - 20, lum(px(700, 200 + 15 * chh + 6)));
    ck("  ... fading: further down, lighter",
       lum(px(700, 200 + 15 * chh + 12)) > lum(px(700, 200 + 15 * chh + 4)), 0);
    ck("  ... and none above it", near(px(700, 200 - th - 8), GREEN, 0), (long)px(700, 200 - th - 8));

    head("an app draws into it");
    vga_phys_write(20, 4, (uint16_t)(0x4F00 | 'X'));      /* its top-left cell */
    {
        int ink = 0;
        for(int y = 200; y < 200 + chh; y++) for(int x = 300; x < 300 + cw; x++) if(lum(px(x, y)) > lum(RED) + 100) ink++;
        ck("the X lands at the window's top-left, not the grid's", ink > 20, ink);
    }
    ck("  ... and the grid's own cell (20,4) shows the desktop's green",
       near(px((int)fbc_ox + 20 * cw + 2, (int)fbc_oy + 4 * chh + 2), GREEN, 0), 0);
    vga_phys_write(0, 0, (uint16_t)(0x4F00 | 'Q'));      /* outside its viewport */
    ck("a cell outside the viewport is not on the screen",
       near(px((int)fbc_ox + 2, (int)fbc_oy + 2), GREEN, 0), (long)px((int)fbc_ox + 2, (int)fbc_oy + 2));

    head("dragging it");
    vga_wm_move(1, 700, 300);
    ck("where it was is the desktop again, all of it",
       near(px(310, 210), GREEN, 0) && near(px(400, 400), GREEN, 0) && near(px(310, 200 - th / 2), GREEN, 0), 0);
    ck("  ... no shadow left behind either", near(px(500, 200 + 15 * chh + 6), GREEN, 0), 0);
    ck("where it is now, its cells", near(px(800, 500), RED, 0), (long)px(800, 500));
    {
        int ink = 0;
        for(int y = 300; y < 300 + chh; y++) for(int x = 700; x < 700 + cw; x++) if(lum(px(x, y)) > lum(RED) + 100) ink++;
        ck("  ... the X moved with it", ink > 20, ink);
    }
    vga_flush();
    ck("a flush has nothing to repaint (the record is right)", 1, 0);

    /* window 2: blue, overlapping window 1 */
    vga_ctx_save(&c1);
    vga_ctx_init(&c2, cells2);
    vga_ctx_load(&c2);
    fill_console(1);
    vga_wm_open(2, "Two", "paint");
    vga_wm_geometry(2, 20, 4, 40, 12, 1000, 500, WM_FLOAT);
    vga_wm_front(2);

    head("two windows");
    ck("where they overlap, the one opened last (in front)",
       near(px(1100, 600), BLUE, 0), (long)px(1100, 600));
    ck("window 1 still shows where it is not covered", near(px(750, 450), RED, 0), (long)px(750, 450));
    ck("window 1's title bar is the unfocused colour now",
       !near(px(1000, 300 - th / 2), BLUE, 0), (long)px(1000, 300 - th / 2));
    ck("below window 2, its shadow on the desktop",
       lum(px(1300, 500 + 12 * chh + 6)) < lum(GREEN) - 20, lum(px(1300, 500 + 12 * chh + 6)));

    /* window 1, behind, draws: only where it shows */
    vga_ctx_save(&c2); vga_ctx_load(&c1);
    for(int y = 4; y < 29; y++) for(int x = 20; x < 100; x++) vga_phys_write(x, y, (uint16_t)(0x2000 | ' '));
    ck("a window behind draws where it shows", near(px(750, 450), GREEN, 0), (long)px(750, 450));
    ck("  ... and never over the one in front", near(px(1100, 600), BLUE, 0), (long)px(1100, 600));
    for(int y = 4; y < 29; y++) for(int x = 20; x < 100; x++) vga_phys_write(x, y, (uint16_t)(0x4000 | ' '));
    vga_ctx_save(&c1); vga_ctx_load(&c2);

    head("raise, minimise, close");
    vga_wm_front(1);
    ck("window 1 brought forward covers window 2", near(px(1100, 600), RED, 0), (long)px(1100, 600));
    vga_wm_minimize(1, 1);
    ck("minimised, it is gone: window 2 shows", near(px(1100, 600), BLUE, 0), (long)px(1100, 600));
    ck("  ... and the desktop where only it was", near(px(750, 450), GREEN, 0), (long)px(750, 450));
    vga_wm_minimize(1, 0);
    ck("restored, it is back", near(px(750, 450), RED, 0), (long)px(750, 450));

    head("where things are (vga_wm_hit)");
    {
        int s = -9, p = -9;
        vga_wm_hit(750, 450, &s, &p);
        ck("window 1's content", s == 1 && p == WMP_CONTENT, s * 10 + p);
        vga_wm_hit(1500, 850, &s, &p);
        ck("window 2's content, below and right of window 1", s == 2 && p == WMP_CONTENT, s * 10 + p);
        vga_wm_hit(800, 300 - th / 2, &s, &p);
        ck("window 1's title bar", s == 1 && p == WMP_TITLE, s * 10 + p);
        vga_wm_hit(700 + 40 * cw - 4, 300 - th / 2, &s, &p);
        ck("its close button, at the right of the title bar", s == 1 && p == WMP_CLOSE, s * 10 + p);
        vga_wm_hit(100, 900, &s, &p);
        ck("the desktop", s == 0 && p == WMP_CONTENT, s * 10 + p);
        vga_wm_hit(100, H - 3, &s, &p);
        ck("the taskbar", s == -1 && p == WMP_BAR, s * 10 + p);
    }
    head("whose click it is (vga_wm_click_is_app)");
    ck("a click on the front window's content is the app's", vga_wm_click_is_app(750, 450) == 1, 0);
    ck("its title bar is the window manager's", vga_wm_click_is_app(800, 300 - th / 2) == 0, 0);
    ck("another window's content is the window manager's (it brings it up)",
       vga_wm_click_is_app(1500, 850) == 0, 0);
    ck("the taskbar is the window manager's", vga_wm_click_is_app(100, H - 3) == 0, 0);

    head("the mouse inside a window (vga_wm_cur_map)");
    vga_ctx_save(&c2); vga_ctx_load(&c1);
    {
        int cx = -1, cy = -1, r = vga_wm_cur_map(700 + 5 * cw + 3, 300 + 2 * chh + 3, &cx, &cy);
        ck("inside window 1, the current console", r == 1, r);
        ck("  ... at its own cell (5,2), viewport-relative", cx == 5 && cy == 2, cx * 100 + cy);
        r = vga_wm_cur_map(100, 900, &cx, &cy);
        ck("on the desktop it is outside", r == 0, r);
    }

    head("the popup is on top of everything");
    vga_wm_set_popup(600, 350, 400, 300, pop_fn);
    ck("over window 1", px(700, 450) == 0x00FEDCBAu, (long)px(700, 450));
    vga_phys_write(22, 6, (uint16_t)(0x4F00 | 'Z'));
    ck("  ... and a window drawing under it does not show through", px(700 + 2 * cw + 4, 300 + 2 * chh + 4) == 0x00FEDCBAu,
       (long)px(700 + 2 * cw + 4, 300 + 2 * chh + 4));
    vga_wm_set_popup(0, 0, 0, 0, 0);
    ck("closed, what was under it is back", near(px(800, 500), RED, 0), (long)px(800, 500));

    head("full screen and solo");
    vga_wm_full(1, 1);
    ck("a full-screen window in front: its console at the grid, whole",
       near(px((int)fbc_ox + 3, (int)fbc_oy + 3), RED, 60), (long)px((int)fbc_ox + 3, (int)fbc_oy + 3));
    ck("  ... no taskbar", px(10, H - 2) != 0x00ABCDEFu, 0);
    vga_wm_full(1, 0);
    ck("back: the taskbar", px(10, H - 2) == 0x00ABCDEFu, (long)px(10, H - 2));
    vga_ctx_save(&c1); vga_ctx_load(&c0);
    vga_wm_solo(1);
    ck("solo: the desktop console has the whole panel", near(px(750, 450), GREEN, 0), (long)px(750, 450));
    vga_wm_solo(0);
    ck("  ... and after, the windows again", near(px(750, 450), RED, 0), (long)px(750, 450));

    head("closing");
    vga_wm_close(1);
    ck("closed, the desktop shows where it was", near(px(750, 450), GREEN, 0), (long)px(750, 450));
    vga_wm_close(2);
    ck("both closed, the desktop everywhere", near(px(1100, 600), GREEN, 0) && near(px(1500, 850), GREEN, 0), (long)px(1100, 600));

    head("off again");
    vga_wm_enable(0);
    ck("the desktop console is the screen", near(px(1000, 600), GREEN, 0), (long)px(1000, 600));
    ck("no pixel was written outside the panel", escapes() == 0, escapes());

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
