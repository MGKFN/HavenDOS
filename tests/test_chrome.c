/* Compile the REAL ui_draw_desktop_bars() natively against a fake VGA and
   check where the chrome actually lands in each text mode.

   The point is twofold: prove the bars follow the active mode, and prove
   that at 80x25 every computed column is byte-identical to the hard-coded
   one it replaced, so the familiar layout did not shift. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
/* HAVENDOS_VER comes from the single source of truth, not a local copy:
   the previous revision of this test hardcoded the version string at the
   top of this file, which is exactly the drift make_version.py --check
   now refuses. */
#include "../include/version.h"

#define VGA_COLS_MAX 90
#define VGA_ROWS_MAX 60

/* ── fake screen ─────────────────────────────────────────────── */
static char  screen[VGA_ROWS_MAX][VGA_COLS_MAX + 1];
static int   g_cols = 80, g_rows = 25;

int vga_cols(void){ return g_cols; }
int vga_rows(void){ return g_rows; }

void vga_putchar_at(char c, int x, int y, uint8_t fg, uint8_t bg)
{
    (void)fg; (void)bg;
    if (x < 0 || y < 0 || x >= g_cols || y >= g_rows) return;
    screen[y][x] = c;
}
void vga_puts_at(const char *s, int x, int y, uint8_t fg, uint8_t bg)
{
    for (int i = 0; s[i]; i++) vga_putchar_at(s[i], x + i, y, fg, bg);
}
static void clear_screen(void)
{
    for (int y = 0; y < VGA_ROWS_MAX; y++) {
        memset(screen[y], '\1', VGA_COLS_MAX);
        screen[y][VGA_COLS_MAX] = 0;
    }
}

/* ── the bits of the OS ui.c leans on ────────────────────────── */
typedef enum { VGA_BLACK, VGA_WHITE = 15 } vga_color_t;
typedef struct { uint8_t bar_fg, bar_bg, accent, desk_bg, win_fg, win_bg,
                         icon_bg, icon_fg; } theme_t;
static theme_t g_theme = {7, 1, 11, 0, 15, 1, 8, 15};
theme_t *ui_theme(void){ return &g_theme; }

void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s){ *h = 13; *m = 45; *s = 7; }
const char *current_username(void){ return "kyan"; }
int current_user_admin(void){ return 1; }
static int g_unread = 0;
int notif_get_unread(void){ return g_unread; }

/* the one function under test, lifted verbatim from shell/ui.c */
#include "bars.inc"

/* ── checks ──────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *w, int c, const char *detail)
{
    total++;
    if (c) printf("  ok    %s\n", w);
    else { fails++; printf("  FAIL  %s  [%s]\n", w, detail ? detail : ""); }
}
static int row_has(int y, const char *needle)
{
    return strstr(screen[y], needle) != NULL;
}
static int col_of(int y, const char *needle)
{
    char *p = strstr(screen[y], needle);
    return p ? (int)(p - screen[y]) : -1;
}
static int row_filled_to(int y, int w)
{
    for (int x = 0; x < w; x++) if (screen[y][x] == '\1') return 0;
    return 1;
}

static void run_mode(int cols, int rows, const char *label)
{
    g_cols = cols; g_rows = rows;
    clear_screen();
    ui_draw_desktop_bars();
    int bar = rows - 1;

    char buf[96];
    printf("\n%s\n", label);

    snprintf(buf, sizeof buf, "row %d", bar);
    ck("taskbar is drawn on the real bottom row", row_has(bar, "HAVENDOS"), buf);
    ck("taskbar spans the full width", row_filled_to(bar, cols), buf);
    ck("top status bar spans the full width", row_filled_to(0, cols), "row 0");
    ck("username is on the taskbar", row_has(bar, "kyan"), NULL);
    ck("clock is on the taskbar", row_has(bar, "13:45:07"), NULL);

    int clock = col_of(bar, "13:45:07");
    snprintf(buf, sizeof buf, "clock at col %d, width %d", clock, cols);
    ck("clock sits fully inside the screen",
       clock >= 0 && clock + 8 <= cols, buf);
    ck("clock is right-anchored, not stranded mid-screen",
       clock >= cols - 12, buf);

    int notif = col_of(bar, "NOTIF");
    snprintf(buf, sizeof buf, "NOTIF at %d, clock at %d", notif, clock);
    ck("NOTIF does not collide with the clock",
       notif >= 0 && notif + 5 <= clock, buf);

    /* nothing may be drawn below the taskbar or in phantom rows */
    int dirty = 0;
    for (int y = bar + 1; y < VGA_ROWS_MAX; y++)
        for (int x = 0; x < VGA_COLS_MAX; x++)
            if (screen[y][x] != '\1') dirty++;
    ck("nothing is drawn past the bottom row", dirty == 0, NULL);
}

int main(void)
{
    printf("desktop chrome geometry\n=======================\n");

    run_mode(80, 25, "80x25  (the mode the layout was designed for)");

    /* the exact columns the hard-coded version used */
    printf("\n80x25 must be byte-identical to the old fixed layout\n");
    g_cols = 80; g_rows = 25; clear_screen(); ui_draw_desktop_bars();
    ck("bell still at column 55", col_of(24, "[!]") == 55, NULL);
    ck("NOTIF still at column 59", col_of(24, "NOTIF") == 59, NULL);
    ck("clock still at column 71", col_of(24, "13:45:07") == 71, NULL);
    ck("taskbar still on row 24", row_has(24, "HAVENDOS"), NULL);

    run_mode(80, 50, "80x50");
    run_mode(90, 30, "90x30");
    run_mode(90, 60, "90x60");

    printf("\nunread badge does not overflow the right edge\n");
    g_unread = 12; g_cols = 90; g_rows = 60;
    clear_screen(); ui_draw_desktop_bars();
    ck("count renders with a 2-digit unread count",
       row_has(59, "9+)"), screen[59]);
    ck("count stays on screen", col_of(59, "9+)") < 90, NULL);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
