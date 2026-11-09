#pragma once
/* ================================================================
   include/kpoll_asm.h -- the two instructions a test cannot run

   KPOLL_ASM_H is a real include guard as well as the #pragma, and it
   is here for exactly the reason MACHINE_ASM_H is in
   include/machine_asm.h and IO_H is in include/io.h: so a native test
   can define it, supply its own versions, and run the real sequence
   in process.

   kernel/kpoll.c has one critical section, and it is the whole
   correctness argument of the file. kpoll_tick() runs in interrupt
   context and sets due flags; kernel_poll() reads and clears them.
   On one CPU that is a read-modify-write on both sides of an
   interrupt, so the read-and-clear is done with interrupts off --
   and NOT with a bare cli/sti pair, because kernel_poll() is called
   from paths that may already have interrupts disabled and a bare
   sti would turn them on behind the caller's back. pushf/popf
   restores whatever the caller had.

   A test cannot execute either: `cli` from ring 3 is a privilege
   fault, which is a SIGSEGV rather than a disabled interrupt.
   tests/test_kpoll.c defines KPOLL_ASM_H, counts the two calls, and
   drives kpoll_tick() by hand -- so the interleaving the real cli
   protects against is one the suite can actually produce, which a
   suite running with real interrupts off could not.
   ================================================================ */
#ifndef KPOLL_ASM_H
#define KPOLL_ASM_H
#include "types.h"

/* Save EFLAGS and disable interrupts.  Returns the saved flags. */
static inline uint32_t kpoll_irq_save(void)
{
    uint32_t f;
    __asm__ volatile("pushfl\n\t"
                     "popl %0\n\t"
                     "cli"
                     : "=r"(f) :: "memory");
    return f;
}

/* Put EFLAGS back exactly as it was -- which may well mean leaving
   interrupts off, if that is how the caller arrived. */
static inline void kpoll_irq_restore(uint32_t f)
{
    __asm__ volatile("pushl %0\n\t"
                     "popfl"
                     :: "r"(f) : "memory", "cc");
}

#endif
