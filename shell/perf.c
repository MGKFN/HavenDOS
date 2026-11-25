/* shell/perf.c -- `perf`: how fast the screen is drawn (v0.9.10).
 *
 * Kyan, 25 Nov: "it still an bit slugish but maybe bc we dont use GPU".
 * Whether it is, and what, is a number now:
 *
 *   perf          what the window manager drew since the last `perf` (or
 *                 the boot): how many draws, how long in total, the
 *                 slowest one, glyphs drawn, pixels copied. Use the
 *                 desktop, then type `perf` again.
 *   perf bench    a fixed set, timed: the whole screen drawn, this window
 *                 moved, 200 lines scrolled, glyphs. The same on two
 *                 machines or two versions is a comparison.
 *
 * Time is the processor's own counter (rdtsc), calibrated against the
 * 100 Hz timer the first time it is needed (a tenth of a second).
 */
#include "../include/vga.h"
#include "../include/wm.h"
#include "../include/pit.h"
#include "../include/string.h"

extern void vga_wm_stats(uint32_t *calls, uint64_t *cyc, uint64_t *worst, uint32_t *glyphs,
                         uint32_t *px, uint32_t *scrolls, int reset);
extern int  task_current(void);
extern void kpoll_sleep_ms(uint32_t ms);

static uint64_t rdtsc(void){ uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static uint32_t g_khz;              /* processor cycles per millisecond */

uint32_t perf_cycles_per_ms(void)
{
    if (g_khz) return g_khz;
    uint32_t t = get_ticks();
    while (get_ticks() == t) __asm__ volatile("pause");
    t = get_ticks();
    uint64_t a = rdtsc();
    while (get_ticks() - t < 10u) __asm__ volatile("pause");       /* 100 ms */
    uint64_t c = rdtsc() - a;
    g_khz = (uint32_t)(c / 100u);
    if (!g_khz) g_khz = 1;
    return g_khz;
}
/* cycles as "12.34 ms" */
static void put_ms(uint64_t cyc)
{
    uint32_t k = perf_cycles_per_ms();
    uint64_t hund = cyc * 100u / k;
    vga_printf("%u.%02u ms", (unsigned)(hund / 100u), (unsigned)(hund % 100u));
}

static void report(const char *what)
{
    uint32_t calls, glyphs, px, scrolls; uint64_t cyc, worst;
    vga_wm_stats(&calls, &cyc, &worst, &glyphs, &px, &scrolls, 1);
    vga_printf("%s\n", what);
    vga_printf("  draws          %u\n", calls);
    vga_puts("  time drawing   "); put_ms(cyc); vga_putchar('\n');
    vga_puts("  slowest draw   "); put_ms(worst); vga_putchar('\n');
    if (calls) { vga_puts("  average draw   "); put_ms(cyc / calls); vga_putchar('\n'); }
    vga_printf("  glyphs drawn   %u\n", glyphs);
    vga_printf("  pixels copied  %u (%u MB)\n", px, (unsigned)((uint64_t)px * 4u / 1048576u));
    vga_printf("  scrolls moved  %u (a line scrolled by moving pixels)\n", scrolls);
}

static void bench(void)
{
    int s = task_current(), x, y, w, h;
    uint64_t t;
    int sw = vga_screen_w(), sh = vga_screen_h();
    vga_printf("perf bench -- %dx%d, processor %u MHz\n", sw, sh, (unsigned)(perf_cycles_per_ms() / 1000u));
    vga_flush();
    /* 1. the whole screen, five times */
    vga_wm_stats(0, 0, 0, 0, 0, 0, 1);
    t = rdtsc();
    for (int i = 0; i < 5; i++) vga_wm_repaint_all();
    vga_puts("  whole screen drawn      "); put_ms((rdtsc() - t) / 5u); vga_puts(" each\n");
    /* 2. this window moved 100 px and back, ten times */
    if (vga_wm_is_open(s)) {
        vga_wm_get(s, &x, &y, &w, &h);
        t = rdtsc();
        for (int i = 0; i < 10; i++) { vga_wm_move(s, x + 100, y); vga_wm_move(s, x, y); }
        vga_puts("  this window moved       "); put_ms((rdtsc() - t) / 20u); vga_puts(" a move\n");
    }
    vga_flush();
    /* 3. 200 lines through the Terminal */
    {
        uint32_t sc0, sc1;
        vga_wm_stats(0, 0, 0, 0, 0, &sc0, 0);
        t = rdtsc();
        for (int i = 0; i < 200; i++) vga_printf("perf bench line %d -- the quick brown fox jumps over the lazy dog\n", i);
        vga_flush();
        uint64_t d = rdtsc() - t;
        vga_wm_stats(0, 0, 0, 0, 0, &sc1, 0);
        vga_puts("  a line scrolled         "); put_ms(d / 200u);
        vga_printf("  (%u of 200 by moving pixels)\n", sc1 - sc0);
    }
    report("  drawing during the bench:");
}

void cmd_perf(const char *arg)
{
    if (!vga_wm_on()) { vga_puts("perf measures the desktop's drawing: run it in the Terminal on the desktop.\n"); return; }
    if (arg && !strcmp(arg, "bench")) { bench(); return; }
    report("Drawing since the last `perf`:");
    vga_puts("Use the desktop for a while, then type `perf` again. `perf bench` times a fixed set.\n");
}
