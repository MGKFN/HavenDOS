/* shell/pxui.c -- the pixel building blocks (v0.9.10).
 *
 * The roadmap's 0.9.10: "pixel buttons, toggles, checkboxes, lists,
 * scrollbars, text fields, dialogs, menus, tooltips, progress bars; theme
 * colours; High Contrast". The apps rebuilt in 0.9.11-0.9.14 are made of
 * these, and they stop zooming then: a pixel app's window is its own
 * canvas (drivers/wm_core.inc vga_wm_canvas) of whatever size the window
 * is, and it lays itself out for it.
 *
 * IMMEDIATE MODE. There is no tree of widgets to keep in step with the
 * app's state: each frame the app calls pxui_button(r, "Open", ...) and
 * the call draws the button and says whether this frame's click was on
 * it. The app's own variables are the state (a list's selection, a
 * toggle's on/off). The Images app (shell/images.c) is the first user.
 *
 * Colours come from the theme's palette, so every theme -- High Contrast
 * included -- gives controls in its own colours; High Contrast also
 * gets 2-pixel outlines and no shading.
 */
#include "../include/pxui.h"
#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/keys.h"
#include "../include/pit.h"
#include "../include/string.h"

extern int  task_current(void);
extern int  keyboard_getchar(void);
extern void mouse_get_px(int *x, int *y);
extern int  mouse_buttons_raw(void);
extern void kpoll_sleep_ms(uint32_t ms);
extern int  kbd_shift(void);

static uint32_t *g_cv; static int g_w, g_h;

/* ── colours ── */
static uint32_t lift(uint32_t c, int amt)     /* toward white (amt>0) or black */
{
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    if (amt > 0) { r += (255 - r) * amt / 255; g += (255 - g) * amt / 255; b += (255 - b) * amt / 255; }
    else { r = r * (255 + amt) / 255; g = g * (255 + amt) / 255; b = b * (255 + amt) / 255; }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static int luma(uint32_t c){ return (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10; }

static pxui_theme_t g_th;
const pxui_theme_t *pxui_theme(void)
{
    pxui_theme_t *t = &g_th;
    uint32_t p0 = vga_palette_rgb(0), p15 = vga_palette_rgb(15);
    t->hc = (p0 & 0xFFFFFF) == 0 && (p15 & 0xFFFFFF) == 0xFFFFFF && luma(vga_palette_rgb(8)) > 160;
    t->bg = wm_rgb(WMC_PANEL);
    if (luma(t->bg) > 200 && !t->hc) t->bg = lift(t->bg, -12);
    t->text = wm_rgb(WMC_TEXT);
    t->muted = wm_rgb(WMC_TEXT_OFF);
    t->accent = wm_rgb(WMC_TITLE);
    if (luma(t->accent) < 40 || t->hc) t->accent = vga_palette_rgb(14);       /* HC: yellow */
    t->on_accent = luma(t->accent) > 150 ? 0x101010u : 0xffffffu;
    t->panel = luma(t->bg) > 128 ? lift(t->bg, -14) : lift(t->bg, 14);
    t->raised = luma(t->bg) > 128 ? lift(t->bg, -26) : lift(t->bg, 30);
    t->field = luma(t->bg) > 128 ? 0xffffffu : lift(t->bg, -20);
    t->border = t->hc ? p15 : (luma(t->bg) > 128 ? lift(t->bg, -60) : lift(t->bg, 55));
    t->sel = t->accent; t->sel_text = t->on_accent;
    t->danger = vga_palette_rgb(4);
    if (t->hc) { t->bg = 0; t->panel = 0; t->raised = 0; t->field = 0; t->text = 0xffffffu; t->muted = 0xffffffu; }
    t->px = vga_wm_font_px();
    if (t->px < 13) t->px = 13;
    t->rad = t->hc ? 0 : t->px / 3;
    return t;
}

/* ── the canvas ── */
int pxui_begin(void)
{
    g_cv = vga_wm_canvas(&g_w, &g_h);
    if (!g_cv) return 0;
    pxui_theme();
    px_canvas_begin(g_cv, g_w, g_h);
    return 1;
}
int  pxui_w(void){ return g_w; }
int  pxui_h(void){ return g_h; }
void pxui_end(void){ px_canvas_end(); }
void pxui_present(void){ px_canvas_end(); vga_wm_present(0, 0, g_w, g_h); px_canvas_begin(g_cv, g_w, g_h); }
void pxui_present_rect(prect_t r){ px_canvas_end(); vga_wm_present(r.x, r.y, r.w, r.h); px_canvas_begin(g_cv, g_w, g_h); }

/* ── input ── */
static int g_was_down, g_last_mx, g_last_my;
static uint32_t g_last_press_t; static int g_last_px, g_last_py;
void pxui_poll(pxui_in_t *in)
{
    int sx, sy, x, y, w, h, s, part;
    memset(in, 0, sizeof *in);
    in->key = keyboard_getchar();
    if (in->key == '\t') in->focus_next = kbd_shift() ? -1 : 1;
    mouse_get_px(&sx, &sy);
    vga_wm_rect(task_current(), &x, &y, &w, &h);
    in->mx = sx - x; in->my = sy - y;
    vga_wm_hit(sx, sy, &s, &part);
    in->in = s == task_current() && part == WMP_CONTENT;
    in->down = (mouse_buttons_raw() & 1) != 0;
    in->pressed = in->down && !g_was_down && in->in;
    in->released = !in->down && g_was_down;
    if (in->pressed) {
        uint32_t now = get_ticks();
        int dx = in->mx - g_last_px, dy = in->my - g_last_py;
        in->dbl = now - g_last_press_t < 40u && dx * dx + dy * dy < 64;
        g_last_press_t = in->dbl ? 0 : now; g_last_px = in->mx; g_last_py = in->my;
    }
    g_was_down = in->down;
}
int pxui_wait(pxui_in_t *in)
{
    for (;;) {
        pxui_poll(in);
        if (in->key >= 0 || in->pressed || in->released || (in->down && (in->mx != g_last_mx || in->my != g_last_my))) break;
        if (in->in && (in->mx != g_last_mx || in->my != g_last_my)) break;     /* hover */
        kpoll_sleep_ms(8);
    }
    g_last_mx = in->mx; g_last_my = in->my;
    return in->key;
}
int pxui_hit(prect_t r, const pxui_in_t *in)
{
    return in && in->in && in->mx >= r.x && in->my >= r.y && in->mx < r.x + r.w && in->my < r.y + r.h;
}

/* ── drawing ── */
void pxui_fill(prect_t r, uint32_t c){ px_fill(r.x, r.y, r.w, r.h, c); }
void pxui_panel(prect_t r, uint32_t c)
{
    if (g_th.hc) { px_fill(r.x, r.y, r.w, r.h, c); return; }
    px_round(r.x, r.y, r.w, r.h, g_th.rad, c, g_th.bg);
}
void pxui_text_px(int x, int y, const char *s, int px, int bold, uint32_t fg, uint32_t bg, int maxw)
{
    px_text(x, y, s, px, bold, fg, bg, maxw);
}
void pxui_text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int maxw)
{
    px_text(x, y, s, g_th.px, 0, fg, bg, maxw);
}
int pxui_text_w(const char *s){ return px_text_w(s, g_th.px, 0); }
int pxui_line_h(void){ return px_text_h(g_th.px, 0); }

static void outline(prect_t r, uint32_t c, int t)
{
    px_fill(r.x, r.y, r.w, t, c); px_fill(r.x, r.y + r.h - t, r.w, t, c);
    px_fill(r.x, r.y, t, r.h, c); px_fill(r.x + r.w - t, r.y, t, r.h, c);
}
/* a rounded outline: the shape in the border colour, the inside over it */
static void boxed(prect_t r, uint32_t fill, uint32_t edge, uint32_t under)
{
    int t = g_th.hc ? 2 : 1;
    if (g_th.hc || !g_th.rad) { px_fill(r.x, r.y, r.w, r.h, fill); outline(r, edge, t); return; }
    px_round(r.x, r.y, r.w, r.h, g_th.rad, edge, under);
    px_round(r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t, g_th.rad - t > 0 ? g_th.rad - t : 0, fill, edge);
}
static int text_y(prect_t r){ return r.y + (r.h - pxui_line_h()) / 2; }

/* ── controls ── */
int pxui_button(prect_t r, const char *label, int primary, int enabled, const pxui_in_t *in)
{
    int hot = enabled && pxui_hit(r, in), held = hot && in->down;
    uint32_t fill = primary ? g_th.accent : g_th.raised, fg = primary ? g_th.on_accent : g_th.text;
    if (!enabled) { fill = g_th.panel; fg = g_th.muted; }
    else if (held) fill = lift(fill, luma(fill) > 128 ? -30 : 30);
    else if (hot) fill = lift(fill, luma(fill) > 128 ? -15 : 18);
    if (g_th.hc && hot) { fill = 0xffffffu; fg = 0; }
    boxed(r, fill, hot || primary ? g_th.accent : g_th.border, g_th.bg);
    int tw = pxui_text_w(label);
    px_text(r.x + (r.w - tw) / 2, text_y(r), label, g_th.px, primary, fg, fill, r.w - 8);
    return enabled && in && in->released && pxui_hit(r, in);
}
int pxui_icon_button(prect_t r, const char *icon, const char *tip, const pxui_in_t *in)
{
    int hot = pxui_hit(r, in);
    uint32_t fill = hot ? g_th.raised : g_th.bg;
    if (hot) boxed(r, fill, g_th.border, g_th.bg); else px_fill(r.x, r.y, r.w, r.h, fill);
    int k = wm_icon_find(icon), s = px_icon_size(r.h - 8);
    if (k >= 0) px_icon(r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, k, s, fill);
    if (hot && tip) pxui_tooltip(r.x, r.y + r.h + 4, tip);
    return in && in->released && hot;
}
int pxui_toggle(prect_t r, const char *label, int *on, const pxui_in_t *in)
{
    int h = pxui_line_h() + 4, w = h * 2, used = 0;
    prect_t sw = { r.x + r.w - w, r.y + (r.h - h) / 2, w, h };
    if (in && in->released && pxui_hit(r, in)) { *on = !*on; used = 1; }
    px_fill(r.x, r.y, r.w, r.h, g_th.bg);
    pxui_text(r.x, text_y(r), label, g_th.text, g_th.bg, r.w - w - 8);
    uint32_t track = *on ? g_th.accent : g_th.raised;
    if (g_th.hc) { px_fill(sw.x, sw.y, sw.w, sw.h, track); outline(sw, 0xffffffu, 2); }
    else { px_round(sw.x, sw.y, sw.w, sw.h, h / 2, g_th.border, g_th.bg);
           px_round(sw.x + 1, sw.y + 1, sw.w - 2, sw.h - 2, h / 2 - 1, track, g_th.border); }
    int k = h - 6, kx = *on ? sw.x + sw.w - k - 3 : sw.x + 3;
    px_round(kx, sw.y + 3, k, k, k / 2, *on ? g_th.on_accent : g_th.text, track);
    return used;
}
int pxui_checkbox(prect_t r, const char *label, int *on, const pxui_in_t *in)
{
    int s = pxui_line_h(), used = 0;
    prect_t b = { r.x, r.y + (r.h - s) / 2, s, s };
    if (in && in->released && pxui_hit(r, in)) { *on = !*on; used = 1; }
    px_fill(r.x, r.y, r.w, r.h, g_th.bg);
    boxed(b, *on ? g_th.accent : g_th.field, *on ? g_th.accent : g_th.border, g_th.bg);
    if (*on) {
        uint32_t c = g_th.on_accent;
        px_line(b.x + s / 4, b.y + s / 2, b.x + s * 5 / 12, b.y + s * 3 / 4 - 1, s > 16 ? 2 : 1, c, g_th.accent);
        px_line(b.x + s * 5 / 12, b.y + s * 3 / 4 - 1, b.x + s * 3 / 4, b.y + s / 4, s > 16 ? 2 : 1, c, g_th.accent);
    }
    pxui_text(r.x + s + 8, text_y(r), label, g_th.text, g_th.bg, r.w - s - 8);
    return used;
}

void pxui_scrollbar(prect_t r, int total, int shown, int *top, const pxui_in_t *in)
{
    px_fill(r.x, r.y, r.w, r.h, g_th.panel);
    if (total <= shown || total <= 0) return;
    int th = r.h * shown / total, maxtop = total - shown;
    if (th < r.w * 2) th = r.w * 2;
    if (th > r.h) th = r.h;
    if (*top > maxtop) *top = maxtop;
    if (*top < 0) *top = 0;
    int ty = r.y + (r.h - th) * *top / (maxtop ? maxtop : 1);
    if (in && in->down && pxui_hit(r, in)) {
        int t = (in->my - r.y - th / 2) * maxtop / (r.h - th > 0 ? r.h - th : 1);
        *top = t < 0 ? 0 : t > maxtop ? maxtop : t;
        ty = r.y + (r.h - th) * *top / (maxtop ? maxtop : 1);
    }
    px_round(r.x + 2, ty + 2, r.w - 4, th - 4, (r.w - 4) / 2, g_th.hc ? 0xffffffu : g_th.border, g_th.panel);
}

int pxui_list(prect_t r, const char *const *items, int n, int *sel, int *top, int focused, const pxui_in_t *in)
{
    int lh = pxui_line_h() + 10, sbw = lh / 2 + 4, rows = r.h / lh, opened = 0;
    if (rows < 1) rows = 1;
    if (focused && in) {
        if (in->key == KEY_UP && *sel > 0) (*sel)--;
        if (in->key == KEY_DOWN && *sel < n - 1) (*sel)++;
        if (in->key == KEY_PGUP) *sel = *sel > rows ? *sel - rows : 0;
        if (in->key == KEY_PGDN) *sel = *sel + rows < n ? *sel + rows : n - 1;
        if (in->key == KEY_HOME) *sel = 0;
        if (in->key == KEY_END) *sel = n - 1;
        if ((in->key == '\n' || in->key == '\r') && *sel >= 0 && *sel < n) opened = 1;
    }
    prect_t body = { r.x, r.y, n > rows ? r.w - sbw : r.w, r.h };
    if (in && in->pressed && pxui_hit(body, in)) {
        int k = *top + (in->my - r.y) / lh;
        if (k >= 0 && k < n) { if (in->dbl && k == *sel) opened = 1; *sel = k; }
    }
    if (*sel >= 0) { if (*sel < *top) *top = *sel; if (*sel >= *top + rows) *top = *sel - rows + 1; }
    if (*top > n - rows) *top = n - rows;
    if (*top < 0) *top = 0;
    px_fill(r.x, r.y, r.w, r.h, g_th.field);
    for (int i = 0; i < rows && *top + i < n; i++) {
        int k = *top + i, y = r.y + i * lh;
        prect_t row = { body.x + 2, y + 1, body.w - 4, lh - 2 };
        uint32_t bg = g_th.field, fg = g_th.text;
        if (k == *sel) { bg = focused ? g_th.sel : g_th.raised; fg = focused ? g_th.sel_text : g_th.text; pxui_panel(row, bg); }
        else if (pxui_hit(row, in)) { bg = lift(g_th.field, luma(g_th.field) > 128 ? -10 : 12); px_round(row.x, row.y, row.w, row.h, g_th.rad, bg, g_th.field); }
        px_text(row.x + 8, y + (lh - pxui_line_h()) / 2, items[k], g_th.px, 0, fg, bg, row.w - 16);
    }
    if (n > rows) {
        prect_t sb = { r.x + r.w - sbw, r.y, sbw, r.h };
        pxui_scrollbar(sb, n, rows, top, in);
    }
    outline(r, focused ? g_th.accent : g_th.border, g_th.hc ? 2 : 1);
    return opened;
}

int pxui_textfield(prect_t r, char *buf, int cap, int *focus, const char *hint, const pxui_in_t *in)
{
    int enter = 0;
    if (in && in->pressed) *focus = pxui_hit(r, in);
    if (*focus && in && in->key > 0) {
        int n = (int)strlen(buf);
        if (in->key == '\b') { if (n) buf[n - 1] = 0; }
        else if (in->key == '\n' || in->key == '\r') enter = 1;
        else if (in->key >= 32 && in->key < 127 && n < cap - 1) { buf[n] = (char)in->key; buf[n + 1] = 0; }
    }
    boxed(r, g_th.field, *focus ? g_th.accent : g_th.border, g_th.bg);
    int ty = text_y(r), x = r.x + 8;
    if (!buf[0] && hint && !*focus) pxui_text(x, ty, hint, g_th.muted, g_th.field, r.w - 16);
    else {
        /* the end of a long text stays in view */
        const char *s = buf;
        while (*s && pxui_text_w(s) > r.w - 20) s++;
        int w = px_text(x, ty, s, g_th.px, 0, g_th.text, g_th.field, 0);
        if (*focus) px_fill(x + w + 1, ty, 2, pxui_line_h(), g_th.accent);
    }
    return enter;
}

void pxui_progress(prect_t r, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int rad = g_th.hc ? 0 : r.h / 2;
    px_round(r.x, r.y, r.w, r.h, rad, g_th.raised, g_th.bg);
    int w = r.w * pct / 100;
    if (w >= r.h || (g_th.hc && w > 0)) px_round(r.x, r.y, w, r.h, rad, g_th.accent, g_th.raised);
    if (g_th.hc) outline(r, 0xffffffu, 2);
}

void pxui_tooltip(int x, int y, const char *text)
{
    int px = g_th.px - 1, w = px_text_w(text, px, 0) + 16, h = px_text_h(px, 0) + 10;
    if (x + w > g_w) x = g_w - w - 2;
    if (y + h > g_h) y = g_h - h - 2;
    if (x < 0) x = 0;
    uint32_t bg = g_th.hc ? 0xffffffu : 0x2a2f38u, fg = g_th.hc ? 0 : 0xf0ece4u;
    px_round(x, y, w, h, g_th.rad / 2, bg, g_th.bg);
    px_text(x + 8, y + 5, text, px, 0, fg, bg, 0);
}

/* ── modal: they run their own loop and return ── */
int pxui_menu(int x, int y, const char *const *items, int n)
{
    int lh = pxui_line_h() + 12, w = 0;
    for (int i = 0; i < n; i++) { int t = pxui_text_w(items[i]); if (t > w) w = t; }
    w += 40;
    int h = n * lh + 8;
    if (x + w > g_w) x = g_w - w - 2;
    if (y + h > g_h) y = g_h - h - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int sel = 0, first = 1;
    pxui_in_t in;
    for (;;) {
        prect_t box = { x, y, w, h };
        boxed(box, g_th.raised, g_th.border, g_th.bg);
        for (int i = 0; i < n; i++) {
            prect_t row = { x + 4, y + 4 + i * lh, w - 8, lh };
            uint32_t bg = i == sel ? g_th.sel : g_th.raised, fg = i == sel ? g_th.sel_text : g_th.text;
            if (i == sel) pxui_panel(row, bg); else px_fill(row.x, row.y, row.w, row.h, bg);
            px_text(row.x + 12, row.y + 6, items[i], g_th.px, 0, fg, bg, row.w - 20);
        }
        pxui_present_rect(box);
        pxui_wait(&in);
        if (first && in.released) { first = 0; continue; }   /* the click that opened it */
        first = 0;
        if (in.key == 0x1B) return -1;
        if (in.key == KEY_UP && sel > 0) sel--;
        if (in.key == KEY_DOWN && sel < n - 1) sel++;
        if (in.key == '\n' || in.key == '\r') return sel;
        if (in.in && in.mx >= x && in.mx < x + w && in.my >= y + 4 && in.my < y + 4 + n * lh)
            sel = (in.my - y - 4) / lh;
        if (in.released) {
            if (in.in && in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h) return sel;
            return -1;
        }
    }
}

int pxui_dialog(const char *title, const char *msg, const char *const *buttons, int n)
{
    int px = g_th.px, w = px_text_w(msg, px, 0) + 64, bw = 0, lh = pxui_line_h();
    for (int i = 0; i < n; i++) { int t = pxui_text_w(buttons[i]) + 32; if (t > bw) bw = t; }
    if (w < n * (bw + 12) + 48) w = n * (bw + 12) + 48;
    if (w < px_text_w(title, px + 3, 1) + 64) w = px_text_w(title, px + 3, 1) + 64;
    if (w > g_w - 16) w = g_w - 16;
    int h = lh * 5 + 48, x = (g_w - w) / 2, y = (g_h - h) / 2, sel = 0;
    pxui_in_t in; memset(&in, 0, sizeof in);
    for (;;) {
        prect_t box = { x, y, w, h };
        boxed(box, g_th.raised, g_th.accent, g_th.bg);
        px_text(x + 24, y + 18, title, px + 3, 1, g_th.text, g_th.raised, w - 48);
        px_text(x + 24, y + 26 + lh * 3 / 2, msg, px, 0, g_th.muted, g_th.raised, w - 48);
        int r = -1;
        for (int i = 0; i < n; i++) {
            prect_t b = { x + w - 24 - (n - i) * (bw + 12) + 12, y + h - lh - 34, bw, lh + 14 };
            if (pxui_button(b, buttons[i], i == sel, 1, &in)) r = i;
        }
        pxui_present_rect(box);
        if (r >= 0) return r;
        pxui_wait(&in);
        if (in.key == 0x1B) return -1;
        if (in.key == '\n' || in.key == '\r') return sel;
        if (in.key == KEY_LEFT && sel > 0) sel--;
        if ((in.key == KEY_RIGHT || in.key == '\t') && sel < n - 1) sel++;
    }
}
