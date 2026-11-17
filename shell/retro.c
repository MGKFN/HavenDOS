/* ════════════════════════════════════════════════════════════════════
   shell/retro.c -- Retro PC: the old builds, in a window.  v0.9.7.

   Kyan's request, 17 November 2026: the first build he still has --
   K-DOS Beta 1.1b, a boot sector and a 1.7 KB shell -- run inside
   HavenDOS. It is a 16-bit real-mode program written against a PC
   BIOS, so it runs on emu/cpu86.c inside the PC of emu/pcbios.c, and
   this file is the window around them.

     Apps > Retro PC      a list: the images built in (archive/retro),
                          then any *.IMG in RETRO on the HavenDOS disk
     retro                the same list
     retro <file>         boots that file straight away

   THE KEYS. Everything goes to the old PC except F4, which leaves,
   and the window's X, which does too. Not F12, the obvious choice:
   neither keyboard driver delivers it, and the handoff already says
   F11 may be a media key on the HP.

   ITS HARD DISK is a file, RETRO/<ID>.HDD for a built-in image, and
   the image's own name with .HDD for one off the disk. Saved when
   you leave, and only if the old system wrote to it. K-DOS's `save`
   writes sector 10 of drive 80h with INT 13h; on the PC it was made
   for that was the real hard disk. Here there is no path from drive
   80h to a block device at all.

   THE FLOPPY is a copy of the image, in memory. A program that writes
   its own boot disk changes the copy for this session and nothing
   else -- the built-in images are in the kernel and cannot change,
   and an image off the disk is not written back.

   Laid out for 80x25, so the desktop boxes it like the other apps,
   and an 80x25 PC screen is exactly that size.
   ════════════════════════════════════════════════════════════════════ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "keys.h"
#include "theme.h"
#include "rtc.h"
#include "toast.h"
#include "emu86.h"
#include "pcbios.h"
#include "retro_images.h"
#include "klog.h"

extern int      keyboard_getchar(void);
extern int      keyboard_waitchar(void);
extern void     kpoll_sleep_ms(uint32_t ms);
extern uint32_t get_ticks(void);
extern void    *kmalloc(size_t);
extern void     kfree(void *);
extern int      vfs_read(const char *, char *, uint32_t);
extern int      vfs_write(const char *, const char *, uint32_t);
extern int      vfs_mkdir(const char *);
extern void     vfs_list_dir(const char *, void (*)(const char *, int, uint32_t));
extern int      chrome_take_close(void);

#define LEAVE_KEY   0x13E          /* F4 */
#define FD_MAX      2949120u       /* a 2.88 MB floppy, the biggest there is */
#define RETRO_DIR   "RETRO"

/* `retro <file>` hands the desktop a path before opening the app */
static char g_next[64];
void retro_set_next(const char *path){
    int i = 0;
    while(path && path[i] && i < 63){ g_next[i] = path[i]; i++; }
    g_next[i] = 0;
}

/* ── what is on the disk ──────────────────────────────────────────── */
#define DISK_MAX 12
static char     g_disk[DISK_MAX][13];
static uint32_t g_disk_size[DISK_MAX];
static int      g_ndisk;

static int ends_img(const char *n){
    int l = (int)strlen(n);
    return l > 4 && n[l - 4] == '.' &&
           (n[l - 3] == 'I' || n[l - 3] == 'i') && (n[l - 2] == 'M' || n[l - 2] == 'm') &&
           (n[l - 1] == 'G' || n[l - 1] == 'g');
}
static void disk_cb(const char *name, int is_dir, uint32_t size){
    if(is_dir || g_ndisk >= DISK_MAX || !ends_img(name)) return;
    strncpy(g_disk[g_ndisk], name, 12); g_disk[g_ndisk][12] = 0;
    g_disk_size[g_ndisk] = size;
    g_ndisk++;
}

/* ── the host side of the PC ──────────────────────────────────────── */
static uint32_t host_ms(void *c){ (void)c; return get_ticks() * 10u; }
static void host_rtc(void *c, int *y, int *mo, int *d, int *h, int *mi, int *s){
    uint8_t hh, mm, ss, dd, mn; uint16_t yy;
    (void)c;
    rtc_get_time(&hh, &mm, &ss);
    rtc_get_date(&dd, &mn, &yy);
    *y = yy < 100 ? 2000 + yy : yy; *mo = mn; *d = dd; *h = hh; *mi = mm; *s = ss;
}
static const pc_host_t g_host = { host_ms, host_rtc };

/* ── drawing ──────────────────────────────────────────────────────── */
static void put(int x, int y, const char *s, vga_color_t fg, vga_color_t bg){
    for(; *s && x < 80; s++, x++) vga_putchar_at(*s, x, y, fg, bg);
}
static void fill(int x, int y, int w, int h, vga_color_t bg){
    for(int r = 0; r < h; r++) for(int c = 0; c < w; c++) vga_putchar_at(' ', x + c, y + r, VGA_WHITE, bg);
}

static void draw_pc(const pc_t *p){
    pc_screen_t s;
    pc_screen(p, &s);
    vga_begin_frame();
    for(int y = 0; y < 25; y++)
        for(int x = 0; x < 80; x++){
            uint8_t ch = 0, at = 0x07;
            if(y < s.rows && x < s.cols){
                ch = s.cells[(y * s.cols + x) * 2];
                at = s.cells[(y * s.cols + x) * 2 + 1];
            }
            /* the blink bit is a bright background on nothing HavenDOS
               draws, so it is dropped rather than shown as a colour */
            vga_putchar_at(ch ? (char)ch : ' ', x, y, (vga_color_t)(at & 0x0F), (vga_color_t)((at >> 4) & 0x07));
        }
    vga_end_frame();
    if(s.cursor_on && s.cx < 80 && s.cy < 25) vga_set_cursor(s.cx, s.cy);
}

/* a box over the old PC's screen, for when it has stopped */
static void draw_stopped(const char *title, const char *l1, const char *l2, const char *l3){
    vga_color_t sf, sb; ui_sel_colors(&sf, &sb);
    vga_begin_frame();
    fill(8, 8, 64, 9, VGA_DARK_GREY);
    fill(9, 9, 62, 7, VGA_BLACK);
    put(11, 9, title, sb, VGA_BLACK);
    put(11, 11, l1, VGA_WHITE, VGA_BLACK);
    if(l2) put(11, 12, l2, VGA_LIGHT_GREY, VGA_BLACK);
    if(l3) put(11, 13, l3, VGA_LIGHT_GREY, VGA_BLACK);
    put(11, 15, "R restarts it.   Any other key leaves Retro PC.", VGA_DARK_GREY, VGA_BLACK);
    vga_end_frame();
}

static void hex(char *o, uint32_t v, int digits){
    static const char h[] = "0123456789ABCDEF";
    for(int i = digits - 1; i >= 0; i--){ o[i] = h[v & 15]; v >>= 4; }
    o[digits] = 0;
}

/* ── running one image ────────────────────────────────────────────── */
static int load_hd(const char *path, uint8_t *hd){
    int n = vfs_read(path, (char *)hd, PC_HD_BYTES);
    if(n == (int)PC_HD_BYTES) return 1;
    memset(hd, 0, PC_HD_BYTES);
    return 0;
}

static void run_image(const char *title, const uint8_t *img, uint32_t size, const char *hdpath){
    uint8_t *mem, *fd, *hd;
    pc_t *p;
    uint32_t fdsz = size > PC_FD_BYTES ? FD_MAX : PC_FD_BYTES;
    uint32_t notices = 0;
    int r, stopped = 0;

    if(size > FD_MAX){
        toast_info("That image is bigger than a 2.88 MB floppy; Retro PC boots floppies");
        return;
    }
    mem = (uint8_t *)kmalloc(EMU86_MEM);
    fd  = (uint8_t *)kmalloc(fdsz);
    hd  = (uint8_t *)kmalloc(PC_HD_BYTES);
    p   = (pc_t *)kmalloc(sizeof(pc_t));
    if(!mem || !fd || !hd || !p){
        if(mem) kfree(mem);
        if(fd)  kfree(fd);
        if(hd)  kfree(hd);
        if(p)   kfree(p);
        toast_info("Retro PC needs about 3 MB of memory, and the heap has less free");
        return;
    }
    memset(fd, 0, fdsz);
    memcpy(fd, img, size);
    load_hd(hdpath, hd);

    pc_init(p, mem, &g_host, 0, fd, fdsz, size, hd, PC_HD_BYTES);
    klog(KLOG_INFO, "retro: booting %s (%u bytes), hard disk %s", title, size, hdpath);
    if(pc_boot(p) != 0){
        draw_stopped("This image will not boot.", p->notice, 0, 0);
        keyboard_waitchar();
        goto out;
    }

    for(;;){
        r = pc_run(p, 100000);
        if(p->notices != notices){
            /* something the old program asked for is not here: say so,
               once per new thing, rather than leave it to guess */
            notices = p->notices;
            toast_info(p->notice);
            klog(KLOG_INFO, "retro: %s", p->notice);
        }
        if(r == EMU_FAULT || r == EMU_HALT || r == PC_STOPPED){
            char where[40], b[3][3];
            const char *why;
            if(r == EMU_FAULT){
                why = p->cpu.fault_why;
                hex(where, p->cpu.fault_cs, 4); where[4] = ':'; hex(where + 5, p->cpu.fault_ip, 4);
                for(int i = 0; i < 3; i++) hex(b[i], p->cpu.fault_op[i], 2);
                strcat(where, "  bytes "); strcat(where, b[0]); strcat(where, " ");
                strcat(where, b[1]); strcat(where, " "); strcat(where, b[2]);
                klog(KLOG_INFO, "retro: stopped at %s: %s", where, why);
            } else if(r == EMU_HALT){
                why = "It halted the CPU with interrupts off: it has finished.";
                where[0] = 0;
            } else {
                why = p->notice; where[0] = 0;
            }
            draw_pc(p);
            draw_stopped(r == EMU_FAULT ? "The old PC stopped: it used an instruction" :
                         "The old PC has stopped.",
                         r == EMU_FAULT ? why : why,
                         r == EMU_FAULT ? "Retro PC is an 80186 with the 386's 16-bit additions." : 0,
                         where[0] ? where : 0);
            stopped = 1;
        } else {
            draw_pc(p);
        }

        {
            int k = keyboard_getchar();
            if(k == -1){
                /* waiting for a key, or idle in HLT: let HavenDOS run */
                if(stopped || r == EMU_WAIT || r == EMU_IDLE) kpoll_sleep_ms(15);
                else kpoll_sleep_ms(1);
                continue;
            }
            if(stopped){
                if(k == 'r' || k == 'R'){
                    stopped = 0;
                    memset(fd, 0, fdsz); memcpy(fd, img, size);
                    if(pc_boot(p) != 0) break;
                    continue;
                }
                break;
            }
            if(k == LEAVE_KEY) break;
            if(k == 27 && chrome_take_close()) break;
            pc_key(p, k);
        }
    }

    if(p->hd_dirty){
        vfs_mkdir(RETRO_DIR);
        if(vfs_write(hdpath, (const char *)hd, PC_HD_BYTES) == (int)PC_HD_BYTES){
            klog(KLOG_INFO, "retro: saved %s", hdpath);
        } else {
            /* say it: a pretend disk that silently is not saved is the
               kind of failure that only shows up next time */
            toast_info("Retro PC could not save the old PC's hard disk");
            klog(KLOG_INFO, "retro: could not save %s", hdpath);
        }
    }
out:
    kfree(p); kfree(hd); kfree(fd); kfree(mem);
}

/* an image off the HavenDOS disk */
static void run_file(const char *path){
    char hdpath[72];
    int n = (int)strlen(path), dot = -1;
    uint8_t *buf = (uint8_t *)kmalloc(FD_MAX);
    int got;
    if(!buf){ toast_info("Retro PC needs about 3 MB of memory, and the heap has less free"); return; }
    got = vfs_read(path, (char *)buf, FD_MAX);
    if(got <= 0){
        kfree(buf);
        toast_info("Retro PC could not read that file");
        return;
    }
    /* its disk beside it: FOO.IMG -> FOO.HDD */
    for(int i = 0; i < n; i++) if(path[i] == '.') dot = i;
    if(n > 64) n = 64;
    memcpy(hdpath, path, (size_t)(dot > 0 ? dot : n));
    hdpath[dot > 0 ? dot : n] = 0;
    strcat(hdpath, ".HDD");
    run_image(path, buf, (uint32_t)got, hdpath);
    kfree(buf);
}

/* ── the list ─────────────────────────────────────────────────────── */
void app_retro(void){
    int sel = 0;
    if(g_next[0]){
        char path[64];
        strcpy(path, g_next); g_next[0] = 0;
        run_file(path);
        return;
    }
    g_ndisk = 0;
    vfs_list_dir(RETRO_DIR, disk_cb);

    for(;;){
        int n = RETRO_NIMAGES + g_ndisk;
        vga_color_t sf, sb; ui_sel_colors(&sf, &sb);
        vga_begin_frame();
        ui_kit_begin("Retro PC", "Up/Down choose   Enter boot   F4 leaves a running PC   Q quit");
        put(4, 3, "The old builds, on an emulated PC: an 80186 with the 386's 16-bit", VGA_LIGHT_GREY, VGA_BLACK);
        put(4, 4, "additions, a BIOS, a floppy and a pretend hard disk of its own.", VGA_LIGHT_GREY, VGA_BLACK);
        ui_kit_heading(4, 6, "Built in");
        for(int i = 0; i < RETRO_NIMAGES; i++)
            ui_kit_row(6, 7 + i, 40, sel == i, retro_images[i].title);
        ui_kit_heading(4, 9 + RETRO_NIMAGES, "On this disk, in RETRO");
        if(!g_ndisk) put(6, 10 + RETRO_NIMAGES, "none -- put a .IMG file in RETRO to see it here", VGA_DARK_GREY, VGA_BLACK);
        for(int i = 0; i < g_ndisk; i++)
            ui_kit_row(6, 10 + RETRO_NIMAGES + i, 40, sel == RETRO_NIMAGES + i, g_disk[i]);

        if(sel < RETRO_NIMAGES){
            char hdp[32];
            strcpy(hdp, "Its hard disk: " RETRO_DIR "/"); strcat(hdp, retro_images[sel].id);
            put(4, 19, retro_images[sel].about, VGA_WHITE, VGA_BLACK);
            put(4, 20, hdp, VGA_DARK_GREY, VGA_BLACK);
            put(4 + (int)strlen(hdp), 20, ".HDD -- a file, never a real disk", VGA_DARK_GREY, VGA_BLACK);
        } else {
            put(4, 19, "An image from the HavenDOS disk. Its hard disk is a .HDD file beside it.", VGA_WHITE, VGA_BLACK);
        }
        vga_end_frame();

        int k = keyboard_waitchar();
        if(k == 'q' || k == 'Q' || k == 27) return;
        if(k == KEY_UP && sel > 0) sel--;
        if(k == KEY_DOWN && sel < n - 1) sel++;
        if(k == '\n' || k == '\r'){
            if(sel < RETRO_NIMAGES){
                char hdpath[24];
                strcpy(hdpath, RETRO_DIR "/"); strcat(hdpath, retro_images[sel].id); strcat(hdpath, ".HDD");
                run_image(retro_images[sel].title, retro_images[sel].data, retro_images[sel].size, hdpath);
            } else {
                char path[32];
                strcpy(path, RETRO_DIR "/"); strcat(path, g_disk[sel - RETRO_NIMAGES]);
                run_file(path);
            }
        }
    }
}
