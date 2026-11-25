/* HavenDOS <-> MicroPython glue. Everything MicroPython needs from the
   machine comes through here: the console, the keyboard, the clock, the
   files and the heap. The kernel side calls python_main(). */
#include <stdint.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "py/mphal.h"
#include "shared/runtime/pyexec.h"
#include "shared/runtime/gchelper.h"

/* the kernel */
extern void  vga_putchar(char c);
extern void  vga_flush(void);
extern int   keyboard_getchar(void);
extern int   keyboard_waitchar(void);
extern int   keyboard_take_ctrl_c(void);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern int   vfs_read(const char *name, char *buf, uint32_t bufsz);
extern int   vfs_exists(const char *name);
extern int   vfs_is_dir(const char *name);
extern uint32_t boot_ms(void);
extern void  kpoll_sleep_ms(uint32_t ms);
extern void  make_path(char *dst, const char *name);

const char havendos_help_text[] =
    "MicroPython on HavenDOS\n"
    "\n"
    "  print(2 + 2)        prints 4\n"
    "  import math         math.sqrt(2), math.pi ...\n"
    "  import random       random.randint(1, 6)\n"
    "  import time         time.sleep(1), time.ticks_ms()\n"
    "  input('name? ')     reads a line\n"
    "  import mymodule     runs mymodule.py from this folder\n"
    "\n"
    "Ctrl+C stops a running program. Ctrl+D (on an empty line) leaves.\n"
    "Run a file from the shell with:  python file.py\n"
    "Not here (yet): open() and files from Python, threads, sockets.\n";

/* ── the console ───────────────────────────────────────────────── */

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    mp_uint_t total = len;
    while (len--) {
        char c = *str++;
        if (c == '\r') continue;          /* the console's newline is \n alone */
        if (c == 27) {                    /* readline's cursor moves: ESC [ n D, ESC [ K ... */
            /* handled by the console as plain motion: \b for left, spaces for erase */
            if (len >= 2 && str[0] == '[') {
                int n = 0; size_t i = 1;
                while (i < len && str[i] >= '0' && str[i] <= '9') { n = n * 10 + (str[i] - '0'); i++; }
                if (i < len) {
                    char f = str[i];
                    if (f == 'D') { if (!n) n = 1; while (n--) vga_putchar('\b'); }
                    else if (f == 'C') { /* right: readline re-prints the chars instead when it can */ }
                    else if (f == 'K') { extern void vga_clear_to_eol(void) __attribute__((weak));
                                         if (vga_clear_to_eol) vga_clear_to_eol(); }
                    str += i + 1; len -= i + 1;
                    continue;
                }
            }
            continue;
        }
        vga_putchar(c);
    }
    return total;
}

/* Keys come back as the terminal codes readline understands */
static char pend[4];
static int npend;

int mp_hal_stdin_rx_chr(void) {
    if (npend) { int c = (unsigned char)pend[0]; memmove(pend, pend + 1, --npend); return c; }
    vga_flush();
    for (;;) {
        int c = keyboard_waitchar();
        const char *seq = 0;
        switch (c) {
            case '\n':   return '\r';
            case 0x148:  seq = "[A"; break;   /* up    -- history */
            case 0x150:  seq = "[B"; break;   /* down */
            case 0x14D:  seq = "[C"; break;   /* right */
            case 0x14B:  seq = "[D"; break;   /* left */
            case 0x147:  seq = "[H"; break;   /* home */
            case 0x14F:  seq = "[F"; break;   /* end */
            case 0x153:  seq = "[3~"; break;  /* delete */
            default: break;
        }
        /* The window's X arrives as ESC with a close request behind it
           (shell/chrome.c). Leave the prompt: Ctrl+C drops the line
           being typed, then Ctrl+D on the empty line leaves. */
        if (c == 27) {
            extern int chrome_take_close(void) __attribute__((weak));
            if (chrome_take_close && chrome_take_close()) { pend[0] = 4; npend = 1; return 3; }
        }
        if (seq) { npend = (int)strlen(seq); memcpy(pend, seq, (size_t)npend); return 27; }
        if (c == '\b') return 8;
        if (c >= 0 && c < 0x100) return c;
        /* any other extended key: ignored */
    }
}

/* Ctrl+C while a program runs: checked every couple of thousand
   bytecodes, without taking any other key out of the queue */
static uint64_t last_poll;
static inline uint64_t tsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
void havendos_poll(void) {
    uint64_t now = tsc();
    if (now - last_poll < 20000000ull) return;   /* ~10 ms on the HP: the keyboard, not every loop */
    last_poll = now;
    if (keyboard_take_ctrl_c()) mp_sched_keyboard_interrupt();
}

/* ── the clock ─────────────────────────────────────────────────── */

mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)boot_ms(); }
mp_uint_t mp_hal_ticks_us(void) { return (mp_uint_t)boot_ms() * 1000u; }
mp_uint_t mp_hal_ticks_cpu(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return lo; }
void mp_hal_delay_ms(mp_uint_t ms) {
    uint32_t end = mp_hal_ticks_ms() + ms;
    while ((int32_t)(end - mp_hal_ticks_ms()) > 0) {
        mp_handle_pending(true);
        uint32_t left = end - mp_hal_ticks_ms();
        kpoll_sleep_ms(left > 10 ? 10 : (left ? left : 1));
    }
}
void mp_hal_delay_us(mp_uint_t us) { mp_hal_delay_ms((us + 999) / 1000); }
uint64_t mp_hal_time_ns(void) {
    extern void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
    extern void rtc_get_date(uint8_t *d, uint8_t *mo, uint16_t *y);
    uint8_t h, mi, s, d, mo; uint16_t y;
    rtc_get_time(&h, &mi, &s); rtc_get_date(&d, &mo, &y);
    /* days since 1970 */
    static const uint16_t cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t days = (uint32_t)(y - 1970) * 365u + (uint32_t)((y - 1969) / 4);
    if (mo >= 1 && mo <= 12) days += cum[mo - 1];
    if (mo > 2 && (y % 4) == 0) days++;
    days += (uint32_t)(d ? d - 1 : 0);
    uint64_t secs = (uint64_t)days * 86400u + h * 3600u + mi * 60u + s;
    return secs * 1000000000ull;
}
uint32_t havendos_random_seed(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return lo ^ hi ^ boot_ms(); }

/* ── files: import and `python file.py` ───────────────────────── */

static char *read_whole(const char *path, uint32_t *len) {
    uint32_t cap = 64 * 1024;
    for (;;) {
        char *buf = (char *)kmalloc(cap + 1);
        if (!buf) return 0;
        int n = vfs_read(path, buf, cap);
        if (n < 0) { kfree(buf); return 0; }
        if ((uint32_t)n < cap || cap >= 4u * 1024 * 1024) { buf[n] = 0; *len = (uint32_t)n; return buf; }
        kfree(buf); cap *= 4;
    }
}

mp_import_stat_t mp_import_stat(const char *path) {
    char full[200];
    make_path(full, path);
    if (vfs_is_dir(full)) return MP_IMPORT_STAT_DIR;
    if (vfs_exists(full)) return MP_IMPORT_STAT_FILE;
    return MP_IMPORT_STAT_NO_EXIST;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    char full[200];
    make_path(full, qstr_str(filename));
    uint32_t n = 0;
    char *src = read_whole(full, &n);
    if (!src) mp_raise_OSError(MP_ENOENT);
    /* the lexer keeps the buffer: copy it onto the GC heap so it is freed with it */
    char *gcsrc = m_new(char, n + 1);
    memcpy(gcsrc, src, n + 1);
    kfree(src);
    return mp_lexer_new_from_str_len(filename, gcsrc, n, n + 1);
}

/* ── the heap and the GC ───────────────────────────────────────── */

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val) {
    (void)val;
    mp_hal_stdout_tx_strn("python: fatal error (nlr)\n", 26);
    for (;;) { __asm__ volatile("hlt"); }
}

void NORETURN __fatal_error(const char *msg) {
    mp_hal_stdout_tx_strn(msg, strlen(msg));
    for (;;) { __asm__ volatile("hlt"); }
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)file; (void)line; (void)func; (void)expr;
    __fatal_error("python: assertion failed\n");
}
#endif

/* ── the entry point ──────────────────────────────────────────── */

static void *g_heap;

/* path: a .py file to run, or 0 for the REPL. Returns 0 on success. */
static int g_busy;

int python_main(const char *path) {
    int stack_dummy;
    int rc = 0;
    /* one interpreter: its state is global, and two windows running it
       at once would share (and wreck) one heap */
    if (g_busy) { mp_hal_stdout_tx_str("Python is already open in another window.\n"); return -1; }
    g_busy = 1;
    if (!g_heap) g_heap = kmalloc(MICROPY_HEAP_SIZE);
    if (!g_heap) { mp_hal_stdout_tx_str("python: not enough memory\n"); g_busy = 0; return -1; }
    mp_stack_set_top(&stack_dummy);
    mp_stack_set_limit(40 * 1024);   /* of the 96 KB a task has */
    gc_init(g_heap, (char *)g_heap + MICROPY_HEAP_SIZE);
    mp_init();
    npend = 0;
    if (path) {
        char full[200];
        make_path(full, path);
        if (!vfs_exists(full)) {
            mp_printf(&mp_plat_print, "python: no such file: %s\n", path);
            rc = -1;
        } else {
            int r = pyexec_file(path);
            rc = (r & PYEXEC_FORCED_EXIT) ? 0 : (r ? 0 : -1);
        }
    } else {
        for (;;) {
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                if (pyexec_raw_repl() != 0) break;
            } else {
                if (pyexec_friendly_repl() != 0) break;
            }
        }
    }
    if (!path) mp_hal_stdout_tx_strn("\n", 1);   /* Ctrl+D leaves the cursor after ">>> " */
    gc_sweep_all();
    mp_deinit();
    vga_flush();
    g_busy = 0;
    return rc;
}
