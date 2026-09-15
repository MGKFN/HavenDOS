/* ============================================================
   BOOT OS - Boot Entry
   Supports: Multiboot1, Multiboot2, UTM SE (iPad QEMU EFI)
   ============================================================ */

/* Multiboot 1 constants */
.set MB1_MAGIC,     0x1BADB002
.set MB1_FLAGS,     0x00000003
.set MB1_CHECKSUM,  -(MB1_MAGIC + MB1_FLAGS)

/* Multiboot 2 constants */
.set MB2_MAGIC,     0xe85250d6
.set MB2_ARCH,      0
.set MB2_HDRLEN,    (mb2_end - mb2_start)
.set MB2_CHECKSUM,  -(MB2_MAGIC + MB2_ARCH + MB2_HDRLEN)

/* GDT selectors */
.set SEG_CODE,  0x08
.set SEG_DATA,  0x10

/* ---- Multiboot 1 header ---- */
.section .multiboot
.align 4
mb1_start:
    .long  MB1_MAGIC
    .long  MB1_FLAGS
    .long  MB1_CHECKSUM

/* ---- Multiboot 2 header (8-byte aligned after MB1) ---- */
.align 8
mb2_start:
    .long  MB2_MAGIC
    .long  MB2_ARCH
    .long  MB2_HDRLEN
    .long  MB2_CHECKSUM
    /* end tag */
    .align 8
    .word  0
    .word  0
    .long  8
mb2_end:

/* ---- GDT ---- */
.section .data
.align 8
gdt_start:
    .quad  0x0000000000000000   /* null */
    .quad  0x00CF9A000000FFFF   /* code: base=0 limit=4G ring0 32bit */
    .quad  0x00CF92000000FFFF   /* data: base=0 limit=4G ring0 32bit */
gdt_end:

gdt_ptr:
    .word  gdt_end - gdt_start - 1
    .long  gdt_start

/* ---- Stack ---- */
.section .bss
.align 16
stack_bottom:
    .skip  262144
stack_top:

/* ---- Entry point ---- */
.section .text
.global _start
.type   _start, @function
_start:
    cli

    /* Save magic + info ptr before we clobber registers */
    movl   %eax, boot_magic
    movl   %ebx, boot_mbinfo

    /* Load our GDT */
    lgdtl  gdt_ptr

    /* Reload data segments */
    movw   $SEG_DATA, %ax
    movw   %ax, %ds
    movw   %ax, %es
    movw   %ax, %fs
    movw   %ax, %gs
    movw   %ax, %ss

    /* Far jump to reload CS */
    ljmpl  $SEG_CODE, $.Lflush
.Lflush:

    /* Stack */
    movl   $stack_top, %esp

    /* Clear eflags */
    pushl  $0
    popf

    /* Call kernel_main(magic, mb_info) */
    pushl  boot_mbinfo
    pushl  boot_magic
    call   kernel_main

.Lhalt:
    cli
    hlt
    jmp    .Lhalt

/* Storage for magic/mbinfo (saved before GDT reload) */
.section .data
boot_magic:  .long 0
boot_mbinfo: .long 0
