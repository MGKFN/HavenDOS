/* tests/test_window.c -- an app's window: where everything goes (v0.9.6.5)
 *
 * The pure half of shell/chrome.c: chrome_layout() and chrome_hit(),
 * compiled out of the file itself. The HP's grid is 128x48; QEMU's
 * default is 80x25, where there is no room and nothing must change.
 */
#include <stdio.h>
#include <stdint.h>

/* chrome.c's non-pure half needs these to compile; the tests never
   reach it */
#include "../include/task.h"
int task_current(void){ return 0; }
int task_front(void){ return 0; }
void kpoll_sleep_ms(uint32_t ms){ (void)ms; }
uint32_t task_id(int s){ (void)s; return 0; }
int task_ready(void){ return 1; }
int task_state(int s){ (void)s; return 0; }
const char *task_name(int s){ (void)s; return ""; }
int task_set_front(int s){ (void)s; return 0; }
int task_wait_front(void){ return 0; }
/* v0.9.6.6: a screen with a grid that can change, so full screen's
   grow (80x25 at a bigger cell) and its way back can be followed cell
   by cell. 128x48 is the HP. */
static int      g_pc = 128, g_pr = 48, g_fillon, fills, modes;
static uint16_t g_scr[240 * 135];
int vga_phys_cols(void){ return g_pc; }
int vga_phys_rows(void){ return g_pr; }
uint16_t vga_phys_read(int x, int y){ return (x >= 0 && y >= 0 && x < g_pc && y < g_pr) ? g_scr[y * g_pc + x] : 0; }
void vga_phys_write(int x, int y, uint16_t v){ if(x >= 0 && y >= 0 && x < g_pc && y < g_pr) g_scr[y * g_pc + x] = v; }
int vga_fb_active(void){ return 1; }
/* v0.9.7 (E-4): the taskbar's pins, as shell/desktop.c would give them */
static int g_npins, g_pin_opened = -1;
int         desktop_tbpin_count(void){ return g_npins; }
const char *desktop_tbpin_icon(int i){ (void)i; return "FM"; }
void        desktop_tbpin_open(int i){ g_pin_opened = i; }
int vga_set_mode(int c, int r){ modes++; g_pc = c; g_pr = r; g_fillon = 0; for(int i = 0; i < 240 * 135; i++) g_scr[i] = 0x0720; return 1; }
int vga_set_mode_fill(int c, int r){ fills++; vga_set_mode(c, r); g_fillon = 1; return 1; }
int vga_viewport_active(void){ return 0; }
void vga_viewport_get(int *x, int *y, int *w, int *h){ *x = *y = *w = *h = 0; }
int vga_viewport_set(int x, int y, int w, int h){ (void)x; (void)y; (void)w; (void)h; return 1; }
void vga_viewport_clear(void){}
void mouse_get_phys(int *x, int *y){ *x = *y = 0; }
/* v0.9.7.10: no framebuffer here -- the cell pointer, no decoration */
void mouse_get_frac(int *x, int *y){ *x = *y = 0; }
int  mouse_pointer_wanted(void){ return 1; }
int  vga_pointer_pixels(void){ return 0; }
int  vga_pointer_show(int c, int r, int fx, int fy, int d){ (void)c;(void)r;(void)fx;(void)fy;(void)d; return 0; }
void vga_pointer_hide(void){}
void vga_deco_window(int s, int a, int b, int c, int d){ (void)s;(void)a;(void)b;(void)c;(void)d; }
void vga_deco_clear(int s){ (void)s; }
int mouse_outside(void){ return 0; }
int mouse_is_ready(void){ return 0; }
int mouse_take_clicks(void){ return 0; }
void mouse_clamp_to_grid(void){}
uint32_t get_ticks(void){ return 0; }
void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s){ *h = 12; *m = 0; *s = 0; }
void keyboard_inject(int c){ (void)c; }
void keyboard_set_chrome_hook(int (*fn)(int)){ (void)fn; }
void toast_info(const char *m){ (void)m; }

#define WM_HAVE_CHROME
#include "wm_stubs.inc"          /* v0.9.8: the window manager, off */
#include "../shell/chrome.c"

static int total, fails;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){ fails++; printf("  FAIL %s (got %ld)\n", what, got); }
    else printf("  ok   %s\n", what);
}

int main(void)
{
    ch_layout_t L;
    int chip;
    printf("chrome: an app's window\n");

    head("an app that uses any size (Settings, the Shell) on the HP's 128x48");
    chrome_layout(128, 48, 0, 0, &L);
    ck("title bar on row 0, the whole width", L.title_y == 0 && L.title_x0 == 0 && L.title_x1 == 127, L.title_y);
    ck("the app below it: 128x46 at 0,1", L.viewport && L.vx == 0 && L.vy == 1 && L.vw == 128 && L.vh == 46, L.vh);
    ck("the taskbar on the last row, 47", L.bar_y == 47, L.bar_y);
    chrome_layout(128, 48, 0, 1, &L);
    ck("full screen: the whole 128x48, no title bar, no taskbar",
       !L.viewport && L.vw == 128 && L.vh == 48 && L.title_y < 0 && L.bar_y < 0, L.vh);

    head("an 80x25 app (boxed) on 128x48");
    chrome_layout(128, 48, 1, 0, &L);
    ck("80x25, centred across", L.vw == 80 && L.vh == 25 && L.vx == 24, L.vx);
    ck("its title row right above it", L.title_y == L.vy - 1 && L.title_y >= 0, L.title_y);
    ck("its frame row and the taskbar both fit below", L.vy + 25 < 47 && L.bar_y == 47, L.vy);
    ck("the title bar spans the frame", L.title_x0 == 23 && L.title_x1 == 104, L.title_x1);
    chrome_layout(128, 48, 1, 1, &L);
    ck("full screen: centred on the whole screen, title bar kept, no taskbar",
       L.vy == 11 && L.title_y == 10 && L.bar_y < 0, L.vy);

    head("no room: QEMU's 80x25 and a 640x480 panel");
    chrome_layout(80, 25, 0, 0, &L);
    ck("80x25, any-size app: the whole screen, as before 0.9.6.5",
       !L.viewport && L.title_y < 0 && L.bar_y < 0, L.title_y);
    chrome_layout(80, 25, 1, 0, &L);
    ck("80x25, boxed app: no box, no chrome", !L.viewport && !L.boxed && L.title_y < 0, L.boxed);
    chrome_layout(100, 27, 1, 0, &L);
    ck("27 rows: a box and its title, but no taskbar", L.viewport && L.title_y == 0 && L.bar_y < 0, L.bar_y);
    chrome_layout(80, 27, 0, 0, &L);
    ck("27 rows, any-size app: 25 for the app, the title and the taskbar",
       L.vh == 25 && L.title_y == 0 && L.bar_y == 26, L.vh);
    chrome_layout(80, 26, 0, 0, &L);
    ck("26 rows: an app would get 24 -- no chrome, the app keeps 25", !L.viewport && L.bar_y < 0, L.vh);

    head("what a click hits");
    chrome_layout(128, 48, 0, 0, &L);
    ck("the X: the last three cells of the title", chrome_hit(&L, 128, 126, 0, 0) == CH_CLOSE &&
       chrome_hit(&L, 128, 125, 0, 0) == CH_CLOSE && chrome_hit(&L, 128, 124, 0, 0) == CH_CLOSE, 0);
    ck("  ... and the gap before it is the title", chrome_hit(&L, 128, 123, 0, 0) == CH_TITLE, 0);
    ck("full screen: the three before the gap", chrome_hit(&L, 128, 121, 0, 0) == CH_FULL &&
       chrome_hit(&L, 128, 120, 0, 0) == CH_FULL, 0);
    ck("minimize: the three before that", chrome_hit(&L, 128, 117, 0, 0) == CH_MIN, 0);
    ck("the name: the title", chrome_hit(&L, 128, 3, 0, 0) == CH_TITLE, 0);
    ck("Start: the first seven cells of the taskbar", chrome_hit(&L, 128, 0, 47, 0) == CH_START &&
       chrome_hit(&L, 128, 6, 47, 0) == CH_START, 0);
    ck("the first chip from column 8, fourteen wide",
       chrome_hit(&L, 128, 8, 47, &chip) == CH_CHIP && chip == 0 &&
       chrome_hit(&L, 128, 21, 47, &chip) == CH_CHIP && chip == 0 &&
       chrome_hit(&L, 128, 22, 47, &chip) == CH_CHIP && chip == 1, chip);
    ck("the clock: nothing", chrome_hit(&L, 128, 124, 47, &chip) == CH_BAR, 0);
    ck("inside the app: nothing of the window's", chrome_hit(&L, 128, 60, 20, 0) == CH_NONE, 0);
    chrome_layout(128, 48, 1, 0, &L);
    ck("a boxed app's X sits inside its frame's last cells",
       chrome_hit(&L, 128, 103, L.title_y, 0) == CH_CLOSE && chrome_hit(&L, 128, 104, L.title_y, 0) == CH_TITLE &&
       chrome_hit(&L, 128, 105, L.title_y, 0) == CH_NONE, 0);
    chrome_layout(128, 48, 0, 1, &L);
    ck("full screen: no title bar, no taskbar to hit", chrome_hit(&L, 128, 126, 0, 0) == CH_NONE &&
       chrome_hit(&L, 128, 2, 47, 0) == CH_NONE, 0);

    head("v0.9.7 (E-4): pinned apps on the taskbar, between Start and the chips");
    {
        int ch;
        chrome_layout(128, 48, 0, 0, &L);
        g_npins = 0;
        ck("no pins: the first chip starts where it always did (x=8)",
           chrome_hit(&L, 128, 8, L.bar_y, &ch) == CH_CHIP && ch == 0, ch);
        g_npins = 2;
        ck("two pins: x=8 is the first pin", chrome_hit(&L, 128, 8, L.bar_y, &ch) == CH_PIN && ch == 0, ch);
        ck("  ... x=11 still the first, x=12 the second",
           chrome_hit(&L, 128, 11, L.bar_y, &ch) == CH_PIN && ch == 0 &&
           chrome_hit(&L, 128, 12, L.bar_y, &ch) == CH_PIN && ch == 1, ch);
        ck("  ... x=16 is the gap, not a chip and not a pin",
           chrome_hit(&L, 128, 16, L.bar_y, &ch) == CH_BAR, ch);
        ck("  ... and the first chip moved to x=17",
           chrome_hit(&L, 128, 17, L.bar_y, &ch) == CH_CHIP && ch == 0 &&
           chrome_hit(&L, 128, 30, L.bar_y, &ch) == CH_CHIP && ch == 0 &&
           chrome_hit(&L, 128, 31, L.bar_y, &ch) == CH_CHIP && ch == 1, ch);
        ck("Start is still Start", chrome_hit(&L, 128, 3, L.bar_y, &ch) == CH_START, 0);
        g_npins = 0;
    }

    head("v0.9.6.6: full screen FILLS for an 80x25 app, and always comes back");
    {
        g_pc = 128; g_pr = 48;
        chrome_layout(128, 48, 1, 0, &L);
        chrome_begin("2048", 1);
        vga_phys_write(L.vx, L.vy, 0x0F41);                 /* 'A', top-left of the box */
        vga_phys_write(L.vx + 79, L.vy + 24, 0x0F5A);       /* 'Z', bottom-right */
        chrome_hook(KEY_FULLSCREEN);
        ck("full screen: the console becomes 80x25 at a bigger cell", g_pc == 80 && g_pr == 25 && g_fillon && fills == 1,
           g_pc * 100L + g_pr);
        ck("  ... with what the app drew, moved to fill it",
           vga_phys_read(0, 0) == 0x0F41 && vga_phys_read(79, 24) == 0x0F5A, vga_phys_read(0, 0));
        chrome_hook(KEY_FULLSCREEN);
        ck("F11 again: back to the grid the person chose", g_pc == 128 && g_pr == 48 && !g_fillon, g_pc);
        ck("  ... the app's cells back in their box",
           vga_phys_read(L.vx, L.vy) == 0x0F41 && vga_phys_read(L.vx + 79, L.vy + 24) == 0x0F5A, vga_phys_read(L.vx, L.vy));
        ck("  ... and the title bar back above it", (vga_phys_read(L.vx + 1, L.title_y) & 0xFF) == '2', vga_phys_read(L.vx + 1, L.title_y));
        chrome_hook(KEY_FULLSCREEN);
        chrome_leave_front();
        ck("grown, then another app takes the screen: the grid goes back first",
           g_pc == 128 && g_pr == 48 && !g_fillon, g_pc);
        chrome_leave_front();
        ck("  ... and leaving again does nothing more", g_pc == 128 && modes == 4, modes);
        chrome_hook(KEY_FULLSCREEN);
        chrome_end();
        ck("grown, then the app closes: the grid goes back", g_pc == 128 && g_pr == 48 && !g_fillon, g_pc);
        chrome_begin("Settings", 0);
        fills = 0;
        chrome_hook(KEY_FULLSCREEN);
        ck("an any-size app's full screen does not touch the grid", fills == 0 && g_pc == 128, fills);
        chrome_hook(KEY_FULLSCREEN);
        chrome_end();
    }

    printf("%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
