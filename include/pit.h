/* ================================================================
   include/pit.h -- the 8253/8254 timer, declared in one place
   ================================================================
   v0.8.8.8.  kernel/pit.c has had no header since it was written.
   Every consumer carried its own copy of the prototype --

       extern void sleep_ms(uint32_t);

   -- in net/dhcp.c, net/tcp.c, net/dns.c, net/http.c, net/tls.c,
   net/icmp.c and net/ipv4.c, seven identical lines maintained by
   hand, and kernel/main.c forgot its one and called sleep_ms()
   through an IMPLICIT DECLARATION.  That compiled, and on this ABI
   it even worked, because an int-returning function taking a
   promoted uint32_t happens to be called the same way.

   It is still the wrong shape of problem to have. A signature that
   lives in eight places changes in seven of them, and the eighth is
   found by a bug rather than by the compiler. More immediately:
   v0.8.8.8 changed what these functions MEAN -- pit_us() was
   counting at twice real time, because channel 0 runs in mode 3
   where the 8254 decrements by two per clock -- and a reader
   chasing that has nowhere to look it up.

   So: one header, and the units written down next to each function.
   ================================================================ */
#ifndef PIT_H
#define PIT_H
#include "types.h"

#define PIT_HZ          100u        /* channel 0 interrupt rate       */
#define PIT_BASE_HZ     1193182u    /* the 8254's input clock         */

/* Programs channel 0 for PIT_HZ in mode 3 (square wave). The mode is
   load-bearing: in mode 3 the counter decrements by TWO per input
   clock, and kernel/pit.c's microsecond arithmetic accounts for it.
   Changing the mode here without changing that constant puts every
   timer in the kernel out by a factor of two -- which is exactly the
   defect v0.8.8.8 fixed, arrived at from the other direction. */
void     pit_init(uint32_t hz);

/* Called by the IRQ0 handler. Optional: everything below works by
   polling if IRQ0 never fires, which is the case under some
   emulators. */
void     pit_tick(void);

/* Milliseconds since boot. The larger of the IRQ count and a polled
   read, because neither is trustworthy alone -- see kernel/pit.c. */
uint32_t boot_ms(void);

/* 10 ms ticks since boot, i.e. a 100 Hz counter. Polled, so it works
   with interrupts off. */
uint32_t get_ticks(void);

/* An accurate busy-wait. Polls the PIT; does NOT service drivers, so
   a wait longer than a few hundred milliseconds in a path that owns
   the CPU wants kernel_poll() beside it. Never call it from an
   interrupt handler. */
void     sleep_ms(uint32_t ms);

#endif /* PIT_H */
