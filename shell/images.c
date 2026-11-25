/* shell/images.c -- Images: look at PNG and BMP pictures (v0.9.10).
 *
 * Kyan, 25 Nov: "maybe add image and screenshot support". The first app
 * made only of the pixel building blocks (shell/pxui.c): its window is
 * its own canvas, of whatever size the window is -- no cells, no zoom.
 *
 *   left     the pictures HavenDOS can find: Pictures (where the PrtSc
 *            key saves), the USB stick's Pictures and its top folder,
 *            and the system disk's top folder
 *   top      Refresh  Fit  -  +  1:1  Set as wallpaper / Remove wallpaper
 *   middle   the picture: fitted to the window, or at a zoom; dragged
 *            with the mouse or moved with the arrow keys when it is
 *            bigger than the window
 *   bottom   its name, size and zoom
 *
 * Keys: Up/Down choose (with the list focused), Tab between the list and
 * the picture, + - 0 (fit) 1 (1:1), W wallpaper, R refresh, Esc closes.
 */
#include "../include/pxui.h"
#include "../include/png.h"
#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/keys.h"
#include "../include/toast.h"
#include "../include/string.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern int   vfs_read(const char *name, char *buf, uint32_t bufsz);
extern void  vfs_list_dir(const char *dir, void (*cb)(const char*, int, uint32_t));
extern int   usbv_present(void);
extern void  cfg_set_wall_img(const char *p);
extern const char *cfg_wall_img(void);
extern void  cfg_save(void);
extern int   ui_desk_pattern(void);
extern void  ui_set_desk_pattern(int on);

#define IM_MAX 96
static char   g_path[IM_MAX][96];
static char   g_name[IM_MAX][48];
static uint32_t g_size[IM_MAX];
static const char *g_label[IM_MAX];
static int    g_n;
static const char *g_dir;

static int ends_with(const char *s, const char *e)
{
    int n = (int)strlen(s), m = (int)strlen(e);
    if (n < m) return 0;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i], b = e[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}
static void found(const char *name, int is_dir, uint32_t size)
{
    if (is_dir || g_n >= IM_MAX) return;
    if (!ends_with(name, ".png") && !ends_with(name, ".bmp")) return;
    char *p = g_path[g_n];
    p[0] = 0;
    if (g_dir[0]) { strncpy(p, g_dir, 60); p[60] = 0; strcat(p, "/"); }
    strncat(p, name, 95 - strlen(p));
    strncpy(g_name[g_n], name, 47); g_name[g_n][47] = 0;
    for (int i = 0; i < g_n; i++) if (!strcmp(g_path[i], p)) return;     /* twice */
    g_size[g_n] = size;
    g_label[g_n] = g_name[g_n];
    g_n++;
}
static void scan(void)
{
    static char home[64];
    extern void screenshot_dir(char *out);
    static const char *dirs[] = { home, "Pictures", "" };
    screenshot_dir(home);
    g_n = 0;
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) { g_dir = dirs[i]; vfs_list_dir(dirs[i], found); }
    if (usbv_present()) {
        g_dir = "/usb/Pictures"; vfs_list_dir("/usb/Pictures", found);
        g_dir = "/usb"; vfs_list_dir("/usb", found);
    }
}

/* the picture */
static uint32_t *g_img; static int g_iw, g_ih, g_cur = -1;
static char g_err[96];
static int  g_zoom;                  /* 0: fit; else percent */
static int  g_panx, g_pany;

static void load(int i)
{
    if (g_img) { kfree(g_img); g_img = 0; }
    g_err[0] = 0; g_cur = i; g_zoom = 0; g_panx = g_pany = 0;
    if (i < 0 || i >= g_n) return;
    uint32_t n = g_size[i] ? g_size[i] : 64u * 1024u * 1024u;
    if (n > 48u * 1024u * 1024u) { strcpy(g_err, "This file is too big (over 48 MB)."); return; }
    uint8_t *f = (uint8_t *)kmalloc(n + 1);
    if (!f) { strcpy(g_err, "Not enough memory to read this file."); return; }
    int got = vfs_read(g_path[i], (char *)f, n);
    if (got <= 0) { kfree(f); strcpy(g_err, "Could not read this file."); return; }
    g_img = image_decode(f, (uint32_t)got, &g_iw, &g_ih, pxui_theme()->bg);
    kfree(f);
    if (!g_img) { strcpy(g_err, "Cannot show it: "); strncat(g_err, png_error ? png_error : "unknown", 70); }
}

/* the picture into the view rectangle of the canvas, scaled */
static void blit(uint32_t *cv, int cw, prect_t v, int *shown_scale)
{
    const pxui_theme_t *t = pxui_theme();
    px_fill(v.x, v.y, v.w, v.h, t->panel);
    if (!g_img) return;
    /* scale, in 1/1024ths */
    int64_t k;
    if (!g_zoom) {
        int64_t a = (int64_t)v.w * 1024 / g_iw, b = (int64_t)v.h * 1024 / g_ih;
        k = a < b ? a : b;
        if (k > 1024 && (g_iw < 64 || g_ih < 64)) ; else if (k > 1024) k = 1024;   /* fit never blows a picture up */
        if (k < 1) k = 1;
    } else k = (int64_t)g_zoom * 1024 / 100;
    *shown_scale = (int)(k * 100 / 1024);
    int dw = (int)((int64_t)g_iw * k / 1024), dh = (int)((int64_t)g_ih * k / 1024);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    /* pan only when bigger than the view */
    int maxx = dw > v.w ? dw - v.w : 0, maxy = dh > v.h ? dh - v.h : 0;
    if (g_panx < 0) g_panx = 0;
    if (g_pany < 0) g_pany = 0;
    if (g_panx > maxx) g_panx = maxx;
    if (g_pany > maxy) g_pany = maxy;
    int ox = dw < v.w ? v.x + (v.w - dw) / 2 : v.x - g_panx;
    int oy = dh < v.h ? v.y + (v.h - dh) / 2 : v.y - g_pany;
    int y0 = oy < v.y ? v.y : oy, y1 = oy + dh > v.y + v.h ? v.y + v.h : oy + dh;
    int x0 = ox < v.x ? v.x : ox, x1 = ox + dw > v.x + v.w ? v.x + v.w : ox + dw;
    int shrink = k < 1024;
    for (int y = y0; y < y1; y++) {
        uint32_t sy = (uint32_t)((int64_t)(y - oy) * 1024 / k), sy2 = sy;
        if (sy >= (uint32_t)g_ih) sy = (uint32_t)g_ih - 1;
        if (shrink) { sy2 = (uint32_t)(((int64_t)(y - oy) * 1024 + 512) / k); if (sy2 >= (uint32_t)g_ih) sy2 = sy; }
        uint32_t *row = cv + (uint32_t)y * (uint32_t)cw;
        const uint32_t *a = g_img + sy * (uint32_t)g_iw, *b = g_img + sy2 * (uint32_t)g_iw;
        for (int x = x0; x < x1; x++) {
            uint32_t sx = (uint32_t)((int64_t)(x - ox) * 1024 / k);
            if (sx >= (uint32_t)g_iw) sx = (uint32_t)g_iw - 1;
            if (!shrink) { row[x] = a[sx]; continue; }
            uint32_t sx2 = (uint32_t)(((int64_t)(x - ox) * 1024 + 512) / k);
            if (sx2 >= (uint32_t)g_iw) sx2 = sx;
            uint32_t p = a[sx], q = a[sx2], r = b[sx], s = b[sx2];
            row[x] = ((((p >> 16) & 255) + ((q >> 16) & 255) + ((r >> 16) & 255) + ((s >> 16) & 255)) / 4 << 16)
                   | ((((p >> 8) & 255) + ((q >> 8) & 255) + ((r >> 8) & 255) + ((s >> 8) & 255)) / 4 << 8)
                   | (((p & 255) + (q & 255) + (r & 255) + (s & 255)) / 4);
        }
    }
}

static void set_wallpaper(void)
{
    if (!g_img) return;
    if (vga_wm_lite()) { toast_warning("Best performance is on (Settings -> Display): no wallpaper picture"); return; }
    if (vga_wm_set_wallpaper(g_img, g_iw, g_ih)) { toast_error("Not enough memory for a wallpaper this size"); return; }
    cfg_set_wall_img(g_path[g_cur]);
    if (ui_desk_pattern()) ui_set_desk_pattern(0);         /* the pattern's dots would be on top */
    cfg_save();
    toast_success("Wallpaper set");
}
static void clear_wallpaper(void)
{
    vga_wm_set_wallpaper(0, 0, 0);
    cfg_set_wall_img("");
    cfg_save();
    toast_info("Wallpaper removed");
}

void app_images(void)
{
    pxui_in_t in; memset(&in, 0, sizeof in);
    if (!pxui_begin()) {
        vga_clear();
        vga_puts("Images draws in pixels: open it from the desktop.\nPress a key.");
        { extern int keyboard_waitchar(void); keyboard_waitchar(); }
        return;
    }
    pxui_end();
    scan();
    int sel = 0, top = 0, focus = 0 /* 0 list, 1 picture */, dragging = 0, dx0 = 0, dy0 = 0, px0 = 0, py0 = 0;
    if (g_n) load(0);
    for (;;) {
        if (!pxui_begin()) break;
        const pxui_theme_t *t = pxui_theme();
        int W = pxui_w(), H = pxui_h(), lh = pxui_line_h(), bh = lh + 16, pad = 8;
        int sideW = W / 4 < 180 ? 180 : W / 4 > 300 ? 300 : W / 4;
        if (W < 520) sideW = W / 3;
        prect_t bar = { 0, 0, W, bh + 2 * pad }, side = { pad, bar.h, sideW, H - bar.h - lh - 2 * pad - pad };
        prect_t status = { 0, H - lh - 2 * pad, W, lh + 2 * pad };
        prect_t view = { side.x + side.w + pad, bar.h, W - side.w - 3 * pad, side.h };
        px_fill(0, 0, W, H, t->bg);
        /* the toolbar */
        int x = pad, act = 0;
        const char *labels[] = { "Refresh", "Fit", "-", "+", "1:1" };
        for (int i = 0; i < 5; i++) {
            int w = pxui_text_w(labels[i]) + 28; if (w < bh) w = bh;
            prect_t b = { x, pad, w, bh };
            if (pxui_button(b, labels[i], 0, i == 0 || g_img != 0, &in)) act = i + 1;
            x += w + 6;
        }
        {
            int has = vga_wm_has_wallpaper();
            const char *l = has && cfg_wall_img()[0] && g_cur >= 0 && !strcmp(cfg_wall_img(), g_path[g_cur]) ? "Remove wallpaper" : "Set as wallpaper";
            int w = pxui_text_w(l) + 28;
            prect_t b = { W - pad - w, pad, w, bh };
            if (b.x > x && pxui_button(b, l, 1, g_img != 0, &in)) act = l[0] == 'R' ? 7 : 6;
        }
        /* the list */
        if (g_n) {
            int was = sel;
            if (pxui_list(side, g_label, g_n, &sel, &top, focus == 0, &in)) focus = 1;
            if (sel != was || g_cur != sel) load(sel);
        } else {
            px_fill(side.x, side.y, side.w, side.h, t->field);
            pxui_text(side.x + 10, side.y + 10, "No pictures yet.", t->text, t->field, side.w - 20);
            pxui_text(side.x + 10, side.y + 14 + lh, "PrtSc takes one.", t->muted, t->field, side.w - 20);
        }
        if (in.pressed && pxui_hit(side, &in)) focus = 0;
        /* the picture */
        int scale = 100;
        {
            uint32_t *cv = vga_wm_canvas(0, 0);
            blit(cv, W, view, &scale);
            if (!g_img && g_err[0]) pxui_text(view.x + 16, view.y + 16, g_err, t->text, t->panel, view.w - 32);
        }
        if (in.pressed && pxui_hit(view, &in)) { focus = 1; dragging = 1; dx0 = in.mx; dy0 = in.my; px0 = g_panx; py0 = g_pany; }
        if (!in.down) dragging = 0;
        if (dragging) { g_panx = px0 - (in.mx - dx0); g_pany = py0 - (in.my - dy0); }
        /* the status line */
        px_fill(status.x, status.y, status.w, status.h, t->panel);
        {
            char s[160]; s[0] = 0;
            if (g_cur >= 0 && g_cur < g_n) {
                strcat(s, g_path[g_cur]);
                if (g_img) {
                    char num[48]; int k = 0, v;
                    strcat(s, "    ");
                    v = g_iw; k = 0; { char r[12]; int j = 0; do { r[j++] = (char)('0' + v % 10); v /= 10; } while (v); while (j) num[k++] = r[--j]; }
                    num[k++] = ' '; num[k++] = 'x'; num[k++] = ' ';
                    v = g_ih; { char r[12]; int j = 0; do { r[j++] = (char)('0' + v % 10); v /= 10; } while (v); while (j) num[k++] = r[--j]; }
                    num[k++] = ' '; num[k++] = ' '; num[k++] = ' ';
                    v = scale; { char r[12]; int j = 0; do { r[j++] = (char)('0' + v % 10); v /= 10; } while (v); while (j) num[k++] = r[--j]; }
                    num[k++] = '%'; num[k] = 0;
                    strcat(s, num);
                    if (!g_zoom) strcat(s, " (fit)");
                }
            } else strcat(s, "Pictures, the top folder and a USB stick are looked in");
            pxui_text(pad * 2, status.y + pad, s, t->muted, t->panel, W - 4 * pad);
        }
        pxui_present();
        pxui_end();

        if (act) memset(&in, 0, sizeof in);      /* the click is used: not again next frame */
        if (act == 1) { scan(); sel = 0; top = 0; load(g_n ? 0 : -1); continue; }
        if (act == 2) { g_zoom = 0; continue; }
        if (act == 3 || act == 4) { int z = g_zoom ? g_zoom : scale; z = act == 4 ? z * 5 / 4 + 1 : z * 4 / 5; g_zoom = z < 5 ? 5 : z > 1600 ? 1600 : z; continue; }
        if (act == 5) { g_zoom = 100; continue; }
        if (act == 6) { set_wallpaper(); continue; }
        if (act == 7) { clear_wallpaper(); continue; }

        int k = pxui_wait(&in);
        if (k == 0x1B) break;
        if (k == '\t') { focus = !focus; continue; }
        if (k == '+' || k == '=') { int z = g_zoom ? g_zoom : scale; z = z * 5 / 4 + 1; g_zoom = z > 1600 ? 1600 : z; }
        else if (k == '-') { int z = g_zoom ? g_zoom : scale; z = z * 4 / 5; g_zoom = z < 5 ? 5 : z; }
        else if (k == '0') g_zoom = 0;
        else if (k == '1') g_zoom = 100;
        else if (k == 'w' || k == 'W') set_wallpaper();
        else if ((k == 'r' || k == 'R') && focus == 0) { scan(); sel = 0; top = 0; load(g_n ? 0 : -1); }
        else if (focus == 1) {
            int st = 64;
            if (k == KEY_LEFT) g_panx -= st;
            if (k == KEY_RIGHT) g_panx += st;
            if (k == KEY_UP) g_pany -= st;
            if (k == KEY_DOWN) g_pany += st;
        }
        /* with the list focused, the key is the list's: it reads it from
           `in` when it is drawn next */
    }
    if (g_img) { kfree(g_img); g_img = 0; }
    g_cur = -1;
}

/* v0.9.10: at the desktop's start -- Best performance, or the picture
   wallpaper set last time (read and decoded again: what is kept is its
   path, in the config) */
void images_desktop_start(void)
{
    extern int cfg_perf_mode(void);
    extern int vfs_exists(const char *name);
    if (cfg_perf_mode()) { vga_wm_set_lite(1); return; }
    const char *p = cfg_wall_img();
    if (!p || !p[0] || vga_wm_has_wallpaper()) return;
    /* the size: read in growing pieces (vfs has no stat that every
       filesystem answers) */
    uint32_t cap = 1u << 20;
    for (;;) {
        uint8_t *f = (uint8_t *)kmalloc(cap);
        if (!f) return;
        int n = vfs_read(p, (char *)f, cap);
        if (n < 0) { kfree(f); return; }
        if ((uint32_t)n < cap || cap >= 32u << 20) {
            int w, h;
            uint32_t *img = image_decode(f, (uint32_t)n, &w, &h, 0);
            kfree(f);
            if (img) { vga_wm_set_wallpaper(img, w, h); kfree(img); }
            return;
        }
        kfree(f); cap <<= 1;
    }
}
