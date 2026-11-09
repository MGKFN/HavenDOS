#pragma once
/* IO_H is a real include guard as well as the #pragma, and it exists so a
   test can define it and supply its own inb/outb.  Every driver in this tree
   talks to hardware through these six functions and nothing else, so with
   them replaceable a driver can be compiled natively against a simulated
   device and actually tested -- which is how the PCI enumeration, and the
   USB controller after it, are checked without booting anything. */
#ifndef IO_H
#define IO_H
#include "types.h"
static inline void outb(uint16_t port, uint8_t val){ __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline void outw(uint16_t port, uint16_t val){ __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port)); }
static inline void outl(uint16_t port, uint32_t val){ __asm__ volatile("outl %0,%1"::"a"(val),"Nd"(port)); }
static inline uint8_t  inb(uint16_t port){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void io_wait(void){ __asm__ volatile("outb %%al,$0x80"::"a"(0)); }

/* The delay an 8042 needs between accesses, in one place.
 *
 * THREE files talk to that controller through ports 0x60 and 0x64:
 * drivers/keyboard.c (an interrupt path and a polling path),
 * drivers/mouse.c, and kernel/machine.c's reset pulse. v0.8.5.2 and
 * v0.8.5.3 were both the same bug -- two accesses with no delay
 * between them, so a controller emulated in SMM served a stale status
 * register and every keystroke arrived twice. v0.8.5.3 taught the rule
 * to ONE of the three: kbc_drain(). keyboard_init() and the whole of
 * mouse.c still had it three releases later, which is the bus-label
 * mistake again -- a fix applied where the symptom was seen rather
 * than where the rule belongs.
 *
 * Four io_wait()s, because an emulated controller is slower to update
 * its status register than a real one. The rule every caller follows:
 * EVERY read of 0x64 is preceded by one of these. Before, not after --
 * a helper that delays after reading leaves the next helper free to
 * read again with nothing in between, and two status reads either side
 * of a call boundary are just as adjacent as two inside one loop.
 *
 * tests/test_kbdinit.c compiles both drivers against a recording I/O
 * seam and fails if any status read is not preceded by a delay.
 */
static inline void i8042_delay(void){ io_wait(); io_wait(); io_wait(); io_wait(); }

/* How many times to poll 0x64 before giving up.
 *
 * This number has to move WITH the delay above, and that is the whole
 * reason it lives here rather than as a 0x10000 written into each
 * driver. An iteration used to be one `inb`, about a microsecond, so
 * 0x10000 was a ~65 ms budget. With i8042_delay() in front of the read
 * an iteration costs about five microseconds, so the same 0x10000 is a
 * third of a SECOND -- per wait.
 *
 * That is not academic. The HP has no PS/2 port of either kind, so
 * mouse_init() times out on every wait it performs, and keeping
 * 0x10000 would have added over a second to boot on the one machine
 * this all runs on. Measured: 327,723 port accesses for one
 * mouse_init() against a controller that never answers.
 *
 * 10000 x ~5 us is a ~50 ms budget, which restores the original
 * wall-clock figure. A real 8042 transaction completes in
 * microseconds, so anything still waiting after 50 ms is not there.
 */
#define I8042_SPINS 10000
#endif
