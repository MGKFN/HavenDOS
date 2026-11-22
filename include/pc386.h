#pragma once
#include "types.h"
#include "emu386.h"

/* ════════════════════════════════════════════════════════════════════
   include/pc386.h -- the PC that Retro PC's 32-bit CPU lives in.
   v0.9.7.2.

   A machine of about 1995, as much of it as a kernel of the BOOT OS
   line touches: two 8259 PICs, the 8254 timer, the 8042 keyboard
   controller (keyboard and PS/2 mouse), the CMOS clock, VGA text mode
   at B8000h, and the absence of everything else -- no PCI devices, no
   disks, no network. A driver that probes for one finds nothing there,
   which every one of those builds already handles, because that is
   what QEMU without the device looks like.

   THERE IS NO GRUB. Every BOOT OS ISO is GRUB plus a Multiboot kernel,
   and GRUB itself needs a BIOS, a CD drive and real mode. So this does
   GRUB's job instead: it reads /boot/grub/grub.cfg off the ISO, offers
   its menu entries, and loads the chosen `multiboot` kernel the way
   GRUB would -- ELF segments at their addresses, a Multiboot
   information structure with memory and the command line, and the CPU
   in flat protected mode at the entry point. The kernel cannot tell.

   Like emu/pcbios.c it knows nothing about HavenDOS: the ISO is a
   buffer, time and the date come from the caller.
   ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t (*now_ms)(void *ctx);
    void     (*rtc)(void *ctx, int *y, int *mo, int *d, int *h, int *mi, int *s);
} pc386_host_t;

typedef struct {
    uint8_t irr, isr, imr, base;
    uint8_t init;          /* ICW step: 0 not initialising, 2..4 the next ICW */
    uint8_t icw4, read_isr;
} pic_t;

#define PC386_ENTRY_MAX  12
#define PC386_NOTICE_LEN 80

typedef struct {
    char title[64];
    char path[64];
    char args[96];
    int  mb2;              /* multiboot2: a 64-bit kernel, not run here */
} pc386_entry_t;

typedef struct {
    cpu386_t cpu;
    uint8_t *mem;  uint32_t mem_size;
    const pc386_host_t *host; void *hctx;

    pic_t    pic[2];

    /* 8254 */
    uint16_t pit_reload[3], pit_latch[3];
    uint8_t  pit_mode[3], pit_access[3], pit_lohi_w[3], pit_lohi_r[3], pit_latched[3];
    uint32_t pit_last_ms, pit_acc_us;
    uint8_t  port61;

    /* 8042 */
    uint8_t  kbc_cmd, kbc_status_aux;
    uint8_t  kbc_ccb;                 /* controller command byte */
    uint8_t  kbc_next;                /* what the next write to 60h is for */
    uint8_t  kbd_next;                /* a keyboard command waiting for its data byte */
    uint8_t  aux_next;
    uint8_t  aux_enabled, kbd_disabled;
    uint8_t  outq[64]; uint8_t outq_aux[64]; int oq_head, oq_tail;   /* replies and scancodes */
    uint8_t  obf, obf_aux, ob;
    uint8_t  keyq[64]; int kq_head, kq_tail;                          /* host keys, as bytes */

    /* CMOS */
    uint8_t  cmos_idx;

    /* VGA */
    uint8_t  crtc_idx, crtc[32], gc_idx, gc[16], seq_idx, seq[8], attr_idx, attr_flip, attr[32];
    uint8_t  retrace;

    uint32_t pci_addr;
    int      stop;                     /* 1 reboot, 2 power off */
    char     notice[PC386_NOTICE_LEN];

    /* what was booted */
    char     kernel[64];
    uint32_t entry;
} pc386_t;

/* What pc386_run() returns, besides the X86_ codes it passes through. */
#define PC386_STOPPED 10

/* The menu of an ISO: its grub.cfg's entries. Returns how many, or -1
   with a reason in why (not an ISO 9660 image, no grub.cfg ...). */
int  pc386_iso_menu(const uint8_t *iso, uint32_t size, pc386_entry_t *out, int max,
                    const char **why);

/* the same, from a grub.cfg's text -- for a built-in image */
int  pc386_cfg_menu(const char *cfg, int len, pc386_entry_t *out, int max, const char **why);

int  pc386_init(pc386_t *p, uint8_t *mem, uint32_t mem_size,
                const pc386_host_t *host, void *hctx);
/* Load entry e's kernel from the ISO and start it. 0, or -1 with notice. */
int  pc386_boot(pc386_t *p, const uint8_t *iso, uint32_t size, const pc386_entry_t *e);
/* the same, from the kernel's bytes -- for a built-in image */
int  pc386_boot_kernel(pc386_t *p, const uint8_t *k, int klen, const pc386_entry_t *e);
int  pc386_run(pc386_t *p, uint32_t budget);
void pc386_key(pc386_t *p, int key);          /* a HavenDOS key code */

typedef struct {
    const uint8_t *cells;              /* char, attr pairs, 80x25        */
    int cols, rows;
    int cx, cy, cursor_on;
    int graphics;                      /* the guest switched to a graphics mode */
} pc386_screen_t;
void pc386_screen(const pc386_t *p, pc386_screen_t *s);

/* for tests: read a file off an ISO by path. Bytes, or -1. */
int  pc386_iso_read(const uint8_t *iso, uint32_t size, const char *path,
                    const uint8_t **data);
