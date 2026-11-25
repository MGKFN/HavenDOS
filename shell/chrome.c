/* shell/chrome.c -- an app's window: title bar and taskbar (v0.9.6.5)
 *
 * Kyan, 16 Nov: "I would like that the taskbar is visible at all times,
 * with the X, full screen and minimize buttons at the window's top bar."
 *
 * Until now an open app owned the whole screen (or an 80x25 box in the
 * middle of it, with a title bar that said "ESC closes"), and the
 * taskbar was the desktop's alone. Now every app opened from the
 * desktop is a window:
 *
 *   row 0 (or the row above a boxed app)  the title bar:
 *       the app's name ....................... [ _ ] [ [] ] [ X ]
 *   in between                             the app, in a viewport
 *   the last row                           the taskbar: Start, a chip
 *                                          for every open app, the clock
 *
 *   _    minimize: the desktop comes forward; the app stays open on the
 *        taskbar (exactly Alt+Tab to the desktop)
 *   []   full screen: no title bar, no taskbar -- the app has the whole
 *        screen; F11 or Alt+Enter brings them back (a boxed 80x25 app
 *        keeps its title bar, centred on the whole screen)
 *   X    ESC, which is how every app here closes -- or, in a sub-screen,
 *        goes back a level. Kyan's choice: nothing unsaved is thrown away
 *   Start  the desktop, with its start menu open
 *   a chip that app, brought forward
 *
 * How it gets the clicks: drivers/mouse.c keeps the pointer on the whole
 * screen since 0.9.6.5, gives an app coordinates inside its own box, and
 * keeps a click made OUTSIDE the box for this file. keyboard_getchar()
 * calls chrome_hook() before the mouse layer (shell/uimouse.c); it draws
 * the pointer while it is outside the box and acts on the click.
 *
 * Per task, like everything an app has: a slot's state is reset when a
 * new task takes it.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/keys.h"
#include "../include/task.h"
#include "../include/chrome.h"
#include "../include/wm.h"

extern void     mouse_get_phys(int *x, int *y);
extern int      mouse_outside(void);
extern int      mouse_is_ready(void);
extern int      mouse_take_clicks(void);
extern void     mouse_clamp_to_grid(void);
extern uint32_t get_ticks(void);
extern void     rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
extern void     keyboard_inject(int c);
extern void     keyboard_set_chrome_hook(int (*fn)(int));
extern void     toast_info(const char *m);

/* ── the layout, pure ────────────────────────────────────────────────── */
/* v0.9.8: how many rows the title bar and the taskbar take. One, except
   with the window manager on a small cell (8x16), where a 16-pixel bar
   would be too small to hold an icon and a title: two. */
static int ch_tr = 1;
void chrome_layout(int pw, int ph, int boxed, int full, ch_layout_t *L)
{
    int tr = ch_tr;
    memset(L, 0, sizeof *L);
    L->title_y = -1; L->bar_y = -1; L->boxed = boxed; L->full = full;
    if(!boxed){
        if(!full && pw >= 80 && ph - 2 * tr >= 25){
            L->vx = 0; L->vy = tr; L->vw = pw; L->vh = ph - 2 * tr;
            L->title_y = 0; L->title_x0 = 0; L->title_x1 = pw - 1;
            L->bar_y = ph - tr; L->viewport = 1;
        } else {
            L->vx = 0; L->vy = 0; L->vw = pw; L->vh = ph;     /* whole screen */
            L->viewport = 0;
        }
        return;
    }
    /* boxed: an 80x25 box, a title row above it, a frame row below it */
    if(pw < 82 || ph < 25 + tr + 1){                         /* no room for a box */
        L->vx = 0; L->vy = 0; L->vw = pw; L->vh = ph; L->viewport = 0; L->boxed = 0;
        return;
    }
    L->vw = 80; L->vh = 25; L->vx = (pw - 80) / 2; L->viewport = 1;
    if(!full && ph >= 25 + 2 * tr + 1){
        int top = (ph - tr - (25 + tr + 1)) / 2;   /* rows 0 .. ph-1-tr hold it */
        L->title_y = top; L->vy = top + tr; L->bar_y = ph - tr;
    } else {
        L->vy = (ph - 25) / 2;
        if(L->vy < tr) L->vy = tr;
        L->title_y = L->vy - tr;
    }
    L->title_x0 = L->vx - 1; L->title_x1 = L->vx + 80;
}

/* The three buttons, right-aligned on the title row: " _ " "[ ]" " X " */
#define BTN_W 3
static int g_close_req;      /* v0.9.6.6: see chrome_take_close() */
static int btn_x(const ch_layout_t *L, int which)   /* 0 min, 1 max, 2 close */
{
    return L->title_x1 - (3 - which) * (BTN_W + 1) + 1;
}

/* Taskbar: "Start", the pinned apps (v0.9.7, four cells each), then
   chips of CHIP_W, the clock at the right. The pins come from
   shell/desktop.c, the same list the desktop's own bar shows. */
#define PIN_X0  8
#define PIN_W   4
extern int         desktop_tbpin_count(void);
extern const char *desktop_tbpin_icon(int i);
extern void        desktop_tbpin_open(int i);
static int chip_x0(void){ int n = desktop_tbpin_count(); return PIN_X0 + n * PIN_W + (n ? 1 : 0); }
#define CHIP_X0 chip_x0()
#define CHIP_W  14

int chrome_hit(const ch_layout_t *L, int pw, int x, int y, int *chip)
{
    int i;
    if(chip) *chip = -1;
    if(L->title_y >= 0 && y == L->title_y && x >= L->title_x0 && x <= L->title_x1){
        for(i = 0; i < 3; i++)
            if(x >= btn_x(L, i) && x < btn_x(L, i) + BTN_W)
                return i == 0 ? CH_MIN : i == 1 ? CH_FULL : CH_CLOSE;
        return CH_TITLE;
    }
    if(L->bar_y >= 0 && y == L->bar_y){
        if(x < 7) return CH_START;
        if(x >= PIN_X0 && x < PIN_X0 + desktop_tbpin_count() * PIN_W){
            if(chip) *chip = (x - PIN_X0) / PIN_W;
            return CH_PIN;
        }
        if(x >= CHIP_X0 && x < pw - 7){
            if(chip) *chip = (x - CHIP_X0) / CHIP_W;
            return CH_CHIP;
        }
        return CH_BAR;
    }
    return CH_NONE;
}

/* ── per task ────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t    id;
    int         on, boxed, full;
    const char *name;
    uint32_t    sig;            /* what the chrome showed last          */
    uint32_t    next_check;     /* ticks                                */
    int         grown;          /* v0.9.6.6: full screen at 80x25, filled */
    int         pc, pr;         /* ... and the grid to go back to         */
    int         moved, wx, wy;  /* v0.9.8: where it was dragged to (px)    */
    int         solo;           /* v0.9.8: it has the whole screen         */
    int         maxed;          /* v0.9.8.2: maximised, title bar and taskbar kept */
} ch_task_t;
static ch_task_t g_t[TASK_MAX];
static void (*g_surround)(int vx, int vy, int vw, int vh, const char *name);
static int g_chip_slot[16], g_nchips;

static ch_task_t *me(void)
{
    int s = task_current();
    if(s < 0 || s >= TASK_MAX) return 0;
    if(g_t[s].id != task_id(s)){ memset(&g_t[s], 0, sizeof g_t[s]); g_t[s].id = task_id(s); }
    return &g_t[s];
}

void chrome_set_surround(void (*fn)(int, int, int, int, const char *)){ g_surround = fn; }

static void put(int x, int y, const char *s, int fg, int bg, int max)
{
    uint16_t a = (uint16_t)(((bg & 0x0F) << 4 | (fg & 0x0F)) << 8);
    while(*s && max-- > 0) vga_phys_write(x++, y, (uint16_t)(a | (uint8_t)*s++));
}
static void fill(int x0, int x1, int y, int fg, int bg)
{
    uint16_t a = (uint16_t)(((bg & 0x0F) << 4 | (fg & 0x0F)) << 8);
    for(; x0 <= x1; x0++) vga_phys_write(x0, y, (uint16_t)(a | ' '));
}

static void draw_title(const ch_layout_t *L, const char *name)
{
    int y = L->title_y, x0 = L->title_x0 < 0 ? 0 : L->title_x0, x1 = L->title_x1;
    if(y < 0) return;
    if(x1 >= vga_phys_cols()) x1 = vga_phys_cols() - 1;
    fill(x0, x1, y, VGA_BLACK, VGA_CYAN);
    put(x0 + 2, y, name ? name : "App", VGA_BLACK, VGA_CYAN, x1 - x0 - 30);
    /* the old title bar's promise, kept where there is room for it.
       v0.9.6.6: not for the Shell, whose ESC clears the line -- it closes
       with `exit` or the X, and the bar said otherwise on the HP. */
    if(btn_x(L, 0) - 12 > x0 + 4 + (name ? (int)strlen(name) : 3)){
        int shell = name && !strcmp(name, "Shell");
        put(btn_x(L, 0) - 12, y, shell ? "exit closes" : "ESC closes", VGA_BLACK, VGA_CYAN, 11);
    }
    put(btn_x(L, 0), y, " _ ", VGA_BLACK, VGA_LIGHT_GREY, 3);
    put(btn_x(L, 1), y, L->full ? "[=]" : "[ ]", VGA_BLACK, VGA_LIGHT_GREY, 3);
    put(btn_x(L, 2), y, " X ", VGA_WHITE, VGA_RED, 3);
}

static uint32_t bar_sig(void)
{
    uint8_t h, m, s; uint32_t sig = 0; int i;
    rtc_get_time(&h, &m, &s);
    sig = (uint32_t)h * 60u + m;
    for(i = 1; i < TASK_MAX; i++)
        if(task_state(i) == TS_READY || task_state(i) == TS_WAIT) sig ^= (task_id(i) * 2654435761u) + (uint32_t)i;
    return sig ^ ((uint32_t)task_current() << 24);
}

static void draw_bar(int y)
{
    int pw = vga_phys_cols(), i, x = CHIP_X0, cur = task_current();
    uint8_t h, m, s; char clk[6];
    if(y < 0) return;
    fill(0, pw - 1, y, VGA_WHITE, VGA_DARK_GREY);
    put(0, y, " Start ", VGA_BLACK, VGA_LIGHT_CYAN, 7);
    for(i = 0; i < desktop_tbpin_count() && PIN_X0 + (i + 1) * PIN_W < pw - 7; i++){
        const char *ic = desktop_tbpin_icon(i);
        int px = PIN_X0 + i * PIN_W;
        put(px, y, " ", VGA_WHITE, VGA_DARK_GREY, 1);
        put(px + 1, y, ic, VGA_BLACK, VGA_LIGHT_GREY, 2);
        put(px + 3, y, " ", VGA_WHITE, VGA_DARK_GREY, 1);
    }
    g_nchips = 0;
    for(i = 1; i < TASK_MAX && x + CHIP_W <= pw - 7 && g_nchips < 16; i++){
        const char *n;
        if(task_state(i) != TS_READY && task_state(i) != TS_WAIT) continue;
        n = task_name(i);
        put(x, y, " ", VGA_WHITE, i == cur ? VGA_BLUE : VGA_DARK_GREY, 1);
        fill(x + 1, x + CHIP_W - 2, y, VGA_WHITE, i == cur ? VGA_BLUE : VGA_DARK_GREY);
        put(x + 1, y, n, VGA_WHITE, i == cur ? VGA_BLUE : VGA_DARK_GREY, CHIP_W - 3);
        g_chip_slot[g_nchips++] = i;
        x += CHIP_W;
    }
    rtc_get_time(&h, &m, &s);
    clk[0] = (char)('0' + h / 10); clk[1] = (char)('0' + h % 10); clk[2] = ':';
    clk[3] = (char)('0' + m / 10); clk[4] = (char)('0' + m % 10); clk[5] = 0;
    put(pw - 6, y, clk, VGA_WHITE, VGA_DARK_GREY, 5);
}

/* v0.9.8.1: a window the window manager draws -- an app in a task of its
   own. An app the desktop runs INLINE, in its own console (safe paths,
   no task free), has the screen to itself (shell/desktop.c makes it solo)
   and its window drawn in cells, as before 0.9.8: otherwise it showed the
   desktop's icons painted over it, and no title bar (Kyan's photos). */
static int wm_managed(void){ return vga_wm_on() && task_current() > 0; }

static void current_layout(ch_task_t *t, ch_layout_t *L)
{
    if(wm_managed()) ch_tr = vga_wm_title_rows();      /* v0.9.8 */
    chrome_layout(vga_phys_cols(), vga_phys_rows(), t->boxed, t->full, L);
}

/* ── v0.9.8: THE WINDOW, IN PIXELS ────────────────────────────────────
   With the window manager on, nothing of the window is drawn into the
   app's console any more: drivers/wm_core.inc draws the title bar, the
   edge and the shadow, and shows the app's viewport -- its 80x25 box, or
   the space between title bar and taskbar -- wherever the window is. The
   layout above still decides the box; this tells the window manager. */
static void wm_sync(ch_task_t *t)
{
    ch_layout_t L; int x, y, s = task_current();
    if(!vga_wm_on() || !vga_wm_is_open(s)) return;
    current_layout(t, &L);
    /* no title bar and taskbar fit (80x25), or full screen: the window has
       the whole screen while it is in front */
    vga_wm_full(s, !L.viewport || t->solo);
    if(!L.viewport || t->solo) return;
    vga_mouse_px(L.vx, L.vy, 0, 0, &x, &y);
    if(t->moved && L.boxed){ x = t->wx; y = t->wy; }
    if(L.boxed && t->maxed){
        /* v0.9.8.2: maximised -- edge to edge between the title bar and
           the taskbar, both kept (Kyan: "maximize should still let the
           buttons and taskbar and all visible"). The app is still 80x25
           until 0.9.9 makes windows resizable, so it sits in the middle. */
        int cw = vga_cell_w() * L.vw, chh = vga_cell_h() * L.vh;
        int top = vga_wm_title_rows() * vga_cell_h(), bot = vga_screen_h() - vga_wm_bar_h();
        int gx, gy; vga_mouse_px(0, 0, 0, 0, &gx, &gy);
        top += gy; if(top < vga_wm_title_h()) top = vga_wm_title_h();
        x = (vga_screen_w() - cw) / 2;
        y = top + (bot - top - chh) / 2; if(y < top) y = top;
        vga_wm_geometry(s, L.vx, L.vy, L.vw, L.vh, x, y, WM_MAXED);
        return;
    }
    vga_wm_geometry(s, L.vx, L.vy, L.vw, L.vh, x, y, L.boxed ? WM_FLOAT : WM_MAXED);
}

/* Paint everything outside the app's box. With the viewport cleared,
   so the surround's ordinary drawing lands on the screen. */
static void wm_sync(ch_task_t *t);
static void wm_full_leave(ch_task_t *t);
static void paint(ch_task_t *t)
{
    ch_layout_t L; int vx, vy, vw, vh, act = vga_viewport_active();
    if(wm_managed()){ wm_sync(t); t->sig = bar_sig(); return; }   /* v0.9.8: pixels */
    current_layout(t, &L);
    vga_viewport_get(&vx, &vy, &vw, &vh);
    vga_viewport_clear();
    if(L.boxed && g_surround) g_surround(L.vx, L.vy, L.vw, L.vh, t->name);
    draw_title(&L, t->name);
    draw_bar(L.bar_y);
    /* v0.9.7.10: a boxed app is a window: shadow and round corners */
    if(L.boxed && L.title_y >= 0)
        vga_deco_window(0, L.title_x0, L.title_y, L.title_x1, L.vy + L.vh - 1);
    else
        vga_deco_clear(0);
    if(act) vga_viewport_set(vx, vy, vw, vh);
    t->sig = bar_sig();
}

/* Move what the app has drawn from one box to another, so a switch to
   or from full screen shows the app at once instead of a blank box
   until its next key. Row by row, as much as both boxes hold. */
static void move_box(int ox, int oy, int ow, int oh, int nx, int ny, int nw, int nh)
{
    static uint16_t row[VGA_COLS_MAX];
    int y, x, w = ow < nw ? ow : nw, h = oh < nh ? oh : nh;
    int down = ny > oy;
    if(w > VGA_COLS_MAX) w = VGA_COLS_MAX;
    for(int k = 0; k < h; k++){
        y = down ? h - 1 - k : k;                     /* never read a row already written */
        for(x = 0; x < w; x++) row[x] = vga_phys_read(ox + x, oy + y);
        for(x = 0; x < w; x++) vga_phys_write(nx + x, ny + y, row[x]);
    }
}

static void apply_layout(ch_task_t *t, int repaint_from_old)
{
    ch_layout_t L; int ox, oy, ow, oh, had = vga_viewport_active();
    vga_viewport_get(&ox, &oy, &ow, &oh);
    if(!had){ ox = 0; oy = 0; ow = vga_phys_cols(); oh = vga_phys_rows(); }
    current_layout(t, &L);
    if(repaint_from_old){
        int x, y, h = oh < L.vh ? oh : L.vh, w = ow < L.vw ? ow : L.vw;
        move_box(ox, oy, ow, oh, L.vx, L.vy, L.vw, L.vh);
        /* what the old box did not cover -- the title bar and taskbar
           going full screen -- is blank until the app's next frame,
           not the chrome it just left behind */
        for(y = 0; y < L.vh; y++)
            for(x = 0; x < L.vw; x++)
                if(y >= h || x >= w) vga_phys_write(L.vx + x, L.vy + y, 0x0720);
    }
    if(L.viewport) vga_viewport_set(L.vx, L.vy, L.vw, L.vh);
    else           vga_viewport_clear();
    mouse_clamp_to_grid();
    if(wm_managed()) wm_sync(t);                     /* v0.9.8: also with no title bar */
    else if(L.title_y >= 0 || L.bar_y >= 0) paint(t);
}

/* ── v0.9.6.6: full screen FILLS for an 80x25 app ─────────────────────
   Kyan chose it on 17 November: an 80x25 app in full screen was the same
   80x25 box, centred, at the size it already had. Now the console itself
   becomes 80x25 with the biggest cell the panel takes
   (vga_set_mode_fill), and the app's 2000 cells move with it. At 80x25
   the chrome has no room for a title bar or taskbar, so neither is drawn,
   exactly as in an any-size app's full screen; F11 or Alt+Enter brings
   them back.

   It is undone whenever the app stops being the one on the screen --
   F11 again, closing it, and before Alt+Tab or a taskbar click hands the
   screen to another app (chrome_leave_front) -- because every other
   app's console is laid out for the grid the person chose. */
static uint16_t g_grow_buf[80 * 25];

static int grow(ch_task_t *t)
{
    ch_layout_t L; int x, y;
    if(!vga_fb_active()) return 0;
    current_layout(t, &L);
    if(!L.boxed || L.vw != 80 || L.vh != 25) return 0;
    for(y = 0; y < 25; y++)
        for(x = 0; x < 80; x++) g_grow_buf[y * 80 + x] = vga_phys_read(L.vx + x, L.vy + y);
    t->pc = vga_phys_cols(); t->pr = vga_phys_rows();
    vga_viewport_clear();
    if(!vga_set_mode_fill(80, 25)){ apply_layout(t, 0); return 0; }
    for(y = 0; y < 25; y++)
        for(x = 0; x < 80; x++) vga_phys_write(x, y, g_grow_buf[y * 80 + x]);
    t->grown = 1; t->full = 1;
    apply_layout(t, 0);
    return 1;
}

static void shrink(ch_task_t *t, int keep)
{
    ch_layout_t L; int x, y;
    if(!t->grown) return;
    if(keep)
        for(y = 0; y < 25; y++)
            for(x = 0; x < 80; x++) g_grow_buf[y * 80 + x] = vga_phys_read(x, y);
    t->grown = 0; t->full = 0;
    vga_viewport_clear();
    vga_set_mode(t->pc, t->pr);
    current_layout(t, &L);
    if(keep && L.vw == 80 && L.vh == 25)
        for(y = 0; y < 25; y++)
            for(x = 0; x < 80; x++) vga_phys_write(L.vx + x, L.vy + y, g_grow_buf[y * 80 + x]);
    apply_layout(t, 0);
}

/* The app in front is about to stop being in front (shell/taskswitch.c,
   and the taskbar below). Nothing to do unless it is grown. */
void chrome_leave_front(void)
{
    ch_task_t *t = me();
    if(t && t->solo){ wm_full_leave(t); return; }     /* v0.9.8 */
    if(t && t->grown) shrink(t, 1);
}

static void wm_full_leave(ch_task_t *t)
{
    if(!t->solo) return;
    t->solo = 0;
    if(t->grown) shrink(t, 1);
    else { t->full = 0; apply_layout(t, 1); }
    wm_sync(t);
}

static void toggle_full(ch_task_t *t)
{
    /* v0.9.8: full screen is the window having the whole screen */
    if(wm_managed()){
        if(t->solo){ wm_full_leave(t); return; }
        t->solo = 1;
        vga_wm_full(task_current(), 1);
        if(t->boxed && grow(t)) return;
        t->full = 1; apply_layout(t, 1);
        if(!t->boxed) toast_info("Full screen -- F11 or Alt+Enter brings the window back");
        return;
    }
    if(t->grown){ shrink(t, 1); return; }
    if(!t->full && t->boxed && grow(t)) return;
    t->full = !t->full;
    apply_layout(t, 1);
    if(t->full && !t->boxed) toast_info("Full screen -- F11 or Alt+Enter brings the taskbar back");
}

/* ── for app_run_inline() ────────────────────────────────────────────── */
int chrome_begin(const char *name, int boxed)
{
    ch_task_t *t = me();
    if(!t || !task_ready()) return 0;
    t->on = 1; t->boxed = boxed; t->full = 0; t->name = name;
    t->moved = 0; t->solo = 0; t->maxed = 0;
    if(wm_managed()){                       /* v0.9.8: a window of pixels */
        extern const char *desktop_app_icon_id(const char *name);
        vga_wm_open(task_current(), name, desktop_app_icon_id(name));
    }
    apply_layout(t, 0);
    return 1;
}

int chrome_on(void){ ch_task_t *t = me(); return t && t->on; }

void chrome_end(void)
{
    ch_task_t *t = me();
    vga_deco_clear(0);                     /* v0.9.7.10: the window is gone */
    if(t) t->solo = 0;                                  /* v0.9.8 */
    vga_wm_close(task_current());
    if(t && t->grown) shrink(t, 0);        /* v0.9.6.6: the grid the person chose */
    if(t) t->on = 0;
    vga_viewport_clear();
    mouse_clamp_to_grid();
}

/* ── the pointer outside the box ─────────────────────────────────────── */
static int      p_x = -1, p_y = -1;
static uint16_t p_saved, p_drawn;
static int      p_arrow;              /* v0.9.7.10: this code put the arrow up */
static void ptr_hide(void)
{
    if(wm_managed()) return;                /* v0.9.8: the hook below draws it */
    /* v0.9.7.10: only an arrow THIS code showed. The hook runs for
       every keyboard poll, also where nothing here is tracking the
       mouse (the desktop), and hiding unconditionally took the
       desktop's own arrow away as fast as it was drawn. */
    if(p_arrow){ p_arrow = 0; vga_pointer_hide(); }
    if(p_x < 0) return;
    if(vga_phys_read(p_x, p_y) == p_drawn) vga_phys_write(p_x, p_y, p_saved);
    p_x = p_y = -1;
}
static void ptr_show(int x, int y)
{
    uint16_t c; uint8_t a, inv;
    if(wm_managed()) return;                /* v0.9.8 */
    /* v0.9.7.10: the arrow, on a framebuffer */
    if(vga_pointer_pixels()){
        extern void mouse_get_frac(int*,int*);
        extern int  mouse_pointer_wanted(void);
        int px, py, fx, fy;
        if(p_x >= 0){ if(vga_phys_read(p_x, p_y) == p_drawn) vga_phys_write(p_x, p_y, p_saved); p_x = p_y = -1; }
        if(!mouse_pointer_wanted()){ ptr_hide(); return; }
        mouse_get_phys(&px, &py); mouse_get_frac(&fx, &fy);
        if(vga_pointer_show(px, py, fx, fy, 0)){ p_arrow = 1; return; }
    }
    if(x == p_x && y == p_y && vga_phys_read(x, y) == p_drawn) return;
    ptr_hide();
    c = vga_phys_read(x, y); a = (uint8_t)(c >> 8);
    inv = (uint8_t)(((a & 0x0Fu) << 4) | ((a >> 4) & 0x07u));
    if((a & 0x0Fu) == ((a >> 4) & 0x07u)) inv = 0x7F;
    p_saved = c; p_drawn = (uint16_t)((c & 0xFFu) | ((uint16_t)inv << 8));
    vga_phys_write(x, y, p_drawn);
    p_x = x; p_y = y;
}

/* ── the hook ────────────────────────────────────────────────────────── */
static void bring(int slot)
{
    ptr_hide();
    chrome_leave_front();
    if(task_set_front(slot) == 0) task_wait_front();
}

static int wm_hook(ch_task_t *t, int key);
int chrome_hook(int key)
{
    ch_task_t *t = me();
    ch_layout_t L;
    int x, y, clicks, chip, what;

    if(vga_wm_on() && !(t && t->on && task_current() == 0)) return wm_hook(t, key);   /* v0.9.8 */

    if(!t || !t->on){ return key; }
    if(key == KEY_FULLSCREEN){
        ptr_hide();
        toggle_full(t);
        return -1;
    }
    /* a real key: any X left unasked-for by the app it was meant for
       is forgotten, so a later ESC of the keyboard's is never taken
       for a click */
    if(key != -1){ g_close_req = 0; ptr_hide(); return key; }

    current_layout(t, &L);
    /* the clock and the chips, about four times a second at most */
    if((L.title_y >= 0 || L.bar_y >= 0) && (int32_t)(get_ticks() - t->next_check) >= 0){
        t->next_check = get_ticks() + 25;
        if(bar_sig() != t->sig || (L.title_y >= 0 &&
           (vga_phys_read(btn_x(&L, 2) + 1, L.title_y) & 0xFF) != 'X')){
            ptr_hide(); paint(t);
        }
    }
    if(!mouse_is_ready()) return -1;
    if(!mouse_outside()){ ptr_hide(); return -1; }

    mouse_get_phys(&x, &y);
    ptr_show(x, y);
    clicks = mouse_take_clicks();
    if(!(clicks & 1)) return -1;

    what = chrome_hit(&L, vga_phys_cols(), x, y, &chip);
    switch(what){
    case CH_CLOSE: ptr_hide(); g_close_req = 1; return 27;
    case CH_FULL:
        ptr_hide(); toggle_full(t);
        if(t->grown) toast_info("Full screen -- F11 or Alt+Enter brings the taskbar back");
        return -1;
    case CH_MIN:   bring(0); paint(t); return -1;
    case CH_START: keyboard_inject(KEY_SUPER); bring(0); paint(t); return -1;
    case CH_PIN:   ptr_hide(); chrome_leave_front(); desktop_tbpin_open(chip); paint(t); return -1;
    case CH_CHIP:
        if(chip >= 0 && chip < g_nchips && g_chip_slot[chip] != task_current()){
            bring(g_chip_slot[chip]); paint(t);
        }
        return -1;
    default:       return -1;
    }
}


/* ════════════════════════════════════════════════════════════════════
   v0.9.8: THE WINDOW MANAGER'S HOOK

   With the window manager on, this runs for every keyboard poll of
   every task, the desktop included. It draws the pointer, takes the
   clicks the mouse driver gave the window manager (anything that is not
   the content of the app in front -- see drivers/mouse.c), and acts on
   them: a title bar dragged moves its window, its buttons minimise,
   fill the screen or close, another window or the desktop clicked comes
   to the front, the taskbar and the Start menu do their things.
   ════════════════════════════════════════════════════════════════════ */
extern void mouse_get_px(int *x, int *y);
extern int  mouse_buttons_raw(void);
extern int  mouse_take_wm_clicks(int *x, int *y);
extern void mouse_repost_clicks(int edges);
extern int  mouse_pointer_wanted(void);
extern void mouse_get_frac(int *fx, int *fy);
extern void mouse_sync(void);
extern void kpoll_sleep_ms(uint32_t ms);
extern int  taskbar_hit(int x, int y, int *idx);
extern int  taskbar_hover(int x, int y);
extern uint32_t taskbar_sig(void);
extern void startmenu_run(void);
extern void desktop_tbpin_open(int i);

static int g_wm_busy, g_wm_modal;
void wm_busy(int on){ g_wm_modal += on ? 1 : -1; if(g_wm_modal < 0) g_wm_modal = 0; }
static int g_hov_slot = -1, g_hov_part;
static uint32_t g_bar_sig, g_bar_next;

void wm_pointer_update(void)
{
    int px, py, fx, fy;
    if(!mouse_is_ready()) return;
    if(!mouse_pointer_wanted()){ vga_pointer_hide(); return; }
    mouse_get_phys(&px, &py); mouse_get_frac(&fx, &fy);
    vga_pointer_show(px, py, fx, fy, mouse_buttons_raw() & 1);
}

static ch_task_t *slot_task(int s)
{
    if(s < 0 || s >= TASK_MAX || g_t[s].id != task_id(s)) return 0;
    return &g_t[s];
}

static void wm_bring(int slot)
{
    if(slot == task_current()) return;
    chrome_leave_front();
    if(task_set_front(slot) == 0) task_wait_front();
}

/* Drag window s by its title bar until the button comes up. */
static void wm_drag(int s, int px0, int py0)
{
    int wx, wy, ww, wh, x, y, nx, ny, lx, ly;
    int sw = vga_screen_w(), sh = vga_screen_h(), th = vga_wm_title_h(), bh = vga_wm_bar_h();
    vga_wm_get(s, &wx, &wy, &ww, &wh);
    lx = wx; ly = wy;
    while(mouse_buttons_raw() & 1){
        mouse_get_px(&x, &y);
        nx = wx + (x - px0); ny = wy + (y - py0);
        /* the title bar stays reachable: on the screen, above the bar */
        if(ny < th) ny = th;
        if(ny > sh - bh - 4) ny = sh - bh - 4;
        if(nx > sw - 3 * th) nx = sw - 3 * th;
        if(nx + ww < 3 * th) nx = 3 * th - ww;
        if(nx != lx || ny != ly){ vga_wm_move(s, nx, ny); lx = nx; ly = ny; }
        wm_pointer_update();
        kpoll_sleep_ms(10);
    }
    {
        ch_task_t *t = slot_task(s);
        if(t){ t->moved = 1; t->wx = lx; t->wy = ly; }
    }
}

static void wm_taskbar_click(int x, int y)
{
    int idx = -1, what = taskbar_hit(x, y, &idx);
    switch(what){
    case 1:                                   /* Start */
        startmenu_run();
        break;
    case 2:                                   /* a pinned app */
        chrome_leave_front();
        desktop_tbpin_open(idx);
        break;
    case 3:                                   /* an open window */
        if(idx == vga_wm_front_slot() && !vga_wm_is_min(idx)){
            vga_wm_minimize(idx, 1);
            wm_bring(0);
        } else {
            if(vga_wm_is_min(idx)) vga_wm_minimize(idx, 0);
            wm_bring(idx);
        }
        break;
    case 4: {                                 /* v0.9.8.2: a flyout, not an app */
        extern void notif_flyout_run(void);
        notif_flyout_run();
        break; }
    default: break;
    }
    vga_wm_repaint_bar();
}

static int wm_hook(ch_task_t *t, int key)
{
    int x, y, s, part, c;
    if(g_wm_busy || g_wm_modal) return key;
    if(key == KEY_SUPER){ startmenu_run(); return -1; }   /* it guards itself (wm_busy) */
    if(key == KEY_FULLSCREEN && t && t->on){ toggle_full(t); return -1; }
    if(key != -1){ g_close_req = 0; return key; }
    if(!mouse_is_ready()) return -1;
    g_wm_busy = 1;
    wm_pointer_update();

    /* the clock and the window list, a few times a second at most */
    if((int32_t)(get_ticks() - g_bar_next) >= 0){
        uint32_t sig = taskbar_sig();
        g_bar_next = get_ticks() + 25;
        if(sig != g_bar_sig){ g_bar_sig = sig; vga_wm_repaint_bar(); }
    }

    mouse_get_px(&x, &y);
    vga_wm_hit(x, y, &s, &part);
    /* the title bar's buttons light up under the pointer */
    {
        int hs = (s > 0 && part >= WMP_MIN && part <= WMP_CLOSE) ? s : -1;
        int hp = hs > 0 ? part : 0;
        if(hs != g_hov_slot || hp != g_hov_part){
            if(g_hov_slot > 0 && g_hov_slot != hs) vga_wm_buttons(g_hov_slot, 0, 0);
            if(hs > 0) vga_wm_buttons(hs, hp, 0);
            g_hov_slot = hs; g_hov_part = hp;
        }
    }
    if(part == WMP_BAR) { if(taskbar_hover(x, y)) vga_wm_repaint_bar(); }
    else if(taskbar_hover(-1, -1)) vga_wm_repaint_bar();

    /* only the task in front acts on clicks: one click, one action */
    if(task_current() != task_front()){ g_wm_busy = 0; return -1; }
    c = mouse_take_wm_clicks(&x, &y);
    if(!c){ g_wm_busy = 0; return -1; }
    vga_wm_hit(x, y, &s, &part);
    g_wm_busy = 0;            /* what follows may switch tasks or open an app */
    if(!(c & 1)){
        /* a right or middle click on another window or the desktop still
           brings it forward, and the app gets it */
        if(part == WMP_CONTENT && s != task_current()){ wm_bring(s); mouse_repost_clicks(c); }
        return -1;
    }
    switch(part){
    case WMP_TITLE:
    case WMP_EDGE:
        if(s != task_current()){ mouse_repost_clicks(0); wm_bring(s); break; }
        if(part == WMP_TITLE && t && t->on && t->boxed && !t->solo && !t->maxed) wm_drag(s, x, y);
        break;
    case WMP_MIN:
        vga_wm_buttons(s, 0, 0);
        vga_wm_minimize(s, 1);
        wm_bring(0);
        break;
    case WMP_MAX:
        /* v0.9.8.2: maximise, not full screen -- the title bar and the
           taskbar stay. Full screen is F11 or Alt+Enter. */
        if(s != task_current()){ wm_bring(s); break; }
        if(t && t->on){
            if(t->boxed){ t->maxed = !t->maxed; wm_sync(t); vga_wm_repaint_bar(); }
            else toast_info("Already as big as it gets -- F11 or Alt+Enter: full screen");
        }
        break;
    case WMP_CLOSE:
        vga_wm_buttons(s, WMP_CLOSE, WMP_CLOSE);
        g_close_req = 1;
        if(s != task_current()){ wm_bring(s); keyboard_inject(27); break; }
        return 27;
    case WMP_CONTENT:
        /* another window, or the desktop: it comes forward and gets the click */
        if(s != task_current()){ wm_bring(s); mouse_repost_clicks(c); }
        break;
    case WMP_BAR:
        wm_taskbar_click(x, y);
        break;
    default: break;
    }
    return -1;
}

void chrome_init(void){ keyboard_set_chrome_hook(chrome_hook); }

/* v0.9.7.1: a screen that takes the WHOLE display from inside an app --
   the sign-in screen, from the Shell's `lock` or the Power app's
   Switch user. Left on, the window's taskbar kept repainting its clock
   over the sign-in screen's bottom strip, and a click on it could still
   bring another app forward past the lock. Paused, the hook passes keys
   straight through and draws nothing; resumed, the window and its box
   are put back. */
int chrome_pause(void)
{
    ch_task_t *t = me();
    if(!t || !t->on) return 0;
    ptr_hide();
    if(t->solo){ wm_full_leave(t); }
    if(t->grown) shrink(t, 1);
    t->on = 0;
    vga_wm_solo(1);                        /* v0.9.8: the whole screen is its */
    return 1;
}

void chrome_resume(int was)
{
    ch_task_t *t = me();
    if(!was || !t) return;
    vga_wm_solo(0);                        /* v0.9.8 */
    t->on = 1;
    apply_layout(t, 0);
}

/* v0.9.6.6: X arrives as ESC, which is what most apps here want --
   except one whose ESC means something else. The Shell's ESC clears the
   line, so on the HP its X did nothing; it asks this after an ESC: was it
   the window's X? One answer per click. */
int chrome_take_close(void){ int r = g_close_req; g_close_req = 0; return r; }
