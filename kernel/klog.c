/*
 * klog.c — the kernel log
 *
 * One buffer, written by each subsystem as it initialises, read by the boot
 * screen and by `dmesg`.  See include/klog.h for why this replaced three
 * screens, two of which were fiction.
 *
 * The rule this file exists to enforce: a line nobody measured is a line
 * that does not get written.  Every caller passes values it has just read
 * from the hardware or computed; there are no literals describing machine
 * state anywhere in the boot path, and tools/check_boot_log.py fails the
 * build if one reappears.
 */
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/klog.h"

extern uint32_t boot_ms(void);          /* kernel/pit.c */

/* ── v0.8.9.6: THE BOOT IS PINNED, EVERYTHING AFTER IT ROTATES ─────────
 *
 * This was 160 lines that kept the START and dropped everything after
 * it. The reasoning was sound for a boot -- the beginning is where a
 * boot that went wrong went wrong -- and wrong for every other hour of
 * the machine's life. On the HP the boot alone writes over a hundred
 * lines, so a few minutes of USB events filled the rest and from then
 * on NOTHING new was kept: `dmesg | grep lockdown` after a failed pair
 * found nothing, because the lines that would have explained it were
 * the dropped ones. A diagnostic log that stops recording is worse
 * than a small one, because it looks like a log of a quiet machine.
 *
 * So: the first KLOG_PIN lines are kept for ever, as before -- that is
 * the boot -- and everything after them goes round a ring of
 * KLOG_RING lines, newest overwriting oldest. klog_dropped() now
 * counts lines ROTATED OUT of the ring, and `dmesg` says where the
 * gap is: between the boot and the oldest line still held.
 *
 * 512 lines of 80 bytes is 40 KB of .bss. The heap has been 125 times
 * bigger than this since 0.8.8.5; 160 was a number from a kernel that
 * had 255 KB to its name. */
#define KLOG_PIN       192
#define KLOG_RING      320
#define KLOG_MAX_LINES (KLOG_PIN + KLOG_RING)
#define KLOG_LINE_LEN  72               /* 80 columns minus the timestamp */

typedef struct {
    uint32_t ms;
    uint8_t  level;
    char     text[KLOG_LINE_LEN];
} klog_line_t;

static klog_line_t g_lines[KLOG_MAX_LINES];
static int         g_nlines;     /* logical lines held: pinned + ring  */
static int         g_ring_head;  /* ring slot holding the OLDEST line  */
static int         g_dropped;    /* lines rotated out of the ring      */

/* Logical index -> storage. 0..KLOG_PIN-1 are the boot, in place; the
   rest are the ring, read from its oldest slot onward. Every accessor
   goes through this, so a reader never sees the ring's seam. */
static klog_line_t *line_at(int i)
{
    if (i < 0 || i >= g_nlines) return 0;
    if (i < KLOG_PIN) return &g_lines[i];
    return &g_lines[KLOG_PIN + (g_ring_head + (i - KLOG_PIN)) % KLOG_RING];
}
static int         g_echo = 1;          /* on from the first line */
static void      (*g_sink)(int, const char *) = 0;

uint32_t klog_now_ms(void) { return boot_ms(); }

void klog_set_echo(int on) { g_echo = on ? 1 : 0; }
void klog_set_sink(void (*fn)(int, const char *)) { g_sink = fn; }
int  klog_echo_on(void)    { return g_echo; }

int         klog_count(void)       { return g_nlines; }
int         klog_dropped(void)     { return g_dropped; }
uint32_t    klog_ms(int i)         { klog_line_t *l = line_at(i); return l ? l->ms : 0; }
int         klog_level(int i)      { klog_line_t *l = line_at(i); return l ? l->level : KLOG_INFO; }
const char *klog_text(int i)       { klog_line_t *l = line_at(i); return l ? l->text : ""; }
/* Where the gap is, for a reader that prints one: the logical index of
   the first ring line, or -1 when nothing has rotated out. */
int         klog_gap_at(void)      { return g_dropped ? KLOG_PIN : -1; }

/* ── a small formatter ────────────────────────────────────────────────
   The same specifiers vga_printf takes, written into a bounded buffer so a
   long line is truncated rather than overrunning.  Hex is uppercase to match
   what lspci and usb already print. */

static void put_c(char *buf, int cap, int *n, char c)
{
    if (*n < cap - 1) buf[(*n)++] = c;
}

static void put_s(char *buf, int cap, int *n, const char *s)
{
    if (!s) s = "(null)";
    while (*s) put_c(buf, cap, n, *s++);
}

static void put_u(char *buf, int cap, int *n, uint32_t v, int base, int pad)
{
    const char *digits = "0123456789ABCDEF";
    char tmp[12];
    int  t = 0;
    if (!v) tmp[t++] = '0';
    while (v) { tmp[t++] = digits[v % (uint32_t)base]; v /= (uint32_t)base; }
    while (t < pad) tmp[t++] = '0';
    while (t) put_c(buf, cap, n, tmp[--t]);
}

static void put_d(char *buf, int cap, int *n, int v)
{
    if (v < 0) { put_c(buf, cap, n, '-'); v = -v; }
    put_u(buf, cap, n, (uint32_t)v, 10, 0);
}

static int klog_format(char *buf, int cap, const char *fmt, va_list ap)
{
    int n = 0;
    while (*fmt) {
        if (*fmt != '%') { put_c(buf, cap, &n, *fmt++); continue; }
        fmt++;
        /* One optional digit of zero padding, so %4x prints 00C1 and the
           device columns in the PCI lines line up. */
        int pad = 0;
        if (*fmt >= '1' && *fmt <= '8') { pad = *fmt - '0'; fmt++; }
        switch (*fmt) {
        case 'd': put_d(buf, cap, &n, va_arg(ap, int)); break;
        case 'u': put_u(buf, cap, &n, va_arg(ap, uint32_t), 10, pad); break;
        case 'x': put_u(buf, cap, &n, va_arg(ap, uint32_t), 16, pad); break;
        case 's': put_s(buf, cap, &n, va_arg(ap, char *)); break;
        case 'c': put_c(buf, cap, &n, (char)va_arg(ap, int)); break;
        case '%': put_c(buf, cap, &n, '%'); break;
        case 0:   buf[n] = 0; return n;
        default:  put_c(buf, cap, &n, '%'); put_c(buf, cap, &n, *fmt); break;
        }
        fmt++;
    }
    buf[n] = 0;
    return n;
}

/* ── echo ─────────────────────────────────────────────────────────────── */

static const char *tag_of(int level)
{
    switch (level) {
    case KLOG_OK:   return "  ok  ";
    case KLOG_WARN: return " warn ";
    case KLOG_ERR:  return " FAIL ";
    default:        return "      ";
    }
}

static vga_color_t colour_of(int level)
{
    switch (level) {
    case KLOG_OK:   return VGA_LIGHT_GREEN;
    case KLOG_WARN: return VGA_YELLOW;
    case KLOG_ERR:  return VGA_LIGHT_RED;
    default:        return VGA_LIGHT_GREY;
    }
}

/* [    1.234] ok  text — the seconds field is wide enough that the columns
   stay lined up for the first ten thousand seconds of uptime. */
static void echo_line(uint32_t ms, int level, const char *text)
{
    char stamp[20];
    int  n = 0;
    put_c(stamp, sizeof(stamp), &n, '[');
    uint32_t sec = ms / 1000, frac = ms % 1000;
    if (sec < 1000) put_c(stamp, sizeof(stamp), &n, ' ');
    if (sec < 100)  put_c(stamp, sizeof(stamp), &n, ' ');
    if (sec < 10)   put_c(stamp, sizeof(stamp), &n, ' ');
    put_u(stamp, sizeof(stamp), &n, sec, 10, 0);
    put_c(stamp, sizeof(stamp), &n, '.');
    put_u(stamp, sizeof(stamp), &n, frac, 10, 3);
    put_c(stamp, sizeof(stamp), &n, ']');
    stamp[n] = 0;

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts(stamp);
    vga_set_color(colour_of(level), VGA_BLACK);
    vga_puts(tag_of(level));
    vga_set_color(level == KLOG_OK ? VGA_LIGHT_GREY : colour_of(level), VGA_BLACK);
    vga_puts(text);
    vga_puts("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Replay one line, rendered by the same function the live echo used, so the
   boot screen and dmesg cannot drift apart.  Paging is the caller's job --
   this file has no business waiting on a keyboard. */
void klog_print_line(int i)
{
    klog_line_t *l = line_at(i);
    if (!l) return;
    echo_line(l->ms, l->level, l->text);
}

void klog_dump(void)
{
    for (int i = 0; i < g_nlines; i++) {
        if (i == klog_gap_at()) {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            /* Formatted here rather than through vga_printf: every
               native suite that links this file would otherwise need a
               printf stub, and klog already owns a formatter. */
            char note[64]; int k = 0;
            put_s(note, (int)sizeof note, &k, "  (... ");
            put_d(note, (int)sizeof note, &k, g_dropped);
            put_s(note, (int)sizeof note, &k,
                  " older lines rotated out after the boot ...)\n");
            note[k] = 0;
            vga_puts(note);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        klog_print_line(i);
    }
}

/* ── the one entry point ──────────────────────────────────────────────── */

void klog(int level, const char *fmt, ...)
{
    char text[KLOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    klog_format(text, (int)sizeof(text), fmt, ap);
    va_end(ap);

    uint32_t ms = boot_ms();

    {
        klog_line_t *l;
        if (g_nlines < KLOG_MAX_LINES) {
            l = &g_lines[g_nlines < KLOG_PIN ? g_nlines
                         : KLOG_PIN + (g_ring_head + (g_nlines - KLOG_PIN))
                                      % KLOG_RING];
            g_nlines++;
        } else {
            /* Full: the oldest RING line gives way. The boot never does
               -- see the note at KLOG_PIN. Counted, so the gap is shown
               rather than hidden. */
            l = &g_lines[KLOG_PIN + g_ring_head];
            g_ring_head = (g_ring_head + 1) % KLOG_RING;
            g_dropped++;
        }
        l->ms    = ms;
        l->level = (uint8_t)level;
        int i = 0;
        while (text[i] && i < KLOG_LINE_LEN - 1) { l->text[i] = text[i]; i++; }
        l->text[i] = 0;
    }

    /* ── WHERE THE LINE GOES ──────────────────────────────────────────
       A sink, if one is installed, takes the line instead of the
       scrolling echo. kernel/bootsplash.c installs itself as one so
       that the loading screen's ring turns once per recorded line and
       its caption is the latest line -- but THIS FILE DOES NOT KNOW
       THAT, and that is the point.

       It was a direct call to bootsplash_active() first. Every native
       suite that links klog.c then failed to link, because none of
       them have a loading screen -- which was going to mean the same
       four-line stub pasted into twenty test files, each one a copy
       that could drift. A sink costs one pointer and nothing has to
       be stubbed.

       Checked before g_echo rather than inside it, because a sink
       that takes the screen turns the echo off -- so an `if (g_echo)`
       wrapper would route nothing to it. The ring buffer above
       already holds the line either way, which is what makes any of
       this safe: a sink is a view, not a filter. */
    if (g_sink) { g_sink(level, text); return; }

    if (g_echo) echo_line(ms, level, text);
}
