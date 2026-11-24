/* tests/test_uimouse.c -- the mouse in every app (v0.9.6.3)
 *
 * Compiles shell/uimouse.c against a model screen (80x25 of cells), a
 * model mouse whose position, clicks and wheel the test sets, and a
 * model task layer. Checks what each click, notch and right-click
 * turns into, that the pointer never leaves a cell behind, and that a
 * task behind never writes into the table of the task in front.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define W 80
#define H 25
static uint16_t scr[W * H];
static int g_rows = H, g_cols = W;
int  vga_rows(void){ return g_rows; }
int  vga_cols(void){ return g_cols; }
uint16_t vga_shadow_read(int x, int y){ return (x < 0 || y < 0 || x >= W || y >= H) ? 0x0720 : scr[y * W + x]; }
/* v0.9.7.10: no framebuffer here, so the cell pointer this suite checks */
int  vga_pointer_pixels(void){ return 0; }
int  vga_pointer_show(int c, int r, int fx, int fy, int d){ (void)c;(void)r;(void)fx;(void)fy;(void)d; return 0; }
void vga_pointer_hide(void){}
void mouse_get_phys(int *x, int *y){ *x = 0; *y = 0; }
void mouse_get_frac(int *x, int *y){ *x = 0; *y = 0; }
int  mouse_pointer_wanted(void){ return 1; }
void vga_shadow_write(int x, int y, uint16_t v){ if(x >= 0 && y >= 0 && x < W && y < H) scr[y * W + x] = v; }
static void (*g_clear)(void);
void vga_set_clear_hook(void (*fn)(void)){ g_clear = fn; }

static int m_ready = 1, m_x, m_y, m_btn, m_clicks, m_wheel;
void mouse_get(int *x, int *y, int *b){ *x = m_x; *y = m_y; *b = m_btn; }
int  mouse_is_ready(void){ return m_ready; }
int  mouse_get_clicks(void){ int c = m_clicks; m_clicks = 0; return c; }
int  mouse_get_wheel(void){ int w = m_wheel; m_wheel = 0; return w; }
static int m_outside;
int  mouse_outside(void){ return m_outside; }
static int (*g_hook)(int);
void keyboard_set_mouse_hook(int (*fn)(int)){ g_hook = fn; }

static int g_slot;
static uint32_t g_ids[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
int      task_current(void){ return g_slot; }
uint32_t task_id(int s){ return g_ids[s]; }

#include "wm_stubs.inc"          /* v0.9.8: the window manager, off */
#include "../shell/uimouse.c"

static int total, fails;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){ fails++; printf("  FAIL %s (got %ld)\n", what, got); }
    else printf("  ok   %s\n", what);
}

static void text(int x, int y, const char *s){ for(; *s; s++, x++) scr[y * W + x] = (uint16_t)(0x0700 | (uint8_t)*s); }
static void blank(void){ int i; for(i = 0; i < W * H; i++) scr[i] = 0x0720; }

static int hk(const char *line, int col){ return um_hint_key(line, (int)strlen(line), col); }

/* drain what the hook has queued for the app */
static int drain(int *out, int max)
{
    int n = 0, k;
    while(n < max && (k = um_hook(-1)) != -1) out[n++] = k;
    return n;
}

int main(void)
{
    int out[64], n;
    const char *f;

    printf("uimouse: the mouse in every app\n");
    um_init();
    ck("um_init installs the keyboard hook and the clear hook", g_hook == um_hook && g_clear != 0, 0);

    /* ── hints ─────────────────────────────────────────────────────── */
    head("a key hint is the key it names");
    f = "Up/Down choose  Enter apply  Left back";
    ck("\"Enter apply\": Enter", hk(f, 17) == '\n' && hk(f, 22) == '\n', hk(f, 17));
    ck("\"Left back\": Left", hk(f, 31) == KEY_LEFT, hk(f, 31));
    ck("\"Up/Down choose\": opposites, no single key", hk(f, 2) == 0, hk(f, 2));
    ck("between hints: nothing", hk(f, 14) == 0 && hk(f, 15) == 0, hk(f, 14));
    f = "P password  A role  D delete  H hide  L auto-login  ESC back";
    ck("\"P password\": p (the letter apps compare)", hk(f, 0) == 'p', hk(f, 0));
    ck("\"L auto-login\": l", hk(f, 39) == 'l', hk(f, 39));
    ck("\"ESC back\": ESC", hk(f, 53) == 27, hk(f, 53));
    f = "Enter/D desktop icon  F favourite  Left back";
    ck("\"Enter/D desktop icon\": the first alternative, Enter", hk(f, 3) == '\n', hk(f, 3));
    f = "Tab=complete  Up/Dn=history  ^R=search";
    ck("\"Tab=complete\": Tab", hk(f, 2) == '\t', hk(f, 2));
    ck("\"Up/Dn=history\": opposites, nothing", hk(f, 16) == 0, hk(f, 16));
    ck("\"^R=search\": Ctrl+R", hk(f, 31) == 0x12, hk(f, 31));
    f = "[Q] quit  [Space] pause";
    ck("\"[Q] quit\": q", hk(f, 1) == 'q', hk(f, 1));
    ck("\"[Space] pause\": space", hk(f, 13) == ' ', hk(f, 13));
    f = " -- more -- SPACE continues, Q stops -- ";
    ck("the pager: \"SPACE continues,\" is Space", hk(f, 13) == ' ', hk(f, 13));
    f = "Talk is cheap. Show me the code.";
    ck("ordinary text is not a hint", hk(f, 1) == 0 && hk(f, 16) == 0, hk(f, 1));
    f = "Enter";
    ck("a key with no words after it is not a hint", hk(f, 1) == 0, hk(f, 1));
    f = "Kyan@havendos:~# res fit";
    ck("a prompt is not a hint", hk(f, 3) == 0 && hk(f, 18) == 0, hk(f, 3));

    /* ── rows ─────────────────────────────────────────────────────── */
    head("a click on a row is the arrows that reach it");
    blank(); um_frame();
    um_row(0, 10, 5, 30, 0); um_row(0, 10, 6, 30, 1); um_row(0, 10, 7, 30, 0); um_row(0, 10, 9, 30, 0);
    n = um_click_keys(15, 9, 0, 0, out, 64);
    ck("selected is row 2, click row 4: Down, Down", n == 2 && out[0] == KEY_DOWN && out[1] == KEY_DOWN, n);
    n = um_click_keys(15, 5, 0, 0, out, 64);
    ck("click row 1: Up", n == 1 && out[0] == KEY_UP, n);
    n = um_click_keys(15, 6, 0, 0, out, 64);
    ck("click the selected row: Enter -- the second click of a double", n == 1 && out[0] == '\n', n);
    n = um_click_keys(15, 8, 0, 0, out, 64);
    ck("the gap between rows: nothing", n == 0, n);
    n = um_click_keys(45, 6, 0, 0, out, 64);
    ck("right of the row: nothing", n == 0, n);
    um_row(0, 10, 6, 30, 0); um_row(0, 10, 7, 30, 1);
    n = um_click_keys(15, 5, 0, 0, out, 64);
    ck("redrawn with the selection moved: the same rows, counted again", n == 2 && out[0] == KEY_UP, n);
    {   int k = '\n'; um_group_prefix(0, &k, 1); }
    n = um_click_keys(15, 9, 0, 0, out, 64);
    ck("a prefix first (Settings with focus on its sidebar): Enter, Down", n == 2 && out[0] == '\n' && out[1] == KEY_DOWN, n);
    n = um_click_keys(15, 7, 0, 0, out, 64);
    ck("  ... and on the selected row only the prefix: no Enter", n == 1 && out[0] == '\n', n);
    um_frame();
    um_row(1, 0, 3, 10, 0); um_row(1, 0, 4, 10, 0);
    n = um_click_keys(2, 4, 0, 0, out, 64);
    ck("no row selected: nothing to count from, nothing sent", n == 0, n);

    head("a button is its keys");
    um_frame();
    {   int seq[3] = { 27, KEY_DOWN, '\n' }; um_keys(0, 2, 12, 1, seq, 3); }
    um_key(30, 20, 1, 1, 'y');
    n = um_click_keys(5, 2, 0, 0, out, 64);
    ck("a sidebar section: ESC, Down, Enter", n == 3 && out[0] == 27 && out[2] == '\n', n);
    n = um_click_keys(30, 20, 0, 0, out, 64);
    ck("a Y button: y", n == 1 && out[0] == 'y', n);
    um_key(0, 2, 12, 1, 'x');
    n = um_click_keys(5, 2, 0, 0, out, 64);
    ck("drawn again at the same place: replaced, not stacked", n == 1 && out[0] == 'x', n);

    /* ── the hook ─────────────────────────────────────────────────── */
    head("the hook, end to end");
    blank(); um_frame();
    text(1, 24, "Up/Down choose  Enter apply  ESC close");
    um_row(0, 10, 5, 30, 1); um_row(0, 10, 6, 30, 0);
    m_x = 12; m_y = 6; m_clicks = 1;
    n = drain(out, 64);
    ck("a click on the second row: Down", n == 1 && out[0] == KEY_DOWN, n);
    m_x = 18; m_y = 24; m_clicks = 1;
    n = drain(out, 64);
    ck("a click on \"Enter apply\" in the bottom bar: Enter", n == 1 && out[0] == '\n', n);
    m_x = 18; m_y = 10; m_clicks = 0;
    text(16, 10, "Enter apply");
    m_clicks = 1; n = drain(out, 64);
    ck("the same words in the middle of the screen: nothing (bottom two rows only)", n == 0, n);
    m_clicks = 2; n = drain(out, 64);
    ck("right-click: ESC", n == 1 && out[0] == 27, n);
    m_wheel = -3; n = drain(out, 64);
    ck("three notches toward you: Down x3", n == 3 && out[0] == KEY_DOWN && out[2] == KEY_DOWN, n);
    m_wheel = 9; n = drain(out, 64);
    ck("a wheel spun hard: five Up at most", n == 5 && out[4] == KEY_UP, n);
    ck("a real key passes straight through", um_hook('a') == 'a', 0);

    head("the pointer leaves nothing behind");
    blank(); text(0, 3, "abc");
    m_x = 1; m_y = 3; m_clicks = 0;
    um_hook(-1);
    ck("drawn on 'b' (grey on black): the letter stays, black on grey",
       scr[3 * W + 1] == 0x7062, scr[3 * W + 1]);
    m_x = 2; um_hook(-1);
    ck("moved: 'b' is back exactly", scr[3 * W + 1] == 0x0762 && scr[3 * W + 2] == 0x7063, scr[3 * W + 1]);
    um_hook('k');
    scr[3 * W + 2] = 0x3063;          /* black on cyan: Settings' selection bar */
    um_hook(-1);
    ck("on a cyan selection bar it is cyan on black -- visible",
       scr[3 * W + 2] == 0x0363, scr[3 * W + 2]);
    um_hook('k'); scr[3 * W + 2] = 0x0763;
    um_hook(-1);
    ck("a key leaving hides it: 'c' is back", um_hook('k') == 'k' && scr[3 * W + 2] == 0x0763, scr[3 * W + 2]);
    um_hook(-1);
    scr[3 * W + 2] = 0x0E5A;          /* the app draws a yellow Z over the pointer */
    m_x = 0; um_hook(-1);
    ck("an app that drew over the pointer keeps its drawing", scr[3 * W + 2] == 0x0E5A, scr[3 * W + 2]);
    um_hook('k');
    m_x = 0; m_y = 24; blank();
    text(0, 24, "Q quit");
    um_hook(-1);                       /* the pointer sits on the Q */
    m_clicks = 1; n = drain(out, 64);
    ck("a click on a hint under the pointer reads the hint, not the pointer", n == 1 && out[0] == 'q', n);

    head("modes");
    um_set_mode(0);
    m_clicks = 1; m_wheel = 2; n = drain(out, 64);
    ck("mode 0 (the desktop, Mines, Chat): nothing, and nothing consumed", n == 0 && m_clicks == 1 && m_wheel == 2, n);
    m_clicks = 0; m_wheel = 0;
    um_set_mode(UM_HINTS | UM_RIGHT);
    m_wheel = 2; m_clicks = 2; n = drain(out, 64);
    ck("a game: right-click yes, wheel no", n == 1 && out[0] == 27 && m_wheel == 2, n);
    m_wheel = 0;
    um_set_mode(UM_WHEEL | UM_POINTER);
    m_clicks = 1; n = drain(out, 64);
    ck("wheel and pointer only: a click is left for the app to read", n == 0 && m_clicks == 1, m_clicks);
    m_clicks = 0;
    um_set_mode(UM_ALL);
    m_ready = 0; m_clicks = 1; n = drain(out, 64);
    ck("no mouse at all: nothing", n == 0, n);
    m_ready = 1; m_clicks = 0;

    head("v0.9.6.5: outside the app's box, the window's");
    blank(); um_frame(); um_row(0, 10, 5, 30, 1); um_row(0, 10, 6, 30, 0);
    m_x = 12; m_y = 6; um_hook(-1);
    ck("inside the box: the pointer is drawn", scr[6 * W + 12] != 0x0720, scr[6 * W + 12]);
    m_outside = 1; m_clicks = 1; m_wheel = -1;
    n = drain(out, 64);
    ck("outside it: the pointer is hidden, nothing is sent, nothing consumed",
       n == 0 && scr[6 * W + 12] == 0x0720 && m_clicks == 1 && m_wheel == -1, n);
    m_outside = 0; m_clicks = 0; m_wheel = 0;

    head("each task has its own table");
    g_slot = 1; um_frame(); um_row(0, 0, 4, 10, 1); um_row(0, 0, 5, 10, 0);
    g_slot = 2; um_frame(); um_set_mode(0);
    g_slot = 1;
    n = um_click_keys(3, 5, 0, 0, out, 64);
    ck("a task behind clearing ITS table leaves the front's rows", n == 1 && out[0] == KEY_DOWN, n);
    ck("  ... and its mode is its own", um_mode() == UM_ALL, um_mode());
    g_ids[1] = 77;                     /* slot 1 reused by a new task */
    n = um_click_keys(3, 5, 0, 0, out, 64);
    ck("a slot reused by a new task starts empty, with every behaviour on",
       n == 0 && um_mode() == UM_ALL, n);
    g_slot = 1; um_row(0, 0, 4, 10, 1); um_row(0, 0, 5, 10, 0);
    g_clear();
    n = um_click_keys(3, 5, 0, 0, out, 64);
    ck("vga_clear() forgets the screen's rows", n == 0, n);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
