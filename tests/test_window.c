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
uint32_t task_id(int s){ (void)s; return 0; }
int task_ready(void){ return 1; }
int task_state(int s){ (void)s; return 0; }
const char *task_name(int s){ (void)s; return ""; }
int task_set_front(int s){ (void)s; return 0; }
int task_wait_front(void){ return 0; }
int vga_phys_cols(void){ return 128; }
int vga_phys_rows(void){ return 48; }
uint16_t vga_phys_read(int x, int y){ (void)x; (void)y; return 0; }
void vga_phys_write(int x, int y, uint16_t v){ (void)x; (void)y; (void)v; }
int vga_viewport_active(void){ return 0; }
void vga_viewport_get(int *x, int *y, int *w, int *h){ *x = *y = *w = *h = 0; }
int vga_viewport_set(int x, int y, int w, int h){ (void)x; (void)y; (void)w; (void)h; return 1; }
void vga_viewport_clear(void){}
void mouse_get_phys(int *x, int *y){ *x = *y = 0; }
int mouse_outside(void){ return 0; }
int mouse_is_ready(void){ return 0; }
int mouse_take_clicks(void){ return 0; }
void mouse_clamp_to_grid(void){}
uint32_t get_ticks(void){ return 0; }
void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s){ *h = 12; *m = 0; *s = 0; }
void keyboard_inject(int c){ (void)c; }
void keyboard_set_chrome_hook(int (*fn)(int)){ (void)fn; }
void toast_info(const char *m){ (void)m; }

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

    printf("%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
