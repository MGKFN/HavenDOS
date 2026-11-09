/* ════════════════════════════════════════════════════════════════════
   shell/mines.c -- Minefield.  v0.8.8.4.

   THE RULES ARE NOT IN HERE. shell/mines_model.c has them, calls
   nothing, and is linked straight into tests/test_mines.c with no
   seam and no simulated hardware -- 79 checks, and
   tools/check_mines_suite.py plants seventeen defects and the suite
   catches all seventeen. This file draws that and reads the keyboard
   and the mouse.

   ── WHY A NEW APP IS THIS ONE ───────────────────────────────────────

   It uses three things this release added and nothing else does:

     THE REAL GRID. The board is sized from vga_cols()/vga_rows(), so
     a bigger console is a bigger minefield. That makes it the first
     app in the registry with fits=1 besides the shell -- it needs no
     viewport, because it was not written for 80x25. A bigger screen
     is a reason to have a bigger screen.

     THE MOUSE. Left reveals, right flags, which is how everyone
     expects to play. v0.8.8.1's pointer work has had no app that
     depended on it; now one does.

     THE TICK. The main loop calls kernel_poll(), so the clock keeps
     time, DHCP keeps retrying and a stick plugged in mid-game is
     still noticed. shell/tetris.c does not, and a long game of it
     stops all three.

   ── THE CELL IS TWO COLUMNS WIDE ────────────────────────────────────

   A text cell is about twice as tall as it is wide, so a board drawn
   one column per cell is a board of tall thin rectangles -- the same
   aspect problem that made 0.8.8's glyphs look stretched until a
   screendump was looked at properly. Two columns per cell is roughly
   square, which is what a minefield has to be for the eye to read
   the neighbourhoods at all.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/mines.h"
#include "../include/keys.h"

extern int      keyboard_getchar(void);
extern int      keyboard_haschar(void);
extern uint32_t get_ticks(void);
extern void     sleep_ms(uint32_t);
extern int      keyboard_waitchar(void);
extern int      kernel_poll(void);
extern uint32_t entropy_u32(void);
extern void     mouse_get(int*,int*,int*);
extern int      mouse_is_ready(void);
extern int      mouse_get_clicks(void);
extern void     ui_draw_taskbar(const char*);

/* 28 KB. In bss and not on the stack: an app's frame lives on the
   kernel's 256 KB stack, which boot/boot.S reserves and which
   `crash stack` demonstrates the end of. */
static mines_t  G;

#define CELL_W 2            /* two columns per cell -- see the header */
#define TOP    3            /* first board row                       */
#define LEFT   1

/* How much of the console the board may take. Three rows of chrome at
   the top and two at the bottom. */
static int board_cols(void){
    int c = (vga_cols() - LEFT*2) / CELL_W;
    if(c > MINES_MAX_W) c = MINES_MAX_W;
    return c;
}
static int board_rows(void){
    int r = vga_rows() - TOP - 2;
    if(r > MINES_MAX_H) r = MINES_MAX_H;
    return r;
}

/* ── difficulty, as a density rather than a table ─────────────────────
   A table of board sizes would be meaningless here: the board is
   whatever the console can hold, and that is not knowable when the
   table is written. A density scales with it. */
static const struct { const char *name; int pct; } levels[] = {
    { "Gentle",  10 },
    { "Normal",  16 },
    { "Nasty",   22 },
};
#define NLEVELS 3
static int level = 1;

static void putu(uint32_t v, int x, int y, vga_color_t fg){
    char b[12]; int n = 0;
    if(!v) b[n++] = '0';
    while(v){ b[n++] = (char)('0' + v % 10); v /= 10; }
    while(n) vga_putchar_at(b[--n], x++, y, fg, VGA_BLACK);
}

static void puti(int v, int x, int y, vga_color_t fg){
    if(v < 0){ vga_putchar_at('-', x++, y, fg, VGA_BLACK); v = -v; }
    putu((uint32_t)v, x, y, fg);
}

/* The classic colours. Not decoration: the number's colour is how a
   player reads a neighbourhood at a glance instead of one cell at a
   time. */
static vga_color_t num_col(int n){
    switch(n){
    case 1:  return VGA_LIGHT_BLUE;
    case 2:  return VGA_LIGHT_GREEN;
    case 3:  return VGA_LIGHT_RED;
    case 4:  return VGA_BLUE;
    case 5:  return VGA_RED;
    case 6:  return VGA_CYAN;
    case 7:  return VGA_WHITE;
    default: return VGA_DARK_GREY;
    }
}

static int cur_x = 0, cur_y = 0;
static uint32_t started_ticks = 0;

static void draw_cell(int x, int y)
{
    int sx = LEFT + x*CELL_W, sy = TOP + y;
    int sel = (x == cur_x && y == cur_y);
    vga_color_t bg = sel ? VGA_BLUE : VGA_BLACK;
    char ch = ' ';
    vga_color_t fg = VGA_LIGHT_GREY;

    if(G.flag[y][x]){
        ch = 'F'; fg = VGA_LIGHT_RED;
        if(!G.shown[y][x] && !sel) bg = VGA_DARK_GREY;
    } else if(!G.shown[y][x]){
        /* A shut cell is a raised block, which is the only thing that
           makes "shut" and "empty" tell apart at a glance. */
        ch = '#'; fg = VGA_DARK_GREY;
    } else if(G.mine[y][x]){
        ch = '*';
        fg = VGA_LIGHT_RED;
        /* The one that was stepped on is marked differently from the
           ones merely revealed at the end, because "which one got me"
           is the first thing a player looks for. */
        if(x == G.boom_x && y == G.boom_y){ bg = VGA_RED; fg = VGA_WHITE; }
    } else {
        int n = mines_neighbours(&G, x, y);
        if(n){ ch = (char)('0' + n); fg = num_col(n); }
        else { ch = '.'; fg = VGA_DARK_GREY; }
    }

    vga_putchar_at(ch,  sx,   sy, fg, bg);
    vga_putchar_at(' ', sx+1, sy, fg, bg);
}

static void draw_status(void)
{
    int y = TOP + G.h;
    int x;
    uint32_t secs = (get_ticks() - started_ticks) / 100u;   /* 10 ms units */

    for(x = 0; x < vga_cols(); x++)
        vga_putchar_at(' ', x, y, VGA_WHITE, VGA_BLACK);

    vga_puts_at("Mines ", LEFT, y, VGA_LIGHT_GREY, VGA_BLACK);
    puti(mines_remaining(&G), LEFT+6, y,
         mines_remaining(&G) < 0 ? VGA_LIGHT_RED : VGA_WHITE);

    vga_puts_at("Time ", LEFT+14, y, VGA_LIGHT_GREY, VGA_BLACK);
    putu(secs, LEFT+19, y, VGA_WHITE);

    vga_puts_at(levels[level].name, LEFT+28, y, VGA_LIGHT_CYAN, VGA_BLACK);

    if(G.state == MINES_WON)
        vga_puts_at("CLEARED", LEFT+40, y, VGA_LIGHT_GREEN, VGA_BLACK);
    else if(G.state == MINES_LOST)
        vga_puts_at("BOOM", LEFT+40, y, VGA_LIGHT_RED, VGA_BLACK);
}

static void draw_hints(void)
{
    int y = vga_rows() - 1;
    int x;
    for(x = 0; x < vga_cols(); x++)
        vga_putchar_at(' ', x, y, VGA_DARK_GREY, VGA_BLACK);
    /* ── BOTH OF THESE FIT 80 COLUMNS WITH THE LEFT MARGIN ───────────
       The mouse line was one character too long and the last word
       read "ESC=qui" on a real boot. tools/check_screen_width.py did
       not catch it: the x here is the computed LEFT, and that checker
       reports it is not checking 232 such sites. So the fix is to
       drop "Space=open" from the mouse line, where it is redundant
       beside "Click=open" anyway, rather than to trim a word and
       leave it one character from wrong again. */
    vga_puts_at(mouse_is_ready()
                ? "Click=open  Right-click=flag  F=flag  R=restart  "
                  "1-3=level  ESC=quit"
                : "Arrows=move  Space=open  F=flag  R=restart  "
                  "1-3=level  ESC=quit",
                LEFT, y, VGA_DARK_GREY, VGA_BLACK);
}

static void draw_all(void)
{
    int x, y;
    vga_clear();
    ui_draw_taskbar("Minefield");
    for(y = 0; y < G.h; y++)
        for(x = 0; x < G.w; x++) draw_cell(x, y);
    draw_status();
    draw_hints();
    vga_flush();
}

static void new_game(void)
{
    int w = board_cols(), h = board_rows();
    int n;

    if(w < 4) w = 4;
    if(h < 4) h = 4;
    n = w*h*levels[level].pct / 100;
    if(n < 1) n = 1;
    if(n > w*h - 9) n = w*h - 9;        /* the first click's nine cells */

    /* entropy_u32() may legitimately return 0, and the model replaces
       a zero seed because 0 is xorshift's dead state -- see
       tests/test_mines.c, which checks exactly that. */
    if(!mines_begin(&G, w, h, n, entropy_u32())){
        /* Should not happen: w and h are clamped above and n is
           clamped to the model's own bound. Said out loud rather than
           drawing an empty board, because a board that silently did
           not start is indistinguishable from one with no mines. */
        vga_clear();
        vga_puts_at("Minefield: this console is too small for a board.",
                    1, 1, VGA_YELLOW, VGA_BLACK);
        vga_puts_at("Press any key.", 1, 3, VGA_LIGHT_GREY, VGA_BLACK);
        vga_flush();
        /* keyboard_waitchar() and not a kernel_poll() loop of our
           own: v0.8.8 put the poll inside that function's wait loop
           precisely so that a screen waiting for a person keeps the
           clock, the DHCP retries and hotplug running, and
           tools/check_kpoll_sites.py keeps the call in the two files
           that are allowed to make it. Writing the loop here would be
           a second copy of a decision that already has one home. */
        (void)keyboard_waitchar();
        G.w = 0;
        return;
    }
    cur_x = G.w/2; cur_y = G.h/2;
    started_ticks = get_ticks();
}

static void finish(void)
{
    if(G.state == MINES_LOST) mines_reveal_all(&G);
    draw_all();
}

void app_mines(void)
{
    int last_secs = -1;

    new_game();
    if(G.w == 0) return;
    draw_all();

    while(1){
        int moved = 0;

        /* The periodic work. Marked rather than hidden: include/kpoll.h
           argues that kernel_poll() may run only where nothing is in
           progress, and the top of a game's event loop is exactly
           that -- no transfer is open, no buffer is half built. */
        kernel_poll();  /* kpoll-site-ok: the top of a game's event loop, which is the long-running screen the clock, the DHCP retries and hotplug all have to survive */

        /* ── THE IDLE PATH ────────────────────────────────────────────
           kernel_poll() every time round, so a long game does not
           stop the clock, the DHCP retries or hotplug. shell/tetris.c
           has no such call and a long game of it stops all three --
           which is what "optimise everything to use the polling"
           meant. */
        /* The clock in the status line, once a second, without
           repainting the board. */
        {
            int secs = (int)((get_ticks() - started_ticks) / 100u);
            if(secs != last_secs && G.state == MINES_PLAYING){
                last_secs = secs;
                draw_status();
                vga_flush();
            }
        }

        /* ── THE MOUSE ────────────────────────────────────────────────
           Read before the keyboard so a click is not delayed behind a
           held key. mouse_get() answers in the LOGICAL grid -- v0.8.8.4
           made the clamp follow the viewport -- and this app has no
           viewport, so its coordinates are the console's. */
        if(mouse_is_ready()){
            int mx, my, mb, clicks = mouse_get_clicks();
            mouse_get(&mx, &my, &mb);
            if(clicks){
                int bx = (mx - LEFT) / CELL_W;
                int by = my - TOP;
                if(bx >= 0 && bx < G.w && by >= 0 && by < G.h){
                    int ox = cur_x, oy = cur_y;
                    cur_x = bx; cur_y = by;
                    if(G.state == MINES_PLAYING){
                        if(clicks & 2) mines_toggle_flag(&G, bx, by);
                        else           mines_reveal(&G, bx, by);
                    }
                    if(G.state != MINES_PLAYING){ finish(); }
                    else {
                        draw_cell(ox, oy);
                        draw_all();
                    }
                    continue;
                }
            }
        }

        /* ── THE IDLE PATH ────────────────────────────────────────────
           sleep_ms() polls the PIT and nothing else, which is what a
           loop that also redraws a clock and reads a mouse needs: it
           has to come back. The periodic work is the kernel_poll()
           above.

           An earlier version of this loop had no poll of its own and
           a comment claiming the work ran from keyboard_waitchar().
           It did not: this loop never calls that function, because it
           cannot block. The comment was wrong in the one direction
           that matters -- it described the behaviour the code was
           supposed to have. shell/tetris.c really does stop the
           clock, the DHCP retries and hotplug for the length of a
           game, and a comment is not what stops this one doing the
           same. */
        if(!keyboard_haschar()){ sleep_ms(10); continue; }

        {
            int c = keyboard_getchar();
            int ox = cur_x, oy = cur_y;

            if(c == 27) return;                       /* ESC */
            if(c == KEY_SUPER) continue;              /* not ours */

            if(c == KEY_LEFT  && cur_x > 0)      { cur_x--; moved = 1; }
            else if(c == KEY_RIGHT && cur_x < G.w-1){ cur_x++; moved = 1; }
            else if(c == KEY_UP    && cur_y > 0)    { cur_y--; moved = 1; }
            else if(c == KEY_DOWN  && cur_y < G.h-1){ cur_y++; moved = 1; }
            else if(c == 'r' || c == 'R'){
                new_game();
                if(G.w == 0) return;
                last_secs = -1;
                draw_all();
                continue;
            }
            else if(c >= '1' && c <= '0' + NLEVELS){
                level = c - '1';
                new_game();
                if(G.w == 0) return;
                last_secs = -1;
                draw_all();
                continue;
            }
            else if(c == 'f' || c == 'F'){
                if(mines_toggle_flag(&G, cur_x, cur_y)){
                    draw_cell(cur_x, cur_y);
                    draw_status();
                    vga_flush();
                }
                continue;
            }
            else if(c == ' ' || c == '\n' || c == '\r'){
                if(G.state == MINES_PLAYING){
                    mines_reveal(&G, cur_x, cur_y);
                    if(G.state != MINES_PLAYING) finish();
                    else                         draw_all();
                }
                continue;
            }

            if(moved){
                /* Two cells, not the whole board: a cursor that
                   repainted 9600 cells per keypress would feel like a
                   machine thinking about it. */
                draw_cell(ox, oy);
                draw_cell(cur_x, cur_y);
                vga_flush();
            }
        }
    }
}
