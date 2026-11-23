#pragma once
/* ════════════════════════════════════════════════════════════════════
   retropc/retropc.h -- Retro PC as a program of its own.  v0.9.7.5.

   Kyan, 23 Nov 2026, choosing what came after 0.9.7.4: "1 and 2" --
   first Retro PC as a standalone program for Windows and Linux, then
   HavenDOS inside HavenDOS.

   The emulators were written for this from the start: emu/cpu86.c,
   emu/pcbios.c, emu/cpu386.c and emu/pc386.c know nothing about
   HavenDOS. Memory is a buffer, ports and the BIOS are callbacks,
   time comes from the caller. tests/test_emu86.c and test_emu386.c
   already boot K-DOS and BOOT OS on the build machine with them. What
   a desktop program needs on top is a window, keys and a clock, and
   that is all a host supplies:

     retropc/retropc.c     the program: the list, the two PCs, the
                           text screen drawn into pixels, the files
     retropc/host_win32.c  Windows: one .exe, no DLLs, GDI
     retropc/host_x11.c    Linux: Xlib, which every desktop has
     retropc/host_null.c   no window: scripted keys, a virtual clock
                           and the screen printed as text -- for
                           `make test` and for anyone without X
     retropc/plat_posix.c  folders and files on Linux (both hosts)

   The emulator sources are compiled unchanged. The same .HDD files
   work in both directions: RETRO/KDOS11B.HDD from a HavenDOS disk
   is the same 504 KB a Retro PC on Windows writes.
   ════════════════════════════════════════════════════════════════════ */
#include <stdint.h>

/* the picture the program draws: 80x25 cells of 8x16 pixels */
#define RP_W 640
#define RP_H 400

/* host_key() returns a HavenDOS key code (ASCII; Enter is '\n';
   0x100 | set-1 scancode for the keys without one) or one of these */
#define RP_KEY_NONE  (-1)
#define RP_KEY_QUIT  (-2)        /* the window was closed           */
#define RP_KEY_DROP  (-3)        /* a file was dropped on the window */

/* ── what a host provides ─────────────────────────────────────────── */
int         host_open(const char *title);          /* 0, or -1       */
void        host_present(const uint32_t *px);      /* RP_W x RP_H    */
int         host_key(void);
uint32_t    host_ms(void);                         /* monotonic      */
void        host_sleep(uint32_t ms);
void        host_title(const char *title);
void        host_close(void);
/* a file the user picked: 0 picked, 1 cancelled, -1 this host has no
   way to ask. host_dropped(): the file behind the last RP_KEY_DROP. */
int         host_pick_file(char *out, int max);
const char *host_dropped(void);

/* folders and files: plat_posix.c, or host_win32.c */
const char *plat_data_dir(void);       /* where RETRO lives: beside the program */
int         plat_mkdir(const char *path);
/* calls cb for each regular file in dir; the number of files, or -1 */
int         plat_list(const char *dir, void (*cb)(const char *name, uint64_t size, void *ctx), void *ctx);
char        plat_sep(void);

/* ── the program ──────────────────────────────────────────────────── */
int  rp_main(int argc, char **argv);

/* for host_null.c */
extern uint32_t rp_frame_cap;          /* instructions per frame, at most  */
extern int      rp_dump;               /* print the screen as text at exit */
void rp_screen_text(char *out);        /* 25 lines of 80, '\n' after each  */
