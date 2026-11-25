/* shell/taskbar.c -- the taskbar, in pixels (v0.9.8)
 *
 * Until 0.9.7.10 there were two taskbars, both one row of characters: the
 * desktop's (shell/ui.c) and the one under an app (shell/chrome.c), with
 * "FM" for a pinned File Manager and a 14-character chip for each app.
 * With the window manager on there is one, drawn here, on top of
 * everything but the Start menu:
 *
 *   [Start] [pins ...] | [window] [window] ...        [bell] [12:34]
 *
 * Start is the HavenDOS grid; each pin and each open window shows its
 * icon (include/icons.h); the window in front is lighter with an oxide
 * line under it, a minimised one is dimmed. The bell carries the count of
 * unread notifications. What the pointer is over is lit.
 *
 * Painting goes through drivers/wm_core.inc's primitives, which paint only
 * what the taskbar layer owns. Hit-testing uses the same layout function
 * as painting, so the two cannot disagree.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/task.h"

extern int         desktop_tbpin_count(void);
extern const char *desktop_tbpin_id(int i);
extern int         notif_get_unread(void);
extern void        rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);

#define TB_MAXI 24
enum { TI_NONE, TI_START, TI_PIN, TI_WIN, TI_NOTIF, TI_CLOCK };
typedef struct { int kind, idx, x0, x1; } tb_item_t;
static tb_item_t g_it[TB_MAXI];
static int       g_nit;
static int       g_hover = -1;

static uint32_t mixc(uint32_t a, uint32_t b, int k)      /* k/255 of a */
{
    uint32_t r = (((a >> 16) & 0xFF) * (uint32_t)k + ((b >> 16) & 0xFF) * (uint32_t)(255 - k)) / 255u;
    uint32_t g = (((a >> 8) & 0xFF) * (uint32_t)k + ((b >> 8) & 0xFF) * (uint32_t)(255 - k)) / 255u;
    uint32_t bl = ((a & 0xFF) * (uint32_t)k + (b & 0xFF) * (uint32_t)(255 - k)) / 255u;
    return (r << 16) | (g << 8) | bl;
}
static uint32_t bar_bg(void){ return mixc(0xffffffu, wm_rgb(WMC_PANEL), 14); }

static void clock_text(char *b)
{
    uint8_t h, m, s;
    rtc_get_time(&h, &m, &s);
    b[0] = (char)('0' + h / 10); b[1] = (char)('0' + h % 10); b[2] = ':';
    b[3] = (char)('0' + m / 10); b[4] = (char)('0' + m % 10); b[5] = 0;
}

static void layout(void)
{
    int rx0, rx1, bh = vga_wm_bar_h(), fp = vga_wm_font_px();
    vga_wm_reach(&rx0, &rx1);           /* where the pointer can go */
    int W = rx1, x = rx0 + bh / 4, n = 0, i;
    char clk[8];
    g_nit = 0;
    g_it[g_nit++] = (tb_item_t){ TI_START, 0, x, x + bh * 5 / 4 };
    x += bh * 5 / 4 + bh / 4;
    for (i = 0; i < desktop_tbpin_count() && g_nit < TB_MAXI - 4; i++) {
        g_it[g_nit++] = (tb_item_t){ TI_PIN, i, x, x + bh };
        x += bh;
    }
    x += bh / 3;
    /* the right side first, so the windows know how much room they have */
    clock_text(clk);
    int cw = px_text_w(clk, fp, 0) + bh / 2;
    int kx1 = W - bh / 4, kx0 = kx1 - cw;
    int nx1 = kx0, nx0 = nx1 - bh;
    for (i = 1; i < TASK_MAX; i++) if (vga_wm_is_open(i)) n++;
    if (n) {
        int avail = nx0 - bh / 3 - x, bw = bh * 6;
        if (bw * n > avail) bw = avail / n;
        if (bw < bh) bw = bh;
        for (i = 1; i < TASK_MAX && g_nit < TB_MAXI - 2; i++) {
            if (!vga_wm_is_open(i)) continue;
            if (x + bw > nx0) break;
            g_it[g_nit++] = (tb_item_t){ TI_WIN, i, x, x + bw - 2 };
            x += bw;
        }
    }
    g_it[g_nit++] = (tb_item_t){ TI_NOTIF, 0, nx0, nx1 };
    g_it[g_nit++] = (tb_item_t){ TI_CLOCK, 0, kx0, kx1 };
}

void taskbar_paint(int x0, int y0, int x1, int y1)
{
    int bh = vga_wm_bar_h(), fp = vga_wm_font_px(), H = vga_screen_h(), y = H - bh;
    uint32_t bg = bar_bg(), fg = wm_rgb(WMC_TEXT), dim = wm_rgb(WMC_TEXT_OFF);
    (void)x0; (void)y0; (void)y1;
    layout();
    px_fill(0, y, x1, bh, bg);
    px_fill(0, y, x1, 1, mixc(0xffffffu, bg, 30));          /* a hairline on top */
    /* v0.9.8.2: bigger -- 24 px on a 32 px bar, 32 on a 44 px one (Kyan:
       "the icons ... can be a bit bigger, especially the start menu button") */
    int isz = px_icon_size(bh * 3 / 4), r = bh / 6;
    for (int k = 0; k < g_nit; k++) {
        tb_item_t *it = &g_it[k];
        int w = it->x1 - it->x0, hov = k == g_hover;
        int front = it->kind == TI_WIN && it->idx == task_front() && !vga_wm_is_min(it->idx);
        uint32_t ib = bg;
        if (front)      ib = mixc(0xffffffu, bg, 34);
        else if (hov)   ib = mixc(0xffffffu, bg, 20);
        if (ib != bg) px_round(it->x0, y + 3, w, bh - 6, r, ib, bg);
        switch (it->kind) {
        case TI_START:
            px_icon(it->x0 + (w - isz) / 2, y + (bh - isz) / 2, wm_icon_find("_start"), isz, ib);
            break;
        case TI_PIN: {
            const char *id = desktop_tbpin_id(it->idx);
            int ic = wm_icon_find(id);
            px_icon(it->x0 + (w - isz) / 2, y + (bh - isz) / 2, ic < 0 ? wm_icon_find("_app") : ic, isz, ib);
            break; }
        case TI_WIN: {
            int min = vga_wm_is_min(it->idx);
            int tx = it->x0 + bh / 5;
            px_icon(tx, y + (bh - isz) / 2, vga_wm_icon_of(it->idx), isz, ib);
            tx += isz + bh / 5;
            if (it->x1 - tx > fp)
                px_text(tx, y + (bh - px_text_h(fp, 0)) / 2, vga_wm_title_of(it->idx), fp, 0,
                        min ? dim : fg, ib, it->x1 - tx - bh / 5);
            if (front) px_round(it->x0 + w / 4, y + bh - 6, w / 2, 3, 1, wm_rgb(WMC_ACCENT), ib);
            else if (!min) px_round(it->x0 + w / 2 - bh / 6, y + bh - 6, bh / 3, 3, 1, dim, ib);
            break; }
        case TI_NOTIF: {
            int n = notif_get_unread();
            int ic = wm_icon_find("_bell");
            px_icon(it->x0 + (w - isz) / 2, y + (bh - isz) / 2, ic, isz, ib);
            if (n > 0) {
                char b[4]; int bs = fp * 9 / 10;
                if (n > 9) { b[0] = '9'; b[1] = '+'; b[2] = 0; }
                else { b[0] = (char)('0' + n); b[1] = 0; }
                int bx = it->x0 + w / 2 + isz / 6, by = y + bh / 8;
                int bw = px_text_w(b, bs * 3 / 4, 1) + bs / 2;
                if (bw < bs) bw = bs;
                px_round(bx, by, bw, bs, bs / 2, wm_rgb(WMC_ACCENT), ib);
                px_text(bx + (bw - px_text_w(b, bs * 3 / 4, 1)) / 2, by + (bs - px_text_h(bs * 3 / 4, 1)) / 2,
                        b, bs * 3 / 4, 1, 0xffffffu, wm_rgb(WMC_ACCENT), 0);
            }
            break; }
        case TI_CLOCK: {
            char clk[8]; clock_text(clk);
            px_text(it->x0 + bh / 4, y + (bh - px_text_h(fp, 0)) / 2, clk, fp, 0, fg, ib, 0);
            break; }
        }
    }
}

/* 1 Start, 2 a pin (*idx which), 3 a window (*idx its slot), 4 the bell,
   5 the clock, 0 nothing */
int taskbar_hit(int x, int y, int *idx)
{
    if (y < vga_screen_h() - vga_wm_bar_h()) return 0;
    layout();
    for (int k = 0; k < g_nit; k++)
        if (x >= g_it[k].x0 && x < g_it[k].x1) {
            if (idx) *idx = g_it[k].idx;
            switch (g_it[k].kind) {
            case TI_START: return 1;
            case TI_PIN:   return 2;
            case TI_WIN:   return 3;
            case TI_NOTIF: return 4;
            case TI_CLOCK: return 5;
            }
        }
    return 0;
}

/* The item under (x,y) lights; 1 if that changed. (-1,-1): none. */
int taskbar_hover(int x, int y)
{
    int h = -1;
    if (x >= 0 && y >= vga_screen_h() - vga_wm_bar_h()) {
        layout();
        for (int k = 0; k < g_nit; k++)
            if (x >= g_it[k].x0 && x < g_it[k].x1 && g_it[k].kind != TI_CLOCK) { h = k; break; }
    }
    if (h == g_hover) return 0;
    g_hover = h;
    return 1;
}

/* Changes when anything the taskbar shows does. */
uint32_t taskbar_sig(void)
{
    uint8_t h, m, s;
    uint32_t sig;
    rtc_get_time(&h, &m, &s);
    sig = (uint32_t)h * 60u + m;
    sig = sig * 31u + (uint32_t)task_front();
    sig = sig * 31u + (uint32_t)notif_get_unread();
    sig = sig * 31u + (uint32_t)desktop_tbpin_count();
    for (int i = 1; i < TASK_MAX; i++) {
        sig = sig * 31u + (uint32_t)(vga_wm_is_open(i) * 2 + vga_wm_is_min(i));
        const char *t = vga_wm_title_of(i);
        while (*t) sig = sig * 33u + (uint8_t)*t++;
    }
    return sig;
}
