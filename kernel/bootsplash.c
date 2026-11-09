/* ════════════════════════════════════════════════════════════════════
   kernel/bootsplash.c -- the loading screen and its ring.  v0.8.8.4.

   include/bootsplash.h says why the ring is driven by klog() rather
   than by a timer, and why there is no percentage.

   ── THE RING, WITHOUT FLOATING POINT ────────────────────────────────

   Twelve positions on an ellipse, as a table of integer offsets.
   There is no FPU set up in this kernel and no soft-float linked, so
   cos() and sin() are not available and would not be wanted in the
   boot path if they were.

   IT IS AN ELLIPSE AND NOT A CIRCLE ON PURPOSE. A text cell is about
   twice as tall as it is wide, so a ring with equal offsets in x and
   y draws as a tall oval. The x offsets are twice the y offsets, which
   is what makes it look round on screen. The same correction is the
   whole of what fbc_fit() got wrong in v0.8.8 -- a 2:1 glyph stretch
   that nobody noticed until a screendump was looked at properly.

   ── WHY THE HEAD MOVES ONE STEP PER LINE ────────────────────────────

   One klog line, one position. Twelve positions is a full turn, so
   the ring goes round roughly every twelve things the kernel does.
   A boot emits enough lines for several turns, and a boot that stalls
   leaves it stopped -- which is the signal, not a defect in the
   animation.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/klog.h"
#include "../include/bootsplash.h"
#include "version.h"

extern int  keyboard_haschar(void);
extern int  keyboard_getchar(void);

/* ── the ring ────────────────────────────────────────────────────────
   cos/sin at 22.5 degree steps, scaled to rx=8 ry=4 and rounded.
   Clockwise from the right-hand side, because that is the direction
   every loading ring a person has seen turns. */
#define RING_N 12
static const signed char ring_dx[RING_N] = {
     6,  5,  3,  0, -3, -5, -6, -5, -3,  0,  3,  5
};
static const signed char ring_dy[RING_N] = {
     0,  2,  3,  3,  3,  2,  0, -2, -3, -3, -3, -2
};

/* The comet: the head is brightest and three positions trail off. Any
   more than that and twelve positions read as a solid ring that
   happens to flicker, which is not a direction of travel. */
#define TAIL_N 4
static const vga_color_t tail_col[TAIL_N] = {
    VGA_WHITE, VGA_LIGHT_CYAN, VGA_CYAN, VGA_DARK_GREY
};
static const char tail_ch[TAIL_N] = { 'O', 'o', 'o', '.' };

static int      g_active = 0;
static uint32_t g_begin_ms = 0;
static int      g_head   = 0;
static int      g_lines  = 0;
static char     g_cap[88];
static int      g_cap_lvl = KLOG_OK;

static void cls(void)
{
    int x, y;
    for(y = 0; y < vga_rows(); y++)
        for(x = 0; x < vga_cols(); x++)
            vga_putchar_at(' ', x, y, VGA_BLACK, VGA_BLACK);
}

static void centre(const char *s, int y, vga_color_t fg)
{
    int len = 0, x;
    while(s[len]) len++;
    x = (vga_cols() - len) / 2;
    if(x < 0) x = 0;
    vga_puts_at(s, x, y, fg, VGA_BLACK);
}

/* ── one frame ───────────────────────────────────────────────────────
   Everything is positioned from vga_cols()/vga_rows() rather than
   from 80x25. The console can be any grid since v0.8.8 and a splash
   nailed to 80x25 would sit in the top-left corner of a 1920x1080
   panel, which is the exact complaint this release set out to fix. */
/* ── the fixed furniture ─────────────────────────────────────────────
   Drawn once. The ring's box is the only thing that repaints, so the
   wordmark cannot flicker and cannot be painted over by a tail
   position -- which is what happened when the two overlapped. */
static int g_ring_y;      /* ring centre row   */
static int g_cap_y;       /* caption row       */

static void layout(void)
{
    int rows = vga_rows();
    int cy   = rows / 2;

    g_ring_y = cy + 2;
    g_cap_y  = g_ring_y + 5;

    /* On a short grid the caption matters more than the spacing: it is
       the last thing a wedged kernel will have said. */
    if(g_cap_y >= rows){
        g_cap_y  = rows - 1;
        g_ring_y = g_cap_y - 5;
    }
    if(g_ring_y < 4) g_ring_y = 4;
}

static void draw_furniture(void)
{
    int cy = g_ring_y - 2;
    centre(HAVENDOS_NAME,                      cy - 4, VGA_LIGHT_CYAN);
    centre(HAVENDOS_VER " " HAVENDOS_CHANNEL,  cy - 3, VGA_WHITE);
    centre("\"" HAVENDOS_CODENAME "\"",        cy - 2, VGA_DARK_GREY);
#if HAVENDOS_PRERELEASE
    /* v0.8.8.8. On the splash as well as after it, because the splash
       is the one screen that is up for the whole of init and the one
       somebody photographs when something goes wrong. */
    centre(HAVENDOS_EA_LINE1, g_ring_y + 4, VGA_YELLOW);
#endif
}

/* ── one frame ───────────────────────────────────────────────────────
   Everything is positioned from vga_cols()/vga_rows() rather than
   from 80x25. The console can be any grid since v0.8.8 and a splash
   nailed to 80x25 would sit in the top-left corner of a 1920x1080
   panel, which is the exact complaint this release set out to fix. */
static void draw(void)
{
    int cx = vga_cols() / 2;
    int i, x, y;

    /* Clear the ring's own box and nothing else. */
    for(y = g_ring_y - 3; y <= g_ring_y + 3; y++)
        for(x = cx - 7; x <= cx + 7; x++)
            if(x >= 0 && x < vga_cols() && y >= 0 && y < vga_rows())
                vga_putchar_at(' ', x, y, VGA_BLACK, VGA_BLACK);

    /* ── THE WHOLE RING FIRST, DIM ────────────────────────────────────
       Looked at on a real boot, a four-position comet on its own did
       not read as a circle at all: four dots in the top-right of an
       empty space, with nothing to say they were part of anything.
       The dim ring is the track; the comet is the thing moving along
       it. That is the difference between a loading circle and a few
       characters that happen to change. */
    for(i = 0; i < RING_N; i++){
        x = cx + ring_dx[i];
        y = g_ring_y + ring_dy[i];
        if(x >= 0 && x < vga_cols() && y >= 0 && y < vga_rows())
            vga_putchar_at('.', x, y, VGA_DARK_GREY, VGA_BLACK);
    }

    /* ── then the comet, over the top ─────────────────────────────────
       Drawn head LAST so that where positions round to the same cell
       on a narrow grid, the brightest one wins rather than being
       buried under its own tail. */
    for(i = TAIL_N - 1; i >= 0; i--){
        int k = ((g_head - i) % RING_N + RING_N) % RING_N;
        x = cx + ring_dx[k];
        y = g_ring_y + ring_dy[k];
        if(x >= 0 && x < vga_cols() && y >= 0 && y < vga_rows())
            vga_putchar_at(tail_ch[i], x, y, tail_col[i], VGA_BLACK);
    }

    /* ── THE CAPTION IS THE LAST LOG LINE, IN FULL ────────────────────
       This is the part that keeps klog.h's promise. A kernel that
       wedges during init leaves the name of the step it wedged in on
       the screen. Truncating it to something tidy would throw away
       exactly the detail that matters when it is the last thing a
       machine ever said. */
    {
        vga_color_t fg = (g_cap_lvl == KLOG_ERR)  ? VGA_LIGHT_RED  :
                         (g_cap_lvl == KLOG_WARN) ? VGA_YELLOW     :
                                                    VGA_LIGHT_GREY;
        for(x = 0; x < vga_cols(); x++)
            vga_putchar_at(' ', x, g_cap_y, VGA_BLACK, VGA_BLACK);
        centre(g_cap, g_cap_y, fg);
    }

    /* The way out, said once it is likely to be read rather than on
       the first frame, when it would flash past. */
    if(g_lines > 8){
        y = vga_rows() - 2;
        if(y > g_cap_y)
            centre("press any key for the boot log", y, VGA_DARK_GREY);
    }

    vga_flush();
}

void bootsplash_begin(void)
{
    g_active  = 1;
    g_head    = 0;
    g_lines   = 0;
    g_cap[0]  = 0;
    g_cap_lvl = KLOG_OK;
    klog_set_echo(0);

    /* ── THROW AWAY WHAT GRUB LEFT BEHIND ─────────────────────────────
       FOUND BY BOOTING IT. The ENTER that chose a GRUB entry -- and
       its release code, which arrives a moment later -- are still in
       the 8042's buffer when this runs, because interrupts were only
       enabled a few instructions ago. Without this drain, the first
       log line saw a pending key, concluded the person had asked for
       the boot log, and dropped the splash before anyone had seen it.
       The loading screen appeared to work and then never showed up.

       Same shape as the crash-recovery window being dismissed by the
       dead app's unread ENTER, found in the same release. A keystroke
       that was meant for something else is not an answer to a
       question that had not been asked yet. */
    while(keyboard_haschar()) (void)keyboard_getchar();
    g_begin_ms = klog_now_ms();

    /* Lines come here from now on rather than scrolling. klog.h says
       why this is a pointer it installs rather than a call klog makes
       by name. */
    klog_set_sink(bootsplash_note);

    cls();
    layout();
    draw_furniture();
    draw();
}

int bootsplash_active(void){ return g_active; }

void bootsplash_note(int level, const char *text)
{
    int i = 0;

    if(!g_active) return;

    /* ── THE KEY THAT DROPS IT ────────────────────────────────────────
       Checked here because this is the only place that runs
       repeatedly during init -- there is no loop in the boot path to
       put a poll in. One check per log line is plenty: a person
       holding a key down gets the log within a few milliseconds.

       keyboard_haschar() and not keyboard_getchar(), so the key that
       asked for the log is consumed deliberately below rather than
       being left to dismiss whatever comes next -- which is the bug
       the crash-recovery window had, where the dead app's unread
       ENTER closed the notice before it could be read. */
    /* The drain at begin() cannot catch a release code that has not
       arrived yet, so for the first quarter second anything in the
       buffer is still GRUB's and is thrown away rather than acted on.
       A person reaching for a key takes longer than that. */
    if(klog_now_ms() - g_begin_ms < 250u){
        while(keyboard_haschar()) (void)keyboard_getchar();
    } else if(keyboard_haschar()){
        (void)keyboard_getchar();
        bootsplash_end(1);
        return;
    }

    while(text[i] && i < (int)sizeof g_cap - 1){ g_cap[i] = text[i]; i++; }
    g_cap[i]  = 0;
    g_cap_lvl = level;
    g_head    = (g_head + 1) % RING_N;
    g_lines++;
    draw();
}

void bootsplash_end(int replay)
{
    if(!g_active) return;
    g_active = 0;
    klog_set_sink(0);
    cls();
    vga_set_cursor(0, 0);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if(replay){
        /* Nothing is lost by turning the splash off: the ring buffer
           has every line, including the ones drawn as captions. */
        klog_set_echo(1);
        klog_dump();
    }
}
