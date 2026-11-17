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
void chrome_layout(int pw, int ph, int boxed, int full, ch_layout_t *L)
{
    memset(L, 0, sizeof *L);
    L->title_y = -1; L->bar_y = -1; L->boxed = boxed; L->full = full;
    if(!boxed){
        if(!full && pw >= 80 && ph - 2 >= 25){
            L->vx = 0; L->vy = 1; L->vw = pw; L->vh = ph - 2;
            L->title_y = 0; L->title_x0 = 0; L->title_x1 = pw - 1;
            L->bar_y = ph - 1; L->viewport = 1;
        } else {
            L->vx = 0; L->vy = 0; L->vw = pw; L->vh = ph;     /* whole screen */
            L->viewport = 0;
        }
        return;
    }
    /* boxed: an 80x25 box, a title row above it, a frame row below it */
    if(pw < 82 || ph < 27){                                 /* no room for a box */
        L->vx = 0; L->vy = 0; L->vw = pw; L->vh = ph; L->viewport = 0; L->boxed = 0;
        return;
    }
    L->vw = 80; L->vh = 25; L->vx = (pw - 80) / 2; L->viewport = 1;
    if(!full && ph >= 28){
        int top = (ph - 1 - 27) / 2;            /* rows 0 .. ph-2 hold it */
        L->title_y = top; L->vy = top + 1; L->bar_y = ph - 1;
    } else {
        L->vy = (ph - 25) / 2;
        if(L->vy < 1) L->vy = 1;
        L->title_y = L->vy - 1;
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

/* Taskbar: "Start" then chips of CHIP_W, the clock at the right. */
#define CHIP_X0 8
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

static void current_layout(ch_task_t *t, ch_layout_t *L)
{
    chrome_layout(vga_phys_cols(), vga_phys_rows(), t->boxed, t->full, L);
}

/* Paint everything outside the app's box. With the viewport cleared,
   so the surround's ordinary drawing lands on the screen. */
static void paint(ch_task_t *t)
{
    ch_layout_t L; int vx, vy, vw, vh, act = vga_viewport_active();
    current_layout(t, &L);
    vga_viewport_get(&vx, &vy, &vw, &vh);
    vga_viewport_clear();
    if(L.boxed && g_surround) g_surround(L.vx, L.vy, L.vw, L.vh, t->name);
    draw_title(&L, t->name);
    draw_bar(L.bar_y);
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
    if(L.title_y >= 0 || L.bar_y >= 0) paint(t);
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
    if(t && t->grown) shrink(t, 1);
}

static void toggle_full(ch_task_t *t)
{
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
    apply_layout(t, 0);
    return 1;
}

int chrome_on(void){ ch_task_t *t = me(); return t && t->on; }

void chrome_end(void)
{
    ch_task_t *t = me();
    if(t && t->grown) shrink(t, 0);        /* v0.9.6.6: the grid the person chose */
    if(t) t->on = 0;
    vga_viewport_clear();
    mouse_clamp_to_grid();
}

/* ── the pointer outside the box ─────────────────────────────────────── */
static int      p_x = -1, p_y = -1;
static uint16_t p_saved, p_drawn;
static void ptr_hide(void)
{
    if(p_x < 0) return;
    if(vga_phys_read(p_x, p_y) == p_drawn) vga_phys_write(p_x, p_y, p_saved);
    p_x = p_y = -1;
}
static void ptr_show(int x, int y)
{
    uint16_t c; uint8_t a, inv;
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

int chrome_hook(int key)
{
    ch_task_t *t = me();
    ch_layout_t L;
    int x, y, clicks, chip, what;

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
    case CH_CHIP:
        if(chip >= 0 && chip < g_nchips && g_chip_slot[chip] != task_current()){
            bring(g_chip_slot[chip]); paint(t);
        }
        return -1;
    default:       return -1;
    }
}

void chrome_init(void){ keyboard_set_chrome_hook(chrome_hook); }

/* v0.9.6.6: X arrives as ESC, which is what most apps here want --
   except one whose ESC means something else. The Shell's ESC clears the
   line, so on the HP its X did nothing; it asks this after an ESC: was it
   the window's X? One answer per click. */
int chrome_take_close(void){ int r = g_close_req; g_close_req = 0; return r; }
