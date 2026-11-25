/* shell/startmenu.c -- the Start menu, in pixels (v0.9.8)
 *
 * The 0.9.7 menu was two columns of characters: categories, and a
 * cascade of the apps in one, fifty of them in All Apps, taller than the
 * screen. This one is a panel above the taskbar's Start button:
 *
 *   +------------------------------------------------+
 *   | (Q) Type to search                             |
 *   |  All apps     | All apps                    50 |
 *   |  Favourites   |  [icon] 2048                   |
 *   |  Recent       |  [icon] ASCII Art            | |
 *   |  Apps         |  ...                         | |
 *   |  System ...   |                                |
 *   |------------------------------------------------|
 *   | (o) admin                         [lock] [pwr] |
 *   +------------------------------------------------+
 *
 * TYPING SEARCHES: any letter goes into the search field and the list
 * becomes every app whose name contains it, case ignored, A to Z. All
 * apps is A to Z too; a category keeps the registry's order, which is
 * the curated one. The list scrolls, by wheel, keys or its scrollbar,
 * instead of growing past the screen.
 *
 * Keys: Up/Down choose, Left/Right between categories and the list,
 * Enter opens, PgUp/PgDn/Home/End, Esc clears the search or closes,
 * Ctrl+D puts the chosen app on the desktop, Ctrl+F in Favourites (D and
 * F alone did that in the old menu; a letter now searches). The mouse:
 * hover chooses, click opens, the wheel scrolls, a click outside closes.
 *
 * It is modal: it runs its own loop in whichever task opened it (from the
 * taskbar or the Windows key, through shell/chrome.c's hook), painting
 * through drivers/wm_core.inc's popup layer, and opens what was chosen
 * the same way a desktop icon does.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/keys.h"

extern int         desktop_app_count(void);
extern const char *desktop_app_id(int i);
extern const char *desktop_app_name(int i);
extern int         desktop_app_cat(int i);
extern int         desktop_fav_count(void);
extern int         desktop_fav_app(int k);
extern int         desktop_open_app(const char *id);
extern int         desktop_app_pin_toggle(const char *id, int fav);
extern int         desktop_app_pinned(const char *id, int fav);
extern void        desktop_lock_screen(void);
extern const char *current_username(void);
extern int         keyboard_getchar(void);
extern int         kbd_ctrl(void);
extern void        mouse_get_px(int *x, int *y);
extern int         mouse_take_wm_clicks(int *x, int *y);
extern int         mouse_get_wheel(void);
extern void        kpoll_sleep_ms(uint32_t ms);
extern void        wm_pointer_update(void);
extern void        wm_busy(int on);
extern void        chrome_leave_front(void);
extern void        toast_info(const char *m);

#define NCATS 9
static const char *const cat_name[NCATS] = {
    "All apps", "Favourites", "Recent", "Apps", "System", "Hardware", "Games", "Media", "Settings" };
static const char *const cat_icon[NCATS] = {
    "_start", "_star", "_history", "_app", "system", "pci", "pong", "music", "settings" };
#define C_ALL 0
#define C_FAV 1
#define C_REC 2

#define MAXL 96
static int  g_list[MAXL], g_nlist;
static int  g_cat, g_sel, g_top, g_focus;          /* focus 0 list, 1 categories */
static char g_q[32]; static int g_qn;
static int  g_hrow = -1, g_hcat = -1, g_hbtn = 0;  /* what the pointer is over */
static char g_recent[8][16]; static int g_nrecent;

/* geometry, from the screen */
static int P_x, P_y, P_w, P_h, fp, pad, rh, catw, sb_h, foot_h, list_x, list_y, list_w, list_rows;

static uint32_t mixc(uint32_t a, uint32_t b, int k)
{
    uint32_t r = (((a >> 16) & 0xFF) * (uint32_t)k + ((b >> 16) & 0xFF) * (uint32_t)(255 - k)) / 255u;
    uint32_t g = (((a >> 8) & 0xFF) * (uint32_t)k + ((b >> 8) & 0xFF) * (uint32_t)(255 - k)) / 255u;
    uint32_t bl = ((a & 0xFF) * (uint32_t)k + (b & 0xFF) * (uint32_t)(255 - k)) / 255u;
    return (r << 16) | (g << 8) | bl;
}
static uint32_t panel_bg(void){ return mixc(0xffffffu, wm_rgb(WMC_PANEL), 20); }

static void geometry(void)
{
    int rx0, rx1, H = vga_screen_h(), bh = vga_wm_bar_h();
    vga_wm_reach(&rx0, &rx1);           /* where the pointer can go */
    int W = rx1 - rx0;
    fp = vga_wm_font_px();
    pad = fp;
    rh = fp * 2 + fp / 4;
    P_w = fp * 40; if (P_w > W - 2 * pad) P_w = W - 2 * pad;
    P_h = fp * 34; if (P_h > H - bh - 2 * pad) P_h = H - bh - 2 * pad;
    P_x = rx0 + pad / 2; P_y = H - bh - pad / 2 - P_h;
    sb_h = fp * 2 + fp / 2;
    foot_h = fp * 3;
    catw = P_w * 32 / 100;
    list_x = P_x + pad + catw + pad / 2;
    list_y = P_y + pad + sb_h + pad + fp * 2;
    list_w = P_x + P_w - pad - list_x;
    list_rows = (P_y + P_h - foot_h - pad / 2 - list_y) / rh;
    if (list_rows < 1) list_rows = 1;
}

static char lc(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int contains(const char *s, const char *q)
{
    for (; *s; s++) {
        int k = 0;
        while (q[k] && s[k] && lc(s[k]) == lc(q[k])) k++;
        if (!q[k]) return 1;
    }
    return 0;
}
static int name_cmp(int a, int b)
{
    const char *x = desktop_app_name(a), *y = desktop_app_name(b);
    while (*x && lc(*x) == lc(*y)) { x++; y++; }
    return lc(*x) - lc(*y);
}
static void sort_az(void)
{
    for (int i = 1; i < g_nlist; i++) {
        int v = g_list[i], j = i;
        while (j > 0 && name_cmp(g_list[j - 1], v) > 0) { g_list[j] = g_list[j - 1]; j--; }
        g_list[j] = v;
    }
}
static int find_id(const char *id)
{
    for (int i = 0; i < desktop_app_count(); i++) if (!strcmp(desktop_app_id(i), id)) return i;
    return -1;
}

static void build(void)
{
    int n = desktop_app_count();
    g_nlist = 0;
    if (g_qn) {
        for (int i = 0; i < n && g_nlist < MAXL; i++) if (contains(desktop_app_name(i), g_q)) g_list[g_nlist++] = i;
        sort_az();
    } else if (g_cat == C_ALL) {
        for (int i = 0; i < n && g_nlist < MAXL; i++) g_list[g_nlist++] = i;
        sort_az();
    } else if (g_cat == C_FAV) {
        for (int k = 0; k < desktop_fav_count() && g_nlist < MAXL; k++) {
            int r = desktop_fav_app(k);
            if (r >= 0) g_list[g_nlist++] = r;
        }
    } else if (g_cat == C_REC) {
        for (int k = 0; k < g_nrecent; k++) { int r = find_id(g_recent[k]); if (r >= 0) g_list[g_nlist++] = r; }
    } else {
        for (int i = 0; i < n && g_nlist < MAXL; i++) if (desktop_app_cat(i) == g_cat - 3) g_list[g_nlist++] = i;
    }
    if (g_sel >= g_nlist) g_sel = g_nlist - 1;
    if (g_sel < 0) g_sel = 0;
}
static void keep_visible(void)
{
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + list_rows) g_top = g_sel - list_rows + 1;
    if (g_top > g_nlist - list_rows) g_top = g_nlist - list_rows;
    if (g_top < 0) g_top = 0;
}

static void remember(const char *id)
{
    int k, j = 0;
    char keep[8][16]; int nk = 0;
    for (k = 0; k < g_nrecent; k++) if (strcmp(g_recent[k], id)) { memcpy(keep[nk++], g_recent[k], 16); }
    strncpy(g_recent[0], id, 15); g_recent[0][15] = 0;
    for (k = 0; k < nk && j < 7; k++) memcpy(g_recent[++j], keep[k], 16);
    g_nrecent = j + 1;
}

/* ── painting ───────────────────────────────────────────────────────── */
static void sm_paint(int x0, int y0, int x1, int y1)
{
    uint32_t bg = panel_bg(), fg = wm_rgb(WMC_TEXT), dim = wm_rgb(WMC_TEXT_OFF), acc = wm_rgb(WMC_ACCENT);
    uint32_t edge = mixc(0xffffffu, bg, 40), field = mixc(0xffffffu, bg, 16);
    (void)x0; (void)y0; (void)x1; (void)y1;
    int r = fp / 2;
    px_round(P_x, P_y, P_w, P_h, r + 2, edge, wm_rgb(WMC_PANEL));
    px_round(P_x + 1, P_y + 1, P_w - 2, P_h - 2, r + 1, bg, edge);

    /* the search field */
    int sx = P_x + pad, sy = P_y + pad, sw = P_w - 2 * pad;
    px_round(sx, sy, sw, sb_h, sb_h / 2, g_qn ? mixc(0xffffffu, bg, 26) : field, bg);
    int isz = px_icon_size(sb_h * 2 / 3);
    px_icon(sx + sb_h / 3, sy + (sb_h - isz) / 2, wm_icon_find("_search"), isz, g_qn ? mixc(0xffffffu, bg, 26) : field);
    {
        int tx = sx + sb_h / 3 + isz + fp / 2, ty = sy + (sb_h - px_text_h(fp, 0)) / 2;
        uint32_t fb = g_qn ? mixc(0xffffffu, bg, 26) : field;
        if (g_qn) {
            int w = px_text(tx, ty, g_q, fp, 0, fg, fb, sw - (tx - sx) - fp);
            px_fill(tx + w + 1, ty, 2, px_text_h(fp, 0), acc);        /* the caret */
        } else px_text(tx, ty, "Type to search apps", fp, 0, dim, fb, 0);
    }

    /* the categories */
    int cy = P_y + pad + sb_h + pad;
    int cisz = px_icon_size(rh * 5 / 8);
    for (int c = 0; c < NCATS; c++) {
        int ry = cy + c * rh;
        if (ry + rh > P_y + P_h - foot_h) break;
        int on = !g_qn && c == g_cat, hov = c == g_hcat;
        uint32_t b = bg, t = on ? fg : dim;
        if (on) b = g_focus ? acc : mixc(0xffffffu, bg, 28);
        else if (hov) b = mixc(0xffffffu, bg, 16);
        if (on && g_focus) t = 0xffffffu;
        if (b != bg) px_round(P_x + pad / 2, ry, catw, rh - 2, fp / 3, b, bg);
        px_icon(P_x + pad / 2 + fp / 2, ry + (rh - 2 - cisz) / 2, wm_icon_find(cat_icon[c]), cisz, b);
        px_text(P_x + pad / 2 + fp / 2 + cisz + fp / 2, ry + (rh - 2 - px_text_h(fp, 0)) / 2, cat_name[c],
                fp, on, t, b, catw - cisz - fp * 2);
    }
    px_fill(list_x - pad / 4 - 1, cy, 1, P_y + P_h - foot_h - cy - pad / 2, edge);

    /* the list's heading */
    {
        char hd[48], num[8];
        const char *h = g_qn ? "Results" : cat_name[g_cat];
        int n = g_nlist, k = 0;
        while (*h && k < 40) hd[k++] = *h++;
        hd[k] = 0;
        px_text(list_x + fp / 2, cy + (rh - px_text_h(fp, 1)) / 2, hd, fp, 1, fg, bg, list_w - fp * 4);
        k = 0;
        if (n >= 10) num[k++] = (char)('0' + n / 10);
        num[k++] = (char)('0' + n % 10); num[k] = 0;
        px_text(list_x + list_w - fp - px_text_w(num, fp, 0), cy + (rh - px_text_h(fp, 0)) / 2, num, fp, 0, dim, bg, 0);
    }

    /* the list */
    int lisz = px_icon_size(rh * 3 / 4);
    for (int k = 0; k < list_rows; k++) {
        int i = g_top + k, ry = list_y + k * rh;
        if (i >= g_nlist) break;
        int sel = i == g_sel, hov = i == g_hrow;
        uint32_t b = bg, t = fg;
        if (sel) { b = g_focus ? mixc(0xffffffu, bg, 28) : acc; t = g_focus ? fg : 0xffffffu; }
        else if (hov) b = mixc(0xffffffu, bg, 16);
        if (b != bg) px_round(list_x, ry, list_w - fp / 2, rh - 2, fp / 3, b, bg);
        const char *id = desktop_app_id(g_list[i]);
        int ic = wm_icon_find(id);
        px_icon(list_x + fp / 2, ry + (rh - 2 - lisz) / 2, ic < 0 ? wm_icon_find("_app") : ic, lisz, b);
        int tx = list_x + fp / 2 + lisz + fp * 3 / 4;
        int w = px_text(tx, ry + (rh - 2 - px_text_h(fp, 0)) / 2, desktop_app_name(g_list[i]), fp, 0, t, b,
                        list_w - (tx - list_x) - fp * 3);
        if (desktop_app_pinned(id, 1))
            px_icon(tx + w + fp / 2, ry + (rh - 2 - px_icon_size(fp)) / 2, wm_icon_find("_star"), px_icon_size(fp), b);
    }
    if (!g_nlist) {
        const char *m = g_qn ? "No app matches that" : (g_cat == C_FAV ? "Nothing in Favourites yet -- Ctrl+F adds one"
                                                     : g_cat == C_REC ? "Apps you open show up here" : "Nothing here");
        px_text(list_x + fp / 2, list_y + rh / 2, m, fp, 0, dim, bg, list_w - fp);
    }
    /* the scrollbar, when the list is longer than the panel */
    if (g_nlist > list_rows) {
        int tx = list_x + list_w - fp / 3, th = list_rows * rh;
        int kh = th * list_rows / g_nlist, ky = list_y + th * g_top / g_nlist;
        if (kh < fp) kh = fp;
        px_round(tx, list_y, fp / 4 + 1, th, fp / 8 + 1, mixc(0xffffffu, bg, 14), bg);
        px_round(tx, ky, fp / 4 + 1, kh, fp / 8 + 1, mixc(0xffffffu, bg, 80), bg);
    }

    /* the foot: who is signed in, lock and power */
    int fy = P_y + P_h - foot_h;
    px_fill(P_x + pad / 2, fy, P_w - pad, 1, edge);
    int uisz = px_icon_size(foot_h / 2);
    px_icon(P_x + pad, fy + (foot_h - uisz) / 2, wm_icon_find("_user"), uisz, bg);
    px_text(P_x + pad + uisz + fp / 2, fy + (foot_h - px_text_h(fp, 1)) / 2, current_username(), fp, 1, fg, bg, P_w / 3);
    px_text(P_x + P_w / 2 - px_text_w("Ctrl+D desktop  Ctrl+F favourite", fp * 3 / 4, 0) / 2 + fp * 2,
            fy + (foot_h - px_text_h(fp * 3 / 4, 0)) / 2, "Ctrl+D desktop  Ctrl+F favourite", fp * 3 / 4, 0, dim, bg, 0);
    for (int k = 0; k < 2; k++) {
        int bw = foot_h - fp / 2, bx = P_x + P_w - pad - (2 - k) * (bw + fp / 4), by = fy + fp / 4;
        uint32_t b = g_hbtn == k + 1 ? mixc(0xffffffu, bg, 22) : bg;
        if (b != bg) px_round(bx, by, bw, bw, fp / 3, b, bg);
        int bs = px_icon_size(bw * 2 / 3);
        px_icon(bx + (bw - bs) / 2, by + (bw - bs) / 2, wm_icon_find(k ? "power" : "_lock"), bs, b);
    }
}

static void repaint(void){ vga_wm_repaint_popup(P_x - 2, P_y - 2, P_x + P_w + 2, P_y + P_h + 2); }

/* v0.9.8.1: what the pointer lights or unlights is repainted alone -- a
   row, a category, a button -- not the whole menu for every pixel it
   moves (in QEMU that was seconds a frame, and the pointer seemed stuck) */
static int g_dx0, g_dy0, g_dx1, g_dy1, g_dirty_part;
static void dirty_add(int x, int y, int w, int h)
{
    if (!g_dirty_part) { g_dx0 = x; g_dy0 = y; g_dx1 = x + w; g_dy1 = y + h; g_dirty_part = 1; return; }
    if (x < g_dx0) g_dx0 = x;
    if (y < g_dy0) g_dy0 = y;
    if (x + w > g_dx1) g_dx1 = x + w;
    if (y + h > g_dy1) g_dy1 = y + h;
}
static void dirty_row(int i)
{
    if (i < g_top || i >= g_top + list_rows) return;
    dirty_add(list_x, list_y + (i - g_top) * rh, list_w, rh);
}
static void dirty_cat(int c)
{
    if (c < 0) return;
    dirty_add(P_x + pad / 2, P_y + pad + sb_h + pad + c * rh, catw, rh);
}
static void dirty_btns(void)
{
    dirty_add(P_x + P_w / 2, P_y + P_h - foot_h, P_w / 2, foot_h);
}

/* what is under (x,y): 1 a list row (*i), 2 a category (*i), 3 lock, 4 power, 5 the panel, 0 outside */
static int sm_hit(int x, int y, int *i)
{
    if (x < P_x || y < P_y || x >= P_x + P_w || y >= P_y + P_h) return 0;
    int cy = P_y + pad + sb_h + pad, fy = P_y + P_h - foot_h;
    if (y >= fy) {
        for (int k = 0; k < 2; k++) {
            int bw = foot_h - fp / 2, bx = P_x + P_w - pad - (2 - k) * (bw + fp / 4);
            if (x >= bx && x < bx + bw) return 3 + k;
        }
        return 5;
    }
    if (x < list_x - pad / 4 && y >= cy) {
        int c = (y - cy) / rh;
        if (c >= 0 && c < NCATS) { *i = c; return 2; }
        return 5;
    }
    if (x >= list_x && y >= list_y && y < list_y + list_rows * rh) {
        int r = g_top + (y - list_y) / rh;
        if (r < g_nlist) { *i = r; return 1; }
    }
    return 5;
}

static void open_chosen(const char *id)
{
    remember(id);
    chrome_leave_front();
    desktop_open_app(id);
}

void startmenu_run(void)
{
    int done = 0, launch = -1, lock = 0, mx = -1, my = -1, lx = -2, ly = -2;
    if (!vga_wm_on()) return;
    wm_busy(1);
    geometry();
    g_qn = 0; g_q[0] = 0; g_focus = 0; g_sel = 0; g_top = 0; g_hrow = g_hcat = -1; g_hbtn = 0; g_dirty_part = 0;
    if (g_cat == C_REC && !g_nrecent) g_cat = C_ALL;
    build();
    mouse_take_wm_clicks(0, 0);                   /* the click that opened it */
    vga_wm_set_popup(P_x, P_y, P_w, P_h, sm_paint);
    while (!done) {
        int k = keyboard_getchar(), dirty = 0, i = -1, what;
        wm_pointer_update();
        mouse_get_px(&mx, &my);
        if (mx != lx || my != ly) {
            lx = mx; ly = my;
            what = sm_hit(mx, my, &i);
            int hr = what == 1 ? i : -1, hc = what == 2 ? i : -1, hb = what == 3 ? 1 : what == 4 ? 2 : 0;
            if (hr != g_hrow) { dirty_row(g_hrow); dirty_row(hr); g_hrow = hr; }
            if (hc != g_hcat) { dirty_cat(g_hcat); dirty_cat(hc); g_hcat = hc; }
            if (hb != g_hbtn) { dirty_btns(); g_hbtn = hb; }
        }
        {
            int w = mouse_get_wheel();
            if (w) { g_top -= 3 * w; if (g_top > g_nlist - list_rows) g_top = g_nlist - list_rows; if (g_top < 0) g_top = 0; dirty = 1; }
        }
        int cx, cy, c = mouse_take_wm_clicks(&cx, &cy);
        if (c & 1) {
            what = sm_hit(cx, cy, &i);
            if (what == 0) done = 1;                              /* outside: close */
            else if (what == 1) { launch = g_list[i]; done = 1; }
            else if (what == 2) { g_cat = i; g_qn = 0; g_q[0] = 0; g_sel = 0; g_top = 0; build(); dirty = 1; }
            else if (what == 3) { lock = 1; done = 1; }
            else if (what == 4) { launch = find_id("power"); done = 1; }
        } else if (c) {
            if (sm_hit(cx, cy, &i) == 0) done = 1;
        }
        if (k != -1) {
            dirty = 1;
            if (k == 27) { if (g_qn) { g_qn = 0; g_q[0] = 0; g_sel = 0; g_top = 0; build(); } else done = 1; }
            else if (k == KEY_SUPER) done = 1;
            else if (k == '\n' || k == '\r') { if (g_focus) { g_focus = 0; } else if (g_nlist) { launch = g_list[g_sel]; done = 1; } }
            else if (k == 0x148) { if (g_focus) { if (g_cat > 0) { g_cat--; g_sel = 0; g_top = 0; build(); } } else if (g_sel > 0) g_sel--; }
            else if (k == 0x150) { if (g_focus) { if (g_cat < NCATS - 1) { g_cat++; g_sel = 0; g_top = 0; build(); } } else if (g_sel < g_nlist - 1) g_sel++; }
            else if (k == 0x14B) { if (!g_qn) g_focus = 1; }
            else if (k == 0x14D || k == '\t') { g_focus = !g_focus && k == '\t' && !g_qn ? 1 : 0; }
            else if (k == 0x149) { g_sel -= list_rows; if (g_sel < 0) g_sel = 0; }
            else if (k == 0x151) { g_sel += list_rows; if (g_sel > g_nlist - 1) g_sel = g_nlist - 1; }
            else if (k == 0x147) g_sel = 0;
            else if (k == 0x14F) g_sel = g_nlist - 1;
            else if (k == 8) { if (g_qn) { g_q[--g_qn] = 0; g_sel = 0; g_top = 0; build(); } }
            else if ((k == 4 || (kbd_ctrl() && (k == 'd' || k == 'D'))) && g_nlist) {
                int r = desktop_app_pin_toggle(desktop_app_id(g_list[g_sel]), 0);
                toast_info(r > 0 ? "Put on the desktop" : r == 0 ? "Taken off the desktop" : "The desktop is full");
            }
            else if ((k == 6 || (kbd_ctrl() && (k == 'f' || k == 'F'))) && g_nlist) {
                int r = desktop_app_pin_toggle(desktop_app_id(g_list[g_sel]), 1);
                toast_info(r > 0 ? "Added to Favourites" : r == 0 ? "Taken out of Favourites" : "Favourites is full");
                if (g_cat == C_FAV) build();
            }
            else if (k >= 32 && k < 127 && g_qn < (int)sizeof g_q - 1) {
                g_q[g_qn++] = (char)k; g_q[g_qn] = 0; g_focus = 0; g_sel = 0; g_top = 0; build();
            }
            if (g_sel < 0) g_sel = 0;
            keep_visible();
        }
        if (dirty) { repaint(); g_dirty_part = 0; }
        else if (g_dirty_part) { vga_wm_repaint_popup(g_dx0, g_dy0, g_dx1, g_dy1); g_dirty_part = 0; }
        if (k == -1 && !c) kpoll_sleep_ms(10);
    }
    vga_wm_set_popup(0, 0, 0, 0, 0);
    wm_busy(0);
    if (launch >= 0) open_chosen(desktop_app_id(launch));
    else if (lock) desktop_lock_screen();
}
