/* drivers/wm_core.inc -- the window manager's pixels (v0.9.8)
 *
 * Included at the end of drivers/vga.c: it works on the same framebuffer,
 * cell renderer and pointer, and needs their statics.
 *
 * Kyan, 24 Nov 2026: HavenDOS "feels old"; he chose a pixel desktop, step
 * by step -- 0.9.8 is the desktop. Until now the screen WAS one console:
 * the task in front owned every cell, and an app's window -- title bar,
 * taskbar, the desktop around an 80x25 box -- was drawn as characters
 * into that app's own console. Here the screen becomes a stack of LAYERS,
 * drawn in pixels:
 *
 *     the desktop      slot 0's console, on the grid as before
 *     the windows      each an app's console, its viewport only, at any
 *                      pixel position, with a title bar, an edge, round
 *                      corners and a shadow -- bottom to top
 *     the taskbar      painted by a callback (shell/taskbar.c)
 *     a popup          the Start menu (shell/startmenu.c), on top
 *     the pointer      over everything
 *
 * EVERY PIXEL IS PAINTED ONCE, BY ITS OWNER. wm_owner(x,y) is the topmost
 * layer whose shape holds the pixel; a layer paints only pixels it owns,
 * darkened by the shadows of windows above it. So a window moving, a cell
 * of a window behind changing, a menu opening, never paint something that
 * is then painted over: nothing flickers, and no off-screen copy of the
 * screen is needed (a 1920x1080 one would be 8 MB of the 128 MB minimum).
 *
 * Cells are still what apps draw. Each console keeps its own record of what
 * is on the glass (wm_painted[slot][], the desktop's is fbc_painted[]), and
 * a cell that changes is painted where its window is now -- also when the
 * window is behind another, and also when it is not the task running.
 *
 * WM mode is on only while the desktop session runs (vga_wm_enable). Off,
 * every line of the console behaves exactly as it did in 0.9.7.10.
 */
#include "../include/icons.h"
#include "../include/font_ui.h"
#include "../include/wm.h"

#define WM_SLOTS 8

typedef struct {
    int       on;
    uint16_t *cells;
    int       vx, vy, vw, vh;      /* its viewport, in its console's cells */
    int       x, y;                /* content's top-left on the screen, px */
    int       mode;                /* WM_FLOAT, WM_MAXED                   */
    int       min;
    char      title[40];
    int       icon;
    int       hover, down;         /* WMP_* of the title bar               */
    int       full;                /* has the whole screen while in front   */
} wm_win_t;

static wm_win_t  wm_w[WM_SLOTS];
static uint16_t  wm_painted[WM_SLOTS][VGA_CELLS_MAX];
static int       wm_z[WM_SLOTS], wm_nz;       /* window slots, bottom to top */
static int       wm_front;
static uint16_t *wm_solo_cells;               /* a console that has the whole screen */
static uint16_t  wm_solo_painted[VGA_CELLS_MAX];
static int       wm_solo_depth;
static wm_paint_fn wm_bar_fn;
static wm_paint_fn wm_pop_fn;
static int       wm_pop_x, wm_pop_y, wm_pop_w, wm_pop_h;

/* ── metrics ────────────────────────────────────────────────────────── */
int vga_wm_title_h(void){ return fbc_chp >= 24u ? (int)fbc_chp : 2 * (int)fbc_chp; }
/* The taskbar covers the grid's last row(s) and whatever is left of the
   screen below them, so nothing of the desktop's own last row shows. */
int vga_wm_bar_h(void)
{
    int tr = fbc_chp >= 24u ? 1 : 2;
    int y0 = (int)fbc_oy + (g_rows - tr) * (int)fbc_chp;
    int h = (int)fbc_h - y0;
    return h < vga_wm_title_h() ? vga_wm_title_h() : h;
}
int vga_wm_title_rows(void){ return fbc_chp >= 24u ? 1 : 2; }
static int wm_radius(void){ int r = vga_wm_title_h() / 4; return r < 4 ? 4 : r; }
static int wm_shadow(void){ int s = vga_wm_title_h() / 2; return s < 8 ? 8 : s; }
int vga_wm_font_px(void)
{
    int t = vga_wm_title_h() / 2;
    return t < 12 ? 12 : t > 24 ? 24 : t;
}
int vga_screen_w(void){ return (int)fbc_w; }
/* The pointer reaches the grid, not the margins beside it: whatever has
   to be clicked is laid out between these two. */
void vga_wm_reach(int *x0, int *x1)
{
    *x0 = (int)fbc_ox; *x1 = (int)fbc_ox + g_cols * (int)fbc_cwp;
}
int vga_screen_h(void){ return (int)fbc_h; }
int vga_cell_w(void){ return (int)fbc_cwp; }
int vga_cell_h(void){ return (int)fbc_chp; }
void vga_mouse_px(int col, int row, int fx, int fy, int *x, int *y)
{
    *x = (int)fbc_ox + col * (int)fbc_cwp + fx;
    *y = (int)fbc_oy + row * (int)fbc_chp + fy;
}

/* ── the layers, rebuilt when anything moves ────────────────────────── */
enum { LT_DESK, LT_WIN, LT_BAR, LT_POP, LT_SOLO };
typedef struct { int type, slot, x0, y0, x1, y1, r, sh; } wm_layer_t;
static wm_layer_t wm_L[WM_SLOTS + 4];
static int        wm_nL;

static void win_content(const wm_win_t *w, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = w->x; *y0 = w->y;
    *x1 = w->x + w->vw * (int)fbc_cwp; *y1 = w->y + w->vh * (int)fbc_chp;
}
static int win_border(const wm_win_t *w){ return w->mode == WM_MAXED ? 0 : 1; }
static void win_frame(const wm_win_t *w, int *x0, int *y0, int *x1, int *y1)
{
    int b = win_border(w);
    win_content(w, x0, y0, x1, y1);
    if (w->mode == WM_MAXED) {            /* edge to edge, from the top */
        /* v0.9.8.2: down to the taskbar, whatever size the content is --
           an 80x25 app maximised sits in the middle of it */
        *x0 = 0; *x1 = (int)fbc_w; *y0 = 0;
        if (wm_bar_fn && (int)fbc_h - vga_wm_bar_h() > *y1) *y1 = (int)fbc_h - vga_wm_bar_h();
        return;
    }
    *x0 -= b; *x1 += b; *y1 += b; *y0 -= vga_wm_title_h() + b;
}
/* the title bar's rectangle */
static void win_title(const wm_win_t *w, int *x0, int *y0, int *x1, int *y1)
{
    int fx0, fy0, fx1, fy1, cx0, cy0, cx1, cy1, b = win_border(w);
    win_frame(w, &fx0, &fy0, &fx1, &fy1);
    win_content(w, &cx0, &cy0, &cx1, &cy1);
    *x0 = fx0 + b; *x1 = fx1 - b; *y0 = fy0 + b; *y1 = cy0;
    if (w->mode == WM_MAXED) {            /* v0.9.8.2: the grid's title rows */
        int t = (int)fbc_oy + vga_wm_title_rows() * (int)fbc_chp;
        if (t < vga_wm_title_h()) t = vga_wm_title_h();
        if (t < *y1) *y1 = t;
    }
}

static int front_full(void)
{
    return wm_front > 0 && wm_front < WM_SLOTS && wm_w[wm_front].on && wm_w[wm_front].full
        && !wm_w[wm_front].min;
}
static void wm_rebuild(void)
{
    wm_nL = 0;
    if (wm_solo_depth) {
        wm_L[wm_nL++] = (wm_layer_t){ LT_SOLO, -1, 0, 0, (int)fbc_w, (int)fbc_h, 0, 0 };
        return;
    }
    if (front_full()) {           /* a window with the whole screen, in front */
        wm_L[wm_nL++] = (wm_layer_t){ LT_SOLO, wm_front, 0, 0, (int)fbc_w, (int)fbc_h, 0, 0 };
        return;
    }
    wm_L[wm_nL++] = (wm_layer_t){ LT_DESK, 0, 0, 0, (int)fbc_w, (int)fbc_h, 0, 0 };
    for (int k = 0; k < wm_nz; k++) {
        wm_win_t *w = &wm_w[wm_z[k]];
        if (!w->on || w->min || w->full) continue;
        wm_layer_t L; L.type = LT_WIN; L.slot = wm_z[k];
        win_frame(w, &L.x0, &L.y0, &L.x1, &L.y1);
        L.r = w->mode == WM_MAXED ? 0 : wm_radius();
        L.sh = w->mode == WM_MAXED ? 0 : 1;
        wm_L[wm_nL++] = L;
    }
    if (wm_bar_fn)
        wm_L[wm_nL++] = (wm_layer_t){ LT_BAR, -1, 0, (int)fbc_h - vga_wm_bar_h(), (int)fbc_w, (int)fbc_h, 0, 0 };
    if (wm_pop_fn)
        wm_L[wm_nL++] = (wm_layer_t){ LT_POP, -1, wm_pop_x, wm_pop_y, wm_pop_x + wm_pop_w,
                                      wm_pop_y + wm_pop_h, wm_radius(), 1 };
}

static inline int in_layer(const wm_layer_t *L, int x, int y)
{
    if (x < L->x0 || y < L->y0 || x >= L->x1 || y >= L->y1) return 0;
    if (L->r) {
        int r = L->r, cx, cy;
        if (x < L->x0 + r)       cx = L->x0 + r;
        else if (x >= L->x1 - r) cx = L->x1 - r - 1;
        else return 1;
        if (y < L->y0 + r)       cy = L->y0 + r;
        else if (y >= L->y1 - r) cy = L->y1 - r - 1;
        else return 1;
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > r * r) return 0;
    }
    return 1;
}
static int wm_owner(int x, int y)
{
    for (int i = wm_nL - 1; i > 0; i--) if (in_layer(&wm_L[i], x, y)) return i;
    return 0;
}

/* The shadow of layer j at (x,y): 0..110 of 255, fading out over the
   shadow's depth, a little more below than to the side. */
static int shadow_at(const wm_layer_t *L, int x, int y)
{
    int s = wm_shadow(), off = s / 3;
    int x0 = L->x0 + off / 2, y0 = L->y0 + off, x1 = L->x1 + off / 2, y1 = L->y1 + off;
    /* light from above and a little left: the shadow is below and to the
       right, and fades three times as fast on the other two sides */
    int dx = x < x0 ? 3 * (x0 - x) : (x >= x1 ? x - x1 + 1 : 0);
    int dy = y < y0 ? 3 * (y0 - y) : (y >= y1 ? y - y1 + 1 : 0);
    int d = dx > dy ? dx + dy / 2 : dy + dx / 2;
    if (d >= s) return 0;
    int t = s - d;                         /* 1 .. s */
    return 110 * t * t / (s * s);
}
static inline uint32_t wm_darken(uint32_t c, int a)
{
    uint32_t k = 255u - (uint32_t)a;
    return ((((c >> 16) & 0xFF) * k / 255u) << 16) | ((((c >> 8) & 0xFF) * k / 255u) << 8)
         | ((c & 0xFF) * k / 255u);
}
static uint32_t shade(int owner, int x, int y, uint32_t c)
{
    int a = 0;
    for (int j = owner + 1; j < wm_nL; j++)
        if (wm_L[j].sh) { int s = shadow_at(&wm_L[j], x, y); if (s > a) a = s; }
    return a ? wm_darken(c, a) : c;
}

/* Nothing above layer i -- no layer, no shadow -- touches this rect: its
   pixels can be written without asking each one who owns it. */
static int clear_above(int i, int x0, int y0, int x1, int y1)
{
    int s = wm_shadow();
    for (int j = i + 1; j < wm_nL; j++) {
        const wm_layer_t *L = &wm_L[j];
        int e = L->sh ? s : 0;
        if (x1 > L->x0 - e && x0 < L->x1 + e && y1 > L->y0 - e && y0 < L->y1 + e) return 0;
    }
    return 1;
}
/* ... and this rect is wholly under one layer above i: nothing of i shows */
static int covered_above(int i, int x0, int y0, int x1, int y1)
{
    for (int j = i + 1; j < wm_nL; j++) {
        const wm_layer_t *L = &wm_L[j];
        int r = L->r;
        if (x0 >= L->x0 + r && x1 <= L->x1 - r && y0 >= L->y0 && y1 <= L->y1) return 1;
        if (x0 >= L->x0 && x1 <= L->x1 && y0 >= L->y0 + r && y1 <= L->y1 - r) return 1;
    }
    return 0;
}

/* ── one pixel, for the layer being painted ─────────────────────────── */
static int wm_cl;                                /* the layer painting now */
static int wm_cx0, wm_cy0, wm_cx1, wm_cy1;      /* and where it may       */
static int wm_fast;                              /* see clear_above()      */
static int wm_ex, wm_ex0, wm_ey0, wm_ex1, wm_ey1; /* a hole the paint skips */

static inline void wm_px(int x, int y, uint32_t c)
{
    if (x < wm_cx0 || y < wm_cy0 || x >= wm_cx1 || y >= wm_cy1) return;
    if (wm_ex && x >= wm_ex0 && x < wm_ex1 && y >= wm_ey0 && y < wm_ey1) return;
    if (wm_fast == 2) {                 /* nothing above: only its own round corners */
        if (wm_L[wm_cl].r && !in_layer(&wm_L[wm_cl], x, y)) return;
    } else if (!wm_fast) {
        if (wm_owner(x, y) != wm_cl) return;
        c = shade(wm_cl, x, y, c);
    }
    fbc_addr[(uint32_t)y * fbc_pitch + (uint32_t)x] = c;
}

static void ptr_over_px(int x0, int y0, int x1, int y1)
{
    if (!fbc_ptr_on) return;
    int px1 = fbc_ptr_x + PTR_W * fbc_ptr_s, py1 = fbc_ptr_y + PTR_H * fbc_ptr_s;
    if (px1 <= x0 || fbc_ptr_x >= x1 || py1 <= y0 || fbc_ptr_y >= y1) return;
    uint32_t edge = 0x13161cu, fill = fbc_ptr_down ? 0xe2673fu : 0xe8e4dau;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fbc_w) x1 = (int)fbc_w;
    if (y1 > (int)fbc_h) y1 = (int)fbc_h;
    for (int y = (y0 > fbc_ptr_y ? y0 : fbc_ptr_y); y < (y1 < py1 ? y1 : py1); y++) {
        const char *row = ptr_shape[(y - fbc_ptr_y) / fbc_ptr_s];
        for (int x = (x0 > fbc_ptr_x ? x0 : fbc_ptr_x); x < (x1 < px1 ? x1 : px1); x++) {
            char c = row[(x - fbc_ptr_x) / fbc_ptr_s];
            if (c == '1')      fbc_addr[(uint32_t)y * fbc_pitch + (uint32_t)x] = edge;
            else if (c == '2') fbc_addr[(uint32_t)y * fbc_pitch + (uint32_t)x] = fill;
        }
    }
}

static void clip_set(int x0, int y0, int x1, int y1)
{
    wm_cx0 = x0 < 0 ? 0 : x0; wm_cy0 = y0 < 0 ? 0 : y0;
    wm_cx1 = x1 > (int)fbc_w ? (int)fbc_w : x1; wm_cy1 = y1 > (int)fbc_h ? (int)fbc_h : y1;
}

static void di_over(int x0, int y0, int x1, int y1);
static int  wm_ndi;
/* One cell of layer i, its top-left at screen (sx,sy), within the clip. */
static void wm_cell(int i, uint16_t entry, int sx, int sy)
{
    int x0 = sx, y0 = sy, x1 = sx + (int)fbc_cwp, y1 = sy + (int)fbc_chp;
    if (x0 < wm_cx0) x0 = wm_cx0;
    if (y0 < wm_cy0) y0 = wm_cy0;
    if (x1 > wm_cx1) x1 = wm_cx1;
    if (y1 > wm_cy1) y1 = wm_cy1;
    if (x0 >= x1 || y0 >= y1) return;
    if (covered_above(i, x0, y0, x1, y1)) return;
    wm_cl = i;
    wm_fast = clear_above(i, x0, y0, x1, y1);
    for (int y = y0; y < y1; y++) {
        glyph_row(entry, (uint32_t)(y - sy), (uint32_t)y, g_rowbuf);
        if (wm_fast) {
            uint32_t *line = fbc_addr + (uint32_t)y * fbc_pitch;
            for (int x = x0; x < x1; x++) line[x] = g_rowbuf[x - sx];
        } else {
            for (int x = x0; x < x1; x++) wm_px(x, y, g_rowbuf[x - sx]);
        }
    }
    wm_fast = 0;
    if (wm_L[i].type == LT_DESK && wm_ndi) di_over(x0, y0, x1, y1);   /* the icons on it */
}

/* ── text and icons, for anything painting in pixels ────────────────── */
static const fui_face_t *wm_face(int px, int bold)
{
    const fui_face_t *best = &fui_faces[0];
    for (int i = 0; i < FUI_NFACES; i++) {
        const fui_face_t *f = &fui_faces[i];
        if (f->bold != (bold ? 1 : 0)) continue;
        if (f->px <= px && (best->bold != f->bold || f->px >= best->px)) best = f;
    }
    return best;
}
static inline uint32_t mix(uint32_t fg, uint32_t bg, uint32_t a)
{
    uint32_t ia = 255u - a;
    return ((((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia) / 255u) << 16
         | ((((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia) / 255u) << 8
         | (((fg & 0xFF) * a + (bg & 0xFF) * ia) / 255u);
}
static inline uint32_t bg_at(uint32_t bg, int y)
{
    return bg == WM_BG_GRAD ? fbc_bg0((uint32_t)(y < 0 ? 0 : y)) : bg;
}

int px_text_w(const char *s, int px, int bold)
{
    const fui_face_t *f = wm_face(px, bold);
    int w = 0;
    for (; *s; s++) {
        int c = (uint8_t)*s;
        if (c < FUI_FIRST || c >= FUI_FIRST + FUI_NCH) c = '?';
        w += f->g[c - FUI_FIRST].adv;
    }
    return w;
}
int px_text_h(int px, int bold){ return wm_face(px, bold)->height; }

/* Text with its top at y, clipped at maxw (with an ellipsis), over a
   known background -- so it is blended without reading the screen. */
int px_text(int x, int y, const char *s, int px, int bold, uint32_t fg, uint32_t bg, int maxw)
{
    const fui_face_t *f = wm_face(px, bold);
    int x0 = x, dots = 0;
    if (maxw > 0 && px_text_w(s, px, bold) > maxw) dots = 1;
    int lim = dots ? x0 + maxw - px_text_w("...", px, bold) : 0x7FFFFFFF;
    for (; *s; s++) {
        int c = (uint8_t)*s;
        if (c < FUI_FIRST || c >= FUI_FIRST + FUI_NCH) c = '?';
        const fui_glyph_t *g = &f->g[c - FUI_FIRST];
        if (x + g->adv > lim) break;
        const uint8_t *a = &f->data[g->off];
        for (int gy = 0; gy < g->h; gy++)
            for (int gx = 0; gx < g->w; gx++) {
                uint32_t v = a[gy * g->w + gx];
                if (!v) continue;
                int X = x + g->bx + gx, Y = y + g->by + gy;
                uint32_t b = bg_at(bg, Y);
                wm_px(X, Y, v == 255u ? fg : mix(fg, b, v));
            }
        x += g->adv;
    }
    if (dots) x += px_text(x, y, "...", px, bold, fg, bg, 0);
    return x - x0;
}

int wm_icon_find(const char *name)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        const char *a = icon_name[i], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}
static int icon_size_index(int size)
{
    int k = 0;
    for (int i = 0; i < ICON_SIZES; i++) if (icon_size_px[i] <= size) k = i;
    return k;
}
int px_icon_size(int want){ return icon_size_px[icon_size_index(want)]; }

/* An icon: the tile in the icon's colour, lighter at the top, and the
   white symbol, both mixed over bg by their coverage. */
void px_icon(int x, int y, int icon, int size, uint32_t bg)
{
    if (icon < 0 || icon >= ICON_COUNT) icon = 0;
    int k = icon_size_index(size), s = icon_size_px[k];
    const uint8_t *p = &icon_px[(uint32_t)icon * ICON_STRIDE + icon_size_off[k]];
    uint32_t base = icon_rgb[icon];
    for (int iy = 0; iy < s; iy++) {
        /* top +15%, bottom -15% */
        int f = 1150 - 300 * iy / (s > 1 ? s - 1 : 1);
        uint32_t r = ((base >> 16) & 0xFF) * (uint32_t)f / 1000u, g = ((base >> 8) & 0xFF) * (uint32_t)f / 1000u,
                 b = (base & 0xFF) * (uint32_t)f / 1000u;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        uint32_t tile = (r << 16) | (g << 8) | b;
        for (int ix = 0; ix < s; ix++) {
            uint8_t v = p[iy * s + ix];
            uint32_t t = (v >> 4) * 17u, sy = (v & 15u) * 17u;
            if (!t && !sy) continue;
            uint32_t c = mix(tile, bg_at(bg, y + iy), t);
            if (sy) c = mix(0xf4f2ecu, c, sy);
            wm_px(x + ix, y + iy, c);
        }
    }
}

void px_fill(int x, int y, int w, int h, uint32_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) wm_px(xx, yy, bg_at(c, yy));
}
/* A filled rectangle with round corners, the curve's edge pixels mixed
   with bg by how much of each the curve covers (4x4 samples). */
void px_round(int x, int y, int w, int h, int r, uint32_t c, uint32_t bg)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            int cx, cy;
            if (xx < x + r) cx = x + r; else if (xx >= x + w - r) cx = x + w - r - 1; else cx = -1;
            if (yy < y + r) cy = y + r; else if (yy >= y + h - r) cy = y + h - r - 1; else cy = -1;
            if (cx < 0 || cy < 0 || r == 0) { wm_px(xx, yy, bg_at(c, yy)); continue; }
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = (xx - cx) * 8 + sx * 2 - 3, dy = (yy - cy) * 8 + sy * 2 - 3;
                    if (dx * dx + dy * dy <= (r * 8 + 4) * (r * 8 + 4)) in++;
                }
            if (in == 16) wm_px(xx, yy, bg_at(c, yy));
            else if (in) wm_px(xx, yy, mix(bg_at(c, yy), bg_at(bg, yy), (uint32_t)in * 255u / 16u));
        }
}
/* A line from (x0,y0) to (x1,y1), `t` pixels thick, edges mixed into bg. */
void px_line(int x0, int y0, int x1, int y1, int t, uint32_t c, uint32_t bg)
{
    int minx = (x0 < x1 ? x0 : x1) - t, maxx = (x0 > x1 ? x0 : x1) + t;
    int miny = (y0 < y1 ? y0 : y1) - t, maxy = (y0 > y1 ? y0 : y1) + t;
    long dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    long qx = (long)(x - x0) * 4 + sx, qy = (long)(y - y0) * 4 + sy;   /* quarter px */
                    long tq = len2 ? (qx * dx + qy * dy) : 0;
                    long ex, ey;
                    if (tq <= 0 || !len2) { ex = qx; ey = qy; }
                    else if (tq >= len2 * 4) { ex = qx - dx * 4; ey = qy - dy * 4; }
                    else { ex = qx - dx * tq / len2; ey = qy - dy * tq / len2; }
                    if (ex * ex + ey * ey <= (long)t * t * 4) in++;
                }
            if (in) wm_px(x, y, in == 16 ? c : mix(c, bg_at(bg, y), (uint32_t)in * 255u / 16u));
        }
}

/* ── the colours, from the theme's palette ──────────────────────────── */
uint32_t wm_rgb(int role)
{
    switch (role) {
    case WMC_TITLE:      return fbc_rgb[1];
    case WMC_TITLE_OFF:  return fbc_rgb[8];
    case WMC_TEXT:       return fbc_rgb[15];
    case WMC_TEXT_OFF:   return fbc_rgb[7];
    case WMC_EDGE:       return fbc_rgb[8];
    case WMC_ACCENT:     return fbc_rgb[12];
    case WMC_CLOSE:      return fbc_rgb[4];
    case WMC_PANEL:      return fbc_grad_on ? fbc_grad_top : fbc_rgb[0];
    default:             return fbc_rgb[7];
    }
}
static uint32_t lighten(uint32_t c, int a)
{
    return mix(0xffffffu, c, (uint32_t)a);
}

/* ── a window's title bar and edge ──────────────────────────────────── */
static void wm_btn_rect(const wm_win_t *w, int part, int *x0, int *y0, int *x1, int *y1)
{
    int tx0, ty0, tx1, ty1, bw = vga_wm_title_h() * 3 / 2;
    win_title(w, &tx0, &ty0, &tx1, &ty1);
    int k = part == WMP_CLOSE ? 1 : part == WMP_MAX ? 2 : 3;
    *x1 = tx1 - (k - 1) * bw; *x0 = *x1 - bw; *y0 = ty0; *y1 = ty1;
}

static void wm_paint_title(int i, const wm_win_t *w, int slot)
{
    int cx0, cy0, cx1, cy1, tx0, ty0, tx1, ty1, b = win_border(w);
    int focus = slot == wm_front, th = vga_wm_title_h();
    int sx0 = wm_cx0, sy0 = wm_cy0, sx1 = wm_cx1, sy1 = wm_cy1;
    win_content(w, &cx0, &cy0, &cx1, &cy1);
    win_title(w, &tx0, &ty0, &tx1, &ty1);
    uint32_t bg = focus ? wm_rgb(WMC_TITLE) : wm_rgb(WMC_TITLE_OFF);
    uint32_t fg = focus ? wm_rgb(WMC_TEXT) : wm_rgb(WMC_TEXT_OFF);
    int fx0, fy0, fx1, fy1;
    win_frame(w, &fx0, &fy0, &fx1, &fy1);
    wm_cl = i;
    wm_fast = clear_above(i, fx0, fy0, fx1, fy1) ? 2 : 0;    /* v0.9.8.1 */
    if (b) {
        /* the edge: a 1 px ring; the title bar and the content are a hole in it */
        wm_ex = 1; wm_ex0 = tx0; wm_ey0 = ty0; wm_ex1 = tx1; wm_ey1 = cy1;
        px_fill(fx0, fy0, fx1 - fx0, fy1 - fy0, focus ? lighten(bg, 50) : wm_rgb(WMC_EDGE));
        wm_ex = 0;
    } else {
        /* edge to edge: the screen beside the grid is the window's too */
        wm_ex = 1; wm_ex0 = cx0; wm_ey0 = cy0; wm_ex1 = cx1; wm_ey1 = cy1;
        px_fill(fx0, ty1, fx1 - fx0, fy1 - ty1, WM_BG_GRAD);
        wm_ex = 0;
    }
    clip_set(sx0 > tx0 ? sx0 : tx0, sy0 > ty0 ? sy0 : ty0, sx1 < tx1 ? sx1 : tx1, sy1 < ty1 ? sy1 : ty1);
    if (wm_cx0 < wm_cx1 && wm_cy0 < wm_cy1) {
        int h = ty1 - ty0;
        px_fill(tx0, ty0, tx1 - tx0, h, bg);
        int fpx = vga_wm_font_px();
        int isz = px_icon_size(th * 2 / 3);
        int tx = (w->mode == WM_MAXED ? (cx0 > tx0 ? cx0 : tx0) : tx0) + th / 4;
        int bx0, by0, bx1, by1;
        px_icon(tx, ty0 + (h - isz) / 2, w->icon, isz, bg);
        tx += isz + th / 4;
        wm_btn_rect(w, WMP_MIN, &bx0, &by0, &bx1, &by1);
        px_text(tx, ty0 + (h - px_text_h(fpx, 1)) / 2, w->title, fpx, 1, fg, bg, bx0 - tx - th / 4);
        /* the three buttons: minimise, full screen, close */
        for (int part = WMP_MIN; part <= WMP_CLOSE; part++) {
            wm_btn_rect(w, part, &bx0, &by0, &bx1, &by1);
            uint32_t c = bg, ink = fg;
            if (w->hover == part || w->down == part) {
                if (part == WMP_CLOSE) { c = wm_rgb(WMC_CLOSE); ink = 0xffffffu; if (w->down == part) c = wm_darken(c, 40); }
                else c = lighten(bg, w->down == part ? 70 : 40);
                px_fill(bx0, by0, bx1 - bx0, by1 - by0, c);
            }
            int mx = (bx0 + bx1) / 2, my = (by0 + by1) / 2, g = th / 7;
            if (g < 4) g = 4;
            if (part == WMP_MIN) px_line(mx - g, my, mx + g, my, 1, ink, c);
            else if (part == WMP_MAX) {
                px_line(mx - g, my - g, mx + g, my - g, 1, ink, c); px_line(mx + g, my - g, mx + g, my + g, 1, ink, c);
                px_line(mx + g, my + g, mx - g, my + g, 1, ink, c); px_line(mx - g, my + g, mx - g, my - g, 1, ink, c);
            } else {
                px_line(mx - g, my - g, mx + g, my + g, 1, ink, c); px_line(mx + g, my - g, mx - g, my + g, 1, ink, c);
            }
        }
    }
    wm_fast = 0;
    wm_cx0 = sx0; wm_cy0 = sy0; wm_cx1 = sx1; wm_cy1 = sy1;
}

/* ── the desktop's icons, in pixels over its cells ──────────────────
   shell/desktop.c still lays its icons out in cells (where a click lands
   is unchanged); with the window manager on it leaves those cells blank
   and registers each tile here, and the tile is painted over them: the
   icon, the name in Plex Sans, a rounded highlight when chosen. */
#define DI_MAX 64
typedef struct { int x0, y0, x1, y1, icon, sel; char name[20]; } wm_di_t;
static wm_di_t wm_di[DI_MAX], wm_di_new[DI_MAX];
static int     wm_ndi_new;

static void di_rect(const wm_di_t *d, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)fbc_ox + d->x0 * (int)fbc_cwp; *y0 = (int)fbc_oy + d->y0 * (int)fbc_chp;
    *x1 = (int)fbc_ox + d->x1 * (int)fbc_cwp; *y1 = (int)fbc_oy + d->y1 * (int)fbc_chp;
}
static void di_paint(const wm_di_t *d)
{
    int x0, y0, x1, y1;
    di_rect(d, &x0, &y0, &x1, &y1);
    int h = y1 - y0, w = x1 - x0, fp = vga_wm_font_px();
    uint32_t bg = WM_BG_GRAD;
    if (h >= 64) {
        /* v0.9.8.2: the icon on top, its name centred under it, on two
           lines if it needs them -- as every other desktop does */
        int tp = fp > 14 ? fp - 1 : fp, lh = px_text_h(tp, 0);
        int isz = px_icon_size(h - 2 * lh - fp > 48 ? 48 : h - 2 * lh - fp);
        if (d->sel) {
            bg = mix(0xffffffu, fbc_bg0((uint32_t)y0), 40);
            px_round(x0 + 2, y0 + 2, w - 4, h - 4, fp / 2, bg, WM_BG_GRAD);
        }
        int iy = y0 + fp / 2;
        px_icon(x0 + (w - isz) / 2, iy, d->icon, isz, bg);
        int ty = iy + isz + fp / 3, maxw = w - fp / 2;
        char l1[20], l2[20]; int n = 0, cut = -1;
        for (int k = 0; d->name[k]; k++) n = k + 1;
        memcpy(l1, d->name, sizeof l1); l2[0] = 0;
        if (px_text_w(d->name, tp, 0) > maxw) {
            /* break at the last space that lets line one fit */
            for (int k = 0; k < n; k++) {
                if (d->name[k] != ' ') continue;
                char t[20]; memcpy(t, d->name, (size_t)k); t[k] = 0;
                if (px_text_w(t, tp, 0) <= maxw) cut = k;
            }
            if (cut > 0) { l1[cut] = 0; memcpy(l2, d->name + cut + 1, sizeof l2 - (size_t)cut - 1); l2[19] = 0; }
        }
        uint32_t fg = wm_rgb(WMC_TEXT);
        int w1 = px_text_w(l1, tp, 0); if (w1 > maxw) w1 = maxw;
        px_text(x0 + (w - w1) / 2, ty, l1, tp, 0, fg, bg, maxw);
        if (l2[0] && ty + 2 * lh <= y1) {
            int w2 = px_text_w(l2, tp, 0); if (w2 > maxw) w2 = maxw;
            px_text(x0 + (w - w2) / 2, ty + lh, l2, tp, 0, fg, bg, maxw);
        }
        return;
    }
    int isz = px_icon_size(h - h / 5 > 48 ? 48 : h - h / 5);
    if (d->sel) {
        bg = mix(0xffffffu, fbc_bg0((uint32_t)y0), 40);
        px_round(x0, y0, x1 - x0, h, fp / 2, bg, WM_BG_GRAD);
    }
    px_icon(x0 + (h - isz) / 2, y0 + (h - isz) / 2, d->icon, isz, bg);
    int tx = x0 + (h - isz) / 2 + isz + fp / 2;
    px_text(tx, y0 + (h - px_text_h(fp, 0)) / 2, d->name, fp, 0, wm_rgb(WMC_TEXT), bg, x1 - tx - fp / 2);
}
static void di_over(int x0, int y0, int x1, int y1)
{
    for (int k = 0; k < wm_ndi; k++) {
        int a, b, c, d;
        di_rect(&wm_di[k], &a, &b, &c, &d);
        if (c <= x0 || a >= x1 || d <= y0 || b >= y1) continue;
        int sx0 = wm_cx0, sy0 = wm_cy0, sx1 = wm_cx1, sy1 = wm_cy1;
        clip_set(x0 > sx0 ? x0 : sx0, y0 > sy0 ? y0 : sy0, x1 < sx1 ? x1 : sx1, y1 < sy1 ? y1 : sy1);
        wm_fast = 0;
        di_paint(&wm_di[k]);
        wm_cx0 = sx0; wm_cy0 = sy0; wm_cx1 = sx1; wm_cy1 = sy1;
    }
}
void vga_wm_desk_icons_begin(void){ wm_ndi_new = 0; }
void vga_wm_desk_icon(int x0, int y0, int x1, int y1, const char *icon, const char *name, int sel)
{
    if (wm_ndi_new >= DI_MAX) return;
    wm_di_t *d = &wm_di_new[wm_ndi_new++];
    memset(d, 0, sizeof *d);
    d->x0 = x0; d->y0 = y0; d->x1 = x1; d->y1 = y1; d->sel = sel ? 1 : 0;
    d->icon = wm_icon_find(icon);
    if (d->icon < 0) d->icon = wm_icon_find("_app");
    for (int n = 0; name[n] && n < 19; n++) d->name[n] = name[n];
}
/* the new set takes over; what changed is repainted */
void vga_wm_desk_icons_end(void)
{
    wm_di_t old[DI_MAX]; int nold = wm_ndi;
    memcpy(old, wm_di, sizeof old);
    memcpy(wm_di, wm_di_new, sizeof wm_di); wm_ndi = wm_ndi_new;
    if (!wm_is_on || !wm_nL || wm_L[0].type != LT_DESK) return;
    for (int k = 0; k < (nold > wm_ndi ? nold : wm_ndi); k++) {
        if (k < nold && k < wm_ndi && !memcmp(&old[k], &wm_di[k], sizeof old[k])) continue;
        int a, b, c, d;
        if (k < nold) { di_rect(&old[k], &a, &b, &c, &d); vga_wm_repaint_rect(a, b, c, d); }
        if (k < wm_ndi) { di_rect(&wm_di[k], &a, &b, &c, &d); vga_wm_repaint_rect(a, b, c, d); }
    }
}

/* ── painting a rectangle of the screen, every layer ────────────────── */
static void paint_cells(int i, uint16_t *cells, uint16_t *painted, int gx, int gy,
                        int vx, int vy, int vw, int vh)
{
    int cw = (int)fbc_cwp, ch = (int)fbc_chp;
    int c0 = (wm_cx0 - gx) / cw, c1 = (wm_cx1 - gx + cw - 1) / cw;
    int r0 = (wm_cy0 - gy) / ch, r1 = (wm_cy1 - gy + ch - 1) / ch;
    if (wm_cx0 < gx) c0 = 0;
    if (wm_cy0 < gy) r0 = 0;
    if (c1 > vw) c1 = vw;
    if (r1 > vh) r1 = vh;
    int sx0 = wm_cx0, sy0 = wm_cy0, sx1 = wm_cx1, sy1 = wm_cy1;
    for (int r = r0; r < r1; r++)
        for (int c = c0; c < c1; c++) {
            int idx = (vy + r) * g_cols + (vx + c);
            if (idx < 0 || idx >= g_rows * g_cols) continue;
            uint16_t e = cells[idx];
            int sx = gx + c * cw, sy = gy + r * ch;
            if (painted && painted[idx] != e) {
                /* a cell that changed is painted whole, never half old */
                clip_set(sx, sy, sx + cw, sy + ch);
                wm_cell(i, e, sx, sy);
                ptr_over_px(sx, sy, sx + cw, sy + ch);
                painted[idx] = e;
                wm_cx0 = sx0; wm_cy0 = sy0; wm_cx1 = sx1; wm_cy1 = sy1;
            } else wm_cell(i, e, sx, sy);
        }
}

static void paint_margins(int i)
{
    int gx0 = (int)fbc_ox, gy0 = (int)fbc_oy;
    int gx1 = gx0 + g_cols * (int)fbc_cwp, gy1 = gy0 + g_rows * (int)fbc_chp;
    wm_cl = i;
    wm_fast = 0;
    for (int y = wm_cy0; y < wm_cy1; y++) {
        uint32_t m = fbc_bg0((uint32_t)y);
        for (int x = wm_cx0; x < wm_cx1; x++)
            if (y < gy0 || y >= gy1 || x < gx0 || x >= gx1) wm_px(x, y, m);
    }
}

static void paint_layer(int i)
{
    const wm_layer_t *L = &wm_L[i];
    int sx0 = wm_cx0, sy0 = wm_cy0, sx1 = wm_cx1, sy1 = wm_cy1;
    if (L->x1 <= wm_cx0 || L->x0 >= wm_cx1 || L->y1 <= wm_cy0 || L->y0 >= wm_cy1) return;
    wm_cl = i; wm_fast = 0;
    switch (L->type) {
    case LT_DESK:
        paint_margins(i);
        if (!fbc_painted_valid) fbc_forget();
        paint_cells(i, shadow_boot, fbc_painted,
                    (int)fbc_ox, (int)fbc_oy, 0, 0, g_cols, g_rows);
        break;
    case LT_SOLO:
        paint_margins(i);
        if (L->slot > 0)
            paint_cells(i, wm_w[L->slot].cells, wm_painted[L->slot], (int)fbc_ox, (int)fbc_oy, 0, 0, g_cols, g_rows);
        else
            paint_cells(i, wm_solo_cells, wm_solo_painted, (int)fbc_ox, (int)fbc_oy, 0, 0, g_cols, g_rows);
        break;
    case LT_WIN: {
        wm_win_t *w = &wm_w[L->slot];
        uint16_t *cells = (w->cells == shadow) ? shadow : w->cells;
        wm_paint_title(i, w, L->slot);
        wm_cl = i;
        paint_cells(i, cells, wm_painted[L->slot], w->x, w->y, w->vx, w->vy, w->vw, w->vh);
        break; }
    case LT_BAR:
    case LT_POP: {
        clip_set(wm_cx0 > L->x0 ? wm_cx0 : L->x0, wm_cy0 > L->y0 ? wm_cy0 : L->y0,
                 wm_cx1 < L->x1 ? wm_cx1 : L->x1, wm_cy1 < L->y1 ? wm_cy1 : L->y1);
        if (wm_cx0 < wm_cx1 && wm_cy0 < wm_cy1) {
            wm_cl = i;
            /* v0.9.8.1: the Start menu and the taskbar are on top -- when
               nothing is above the part being painted, no pixel has to ask
               who owns it (Kyan: "buggy" -- the menu took seconds a frame) */
            wm_fast = clear_above(i, wm_cx0, wm_cy0, wm_cx1, wm_cy1) ? 2 : 0;
            if (L->type == LT_BAR) wm_bar_fn(L->x0, L->y0, L->x1, L->y1);
            else                   wm_pop_fn(L->x0, L->y0, L->x1, L->y1);
            wm_fast = 0;
        }
        break; }
    }
    wm_cx0 = sx0; wm_cy0 = sy0; wm_cx1 = sx1; wm_cy1 = sy1;
}

void vga_wm_repaint_rect(int x0, int y0, int x1, int y1)
{
    if (!wm_is_on || !fbc_active || !fbc_addr) return;
    clip_set(x0, y0, x1, y1);
    if (wm_cx0 >= wm_cx1 || wm_cy0 >= wm_cy1) return;
    for (int i = 0; i < wm_nL; i++) paint_layer(i);
    ptr_over_px(x0, y0, x1, y1);
}
void vga_wm_repaint_all(void){ vga_wm_repaint_rect(0, 0, (int)fbc_w, (int)fbc_h); }

static void layer_rect(int i, int *x0, int *y0, int *x1, int *y1)
{
    const wm_layer_t *L = &wm_L[i];
    int e = L->sh ? wm_shadow() : 0;
    *x0 = L->x0 - e; *y0 = L->y0 - e; *x1 = L->x1 + e; *y1 = L->y1 + e;
}
static void repaint_slot(int slot)
{
    for (int i = 0; i < wm_nL; i++)
        if (wm_L[i].type == LT_WIN && wm_L[i].slot == slot) {
            int a, b, c, d; layer_rect(i, &a, &b, &c, &d);
            vga_wm_repaint_rect(a, b, c, d);
        }
}
void vga_wm_repaint_title(int slot)
{
    wm_win_t *w = &wm_w[slot];
    if (!wm_is_on || !w->on || w->min || wm_solo_depth) return;
    int x0, y0, x1, y1;
    win_frame(w, &x0, &y0, &x1, &y1);
    vga_wm_repaint_rect(x0, y0, x1, w->y + 1);
}

/* ── the current console, as apps draw ──────────────────────────────── */
/* Which layer the CURRENT console is, and its record of the screen. */
static int cur_layer(uint16_t **painted, int *gx, int *gy, int *vx, int *vy, int *vw, int *vh)
{
    if (wm_nL && wm_L[0].type == LT_SOLO) {
        int sl = wm_L[0].slot;
        if (shadow != (sl > 0 ? wm_w[sl].cells : wm_solo_cells)) return -1;
        *painted = sl > 0 ? wm_painted[sl] : wm_solo_painted; *gx = (int)fbc_ox; *gy = (int)fbc_oy;
        *vx = 0; *vy = 0; *vw = g_cols; *vh = g_rows;
        return 0;
    }
    if (shadow == shadow_boot) {
        if (!fbc_painted_valid) fbc_forget();
        *painted = fbc_painted; *gx = (int)fbc_ox; *gy = (int)fbc_oy;
        *vx = 0; *vy = 0; *vw = g_cols; *vh = g_rows;
        return 0;
    }
    for (int i = 1; i < wm_nL; i++)
        if (wm_L[i].type == LT_WIN && wm_w[wm_L[i].slot].cells == shadow) {
            wm_win_t *w = &wm_w[wm_L[i].slot];
            *painted = wm_painted[wm_L[i].slot]; *gx = w->x; *gy = w->y;
            *vx = w->vx; *vy = w->vy; *vw = w->vw; *vh = w->vh;
            return i;
        }
    return -1;      /* not on the screen: a minimised window, a task with no window */
}

static void wm_put_cur(int idx, uint16_t entry)
{
    uint16_t *painted; int gx, gy, vx, vy, vw, vh;
    int i = cur_layer(&painted, &gx, &gy, &vx, &vy, &vw, &vh);
    if (i < 0) return;
    if (painted[idx] == entry) return;
    int cx = idx % g_cols - vx, cy = idx / g_cols - vy;
    if (cx < 0 || cy < 0 || cx >= vw || cy >= vh) return;
    painted[idx] = entry;
    int sx = gx + cx * (int)fbc_cwp, sy = gy + cy * (int)fbc_chp;
    clip_set(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
    wm_cell(i, entry, sx, sy);
    ptr_over_px(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
}

static void wm_flush_cur(void)
{
    uint16_t *painted; int gx, gy, vx, vy, vw, vh;
    int i = cur_layer(&painted, &gx, &gy, &vx, &vy, &vw, &vh);
    if (i < 0) return;
    for (int r = 0; r < vh; r++)
        for (int c = 0; c < vw; c++) {
            int idx = (vy + r) * g_cols + vx + c;
            uint16_t e = shadow[idx];
            if (painted[idx] == e) continue;
            painted[idx] = e;
            int sx = gx + c * (int)fbc_cwp, sy = gy + r * (int)fbc_chp;
            clip_set(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
            wm_cell(i, e, sx, sy);
            ptr_over_px(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
        }
}

/* The caret: the cell under it drawn with its colours swapped, and the
   record told the cell is unknown, so the next write repaints it. */
static int wm_caret_idx = -1;
static uint16_t *wm_caret_cells;
static void wm_caret(int idx)
{
    uint16_t *painted; int gx, gy, vx, vy, vw, vh;
    if (wm_caret_cells == shadow && wm_caret_idx >= 0) {
        int i0 = cur_layer(&painted, &gx, &gy, &vx, &vy, &vw, &vh);
        if (i0 >= 0) { painted[wm_caret_idx] = 0xFFFF; wm_put_cur(wm_caret_idx, shadow[wm_caret_idx]); }
    }
    wm_caret_idx = -1;
    int i = cur_layer(&painted, &gx, &gy, &vx, &vy, &vw, &vh);
    if (i < 0 || idx < 0 || idx >= g_rows * g_cols) return;
    int cx = idx % g_cols - vx, cy = idx / g_cols - vy;
    if (cx < 0 || cy < 0 || cx >= vw || cy >= vh) return;
    uint16_t e = shadow[idx];
    uint16_t sw = (uint16_t)((e & 0x00FF) | ((uint16_t)(((e >> 12) & 0x0F) | (((e >> 8) & 0x0F) << 4)) << 8));
    int sx = gx + cx * (int)fbc_cwp, sy = gy + cy * (int)fbc_chp;
    clip_set(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
    wm_cell(i, sw, sx, sy);
    ptr_over_px(sx, sy, sx + (int)fbc_cwp, sy + (int)fbc_chp);
    painted[idx] = 0xFFFF;
    wm_caret_idx = idx; wm_caret_cells = shadow;
}

/* ── the pointer, in WM mode: what was under it repainted from layers ── */
static void wm_ptr_touch(void)
{
    int x0 = fbc_ptr_x, y0 = fbc_ptr_y;
    vga_wm_repaint_rect(x0, y0, x0 + PTR_W * fbc_ptr_s, y0 + PTR_H * fbc_ptr_s);
}

/* ── the API ────────────────────────────────────────────────────────── */
static void wm_invalidate_all(void)
{
    for (int s = 0; s < WM_SLOTS; s++)
        for (int k = 0; k < VGA_CELLS_MAX; k++) wm_painted[s][k] = 0xFFFF;
    for (int k = 0; k < VGA_CELLS_MAX; k++) wm_solo_painted[k] = 0xFFFF;
    fbc_forget();
    wm_caret_idx = -1;
}

void vga_wm_enable(int on)
{
    if (!fbc_active || !fbc_addr) { wm_is_on = 0; return; }
    if ((on ? 1 : 0) == wm_is_on) return;
    wm_is_on = on ? 1 : 0;
    for (int i = 0; i < DECO_MAX; i++) fbc_deco[i].on = 0;
    wm_invalidate_all();
    wm_rebuild();
    if (wm_is_on) vga_wm_repaint_all();
    else {
        /* back to one console on the screen: the front one repaints */
        fbc_clear_margin();
        g_screen_owner = 0;
        if (g_vis) vga_flush();
    }
}
int vga_wm_on(void){ return wm_is_on; }

void vga_wm_open(int slot, const char *title, const char *icon)
{
    if (slot <= 0 || slot >= WM_SLOTS) return;
    wm_win_t *w = &wm_w[slot];
    memset(w, 0, sizeof *w);
    w->on = 1; w->cells = shadow;
    w->icon = icon ? wm_icon_find(icon) : -1;
    if (w->icon < 0) w->icon = wm_icon_find("_app");
    int n = 0;
    if (title) for (; title[n] && n < (int)sizeof w->title - 1; n++) w->title[n] = title[n];
    w->title[n] = 0;
    for (int k = 0; k < VGA_CELLS_MAX; k++) wm_painted[slot][k] = 0xFFFF;
    int k = 0;
    for (int j = 0; j < wm_nz; j++) if (wm_z[j] != slot) wm_z[k++] = wm_z[j];
    wm_z[k++] = slot; wm_nz = k;
}

void vga_wm_close(int slot)
{
    if (slot <= 0 || slot >= WM_SLOTS || !wm_w[slot].on) return;
    int x0, y0, x1, y1, e = wm_shadow();
    win_frame(&wm_w[slot], &x0, &y0, &x1, &y1);
    int was = !wm_w[slot].min, full = wm_w[slot].full && slot == wm_front;
    wm_w[slot].on = 0;
    int k = 0;
    for (int j = 0; j < wm_nz; j++) if (wm_z[j] != slot) wm_z[k++] = wm_z[j];
    wm_nz = k;
    wm_rebuild();
    if (full) { wm_invalidate_all(); vga_wm_repaint_all(); }
    else if (was) vga_wm_repaint_rect(x0 - e, y0 - e, x1 + e, y1 + e);
}

/* The viewport the window shows (in its console's cells) and where. */
void vga_wm_geometry(int slot, int vx, int vy, int vw, int vh, int x, int y, int mode)
{
    if (slot <= 0 || slot >= WM_SLOTS || !wm_w[slot].on) return;
    wm_win_t *w = &wm_w[slot];
    int e = wm_shadow(), ox0, oy0, ox1, oy1, vis = !w->min && wm_is_on;
    win_frame(w, &ox0, &oy0, &ox1, &oy1);
    int same = w->vx == vx && w->vy == vy && w->vw == vw && w->vh == vh && w->x == x && w->y == y && w->mode == mode;
    w->vx = vx; w->vy = vy; w->vw = vw; w->vh = vh; w->x = x; w->y = y; w->mode = mode;
    w->cells = w->cells ? w->cells : shadow;
    if (same) return;
    wm_rebuild();
    if (!vis) return;
    int nx0, ny0, nx1, ny1;
    win_frame(w, &nx0, &ny0, &nx1, &ny1);
    if (ox1 > ox0 && (ox0 != nx0 || oy0 != ny0 || ox1 != nx1 || oy1 != ny1))
        vga_wm_repaint_rect(ox0 - e, oy0 - e, ox1 + e, oy1 + e);
    vga_wm_repaint_rect(nx0 - e, ny0 - e, nx1 + e, ny1 + e);
}
void vga_wm_get(int slot, int *x, int *y, int *w, int *h)
{
    wm_win_t *v = &wm_w[slot < 0 || slot >= WM_SLOTS ? 0 : slot];
    if (x) *x = v->x;
    if (y) *y = v->y;
    if (w) *w = v->vw * (int)fbc_cwp;
    if (h) *h = v->vh * (int)fbc_chp;
}
int vga_wm_is_open(int slot){ return slot > 0 && slot < WM_SLOTS && wm_w[slot].on; }
int vga_wm_is_min(int slot) { return vga_wm_is_open(slot) && wm_w[slot].min; }

void vga_wm_move(int slot, int x, int y)
{
    if (!vga_wm_is_open(slot)) return;
    wm_win_t *w = &wm_w[slot];
    vga_wm_geometry(slot, w->vx, w->vy, w->vw, w->vh, x, y, w->mode);
}

void vga_wm_minimize(int slot, int on)
{
    if (!vga_wm_is_open(slot) || wm_w[slot].min == (on ? 1 : 0)) return;
    int x0, y0, x1, y1, e = wm_shadow();
    wm_win_t *w = &wm_w[slot];
    w->min = on ? 1 : 0;
    if (!on) for (int k = 0; k < VGA_CELLS_MAX; k++) wm_painted[slot][k] = 0xFFFF;
    win_frame(w, &x0, &y0, &x1, &y1);
    wm_rebuild();
    vga_wm_repaint_rect(x0 - e, y0 - e, x1 + e, y1 + e);
}

void vga_wm_raise(int slot)
{
    if (!vga_wm_is_open(slot)) return;
    if (wm_nz && wm_z[wm_nz - 1] == slot) return;
    int k = 0;
    for (int j = 0; j < wm_nz; j++) if (wm_z[j] != slot) wm_z[k++] = wm_z[j];
    wm_z[k++] = slot; wm_nz = k;
    wm_rebuild();
    repaint_slot(slot);
}

/* The task in front changed (kernel/task.c): its window comes up, and the
   two title bars change colour. */
void vga_wm_front(int slot)
{
    int old = wm_front, was = front_full();
    wm_front = slot;
    if (!wm_is_on) return;
    if (was || front_full()) {       /* a whole-screen window came or went */
        if (vga_wm_is_open(slot)) {
            int k = 0;
            for (int j = 0; j < wm_nz; j++) if (wm_z[j] != slot) wm_z[k++] = wm_z[j];
            wm_z[k++] = slot; wm_nz = k;
        }
        wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all();
        return;
    }
    if (vga_wm_is_open(slot)) vga_wm_raise(slot);
    if (vga_wm_is_open(old) && old != slot) vga_wm_repaint_title(old);
    if (vga_wm_is_open(slot)) vga_wm_repaint_title(slot);
}
int vga_wm_front_slot(void){ return wm_front; }

void vga_wm_title(int slot, const char *title)
{
    if (!vga_wm_is_open(slot)) return;
    int n = 0;
    for (; title[n] && n < (int)sizeof wm_w[slot].title - 1; n++) wm_w[slot].title[n] = title[n];
    wm_w[slot].title[n] = 0;
    vga_wm_repaint_title(slot);
}
const char *vga_wm_title_of(int slot){ return vga_wm_is_open(slot) ? wm_w[slot].title : ""; }
int vga_wm_icon_of(int slot){ return vga_wm_is_open(slot) ? wm_w[slot].icon : wm_icon_find("_app"); }

void vga_wm_buttons(int slot, int hover, int down)
{
    if (!vga_wm_is_open(slot)) return;
    wm_win_t *w = &wm_w[slot];
    if (w->hover == hover && w->down == down) return;
    w->hover = hover; w->down = down;
    vga_wm_repaint_title(slot);
}

/* The current console takes the whole screen -- the sign-in screen, a
   screensaver, full screen, Alt+Tab -- until vga_wm_solo(0). */
void vga_wm_solo(int on)
{
    if (!wm_is_on) return;
    if (on) {
        if (wm_solo_depth++ == 0 || wm_solo_cells != shadow) {
            wm_solo_cells = shadow;
            for (int k = 0; k < VGA_CELLS_MAX; k++) wm_solo_painted[k] = 0xFFFF;
        }
    } else {
        if (wm_solo_depth == 0) return;
        if (--wm_solo_depth) return;
        wm_solo_cells = 0;
        wm_invalidate_all();
    }
    wm_rebuild();
    vga_wm_repaint_all();
}
int vga_wm_solo_on(void){ return wm_solo_depth > 0 || front_full(); }

/* A window that has the whole screen whenever it is in front: full
   screen, or an app on a grid with no room for a title bar (80x25). */
void vga_wm_full(int slot, int on)
{
    if (!vga_wm_is_open(slot) || wm_w[slot].full == (on ? 1 : 0)) return;
    wm_w[slot].full = on ? 1 : 0;
    if (!wm_is_on) return;
    wm_invalidate_all(); wm_rebuild(); vga_wm_repaint_all();
}

void vga_wm_set_bar(wm_paint_fn fn)
{
    wm_bar_fn = fn;
    wm_rebuild();
    if (wm_is_on) vga_wm_repaint_rect(0, (int)fbc_h - vga_wm_bar_h(), (int)fbc_w, (int)fbc_h);
}
void vga_wm_repaint_bar(void)
{
    if (wm_bar_fn) vga_wm_repaint_rect(0, (int)fbc_h - vga_wm_bar_h(), (int)fbc_w, (int)fbc_h);
}
void vga_wm_set_popup(int x, int y, int w, int h, wm_paint_fn fn)
{
    int e = wm_shadow();
    int ox = wm_pop_x, oy = wm_pop_y, ow = wm_pop_w, oh = wm_pop_h, had = wm_pop_fn != 0;
    wm_pop_fn = fn; wm_pop_x = x; wm_pop_y = y; wm_pop_w = w; wm_pop_h = h;
    wm_rebuild();
    if (!wm_is_on) return;
    if (had && (!fn || ox != x || oy != y || ow != w || oh != h))
        vga_wm_repaint_rect(ox - e, oy - e, ox + ow + e, oy + oh + e);
    if (fn) vga_wm_repaint_rect(x - e, y - e, x + w + e, y + h + e);
}
void vga_wm_repaint_popup(int x0, int y0, int x1, int y1)
{
    if (wm_pop_fn) vga_wm_repaint_rect(x0, y0, x1, y1);
}

/* What is under a pixel: the slot whose CONTENT is there (0 the desktop),
   or -1 for the window manager's own -- a title bar, an edge, the taskbar,
   the popup -- with *part saying which. */
int vga_wm_hit(int x, int y, int *slot, int *part)
{
    int s = 0, p = WMP_NONE;
    if (!wm_is_on || (wm_nL && wm_L[0].type == LT_SOLO)) { if (slot) *slot = wm_front; if (part) *part = WMP_CONTENT; return 1; }
    int i = wm_owner(x, y);
    const wm_layer_t *L = &wm_L[i];
    switch (L->type) {
    case LT_DESK: s = 0; p = WMP_CONTENT; break;
    case LT_BAR:  s = -1; p = WMP_BAR; break;
    case LT_POP:  s = -1; p = WMP_POPUP; break;
    case LT_WIN: {
        wm_win_t *w = &wm_w[L->slot];
        int cx0, cy0, cx1, cy1, bx0, by0, bx1, by1;
        s = L->slot;
        win_content(w, &cx0, &cy0, &cx1, &cy1);
        if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) { p = WMP_CONTENT; break; }
        p = WMP_EDGE;
        int tx0, ty0, tx1, ty1;
        win_title(w, &tx0, &ty0, &tx1, &ty1);
        if (y < ty1 && y >= ty0 && x >= tx0 && x < tx1) {
            p = WMP_TITLE;
            for (int q = WMP_MIN; q <= WMP_CLOSE; q++) {
                wm_btn_rect(w, q, &bx0, &by0, &bx1, &by1);
                if (x >= bx0 && x < bx1) p = q;
            }
        }
        break; }
    default: break;
    }
    if (slot) *slot = s;
    if (part) *part = p;
    return p == WMP_CONTENT;
}

/* For the mouse driver: is (x,y) in the CURRENT console's own content,
   and which of its cells? -1 when the window manager does not place the
   current console (it is off, or this is the desktop's grid). */
int vga_wm_cur_map(int x, int y, int *cx, int *cy)
{
    int solo = wm_nL && wm_L[0].type == LT_SOLO;
    if (!wm_is_on || solo || shadow == shadow_boot) {
        if (wm_is_on && !solo && shadow == shadow_boot) {
            int s, p; vga_wm_hit(x, y, &s, &p);
            return -2 + (s == 0 && p == WMP_CONTENT);      /* -2 outside, -1 inside */
        }
        return -1;
    }
    for (int k = 1; k < WM_SLOTS; k++) {
        wm_win_t *w = &wm_w[k];
        if (!w->on || w->cells != shadow) continue;
        int lx = (x - w->x) / (int)fbc_cwp, ly = (y - w->y) / (int)fbc_chp;
        int in = x >= w->x && y >= w->y && lx < w->vw && ly < w->vh && !w->min;
        if (in) { int s, p; vga_wm_hit(x, y, &s, &p); in = s == k && p == WMP_CONTENT; }
        if (lx < 0 || x < w->x) lx = 0;
        if (ly < 0 || y < w->y) ly = 0;
        if (lx > w->vw - 1) lx = w->vw - 1;
        if (ly > w->vh - 1) ly = w->vh - 1;
        *cx = lx; *cy = ly;
        return in;
    }
    return 0;      /* a console with no window: nothing of it is on the screen */
}

/* For the mouse driver: does a press at (x,y) belong to the app in front
   (1), or to the window manager (0)? */
int vga_wm_click_is_app(int x, int y)
{
    int s, p;
    if (!wm_is_on || (wm_nL && wm_L[0].type == LT_SOLO)) return 1;
    vga_wm_hit(x, y, &s, &p);
    return p == WMP_CONTENT && s == wm_front;
}
