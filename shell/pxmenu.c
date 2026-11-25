/* shell/pxmenu.c -- a right-click menu, in pixels (v0.9.10).
 *
 * Kyan, 25 Nov: "click menu is old". The desktop's right-click menu was
 * still a box of characters with a border of line-drawing glyphs, drawn
 * into the desktop's grid. This is the same menu as the Start menu is
 * made of: a rounded panel with a shadow, IBM Plex Sans rows, the row
 * under the pointer lit, on the window manager's own menu layer -- so it
 * opens over the desktop, over the Start menu, over a window, anywhere.
 *
 *   int pxmenu_run(x, y, title, items, n)
 *
 * runs until something is chosen (its index), or Esc / a click outside
 * (-1). Arrows and Enter work too: the desktop runs fine with no mouse.
 */
#include "../include/wm.h"
#include "../include/keys.h"
#include "../include/string.h"

extern int  keyboard_getchar(void);
extern void mouse_get_px(int *x, int *y);
extern int  mouse_take_wm_clicks(int *x, int *y);
extern void wm_pointer_update(void);
extern void kpoll_sleep_ms(uint32_t ms);

#define PM_MAX 12
static const char *const *g_items; static int g_n, g_sel, g_x, g_y, g_w, g_h, g_rh, g_pad, g_px, g_hy;
static const char *g_title;

static void pm_paint(int x0, int y0, int x1, int y1)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
    uint32_t bg = wm_rgb(WMC_PANEL), fg = wm_rgb(WMC_TEXT), off = wm_rgb(WMC_TEXT_OFF), sel = wm_rgb(WMC_TITLE);
    int r = vga_wm_lite() ? 0 : g_rh / 4;
    px_round(g_x, g_y, g_w, g_h, r, wm_rgb(WMC_EDGE), WM_BG_GRAD);
    px_round(g_x + 1, g_y + 1, g_w - 2, g_h - 2, r > 1 ? r - 1 : 0, bg, wm_rgb(WMC_EDGE));
    int y = g_y + g_pad;
    if (g_title) {
        px_text(g_x + g_pad * 2, y + (g_hy - px_text_h(g_px, 1)) / 2, g_title, g_px, 1, off, bg, g_w - g_pad * 4);
        y += g_hy;
        px_fill(g_x + g_pad, y - 1, g_w - 2 * g_pad, 1, wm_rgb(WMC_EDGE));
    }
    for (int i = 0; i < g_n; i++, y += g_rh) {
        uint32_t b = i == g_sel ? sel : bg, f = i == g_sel ? wm_rgb(WMC_TEXT) : fg;
        if (i == g_sel) px_round(g_x + g_pad, y, g_w - 2 * g_pad, g_rh, r, b, bg);
        else px_fill(g_x + g_pad, y, g_w - 2 * g_pad, g_rh, bg);
        px_text(g_x + g_pad * 3, y + (g_rh - px_text_h(g_px, 0)) / 2, g_items[i], g_px, 0, f, b, g_w - g_pad * 6);
    }
}

static int row_at(int mx, int my)
{
    int top = g_y + g_pad + (g_title ? g_hy : 0);
    if (mx < g_x || mx >= g_x + g_w || my < top || my >= top + g_n * g_rh) return -1;
    return (my - top) / g_rh;
}

int pxmenu_run(int x, int y, const char *title, const char *const *items, int n)
{
    if (!vga_wm_on() || n <= 0) return -1;
    if (n > PM_MAX) n = PM_MAX;
    g_items = items; g_n = n; g_title = title; g_sel = -1;
    g_px = vga_wm_font_px();
    g_rh = g_px * 2 + 4; g_pad = g_px / 3 + 2; g_hy = g_title ? g_rh : 0;
    int w = title ? px_text_w(title, g_px, 1) : 0;
    for (int i = 0; i < n; i++) { int t = px_text_w(items[i], g_px, 0); if (t > w) w = t; }
    g_w = w + g_pad * 6 + g_px * 2; g_h = n * g_rh + 2 * g_pad + g_hy;
    int rx0, rx1, sh = vga_screen_h() - vga_wm_bar_h();
    vga_wm_reach(&rx0, &rx1);
    if (x + g_w > rx1) x = rx1 - g_w;
    if (x < rx0) x = rx0;
    if (y + g_h > sh) y = sh - g_h;
    if (y < 0) y = 0;
    g_x = x; g_y = y;
    mouse_take_wm_clicks(0, 0);                       /* the click that opened it */
    vga_wm_set_menu(g_x, g_y, g_w, g_h, pm_paint);
    int choice = -2, lx = -1, ly = -1;
    while (choice == -2) {
        int k = keyboard_getchar(), mx, my, cx, cy;
        wm_pointer_update();
        mouse_get_px(&mx, &my);
        if (mx != lx || my != ly) {
            lx = mx; ly = my;
            int h = row_at(mx, my);
            if (h >= 0 && h != g_sel) { g_sel = h; vga_wm_repaint_rect(g_x, g_y, g_x + g_w, g_y + g_h); }
        }
        int c = mouse_take_wm_clicks(&cx, &cy);
        if (c) {
            int h = row_at(cx, cy);
            if (c & 1 && h >= 0) choice = h;
            else if (!(cx >= g_x && cx < g_x + g_w && cy >= g_y && cy < g_y + g_h)) choice = -1;
        }
        if (k == 0x1B) choice = -1;
        else if (k == KEY_UP) { g_sel = g_sel <= 0 ? n - 1 : g_sel - 1; vga_wm_repaint_rect(g_x, g_y, g_x + g_w, g_y + g_h); }
        else if (k == KEY_DOWN) { g_sel = g_sel >= n - 1 ? 0 : g_sel + 1; vga_wm_repaint_rect(g_x, g_y, g_x + g_w, g_y + g_h); }
        else if ((k == '\n' || k == '\r') && g_sel >= 0) choice = g_sel;
        if (k == -1 && !c) kpoll_sleep_ms(10);
    }
    vga_wm_set_menu(0, 0, 0, 0, 0);
    return choice;
}
