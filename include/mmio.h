#pragma once
/* ================================================================
   include/mmio.h -- memory-mapped device registers, through a seam
   ================================================================
   MMIO_H is a real include guard as well as the #pragma, and it is
   here for exactly the reason IO_H is in include/io.h: so that a
   native test can define it, supply its own accessors, and run a
   driver against a simulated controller built in process memory.

   Every port-I/O driver in this tree reaches hardware through the six
   functions in io.h and nothing else, which is how PCI enumeration and
   the USB controller are tested without booting. MMIO had no such
   seam: drivers/nvme.c and drivers/uhci.c dereference volatile
   pointers directly, so nothing that touches a memory-mapped register
   could be tested at all.

   That mattered little while NVMe was read-only and UHCI's MMIO was
   incidental. It matters now: AHCI is entirely MMIO -- a command list,
   a FIS receive area, command tables and a scatter-gather table, all
   reached through one 4 KB register window -- and writing it blind
   against real hardware, to a disk with somebody's Windows on it, is
   not a thing to do.

   Adopted in drivers/nvme.c first, deliberately. There is already a
   passing suite over that driver, so if these functions are wrong the
   existing tests say so before a new driver is built on top of them.

   volatile everywhere: these are device registers, and the compiler
   must not cache, reorder or elide a single access. A missing volatile
   here is the kind of bug that appears only with optimisation on, only
   on one machine, and looks like a hardware fault.
   ================================================================ */
#ifndef MMIO_H
#define MMIO_H
#include "types.h"

static inline uint32_t mmio_read32(uint32_t base, uint32_t off){
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_write32(uint32_t base, uint32_t off, uint32_t v){
    *(volatile uint32_t *)(base + off) = v;
}
static inline uint16_t mmio_read16(uint32_t base, uint32_t off){
    return *(volatile uint16_t *)(base + off);
}
static inline void mmio_write16(uint32_t base, uint32_t off, uint16_t v){
    *(volatile uint16_t *)(base + off) = v;
}
static inline uint8_t mmio_read8(uint32_t base, uint32_t off){
    return *(volatile uint8_t *)(base + off);
}
static inline void mmio_write8(uint32_t base, uint32_t off, uint8_t v){
    *(volatile uint8_t *)(base + off) = v;
}

/* 64-bit registers are read and written as two 32-bit halves, low first.

   Not because a 64-bit access would be wrong on paper, but because this
   kernel is 32-bit: a `volatile uint64_t *` dereference compiles to two
   32-bit accesses anyway on i386, in an order the compiler chooses, and
   on a register pair where the order matters that is a bug waiting for
   a different compiler version. Written out, the order is ours.

   Low half first on read and on write is what the NVMe specification
   requires for its 64-bit queue base registers, and what AHCI's command
   list and FIS base pairs expect too. */
static inline uint64_t mmio_read64(uint32_t base, uint32_t off){
    uint32_t lo = mmio_read32(base, off);
    uint32_t hi = mmio_read32(base, off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
static inline void mmio_write64(uint32_t base, uint32_t off, uint64_t v){
    mmio_write32(base, off,     (uint32_t)v);
    mmio_write32(base, off + 4, (uint32_t)(v >> 32));
}

#endif /* MMIO_H */
