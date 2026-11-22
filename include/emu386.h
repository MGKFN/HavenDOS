#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/emu386.h -- a 32-bit x86 interpreter for Retro PC.  v0.9.7.2.

   Kyan, 22 Nov: "older isos not. I wish to use those to within
   HavenDOS". His BOOT OS ISOs -- 0.3.1, 0.5.0, 0.5.2, the line that
   became HavenDOS -- are 32-bit protected-mode kernels started by GRUB.
   emu/cpu86.c is an 8086 and cannot run a single instruction of them.
   This is the CPU that can.

   WHAT IT IS: the 386's protected mode as a flat kernel uses it, which
   is how every one of those builds uses it -- ring 0, a GDT whose
   segments are loaded and honoured (base only), an IDT through which
   exceptions and hardware interrupts are delivered, no paging. The
   integer instruction set gcc -m32 emits, plus the 486/686 additions
   it reaches for (CMOVcc, BSWAP, XADD, CMPXCHG, CPUID, RDTSC). No FPU:
   none of the builds has a single x87 instruction, so an escape opcode
   is consumed and does nothing.

   WHAT IT IS NOT: a virtual machine of the real CPU. It is an
   interpreter, for the reason include/emu86.h gives for the 8086 --
   a guest that goes wrong can reach nothing but its own buffer, and
   it can be tested on the build machine against the build machine's
   own processor (tests/test_emu386.c does, flag for flag).

   Memory is linear = physical, mem_size bytes the caller owns. Reads
   past the end are 0xFF and writes there are dropped, the way an
   empty bus behaves; a kernel that probes for memory finds the end.
   ════════════════════════════════════════════════════════════════════ */

enum { X_EAX, X_ECX, X_EDX, X_EBX, X_ESP, X_EBP, X_ESI, X_EDI };
enum { X_ES, X_CS, X_SS, X_DS, X_FS, X_GS };

#define X_CF 0x0001u
#define X_PF 0x0004u
#define X_AF 0x0010u
#define X_ZF 0x0040u
#define X_SF 0x0080u
#define X_TF 0x0100u
#define X_IF 0x0200u
#define X_DF 0x0400u
#define X_OF 0x0800u

/* what cpu386_run() stopped for */
#define X86_BUDGET 0    /* ran the instructions it was given            */
#define X86_IDLE   1    /* HLT with interrupts on: an IRQ will wake it   */
#define X86_HALT   2    /* HLT with interrupts off: it never will        */
#define X86_FAULT  3    /* a triple fault, or an exception with no IDT   */

typedef struct cpu386 cpu386_t;

typedef struct {
    uint32_t (*in)(void *ctx, uint16_t port, int size);          /* 1 2 4 */
    void     (*out)(void *ctx, uint16_t port, uint32_t v, int size);
    /* the vector of the interrupt the PIC would deliver now, taking it
       (INTA); -1 if none. Only asked when IF is set. */
    int      (*intack)(void *ctx);
} cpu386_host_t;

struct cpu386 {
    uint8_t  *mem;
    uint32_t  mem_size;
    uint32_t  r[8];
    uint32_t  eip, eflags;
    uint16_t  sel[6];
    uint32_t  base[6];
    uint32_t  cr0, cr2, cr3, cr4;
    uint32_t  gdt_base, idt_base;
    uint16_t  gdt_lim, idt_lim;

    const cpu386_host_t *host;
    void     *ctx;

    int       halted;
    int       intr;       /* the host says an interrupt may be waiting:
                             intack() is only asked when this is set   */
    uint32_t  icount;
    uint64_t  tsc;

    /* why it stopped, when it was a fault */
    uint32_t  fault_eip;
    int       fault_vec;
    const char *fault_why;

    /* decode state of the instruction being executed */
    uint32_t  op_eip;
    int       seg;        /* segment override, -1 none */
    int       rep;        /* 0, 0xF2 or 0xF3 */
    int       o16;        /* 0x66 seen: 16-bit operands */
    int       a16;        /* 0x67 seen: 16-bit addressing */
    int       exc;        /* an exception raised by this instruction, -1 none */
    uint32_t  exc_err;
    int       exc_has_err;
    int       nested;     /* exceptions while delivering one */
};

void     cpu386_reset(cpu386_t *c, uint8_t *mem, uint32_t mem_size,
                      const cpu386_host_t *h, void *ctx);
/* Protected mode, flat 4 GB code and data, interrupts off: what a
   Multiboot loader hands a kernel. */
void     cpu386_flat(cpu386_t *c, uint32_t eip);
int      cpu386_run(cpu386_t *c, uint32_t budget);

uint8_t  cpu386_rd8 (const cpu386_t *c, uint32_t a);
uint32_t cpu386_rd32(const cpu386_t *c, uint32_t a);
void     cpu386_wr8 (cpu386_t *c, uint32_t a, uint8_t v);
void     cpu386_wr32(cpu386_t *c, uint32_t a, uint32_t v);

/* for tests/test_emu386.c: one ALU operation and the flags it leaves,
   exactly as an instruction of that operand size would. op is the
   /digit of opcodes 80-83 (0 add .. 7 cmp); size 1, 2 or 4. */
uint32_t cpu386_alu(cpu386_t *c, int op, uint32_t a, uint32_t b, int size);
uint32_t cpu386_shift(cpu386_t *c, int op, uint32_t a, uint8_t n, int size);
