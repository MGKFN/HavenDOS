#include "../include/io.h"
#include "../include/types.h"

/* ================================================================
   PIT - Programmable Interval Timer
   BOOT OS uses two timing sources:
   1. IRQ0-based ticks (when interrupts work)
   2. Direct PIT counter polling (always works, even on UTM TCG)
   ================================================================ */

static uint16_t last_pit_val = 0;

/* PIT runs at 1193182 Hz always */
#define PIT_BASE_HZ  1193182UL
#define PIT_HZ       100
#define PIT_DIVISOR  (PIT_BASE_HZ / PIT_HZ)  /* ~11932 */

/* v0.7.5: pit_us() below reconstructs elapsed time from the counter's
   wrap point, so it MUST know the divisor that was actually programmed.
   It previously hardcoded PIT_DIVISOR while pit_init() accepted any hz —
   any call other than pit_init(100) silently corrupted every timing
   calculation in the OS. */
static uint32_t g_pit_divisor = PIT_DIVISOR;

void pit_init(uint32_t hz){
    uint32_t div = PIT_BASE_HZ / hz;
    if(div == 0 || div > 65535) div = 65536;   /* 0 means full 16-bit range */
    g_pit_divisor = div;
    outb(0x43, 0x36);           /* channel 0, lo/hi, mode 3, binary */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
    last_pit_val = (uint16_t)(div & 0xFFFF);
}

/* ----------------------------------------------------------------
   Read current PIT channel 0 counter (counts DOWN from divisor)
   Returns a value 0..PIT_DIVISOR
   ---------------------------------------------------------------- */
static uint16_t pit_read_counter(void){
    outb(0x43, 0x00);           /* latch channel 0 */
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (hi << 8) | lo;
}

/* ----------------------------------------------------------------
   Monotonic microsecond counter using PIT polling
   Works even when IRQ0 never fires (UTM TCG mode)
   ---------------------------------------------------------------- */
static volatile uint32_t pit_overflows = 0;  /* incremented by IRQ0 */
static uint32_t us_accumulator = 0;
static uint32_t us_frac        = 0;   /* 16.16 carry — see pit_us() */

/* Called by IRQ0 when it fires (bonus accuracy if IRQs work) */
void pit_tick(void){ pit_overflows++; }

/* Get microseconds elapsed since boot - pure polling, no IRQ needed */
static uint32_t pit_us(void){
    uint16_t cur = pit_read_counter();
    /* PIT counts down - if cur > last, it wrapped */
    uint16_t elapsed;
    if(cur <= last_pit_val){
        elapsed = last_pit_val - cur;
    } else {
        elapsed = (uint16_t)((g_pit_divisor - cur) + last_pit_val);
    }
    last_pit_val = cur;

    /* v0.7.12 FIX — the old line was:
     *     us_accumulator += (uint32_t)elapsed * 1000000UL / PIT_BASE_HZ;
     *
     * Two separate defects.
     *
     * 1. TRUNCATION THAT DESTROYED TIME.  One PIT tick is 0.838 us, so for
     *    elapsed == 1 the expression evaluates to 1000000/1193182 == 0.  The
     *    tick was consumed (last_pit_val had already advanced) but contributed
     *    nothing.  pit_read_counter() costs three port accesses, which is on
     *    the order of one to four PIT ticks, so in the tight sleep_ms() poll
     *    loop this threw away a large fraction of every interval.  sleep_ms()
     *    therefore slept LONGER than asked — unboundedly so on a fast host —
     *    and get_ticks() ran slow, which drags every network timeout, the TCP
     *    retransmission timer and the DNS cache with it.
     *
     * 2. OVERFLOW.  elapsed can be as large as g_pit_divisor (11932 at 100 Hz)
     *    after a wrap, and 11932 * 1000000 is 1.19e10 — it does not fit in the
     *    uint32_t the multiplication is performed in.
     *
     * Both are fixed by carrying the remainder in 16.16 fixed point.
     * 1000000/1193182 * 65536 = 54927 (relative error ~1e-6), and the largest
     * possible product 65536 * 54927 = 3.60e9 still fits in a uint32_t. */
    /* ── v0.8.8.8: MODE 3 DECREMENTS THE COUNTER BY TWO ─────────────
     *
     * The constant above was 54927 = 1000000/1193182 * 65536: the
     * microseconds in ONE INPUT CLOCK. That is the right number for a
     * counter that decrements by one per clock, and channel 0 is
     * programmed `outb(0x43, 0x36)` -- mode 3, square wave -- where
     * the 8254 decrements by TWO on every clock so that the output
     * spends half the period high and half low.
     *
     * So a counter delta of N spans N/2 input clocks, not N, and this
     * function has been reporting twice the elapsed time since it was
     * written. Everything downstream inherited it:
     *
     *   sleep_ms(100)  slept about 52 ms
     *   get_ticks()    ran at twice real time
     *   boot_ms()      takes the MAX of the IRQ count and this one,
     *                  so the polled value always won and every
     *                  timestamp in `dmesg` is roughly doubled
     *
     * and with them every network timeout, every retransmission
     * timer, every animation delay and the screensaver countdown --
     * all of them half the duration their call site asks for.
     *
     * FOUND BY MEASURING A SCREEN. v0.8.8.8's early-access notice
     * waits fifty slices of sleep_ms(100) and should hold the screen
     * for five seconds; screenshotting the boot with wall-clock
     * timestamps showed it up for 2.6, and the notice's own slice
     * counter read s49, so the loop was completing all fifty. Fifty
     * hundred-millisecond sleeps in 2.6 seconds is the whole finding.
     *
     * 27464 is the same constant halved: 0.5 / 1193182 * 65536 =
     * 27463.6, so the rounding error is about 2e-5. Mode 3 is left
     * alone rather than switched to mode 2, because the mode decides
     * the IRQ0 waveform and this is arithmetic, not hardware. */
    {
        uint32_t acc = (uint32_t)elapsed * 27464u + us_frac;
        us_accumulator += acc >> 16;
        us_frac         = acc & 0xFFFFu;
    }
    return us_accumulator;
}

/* ----------------------------------------------------------------
   get_ticks() - returns 10ms ticks (100Hz equivalent)
   Uses PIT polling so it works regardless of IRQ state
   ---------------------------------------------------------------- */
uint32_t get_ticks(void){
    return pit_us() / 10000;   /* 10000 us = 10ms = one 100Hz tick */
}

/* ----------------------------------------------------------------
   boot_ms() - milliseconds since boot, for the kernel log

   Neither clock here is trustworthy on its own:

     - pit_us() is polled, and the PIT counter wraps every 10 ms at 100 Hz.
       Two calls more than one wrap apart lose whole wraps, so it UNDERCOUNTS
       whenever nothing is polling -- which during init is most of the time,
       because a driver probing hardware is not calling the clock.

     - pit_overflows is incremented by IRQ0, which does not depend on anyone
       polling, but IRQs are unreliable under UTM SE's TCG, so it UNDERCOUNTS
       whenever interrupts are being dropped.

   Both are therefore lower bounds on the elapsed time, and the larger of two
   lower bounds is the better one.  Taking the max also means the log's
   timestamps can never run backwards, whichever clock is misbehaving.

   This is not a precise clock and it is not presented as one; it is good to
   about 10 ms, which is the resolution at which "what is slow during boot"
   is an interesting question.
   ---------------------------------------------------------------- */
uint32_t pit_divisor_actual(void){ return g_pit_divisor; }
uint32_t pit_hz_actual(void){
    return g_pit_divisor ? (PIT_BASE_HZ / g_pit_divisor) : 0;
}

uint32_t boot_ms(void){
    uint32_t irq_ms    = pit_overflows * (1000u / PIT_HZ);
    uint32_t polled_ms = pit_us() / 1000u;
    return irq_ms > polled_ms ? irq_ms : polled_ms;
}

/* ----------------------------------------------------------------
   sleep_ms() - accurate busy-wait using PIT polling
   ---------------------------------------------------------------- */
/* Pushing the shadow buffer to the screen before a wait is what makes a frame
   atomic, and keyboard_waitchar() has always done it.  But a program that
   polls instead of blocking never goes through there: app_clock() and every
   other draw-poll-sleep loop - the dashboard, the monitor, the games, the
   screensavers - reach only this function.  vga_clear() is deliberately
   shadow-only, so without a flush here the cells such a program does not draw
   kept showing whatever was on screen before it started, which is the desktop
   it was launched from.  The flush is a comparison pass over the cells and
   writes only the ones that differ, so a sleep in a driver costs nothing. */
extern void vga_present(void);

void sleep_ms(uint32_t ms){
    vga_present();
    if(ms == 0) return;
    uint32_t start = pit_us();
    uint32_t target = ms * 1000;   /* convert to microseconds */
    while((pit_us() - start) < target){
        __asm__ volatile("pause");
    }
}
