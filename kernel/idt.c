#include "../include/types.h"
#include "../include/io.h"
#include "../include/vga.h"
#include "../include/appguard.h"

typedef struct {
    uint16_t low;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

static void idt_set(int n, uint32_t handler) {
    idt[n].low   = handler & 0xFFFF;
    idt[n].sel   = 0x08;
    idt[n].zero  = 0;
    idt[n].flags = 0x8E;   /* present, ring 0, 32-bit interrupt gate */
    idt[n].high  = (handler >> 16) & 0xFFFF;
}

/* ---- ISR/IRQ stubs ---- */
/* Note: NO 'sti' before iret - iret restores eflags which re-enables IF */
__asm__(
".macro ISR_NOERR num\n"
".global isr\\num\n"
"isr\\num:\n"
"  push $0\n"
"  push $\\num\n"
"  jmp isr_common_stub\n"
".endm\n"

".macro ISR_ERR num\n"
".global isr\\num\n"
"isr\\num:\n"
"  push $\\num\n"
"  jmp isr_common_stub\n"
".endm\n"

".macro IRQ entry vec\n"
".global irq\\entry\n"
"irq\\entry:\n"
"  push $0\n"
"  push $\\vec\n"
"  jmp irq_common_stub\n"
".endm\n"

"ISR_NOERR 0\n"  "ISR_NOERR 1\n"  "ISR_NOERR 2\n"  "ISR_NOERR 3\n"
"ISR_NOERR 4\n"  "ISR_NOERR 5\n"  "ISR_NOERR 6\n"  "ISR_NOERR 7\n"
"ISR_ERR   8\n"  "ISR_NOERR 9\n"  "ISR_ERR   10\n" "ISR_ERR   11\n"
"ISR_ERR   12\n" "ISR_ERR   13\n" "ISR_ERR   14\n" "ISR_NOERR 15\n"

"IRQ  0  32\n"  "IRQ  1  33\n"  "IRQ  2  34\n"  "IRQ  3  35\n"
"IRQ  4  36\n"  "IRQ  5  37\n"  "IRQ  6  38\n"  "IRQ  7  39\n"
"IRQ  8  40\n"  "IRQ  9  41\n"  "IRQ 10  42\n"  "IRQ 11  43\n"
"IRQ 12  44\n"  "IRQ 13  45\n"  "IRQ 14  46\n"  "IRQ 15  47\n"

"isr_common_stub:\n"
"  pusha\n"
"  mov  %ds, %ax\n"
"  push %eax\n"
"  mov  $0x10, %ax\n"
"  mov  %ax, %ds\n"
"  mov  %ax, %es\n"
"  mov  %ax, %fs\n"
"  mov  %ax, %gs\n"
/* The frame pointer isr_handler takes as its argument. Without this the
   call passed no argument at all, so `r` was whatever sat above the
   return address -- the saved DS selector, 0x10 -- and every field the
   panic screen printed was read from low memory instead of the fault.
   The exception NAME comes from r->int_no, so the screen did not even
   report which exception had happened: a panic on real hardware showed
   "Divide By Zero", EIP 0x0, and registers holding fragments of GRUB's
   GCC version string. Nothing in the kernel noticed, because HavenDOS
   polls its devices and nothing depends on this path until something
   faults. */
"  push %esp\n"
"  call isr_handler\n"
"  add  $4, %esp\n"
"  pop  %eax\n"
"  mov  %ax, %ds\n"
"  mov  %ax, %es\n"
"  mov  %ax, %fs\n"
"  mov  %ax, %gs\n"
"  popa\n"
"  add  $8, %esp\n"
"  iret\n"         /* iret restores EFLAGS.IF automatically */

"irq_common_stub:\n"
"  pusha\n"
"  mov  %ds, %ax\n"
"  push %eax\n"
"  mov  $0x10, %ax\n"
"  mov  %ax, %ds\n"
"  mov  %ax, %es\n"
"  mov  %ax, %fs\n"
"  mov  %ax, %gs\n"
/* The same missing argument as isr_common_stub, and this one ran a
   hundred times a second rather than only on a fault.

   irq_handler() reads r->int_no to decide which IRQ fired and then calls
   irq_handlers[irq]() -- so with `r` pointing at the saved DS selector
   instead of the frame, the index came out of low memory and the kernel
   was picking a function pointer out of an array by lottery, on every
   timer tick. It also meant IRQ0 almost never reached pit_tick(), so
   pit_overflows -- which boot_ms() reads as one of its two clocks --
   stayed near zero and the polled clock carried everything alone.

   Nothing depended on IRQs working, so nothing reported them broken. */
"  push %esp\n"
"  call irq_handler\n"
"  add  $4, %esp\n"
"  pop  %eax\n"
"  mov  %ax, %ds\n"
"  mov  %ax, %es\n"
"  mov  %ax, %fs\n"
"  mov  %ax, %gs\n"
"  popa\n"
"  add  $8, %esp\n"
"  iret\n"
);

/* ---- Registers struct (matches stub stack frame) ---- */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

/* ---- IRQ handler table ---- */
typedef void (*irq_cb_t)(void);
static irq_cb_t irq_handlers[16];

void irq_register(int n, irq_cb_t h) { if(n>=0&&n<16) irq_handlers[n]=h; }

extern void pit_tick(void);   /* in pit.c */
extern uint32_t get_ticks(void);  /* in pit.c */
extern void kpoll_tick(void); /* in kpoll.c -- counts and flags, no work */

/* ── v0.8.8 "Helios": WHAT IRQ0 DOES, AND WHAT IT MUST NOT ───────────
 *
 * Two counters and a handful of compares.  pit_tick() increments
 * pit_overflows, which boot_ms() reads as one of its two clocks;
 * kpoll_tick() increments its own tick count and sets the due flags
 * kernel_poll() reads.  Neither does any work, and that is the whole
 * design -- include/kpoll.h has the argument, and it comes down to
 * every layer in this tree sharing module-level buffers that are not
 * reentrant.
 *
 * NOTHING CALLED FROM HERE MAY READ THE POLLED CLOCK.  get_ticks() goes
 * through pit_us(), which mutates last_pit_val and us_frac and is
 * itself non-reentrant: an IRQ0 that read the clock would corrupt the
 * clock.  It is declared above and deliberately not called; v0.7.12's
 * two pit_us() defects are what that function's header is about, and
 * the second one -- an overflow after a wrap -- is reachable precisely
 * when two readers interleave.
 *
 * kpoll_tick() is called unconditionally for IRQ0, before the
 * registered handler rather than after, so a handler that spends time
 * cannot delay the flags.  tools/check_kpoll_sites.py refuses a call
 * to kernel_poll() from this file. */
void irq_handler(regs_t *r) {
    int irq = (int)r->int_no - 32;
    if(irq == 0){ pit_tick(); kpoll_tick(); }
    if(irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq]();
    /* Send EOI */
    if(irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* ---- Exception handler ---- */
static const char *exc_names[] = {
    "Divide By Zero",        "Debug",              "NMI",
    "Breakpoint",            "Overflow",           "Bound Range",
    "Invalid Opcode",        "Device Not Available","Double Fault",
    "Coprocessor Overrun",   "Invalid TSS",        "Segment Not Present",
    "Stack Fault",           "General Protection", "Page Fault",
    "Reserved"
};

void isr_handler(regs_t *r) {
    /* Disable interrupts and show panic screen */
    __asm__ volatile("cli");

    /* ── AN APP'S FAULT IS NOT THE SYSTEM'S, v0.8.8.4 ─────────────────
       If the desktop armed a guard around an app, hand the fault to
       kernel/appguard.c, which restores the desktop's stack and
       returns there instead of halting. include/appguard.h has the
       limits -- the short version is that this catches the fault and
       not the damage, because there is no memory protection here.

       #DF IS EXCLUDED ON PURPOSE. A double fault means the CPU could
       not deliver an earlier exception, usually because the stack is
       gone, and a recovery that needs a working stack cannot be the
       answer to not having one. That one still panics. */
    if(r->int_no != 8 && appguard_armed())
        appguard_fault(r->int_no, r->eip);   /* does not return */

    vga_set_color(VGA_WHITE, VGA_RED);
    /* ── v0.8.8: THE WHOLE GRID, not 80x25 ────────────────────────────
       This painted a fixed 80x25 rectangle.  Correct while that was the
       largest grid there was; on a 1080p framebuffer console at 240x135
       it is a small red box in the top-left sixth of a screen still
       showing whatever was on it, with the register dump inside the box
       and the rest of the panel holding the desktop.
       Found by tools/check_console_grid.py's sibling rule while that
       check was being written, and it is the one screen in the system
       that absolutely must not be half-drawn. */
    for(int y=0;y<vga_rows();y++) for(int x=0;x<vga_cols();x++)
        vga_putchar_at(' ',x,y,VGA_WHITE,VGA_RED);
    vga_set_cursor(0,0);
    vga_puts("================================================================================");
    vga_puts("                           *** KERNEL PANIC ***                                 ");
    vga_puts("================================================================================");
    vga_set_cursor(2,4);
    if(r->int_no < 16)
        vga_printf("Exception: %s  (#%u)", exc_names[r->int_no], r->int_no);
    else
        vga_printf("Exception #%u", r->int_no);
    vga_set_cursor(2,6);
    vga_printf("Error Code : 0x%x", r->err_code);
    vga_set_cursor(2,7);
    vga_printf("EIP        : 0x%x", r->eip);
    vga_set_cursor(2,8);
    vga_printf("CS         : 0x%x  EFLAGS: 0x%x", r->cs, r->eflags);
    vga_set_cursor(2,10);
    vga_printf("EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x",
               r->eax, r->ebx, r->ecx, r->edx);
    vga_set_cursor(2,11);
    vga_printf("ESI=0x%x  EDI=0x%x  EBP=0x%x",
               r->esi, r->edi, r->ebp);
    vga_set_cursor(2,14);
    vga_puts("System halted. Please reboot.");
    for(;;) __asm__ volatile("hlt");
}

/* ---- Forward decls ---- */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

void idt_init(void) {
    /* Zero IDT */
    for(int i=0;i<256;i++) {
        idt[i].low=0; idt[i].sel=0x08;
        idt[i].zero=0; idt[i].flags=0; idt[i].high=0;
    }

    /* Exceptions */
    idt_set(0,(uint32_t)isr0);   idt_set(1,(uint32_t)isr1);
    idt_set(2,(uint32_t)isr2);   idt_set(3,(uint32_t)isr3);
    idt_set(4,(uint32_t)isr4);   idt_set(5,(uint32_t)isr5);
    idt_set(6,(uint32_t)isr6);   idt_set(7,(uint32_t)isr7);
    idt_set(8,(uint32_t)isr8);   idt_set(9,(uint32_t)isr9);
    idt_set(10,(uint32_t)isr10); idt_set(11,(uint32_t)isr11);
    idt_set(12,(uint32_t)isr12); idt_set(13,(uint32_t)isr13);
    idt_set(14,(uint32_t)isr14); idt_set(15,(uint32_t)isr15);

    /* IRQs (remapped to 0x20-0x2F) */
    idt_set(32,(uint32_t)irq0);  idt_set(33,(uint32_t)irq1);
    idt_set(34,(uint32_t)irq2);  idt_set(35,(uint32_t)irq3);
    idt_set(36,(uint32_t)irq4);  idt_set(37,(uint32_t)irq5);
    idt_set(38,(uint32_t)irq6);  idt_set(39,(uint32_t)irq7);
    idt_set(40,(uint32_t)irq8);  idt_set(41,(uint32_t)irq9);
    idt_set(42,(uint32_t)irq10); idt_set(43,(uint32_t)irq11);
    idt_set(44,(uint32_t)irq12); idt_set(45,(uint32_t)irq13);
    idt_set(46,(uint32_t)irq14); idt_set(47,(uint32_t)irq15);

    /* Load IDT */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    __asm__ volatile("lidt (%0)" :: "r"(&idt_ptr) : "memory");

    /* Remap PIC: IRQ0-7 -> INT 0x20-0x27, IRQ8-15 -> INT 0x28-0x2F */
    outb(0x20,0x11); io_wait();
    outb(0xA0,0x11); io_wait();
    outb(0x21,0x20); io_wait();
    outb(0xA1,0x28); io_wait();
    outb(0x21,0x04); io_wait();
    outb(0xA1,0x02); io_wait();
    outb(0x21,0x01); io_wait();
    outb(0xA1,0x01); io_wait();

    /* Mask ALL IRQs initially - unmask only what we need */
    outb(0x21,0xFF);
    outb(0xA1,0xFF);

    /* Interrupts stay OFF until caller does sti */
}

/* Call after IDT is loaded to enable just timer + keyboard */
void irq_enable_basic(void) {
    outb(0x21, 0xFC);   /* unmask IRQ0 (timer) + IRQ1 (keyboard) */
    outb(0xA1, 0xFF);   /* keep slave masked */
    __asm__ volatile("sti");
}

/* ── Enable mouse (IRQ12 via slave PIC) ──────────────────────────────
 *
 * v0.8.8, AND IT IS NOT WHAT THE BOOT LOG SAID.  The line
 *
 *     ok  PS/2: keyboard on IRQ1, mouse on IRQ12
 *
 * was wrong, and the reason it was wrong is NOT the reason that was
 * written down.  The note it came from said the slave PIC stays masked
 * and so IRQ12 cannot fire.  That is false: this function unmasks
 * IRQ12 and the cascade on IRQ2, and kernel/main.c calls it on the
 * line immediately before printing that log entry.
 *
 * THREE layers decide whether an IRQ12 reaches a handler, and the PIC
 * is only the middle one:
 *
 *   1. the 8042 controller must GENERATE it.  drivers/mouse.c's
 *      mouse_init() clears bit 1 of the config byte -- "IRQ12 stays
 *      off, we poll instead" -- so it never does.
 *   2. the PIC must not mask it.  This function unmasks it.
 *   3. something must be REGISTERED for it.  Nothing is:
 *      drivers/keyboard.c is the only caller of irq_register() in the
 *      tree and it registers IRQ1.
 *
 * So the mouse is polled, the log line was fiction, and a note that
 * blamed layer 2 would have sent somebody to edit the one layer that
 * was innocent.  A recorded gap is a hypothesis, not a finding --
 * v0.8.7.6 wrote down that it could not tell tx_seq from rx_seq and
 * was right that the check was impossible and wrong about why.
 *
 * The unmask STAYS, and that is a deliberate choice rather than
 * neglect.  It costs nothing, a spurious IRQ12 is already handled
 * correctly (no handler, EOI to both PICs, return), and removing it
 * would be a third edit in a release whose subject is that this area
 * had one claim too many.  tools/check_boot_log.py now refuses a boot
 * log line that claims an IRQ without BOTH an unmask and an
 * irq_register(), so the claim cannot come back while layer 3 is
 * missing. */
void irq_enable_mouse(void) {
    outb(0xA1, inb(0xA1) & ~(1<<4));  /* unmask IRQ12 */
    outb(0x21, inb(0x21) & ~(1<<2));  /* unmask IRQ2 (cascade) */
}
