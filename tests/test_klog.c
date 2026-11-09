/* The kernel log, natively.
 *
 * This exists because the thing it replaced was a lie that nobody could
 * test: 24 string literals describing a machine, four of which were wrong on
 * every machine, and a dmesg that printed timestamps typed one millisecond
 * apart.  None of that could fail a test, because none of it was derived
 * from anything.
 *
 * So the checks here are about the properties that make a log trustworthy:
 * what goes in comes out, in order, with the level it was given; a long line
 * is truncated rather than overrunning the buffer behind it; a full buffer
 * keeps the START of the boot and says how much it dropped; and the live
 * echo and the replay render identically, because a dmesg that disagrees
 * with what scrolled past is a third source of fiction.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/types.h"
#include "../include/vga.h"

/* ── the screen, captured instead of drawn ─────────────────────────────
   These are the real prototypes from vga.h, so the driver's own header can
   be included and klog.c compiles here exactly as it does in the kernel. */
static char screen[16384];
static int  screen_len;
void vga_puts(const char *s)
{
    while (*s && screen_len < (int)sizeof(screen) - 1) screen[screen_len++] = *s++;
    screen[screen_len] = 0;
}
void vga_set_color(vga_color_t a, vga_color_t b) { (void)a; (void)b; }
static void screen_clear(void) { screen_len = 0; screen[0] = 0; }

/* ── the clock, under the test's control ──────────────────────────────── */
static uint32_t v_ms;
uint32_t boot_ms(void) { return v_ms; }

#include "../include/klog.h"
#include "../kernel/klog.c"

/* ── checks ───────────────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if (cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what, detail ? detail : ""); }
}
static void head(const char *t) { printf("\n%s\n", t); }

static void reset(void)
{
    g_nlines = 0; g_dropped = 0; g_echo = 1; v_ms = 0;
    screen_clear();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("kernel log\n");
    printf("==========\n");

    head("what goes in comes out");
    reset();
    v_ms = 5;    klog(KLOG_OK,   "PCI: %u devices on bus 0", 7u);
    v_ms = 120;  klog(KLOG_WARN, "ATA: nothing on 0x%x", 0x1F0u);
    v_ms = 2400; klog(KLOG_ERR,  "UHCI: %u controller(s) running", 0u);
    ck("three lines recorded", klog_count() == 3, "");
    ck("the text is what was formatted",
       !strcmp(klog_text(0), "PCI: 7 devices on bus 0"), klog_text(0));
    ck("hex is uppercase, matching lspci",
       !strcmp(klog_text(1), "ATA: nothing on 0x1F0"), klog_text(1));
    ck("levels are kept", klog_level(0) == KLOG_OK
       && klog_level(1) == KLOG_WARN && klog_level(2) == KLOG_ERR, "");
    ck("each line carries the time it was written",
       klog_ms(0) == 5 && klog_ms(1) == 120 && klog_ms(2) == 2400, "");

    head("timestamps never run backwards");
    {
        /* boot_ms() takes the larger of two clocks that each undercount in
           different circumstances, so it cannot go back -- but the log is
           also written from init paths that call it at odd moments, and a
           line timestamped before the one above it would be worse than no
           timestamp at all. */
        int ok = 1;
        for (int i = 1; i < klog_count(); i++)
            if (klog_ms(i) < klog_ms(i - 1)) ok = 0;
        ck("the recorded times are non-decreasing", ok, "");
    }

    head("the formatter");
    reset();
    klog(KLOG_OK, "d=%d u=%u x=%x c=%c s=%s %%", -42, 42u, 0xBEEFu, 'Z', "str");
    ck("every specifier", !strcmp(klog_text(0),
       "d=-42 u=42 x=BEEF c=Z s=str %"), klog_text(0));
    klog(KLOG_OK, "%4x:%4x %2u", 0xC1u, 0x7020u, 3u);
    ck("field widths line the columns up",
       !strcmp(klog_text(1), "00C1:7020 03"), klog_text(1));
    klog(KLOG_OK, "null is survivable: %s", (char *)0);
    ck("a null string does not crash it",
       !strcmp(klog_text(2), "null is survivable: (null)"), klog_text(2));
    klog(KLOG_OK, "trailing percent %");
    ck("a dangling %% is not read past the end",
       !strcmp(klog_text(3), "trailing percent "), klog_text(3));

    head("a long line is truncated, not written past the end");
    reset();
    {
        char big[400];
        for (int i = 0; i < 399; i++) big[i] = 'x';
        big[399] = 0;
        klog(KLOG_OK, "%s", big);
        ck("the line is clipped to the buffer",
           (int)strlen(klog_text(0)) < 72,
           "a log that overruns its own buffer corrupts the line after it");
        ck("and it is still a valid string",
           klog_text(0)[strlen(klog_text(0))] == 0, "");
    }

    head("a full buffer keeps the beginning");
    reset();
    for (int i = 0; i < KLOG_MAX_LINES + 25; i++) {
        v_ms = (uint32_t)i;
        klog(KLOG_OK, "line %u", (uint32_t)i);
    }
    ck("it stops at capacity", klog_count() == KLOG_MAX_LINES, "");
    ck("the FIRST line is still line 0", !strcmp(klog_text(0), "line 0"),
       "a boot that went wrong went wrong at the start, so the start is "
       "what a full log has to keep");
    ck("and the loss is counted, not silent", klog_dropped() == 25, "");

    head("echo can be turned off, and the line is still recorded");
    reset();
    klog(KLOG_OK, "printed");
    ck("echo is on by default", screen_len > 0,
       "a kernel that dies during init must already have said how far it "
       "got, so this cannot default to off");
    {
        int before = screen_len;
        klog_set_echo(0);
        klog(KLOG_OK, "not printed");
        ck("quiet stops the printing", screen_len == before, "");
        ck("but the line is still in the log for dmesg", klog_count() == 2, "");
        ck("klog_echo_on reports it", klog_echo_on() == 0, "");
        klog_set_echo(1);
    }

    head("the replay renders exactly what scrolled past");
    reset();
    v_ms = 1234; klog(KLOG_OK,   "first");
    v_ms = 5678; klog(KLOG_WARN, "second");
    {
        char live[8192];
        memcpy(live, screen, (size_t)screen_len + 1);
        int live_len = screen_len;
        screen_clear();
        klog_set_echo(0);     /* so the dump is the only thing on screen */
        klog_dump();
        ck("dmesg output is identical to the live echo",
           live_len == screen_len && !memcmp(live, screen, (size_t)live_len),
           "they are two views of one buffer; if they can differ, one of "
           "them is wrong and you cannot tell which");
        klog_set_echo(1);
    }

    head("the timestamp is formatted as seconds and milliseconds");
    reset();
    klog_set_echo(1);
    v_ms = 3261; klog(KLOG_OK, "ready");
    ck("3261 ms reads as 3.261", strstr(screen, "3.261") != 0, screen);
    reset();
    v_ms = 7; klog(KLOG_OK, "early");
    ck("7 ms reads as 0.007, not 0.7", strstr(screen, "0.007") != 0, screen);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
