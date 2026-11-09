#pragma once
/* ================================================================
   include/machine_asm.h -- the two instructions a test cannot run
   ================================================================
   MACHINE_ASM_H is a real include guard as well as the #pragma, and
   it is here for exactly the reason IO_H is in include/io.h and
   MMIO_H is in include/mmio.h: so a native test can define it,
   supply its own versions, and run the real sequence in process.

   kernel/machine.c is otherwise pure port I/O, which io.h already
   makes replaceable. These two are not:

     machine_triple_fault()  loads a null IDT and faults. Under a
                             test that is a SIGSEGV or SIGTRAP, not
                             a reset.
     machine_park()          never returns. A suite that called it
                             would hang instead of reporting.

   With both behind this seam the escalation in machine_reset() --
   every stage, in order, including the ones only reached when the
   stage before failed -- is what tests/test_machine.c executes.
   Nothing in that file is a copy of the sequence.
   ================================================================ */
#ifndef MACHINE_ASM_H
#define MACHINE_ASM_H
#include "types.h"

/* A null IDT: limit 0, base 0. Any interrupt then fails to be
   dispatched, the fault handler for THAT fails as well, and the third
   fault is unrecoverable -- the CPU asserts its reset line. It is the
   one reset method that needs no chipset co-operation at all, which
   is why it is last rather than absent.

   `cli` first, and it matters for the case where this does NOT work.
   Exceptions are not maskable, so the deliberate int3 still triple
   faults -- but a hypervisor that catches the triple fault leaves us
   executing with a destroyed IDT, and then the next timer tick is a
   fault with nowhere to go. With interrupts off, the only thing that
   can fault is the int3 on the line below, so if we do come back from
   this the caller reliably reaches its failure message instead of
   dying somewhere in the middle of drawing it. */
static inline void machine_triple_fault(void)
{
    static const struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };

    __asm__ volatile("cli\n\t"
                     "lidt %0\n\t"
                     "int  $3\n\t"
                     :: "m"(null_idt) : "memory");
}

/* Interrupts off and halt, for ever. Reached only when every reset
   method has been tried and the machine is still executing. */
static inline void machine_park(void)
{
    for(;;) __asm__ volatile("cli; hlt");
}

#endif
