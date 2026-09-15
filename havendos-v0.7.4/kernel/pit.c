#include "../include/io.h"
#include "../include/types.h"

/* ================================================================
   PIT - Programmable Interval Timer
   BOOT OS uses two timing sources:
   1. IRQ0-based ticks (when interrupts work)
   2. Direct PIT counter polling (always works, even on UTM TCG)
   ================================================================ */

/* PIT runs at 1193182 Hz always */
#define PIT_BASE_HZ  1193182UL
#define PIT_HZ       100
#define PIT_DIVISOR  (PIT_BASE_HZ / PIT_HZ)  /* ~11932 */

void pit_init(uint32_t hz){
    uint32_t div = PIT_BASE_HZ / hz;
    outb(0x43, 0x36);           /* channel 0, lo/hi, mode 3, binary */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)(div >> 8));
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
static uint16_t last_pit_val = 0;
static uint32_t us_accumulator = 0;

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
        elapsed = (PIT_DIVISOR - cur) + last_pit_val;
    }
    last_pit_val = cur;
    /* elapsed ticks at 1193182 Hz -> microseconds */
    us_accumulator += (uint32_t)elapsed * 1000000UL / PIT_BASE_HZ;
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
   sleep_ms() - accurate busy-wait using PIT polling
   ---------------------------------------------------------------- */
void sleep_ms(uint32_t ms){
    if(ms == 0) return;
    uint32_t start = pit_us();
    uint32_t target = ms * 1000;   /* convert to microseconds */
    while((pit_us() - start) < target){
        __asm__ volatile("pause");
    }
}
