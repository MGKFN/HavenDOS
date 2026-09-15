#include "../include/types.h"
#include "../include/io.h"
#include "../include/vga.h"

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
"  call isr_handler\n"
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
"  call irq_handler\n"
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

void irq_handler(regs_t *r) {
    int irq = (int)r->int_no - 32;
    if(irq == 0) pit_tick();  /* bonus tick count when IRQ0 fires */
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
    vga_set_color(VGA_WHITE, VGA_RED);
    /* Clear screen red */
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
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

/* Enable mouse (IRQ12 via slave PIC) */
void irq_enable_mouse(void) {
    outb(0xA1, inb(0xA1) & ~(1<<4));  /* unmask IRQ12 */
    outb(0x21, inb(0x21) & ~(1<<2));  /* unmask IRQ2 (cascade) */
}
