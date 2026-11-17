#pragma once
#include "types.h"
#include "emu86.h"

/* ════════════════════════════════════════════════════════════════════
   include/pcbios.h -- the PC that Retro PC's 8086 lives in.  v0.9.7.

   emu/pcbios.c: a BIOS ROM with real interrupt vectors, a timer tick,
   a keyboard controller, a floppy, a hard disk and a text screen.
   Like the CPU it knows nothing about HavenDOS: the time, the date and
   the disks' storage come from the caller, which is shell/retro.c in
   the kernel and tests/test_emu86.c on the build machine.

   ── THE DISKS ARE NEVER THE REAL ONES ───────────────────────────────
   K-DOS Beta 1.1b's `save` and `format` write CHS sector 10 of drive
   80h with INT 13h. On the PC it was written for, that was the hard
   disk. Here drive 80h is a buffer the caller hands in -- Retro PC
   keeps it as a file -- and there is no path from this file to a
   block device. A guest can destroy its own floppy and its own
   pretend hard disk and nothing else.
   ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t (*now_ms)(void *ctx);              /* monotonic */
    void     (*rtc)(void *ctx, int *y, int *mo, int *d, int *h, int *mi, int *s);
} pc_host_t;

#define PC_NOTICE_LEN 72

typedef struct {
    emu86_t     cpu;
    uint8_t    *mem;                 /* EMU86_MEM bytes                  */

    /* drive 00h: the image that was booted. A copy, so a guest that
       writes its own floppy changes nothing outside this session. */
    uint8_t    *fd;  uint32_t fd_size;  int fd_c, fd_h, fd_s;
    /* drive 80h: the pretend hard disk */
    uint8_t    *hd;  uint32_t hd_size;  int hd_c, hd_h, hd_s;
    int         hd_dirty;
    uint8_t     last_status;

    const pc_host_t *host;
    void       *hctx;

    /* keyboard: keys from HavenDOS wait here until the guest has taken
       the last one through port 60h -- one IRQ 1 per make code, then
       one per break code, as a real 8042 delivers them. */
    uint16_t    keyq[32];            /* scancode << 8 | ascii            */
    int         kq_head, kq_tail;
    uint8_t     port60, obf;
    uint8_t     key_ascii;           /* the ASCII of the make in port60  */
    uint8_t     break_due;           /* scancode whose break comes next  */

    /* the 8253: channel 0's period, in microseconds */
    uint32_t    tick_us, tick_acc_us, last_ms;
    uint16_t    pit_latch;
    uint8_t     pit_lohi, pit_cmd;

    /* the 6845 */
    uint8_t     crtc_idx, crtc[32];
    uint8_t     retrace;
    uint8_t     port61;

    int         mode;                /* the BIOS video mode              */
    int         stop;                /* INT 18h: nothing left to boot     */
    char        notice[PC_NOTICE_LEN];  /* the last thing worth saying  */
    uint32_t    notices;             /* how many, for the tests          */
} pc_t;

/* What pc_run() returns, besides the EMU_ codes it passes through. */
#define PC_STOPPED  10

int  pc_init(pc_t *p, uint8_t *mem, const pc_host_t *host, void *hctx,
             uint8_t *fd, uint32_t fd_size, uint32_t fd_used,
             uint8_t *hd, uint32_t hd_size);
int  pc_boot(pc_t *p);                       /* 0, or -1 with notice */
int  pc_run(pc_t *p, uint32_t budget);
void pc_key(pc_t *p, int key);               /* a HavenDOS key code  */
int  pc_keys_waiting(const pc_t *p);

/* the screen, for the window that shows it */
typedef struct {
    const uint8_t *cells;            /* char, attr pairs                 */
    int cols, rows;
    int cx, cy, cursor_on;
    int text;                        /* 0: a graphics mode -- see notice */
} pc_screen_t;
void pc_screen(const pc_t *p, pc_screen_t *s);

/* the geometry pc_init() gives a disk of a given size */
void pc_geometry(uint32_t bytes, int floppy, int *c, int *h, int *s);
#define PC_HD_BYTES (1u * 16u * 63u * 512u)   /* 504 KB: 1 cylinder */
#define PC_FD_BYTES (1474560u)                /* a 1.44 MB floppy   */
