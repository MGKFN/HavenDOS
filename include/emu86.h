#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/emu86.h -- an 8086/80186 interpreter.  v0.9.7.

   Retro PC runs Kyan's oldest surviving builds -- the 16-bit KOS/KDOS
   line that came before BOOT OS -- inside a HavenDOS window. Those are
   real-mode boot sectors written against a PC BIOS, and HavenDOS is a
   32-bit protected-mode kernel, so they cannot simply be jumped to.

   This is the CPU. It knows nothing about HavenDOS, the screen or the
   disk: memory is a buffer the caller owns, I/O ports and the BIOS are
   callbacks. That is what lets tests/test_emu86.c compile it natively
   and boot K-DOS Beta 1.1b without a kernel, and what keeps a guest
   that misbehaves from reaching anything but its own megabyte.

   emu/pcbios.c is the PC around it: the BIOS, the timer, the keyboard
   and the disks. shell/retro.c is the window.

   ── WHY AN INTERPRETER AND NOT VIRTUAL-8086 MODE ────────────────────
   The CPU in every machine HavenDOS runs on can execute this code
   itself, in v8086 mode. That needs a TSS, a monitor for every
   privileged instruction and every interrupt, and it runs on the real
   processor -- one mistake in the monitor is a triple fault on the HP.
   An interpreter is slower, which does not matter for a 1.7 KB shell
   that spends its life waiting for a key, and it can be tested on the
   build machine instruction by instruction.
   ════════════════════════════════════════════════════════════════════ */

#define EMU86_MEM   0x100000u        /* 1 MB: real mode, A20 off       */
#ifndef EMU86_ROM
#define EMU86_ROM   0xF0000u         /* writes at and above are ignored */
#endif

/* registers, in the order the instruction set encodes them */
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

/* flags */
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800
/* bits 12-15 read as 1 on an 8086 and an 80186; software tells the
   processors apart by exactly this. Bit 1 is always 1. */
#define F_FIXED 0xF002

/* what emu86_run() stopped for */
#define EMU_BUDGET  0   /* ran the instructions it was given             */
#define EMU_WAIT    1   /* a BIOS call is waiting (a key, a delay)       */
#define EMU_HALT    2   /* HLT with interrupts off: it will never wake   */
#define EMU_FAULT   3   /* an instruction this CPU does not have         */
#define EMU_IDLE    4   /* HLT with interrupts on: wake it with an IRQ   */
#define EMU_REBOOT  5   /* the guest asked for INT 19h                   */

/* what a BIOS trap returns */
#define BIOS_DONE   0
#define BIOS_WAIT   1   /* not yet: run the same call again later        */
#define BIOS_REBOOT 2

typedef struct emu86 emu86_t;

typedef struct {
    uint8_t (*in8)(void *ctx, uint16_t port);
    void    (*out8)(void *ctx, uint16_t port, uint8_t v);
    /* A BIOS service. The ROM's vector stubs execute the trap 0F FF nn
       (only in segment F000, so guest code cannot forge one) and this
       is called with the guest's registers as they were at the INT.
       The flags the INT pushed are at SS:SP+4; a service that returns
       a flag (CF for errors, ZF for "no key") changes them there,
       which is what a real BIOS does. */
    int     (*bios)(void *ctx, emu86_t *e, uint8_t vector);
} emu86_host_t;

struct emu86 {
    uint8_t  *mem;                 /* EMU86_MEM bytes, owned by caller */
    uint16_t  r[8];
    uint16_t  sr[6];                /* ES CS SS DS, and the 386's FS GS */
    uint16_t  ip, flags;

    const emu86_host_t *host;
    void     *ctx;

    /* hardware interrupts: a line raised by emu86_irq() is delivered at
       the next instruction boundary with IF set, unless it is still in
       service -- the guest (or the ROM) ends service with an EOI to
       port 0x20, as it would on a real 8259. */
    uint8_t   irq_pending, irq_service, irq_mask;

    /* why it stopped, when it was a fault */
    uint16_t  fault_cs, fault_ip;
    uint8_t   fault_op[4];
    const char *fault_why;

    uint32_t  icount;              /* instructions retired            */
    int       halted;

    /* decode state of the instruction being executed */
    int       seg;                 /* segment override, -1 none       */
    int       rep;                 /* 0, 0xF2 or 0xF3                 */
    uint16_t  op_cs, op_ip;        /* where it started               */
};

void     emu86_reset(emu86_t *e, uint8_t *mem, const emu86_host_t *h, void *ctx);
int      emu86_run(emu86_t *e, uint32_t budget);
void     emu86_irq(emu86_t *e, int line);
void     emu86_eoi(emu86_t *e);

/* guest memory, for the BIOS and the screen */
uint8_t  emu86_rd8 (const emu86_t *e, uint32_t a);
uint16_t emu86_rd16(const emu86_t *e, uint32_t a);
void     emu86_wr8 (emu86_t *e, uint32_t a, uint8_t v);
void     emu86_wr16(emu86_t *e, uint32_t a, uint16_t v);
static inline uint32_t emu86_lin(uint16_t seg, uint16_t off){
    return (((uint32_t)seg << 4) + off) & (EMU86_MEM - 1);
}
/* set or clear a flag in the image an INT pushed, at SS:SP+4 */
void     emu86_ret_flag(emu86_t *e, uint16_t flag, int on);
