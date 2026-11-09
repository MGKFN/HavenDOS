/* ============================================================
   BOOT OS - Boot Entry
   Supports: Multiboot1, Multiboot2, UTM SE (iPad QEMU EFI)
   ============================================================ */

/* Multiboot 1 constants
   bit 0 align modules, bit 1 memory map, bit 2 VIDEO MODE.

   Bit 2 is what asks GRUB for a linear framebuffer.  Without it GRUB leaves
   the card in VGA text mode and the kernel can only write 0xB8000 -- which
   is 720x400 on a BIOS machine and does not exist at all under UEFI.  With
   it, the whole OS renders through the framebuffer console in vga.c.

   Requesting video means the header must carry the five address fields as
   well, because the video fields that follow them live at fixed offsets. */
.set MB1_MAGIC,     0x1BADB002
.set MB1_FLAGS,     0x00000007
.set MB1_CHECKSUM,  -(MB1_MAGIC + MB1_FLAGS)
/* ── THE MODE THE KERNEL ASKS FOR, AND WHY IT IS NOT A LIST ──────────
   v0.8.8.7, recorded because the tree believed something false.

   GRUB honours the mode in THIS HEADER above `gfxmode` and
   `gfxpayload`. So the list added to isodir/boot/grub/grub.cfg in
   v0.8.8.6 --
       set gfxmode=1920x1080,1600x900,...,1024x768,800x600,auto
   -- styles GRUB'S OWN MENU and never reaches the kernel. The kernel
   has asked for 1280x800 on every boot since the field was written.

   That is also the real cause of the 64x24 console. 1280/16 x 800/32
   is exactly 80x25, so on any machine that can do 1280x800 -- QEMU
   included -- the grid came out right and nothing looked wrong for
   three releases. The HP cannot, GRUB fell back to its "auto", that
   landed on 1024x768, and 1024/16 x 768/32 is 64x24.

   Setting these to 0 ("no preference", which both Multiboot 1 and the
   Multiboot 2 framebuffer tag define) was tried and is WORSE: GRUB
   does not then consult gfxmode, it goes straight to "auto", and
   under QEMU that produced 800x600 -- measured, not assumed. So the
   request stays explicit, and the console copes with whatever
   actually arrives (drivers/vga.c, vga_fb_app_grid).

   The DEPTH is not a preference but a requirement: the framebuffer
   console only writes 32bpp pixels. */
.set FB_WIDTH,      1280
.set FB_HEIGHT,     800
.set FB_DEPTH,      32

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
    .long  0, 0, 0, 0, 0      /* header_addr..entry_addr, unused (bit 16 clear) */
    .long  0                  /* mode_type 0 = linear graphics              */
    .long  FB_WIDTH
    .long  FB_HEIGHT
    .long  FB_DEPTH

/* ---- Multiboot 2 header (8-byte aligned after MB1) ---- */
.align 8
mb2_start:
    .long  MB2_MAGIC
    .long  MB2_ARCH
    .long  MB2_HDRLEN
    .long  MB2_CHECKSUM
    /* framebuffer tag — the Multiboot 2 spelling of the same request */
    .align 8
    .word  5                  /* tag type 5: framebuffer */
    .word  0
    .long  20
    .long  FB_WIDTH
    .long  FB_HEIGHT
    .long  FB_DEPTH
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

/* Mark the stack non-executable.  Without this section ld emits
   "missing .note.GNU-stack section implies executable stack" and
   conservatively marks the whole image's stack executable. */
.section .note.GNU-stack,"",@progbits
