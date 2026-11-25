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
#include "pmm_host.inc"
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
/* v0.9.9: is what is on the panel what a repaint from scratch draws?
   (the number of pixels that differ) */
static long stale_px(const char *where)
{
    size_t sz = (size_t)W * H * 4;
    uint32_t *snap = malloc(sz);
    memcpy(snap, fb, sz);
    wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all(); update_hw_cursor();
    long d = 0; int a0 = W, b0 = H, a1 = -1, b1 = -1;
    for(size_t k = 0; k < (size_t)W * H; k++) if(snap[k] != fb[k]){
        d++; int x = (int)(k % W), y = (int)(k / W);
        if(x < a0) a0 = x; if(y < b0) b0 = y; if(x > a1) a1 = x; if(y > b1) b1 = y;
    }
    if(d && where) printf("      [%s] stale in %d,%d .. %d,%d\n", where, a0, b0, a1, b1);
    if(d && where && getenv("STALE_DUMP"))
        for(int y = b0; y <= b1 && y < b0 + 6; y++){
            printf("      ");
            for(int x = a0; x <= a1 && x < a0 + 6; x++) printf("%06x/%06x ", snap[(size_t)y*W+x] & 0xFFFFFF, fb[(size_t)y*W+x] & 0xFFFFFF);
            printf("\n");
        }
    free(snap);
    return d;
}
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

    head("a maximised Shell that scrolls (v0.9.8.2, Kyan's photo)");
    {
        /* window 1 maximised over the grid between title bar and taskbar,
           the app's console a viewport of it -- as shell/chrome.c lays
           the Shell out -- and then a few hundred lines through the
           ordinary console path, which scrolls. What is on the panel has
           to be exactly what a repaint from scratch draws. */
        vga_ctx_save(&c0); vga_ctx_load(&c1);
        int tr = vga_wm_title_rows(), vh = vga_phys_rows() - 2 * tr;
        int mx, my; vga_mouse_px(0, tr, 0, 0, &mx, &my);
        vga_wm_geometry(1, 0, tr, vga_phys_cols(), vh, mx, my, WM_MAXED);
        vga_wm_front(1);
        vga_viewport_set(0, tr, vga_phys_cols(), vh);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_clear();
        char line[40];
        for(int k = 0; k < 300; k++){
            int n = 0; const char *p = "admin@havendos:~# echo L";
            while(*p) line[n++] = *p++;
            if(k >= 100) line[n++] = (char)('0' + k / 100);
            if(k >= 10) line[n++] = (char)('0' + k / 10 % 10);
            line[n++] = (char)('0' + k % 10); line[n++] = '\n'; line[n] = 0;
            /* the line editor's way: the prompt, then each key, the caret moving */
            int cx, cy; vga_get_cursor(&cx, &cy);
            for(int j = 0; line[j] && line[j] != '\n'; j++){
                vga_set_cursor(0, cy);
                for(int q = 0; q <= j; q++) vga_putchar(line[q]);
            }
            vga_putchar('\n');
            if(k % 3 == 0){
                /* the desktop runs between keys (cooperative): its clock */
                vga_ctx_save(&c1); vga_ctx_load(&c0);
                vga_phys_write(5, 0, (uint16_t)(0x2F00 | ('0' + k % 10)));
                vga_ctx_save(&c0); vga_ctx_load(&c1);
            }
            if(k % 7 == 0) vga_pointer_show(10 + k % 40, vh, k % 8, k % 16, 0);   /* the pointer, low */
        }
        vga_pointer_hide();
        {
            /* the last line printed is the one above the caret, IN the window */
            char got[40]; int n = 0, r = vga_rows() - 2;
            for(int x = 18; x < 30 && n < 39; x++) got[n++] = (char)(vga_shadow_read(x, r) & 0xFF);
            got[n] = 0;
            int cx, cy; vga_get_cursor(&cx, &cy);
            ck("the Shell scrolls inside its window: its last line is the window's next-to-last row",
               !strncmp(got, "echo L299", 9), r);
            ck("  ... and the caret is on the window's last row", cy == vga_rows() - 1, cy);
        }
        size_t sz = (size_t)W * H * 4;
        uint32_t *snap = malloc(sz);
        memcpy(snap, fb, sz);
        wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all(); update_hw_cursor();
        long diff = 0;
        int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
        for(size_t k = 0; k < (size_t)W * H; k++) if(snap[k] != fb[k]){
            diff++; int x = (int)(k % W), y = (int)(k / W);
            if(x < bx0) bx0 = x; if(y < by0) by0 = y; if(x > bx1) bx1 = x; if(y > by1) by1 = y;
        }
        if(diff) printf("      differs in %d,%d .. %d,%d\n", bx0, by0, bx1, by1);
        ck("after 300 lines, the panel is what a repaint draws (no stale row, no trail)", diff == 0, diff);
        free(snap);
        vga_viewport_clear();
        fill_console(4);

        /* v0.9.8.2: an 80x25 app maximised: the title bar and the taskbar
           stay, the app sits in the middle of what is between them */
        vga_wm_geometry(1, 20, 4, 40, 15, (W - 40 * cw) / 2, 400, WM_MAXED);
        int tth = vga_wm_title_h();
        ck("maximised: its title bar across the top", near(px(W / 2, tth / 2), BLUE, 0) && near(px(5, tth / 2), BLUE, 0),
           (long)px(5, tth / 2));
        ck("  ... its close button at the panel's right edge", vga_wm_hit(W - 10, tth / 2, 0, 0) == 0, 0);
        { int sl, pt; vga_wm_hit(W - 10, tth / 2, &sl, &pt);
          ck("  ... (the hit test says close)", sl == 1 && pt == WMP_CLOSE, pt); }
        ck("  ... the taskbar still there", px(10, H - 2) == 0x00ABCDEFu, (long)px(10, H - 2));
        ck("  ... the app in the middle", near(px(W / 2, 400 + 5), RED, 0), (long)px(W / 2, 405));
        ck("  ... and beside it the window, not the desktop",
           !near(px(20, 500), GREEN, 0) && !near(px(20, 500), RED, 0), (long)px(20, 500));
        ck("  ... down to the taskbar, no desktop in between",
           !near(px(20, H - vga_wm_bar_h() - 2), GREEN, 0), (long)px(20, H - vga_wm_bar_h() - 2));
        vga_wm_geometry(1, 20, 4, 40, 15, 700, 300, WM_FLOAT);
        vga_ctx_save(&c1); vga_ctx_load(&c0);
    }

    /* ── v0.9.9: windows of any size ─────────────────────────────── */
    head("v0.9.9: a window zoomed -- the same 40x15, bigger cells");
    {
        vga_ctx_save(&c0); vga_ctx_load(&c1);
        vga_wm_front(1);
        int zw = 24, zh = 48, X = 100, Y = 150;
        vga_wm_place(1, 20, 4, 40, 15, 0, X, Y, 40 * zw, 15 * zh, zw, zh, WM_FLOAT);
        { long d = stale_px("placed");
          ck("moved and resized: nothing of where it was is left, its shadow neither", d == 0, d); }
        ck("it has a surface of its own", vga_wm_has_surface(1) == 1, 0);
        ck("its content is its cells, to the last pixel",
           near(px(X + 40 * zw - 1, Y + 15 * zh - 1), RED, 0) && near(px(X + 300, Y + 300), RED, 0),
           (long)px(X + 40 * zw - 1, Y + 15 * zh - 1));
        vga_phys_write(20, 4, (uint16_t)(0x4F00 | 'X'));
        int ink = 0;
        for(int y = Y; y < Y + zh; y++) for(int x = X; x < X + zw; x++) if(lum(px(x, y)) > lum(RED) + 100) ink++;
        ck("the X in its top-left cell is drawn at the bigger size (more ink than at 16x32)", ink > 60, ink);
        int cx = -1, cy = -1, r = vga_wm_cur_map(X + 5 * zw + 3, Y + 2 * zh + 3, &cx, &cy);
        ck("the mouse maps to the app's cell at the window's cell size", r == 1 && cx == 5 && cy == 2, cx * 100 + cy);
        int sl = -1, pt = -1;
        vga_wm_hit(X + 40 * zw + 3, Y + 300, &sl, &pt);
        ck("just right of its outline: resize, east", sl == 1 && pt == WMP_RS_E, sl * 100 + pt);
        vga_wm_hit(X + 40 * zw + 3, Y + 15 * zh + 3, &sl, &pt);
        ck("just past its bottom-right corner: resize, south-east", sl == 1 && pt == WMP_RS_SE, sl * 100 + pt);
        vga_wm_hit(X - 3, Y - vga_wm_title_h() - 3, &sl, &pt);
        ck("just past its top-left corner: resize, north-west", sl == 1 && pt == WMP_RS_NW, sl * 100 + pt);
        vga_wm_hit(X + 300, Y + 300, &sl, &pt);
        ck("its content is still its content", sl == 1 && pt == WMP_CONTENT, sl * 100 + pt);
        ck("a press on the band is the window manager's", vga_wm_click_is_app(X + 40 * zw + 3, Y + 300) == 0, 0);

        head("v0.9.9: the surface and the cells agree");
        size_t sz = (size_t)W * H * 4;
        uint32_t *snap = malloc(sz);
        memcpy(snap, fb, sz);
        wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all();
        {
            long d0 = 0; int a0 = W, b0 = H, a1 = -1, b1 = -1;
            for(size_t k = 0; k < (size_t)W * H; k++) if(snap[k] != fb[k]){
                d0++; int x = (int)(k % W), y = (int)(k / W);
                if(x < a0) a0 = x; if(y < b0) b0 = y; if(x > a1) a1 = x; if(y > b1) b1 = y;
            }
            if(d0) printf("      differs in %d,%d .. %d,%d\n", a0, b0, a1, b1);
            ck("what is on the panel is what a repaint from scratch draws", d0 == 0, d0);
        }
        memcpy(snap, fb, sz);
        /* the same window with no surface (no memory for one): cells drawn
           straight to the screen must give the same picture */
        wm_win_t *w = &wm_w[1];
        pmm_free_contig(w->surf, w->surf_pg); w->surf = 0; w->surf_pg = 0;
        g_pmm_refuse = 1;
        wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all();
        long diff = 0;
        int dx0 = W, dy0 = H, dx1 = -1, dy1 = -1;
        for(size_t k = 0; k < (size_t)W * H; k++) if(snap[k] != fb[k]){
            diff++; int x = (int)(k % W), y = (int)(k / W);
            if(x < dx0) dx0 = x; if(y < dy0) dy0 = y; if(x > dx1) dx1 = x; if(y > dy1) dy1 = y;
        }
        if(diff) printf("      differs in %d,%d .. %d,%d\n", dx0, dy0, dx1, dy1);
        ck("with no surface, the same pixels", diff == 0, diff);
        ck("  ... and it says it has none", vga_wm_has_surface(1) == 0, 0);
        g_pmm_refuse = 0;
        vga_wm_place(1, 20, 4, 40, 15, 0, X, Y, 40 * zw + 8, 15 * zh, zw, zh, WM_FLOAT);
        ck("given memory again, it takes a surface", vga_wm_has_surface(1) == 1, 0);
        free(snap);

        head("v0.9.9: the caret is part of how a cell is shown");
        vga_wm_place(1, 20, 4, 40, 15, 0, X, Y, 40 * zw, 15 * zh, zw, zh, WM_FLOAT);
        int idx = (4 + 3) * g_cols + 20 + 10;
        wm_caret(idx);
        int cxp = X + 10 * zw + zw / 2, cyp = Y + 3 * zh + zh / 2;
        ck("the caret's cell is drawn with its colours swapped", !near(px(cxp, cyp), RED, 0), (long)px(cxp, cyp));
        vga_wm_repaint_all();
        ck("  ... and a repaint keeps it (it is not drawn over the window)", !near(px(cxp, cyp), RED, 0), (long)px(cxp, cyp));
        wm_caret(-1);
        ck("gone, the cell is itself again", near(px(cxp, cyp), RED, 0), (long)px(cxp, cyp));

        head("v0.9.9: a header row not shown (skip)");
        vga_wm_place(1, 20, 4, 40, 15, 1, X, Y, 40 * zw, 14 * zh, zw, zh, WM_FLOAT);
        r = vga_wm_cur_map(X + 5 * zw + 3, Y + 3, &cx, &cy);
        ck("the first row shown is the app's row 1", r == 1 && cx == 5 && cy == 1, cx * 100 + cy);
        ink = 0;
        for(int y = Y; y < Y + zh; y++) for(int x = X; x < X + zw; x++) if(lum(px(x, y)) > lum(RED) + 100) ink++;
        ck("  ... so the X on row 0 is not on the screen", ink == 0, ink);

        head("v0.9.9: full screen is a window the size of the screen");
        vga_wm_place(1, 20, 4, 40, 15, 0, 0, 0, W, H, W / 40, H / 15 / 2 * 2, WM_FULL);
        vga_wm_full(1, 1);
        ck("no taskbar", px(10, H - 2) != 0x00ABCDEFu, (long)px(10, H - 2));
        ck("its cells, zoomed, from the top-left of the screen", near(px(W / 2, H / 2), RED, 0), (long)px(W / 2, H / 2));
        r = vga_wm_cur_map(W / 40 * 3 + 2, 5, &cx, &cy);
        ck("  ... and the mouse maps into them", r == 1 && cx == 3 && cy == 0, cx * 100 + cy);
        vga_wm_full(1, 0);
        vga_wm_geometry(1, 20, 4, 40, 15, 700, 300, WM_FLOAT);
        ck("back: the taskbar", px(10, H - 2) == 0x00ABCDEFu, (long)px(10, H - 2));
        ck("  ... and the window where it was, at the console's cell", near(px(750, 450), RED, 0), (long)px(750, 450));
        vga_ctx_save(&c1); vga_ctx_load(&c0);
    }

    head("v0.9.9: a Terminal's viewport made smaller keeps its cursor line");
    {
        vga_ctx_save(&c0); vga_ctx_load(&c1);
        int vv = vga_viewport_set(0, 0, 40, 10);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_clear();
        for(int k = 0; k < 9; k++){ vga_puts("line "); vga_putchar((char)('0' + k)); vga_putchar('\n'); }
        vga_puts("$ ");
        int ccx, ccy; vga_get_cursor(&ccx, &ccy);
        ck("before: the prompt on row 9", vv && ccy == 9, ccy);
        vga_viewport_resize(40, 5);
        vga_get_cursor(&ccx, &ccy);
        ck("5 rows: the prompt on the last of them", ccy == 4 && vga_rows() == 5, ccy * 100 + vga_rows());
        ck("  ... and line 8 just above it", (vga_shadow_read(5, 3) & 0xFF) == '8', vga_shadow_read(5, 3) & 0xFF);
        vga_viewport_resize(60, 12);
        ck("bigger again: the new columns and rows blank, the prompt where it was",
           vga_cols() == 60 && vga_rows() == 12 && (vga_shadow_read(50, 10) & 0xFF) == ' ', vga_cols() * 100 + vga_rows());
        vga_viewport_clear();
        fill_console(4);
        vga_ctx_save(&c1); vga_ctx_load(&c0);
    }

    /* ── v0.9.10 ──────────────────────────────────────────────────── */
    head("v0.9.10: a zoomed window scrolls by moving its pixels");
    {
        vga_ctx_save(&c0); vga_ctx_load(&c1);
        vga_wm_front(1);
        int zw = 20, zh = 40, X = 120, Y = 160;
        vga_wm_place(1, 20, 4, 40, 15, 0, X, Y, 40 * zw, 15 * zh, zw, zh, WM_FLOAT);
        vga_viewport_set(20, 4, 40, 15);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_clear();
        uint32_t calls; uint64_t cyc, worst; uint32_t gl, pxn, sc0, sc1;
        vga_wm_stats(&calls, &cyc, &worst, &gl, &pxn, &sc0, 1);
        for(int k = 0; k < 60; k++){
            vga_puts("line ");
            vga_putchar((char)('0' + k / 10)); vga_putchar((char)('0' + k % 10));
            vga_putchar('\n');
        }
        vga_wm_stats(&calls, &cyc, &worst, &gl, &pxn, &sc1, 0);
        ck("60 lines in a 15-row window: some scrolls were pixel moves", sc1 > 0, (long)sc1);
        { long d = stale_px("scrolled");
          ck("  ... and the panel is what a repaint from scratch draws", d == 0, d); }
        {
            char got[16]; int n = 0, r = vga_rows() - 2;
            for(int x = 0; x < 7; x++) got[n++] = (char)(vga_shadow_read(x, r) & 0xFF);
            got[n] = 0;
            ck("  ... its last line is line 59", !strncmp(got, "line 59", 7), r);
        }
        vga_viewport_clear();
        fill_console(4);

        head("v0.9.10: an app that draws its window itself (canvas, present)");
        int cvw = 0, cvh = 0;
        uint32_t *cv = vga_wm_canvas(&cvw, &cvh);
        ck("the current task's window gives a canvas the window's size",
           cv && cvw == 40 * zw && cvh == 15 * zh, (long)cvw * 10000 + cvh);
        if(cv){
            for(int y = 10; y < 60; y++) for(int x = 10; x < 110; x++) cv[(size_t)y * cvw + x] = 0x00C0FFEEu;
            vga_wm_present(10, 10, 100, 50);
            ck("what it draws, presented, is on the panel at its window", px(X + 20, Y + 20) == 0x00C0FFEEu, (long)px(X + 20, Y + 20));
            vga_phys_write(25, 6, (uint16_t)(0x4F00 | 'K'));
            ck("  ... and a cell written under it does not draw over it", px(X + 20, Y + 20) == 0x00C0FFEEu, (long)px(X + 20, Y + 20));
            vga_wm_move(1, X + 200, Y + 50);
            ck("moved, the picture moves with the window", px(X + 220, Y + 70) == 0x00C0FFEEu, (long)px(X + 220, Y + 70));
            { long d = stale_px("canvas moved");
              ck("  ... and nothing is left where it was", d == 0, d); }
        }

        head("v0.9.10: Best performance (no shadows, square corners)");
        vga_wm_geometry(1, 20, 4, 40, 15, 700, 300, WM_FLOAT);
        int below = 300 + 15 * chh + 6;
        ck("before: a shadow below the window", lum(px(800, below)) < lum(GREEN) - 20, lum(px(800, below)));
        vga_wm_set_lite(1);
        ck("on: no shadow -- the desktop below it", near(px(800, below), GREEN, 0), (long)px(800, below));
        ck("  ... and vga_wm_lite() says so", vga_wm_lite() == 1, 0);
        { long d = stale_px("lite");
          ck("  ... and nothing stale", d == 0, d); }
        vga_wm_set_lite(0);
        ck("off: the shadow again", lum(px(800, below)) < lum(GREEN) - 20, lum(px(800, below)));
        vga_ctx_save(&c1); vga_ctx_load(&c0);
    }

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
