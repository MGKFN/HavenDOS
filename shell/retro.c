/* ════════════════════════════════════════════════════════════════════
   shell/retro.c -- Retro PC: the old builds, in a window.  v0.9.7.

   Kyan's request, 17 November 2026: the first build he still has --
   K-DOS Beta 1.1b, a boot sector and a 1.7 KB shell -- run inside
   HavenDOS. It is a 16-bit real-mode program written against a PC
   BIOS, so it runs on emu/cpu86.c inside the PC of emu/pcbios.c, and
   this file is the window around them.

     Apps > Retro PC      a list: the images built in (archive/retro),
                          then any *.IMG in RETRO on the HavenDOS disk,
                          then any *.IMG in RETRO on a USB stick (0.9.7.1)
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
#include "pc386.h"
#include "retro_images.h"
#include "klog.h"
#include "fat32vol.h"
#include "fat32.h"
#include "exfat.h"
#include "blkdev.h"

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

static int ends_with(const char *n, const char *ext){
    int l = (int)strlen(n);
    if(l <= 4 || n[l - 4] != '.') return 0;
    for(int i = 0; i < 3; i++){
        char c = n[l - 3 + i];
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        if(c != ext[i]) return 0;
    }
    return 1;
}
/* v0.9.7.2: a CD image too -- its kernel runs on the 386 PC */
static int is_iso(const char *n){ return ends_with(n, "ISO"); }
static int ends_img(const char *n){ return ends_with(n, "IMG") || is_iso(n); }
static void disk_cb(const char *name, int is_dir, uint32_t size){
    if(is_dir || g_ndisk >= DISK_MAX || !ends_img(name)) return;
    strncpy(g_disk[g_ndisk], name, 12); g_disk[g_ndisk][12] = 0;
    g_disk_size[g_ndisk] = size;
    g_ndisk++;
}

/* ── v0.9.7.1: what is on a USB stick ─────────────────────────────────
   Kyan, 22 Nov: "how do i add my own imgs???" -- and 0.9.7 had no
   answer he could use. RETRO on the HavenDOS disk was the only place
   looked, and on the HP nothing could put a file there: `cp` stopped
   at 4 KB and Files cannot see a stick. A stick is where a file from
   Windows arrives, so a stick is looked at too: a RETRO folder on any
   FAT32 or exFAT volume of a REMOVABLE disk, the test fs/update_src.c
   uses for an update stick. The image is read into memory to boot;
   its hard disk is a .HDD on the HavenDOS disk, in RETRO, because a
   stick is mounted read-only here. */
#define USB_MAX 16
typedef struct {
    char        name[13];
    uint32_t    size;
    fat32_vol_t vol;
    int         exfat;
} usb_img_t;
static usb_img_t g_usb[USB_MAX];
static int       g_nusb;
static fat32_vol_t g_cur_vol;
static int         g_cur_exfat;

static void usb_cb(const char *name, int is_dir, uint32_t size){
    if(is_dir || g_nusb >= USB_MAX || !ends_img(name)) return;
    usb_img_t *u = &g_usb[g_nusb++];
    strncpy(u->name, name, 12); u->name[12] = 0;
    u->size = size; u->vol = g_cur_vol; u->exfat = g_cur_exfat;
}

static void usb_scan(void){
    fat32_vol_t v[FAT32_MAX_VOLS];
    int n;
    g_nusb = 0;
    n = fat32_scan_volumes(v, FAT32_MAX_VOLS);
    for(int i = 0; i < n && g_nusb < USB_MAX; i++){
        if(!blk_removable(v[i].disk)) continue;
        if(!fat32_vol_mount_ro(&v[i])) continue;
        g_cur_vol = v[i]; g_cur_exfat = 0;
        if(fat32_is_dir("/RETRO")) fat32_list_dir("/RETRO", usb_cb);
        fat32_vol_release();
    }
    n = exfat_scan_volumes(v, FAT32_MAX_VOLS);
    for(int i = 0; i < n && g_nusb < USB_MAX; i++){
        exfat_dir_t d; exfat_dirent_t e;
        if(!blk_removable(v[i].disk)) continue;
        if(!exfat_vol_mount_ro(&v[i])) continue;
        g_cur_vol = v[i]; g_cur_exfat = 1;
        if(exfat_opendir("/RETRO", &d) == EXFAT_OK)
            while(exfat_readdir(&d, &e) == 1 && g_nusb < USB_MAX)
                if(!e.is_dir && e.size <= 0xFFFFFFFFull && strlen(e.name) <= 12)
                    usb_cb(e.name, 0, (uint32_t)e.size);
        exfat_unmount();
    }
    klog(KLOG_INFO, "retro: %d image(s) in RETRO on USB sticks", g_nusb);
}

/* Read one, whole, into buf. Bytes read, or <= 0. */
static int usb_read(const usb_img_t *u, uint8_t *buf, uint32_t max){
    char path[24];
    int got = -1;
    strcpy(path, "/RETRO/"); strcat(path, u->name);
    if(u->size > max) return -2;
    if(u->exfat){
        if(!exfat_vol_mount_ro(&u->vol)) return -1;
        got = exfat_read(path, 0, buf, u->size);
        exfat_unmount();
    } else {
        if(!fat32_vol_mount_ro(&u->vol)) return -1;
        got = fat32_read(path, (char *)buf, max);
        fat32_vol_release();
    }
    return got;
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
static const pc386_host_t g_host386 = { host_ms, host_rtc };

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

/* ── v0.9.7.2: the 386 PC ─────────────────────────────────────────── */
#define ISO_MAX   (48u * 1024u * 1024u)
#define M386_MEM  (16u * 1024u * 1024u)

static void draw_386(const pc386_t *p){
    pc386_screen_t s;
    pc386_screen(p, &s);
    vga_begin_frame();
    for(int y = 0; y < 25; y++)
        for(int x = 0; x < 80; x++){
            uint8_t ch = s.cells[(y * 80 + x) * 2], at = s.cells[(y * 80 + x) * 2 + 1];
            vga_putchar_at(ch ? (char)ch : ' ', x, y, (vga_color_t)(at & 0x0F), (vga_color_t)((at >> 4) & 0x07));
        }
    if(s.graphics){
        fill(14, 11, 52, 3, VGA_DARK_GREY);
        put(16, 12, "It switched to a graphics mode Retro PC cannot show yet.", VGA_WHITE, VGA_DARK_GREY);
    }
    vga_end_frame();
    if(s.cursor_on && !s.graphics) vga_set_cursor(s.cx, s.cy);
}

/* GRUB's menu, as GRUB would draw it. The entry to boot, or -1. */
static int pick_entry(const char *title, pc386_entry_t *e, int n){
    int sel = 0;
    for(int i = 0; i < n; i++)
        if(!e[i].mb2 && strstr(e[i].args, "gfx=text") && !strstr(e[i].args, "mode=safe")){ sel = i; break; }
    for(;;){
        vga_color_t sf, sb; ui_sel_colors(&sf, &sb);
        vga_begin_frame();
        ui_kit_begin("Retro PC", "Up/Down choose   Enter boot   ESC back");
        put(4, 3, title, VGA_WHITE, VGA_BLACK);
        put(4, 4, "Its boot menu, as GRUB showed it:", VGA_LIGHT_GREY, VGA_BLACK);
        for(int i = 0; i < n && i < 12; i++){
            char row[80];
            strncpy(row, e[i].title[0] ? e[i].title : e[i].path, 60); row[60] = 0;
            ui_kit_row(4, 6 + i, 72, sel == i, row);
            if(e[i].mb2) put(66, 6 + i, "64-bit", sel == i ? VGA_WHITE : VGA_DARK_GREY, VGA_BLACK);
            else if(strstr(e[i].args, "gfx=mode13") || strstr(e[i].args, "gfx=vesa"))
                put(64, 6 + i, "graphics", sel == i ? VGA_WHITE : VGA_DARK_GREY, VGA_BLACK);
        }
        put(4, 19, e[sel].mb2 ? "A 64-bit kernel: Retro PC's 386 cannot run it yet." :
                   (strstr(e[sel].args, "gfx=mode13") || strstr(e[sel].args, "gfx=vesa")) ?
                   "Graphics entries boot, but the screen is not shown yet: choose a text one." :
                   "A 32-bit kernel, loaded the way GRUB loads it, on an emulated 386.",
            VGA_DARK_GREY, VGA_BLACK);
        put(4, 21, "Inside: F4 leaves. Old builds count their own delays, so their boot", VGA_DARK_GREY, VGA_BLACK);
        put(4, 22, "animations run slower here than they did on a real PC.", VGA_DARK_GREY, VGA_BLACK);
        vga_end_frame();
        {
            int k = keyboard_waitchar();
            if(k == 27 || k == 'q' || k == 'Q') return -1;
            if(k == KEY_UP && sel > 0) sel--;
            if(k == KEY_DOWN && sel < n - 1 && sel < 11) sel++;
            if(k == '\n' || k == '\r') return sel;
        }
    }
}

/* A 32-bit build: either its kernel and grub.cfg (built in), or a whole
   ISO, whose grub.cfg and kernel are read off it. */
static void run_386(const char *title, const uint8_t *kernel, uint32_t klen,
                    const char *cfg, uint32_t cfglen, const uint8_t *iso, uint32_t isosz){
    pc386_entry_t ent[PC386_ENTRY_MAX];
    const char *why = "";
    int n, sel, r, stopped = 0;
    uint32_t memsz = M386_MEM;
    uint8_t *mem;
    pc386_t *p;

    n = iso ? pc386_iso_menu(iso, isosz, ent, PC386_ENTRY_MAX, &why)
            : pc386_cfg_menu(cfg, (int)cfglen, ent, PC386_ENTRY_MAX, &why);
    if(n <= 0){
        vga_begin_frame(); ui_kit_begin("Retro PC", "Any key goes back");
        put(4, 4, title, VGA_WHITE, VGA_BLACK);
        put(4, 6, "This will not boot here:", VGA_LIGHT_GREY, VGA_BLACK);
        put(6, 7, why, VGA_YELLOW, VGA_BLACK);
        vga_end_frame();
        klog(KLOG_INFO, "retro: %s: %s", title, why);
        keyboard_waitchar();
        return;
    }
again:
    sel = pick_entry(title, ent, n);
    if(sel < 0) return;
    if(ent[sel].mb2){ toast_info("A 64-bit kernel: Retro PC's 386 cannot run it yet"); goto again; }

    mem = (uint8_t *)kmalloc(memsz);
    if(!mem){ memsz = 8u * 1024u * 1024u; mem = (uint8_t *)kmalloc(memsz); }
    p = (pc386_t *)kmalloc(sizeof(pc386_t));
    if(!mem || !p){
        if(mem) kfree(mem);
        if(p) kfree(p);
        toast_info("Retro PC's 386 needs 8 to 16 MB of memory, and the heap has less free");
        return;
    }
    pc386_init(p, mem, memsz, &g_host386, 0);
    if((iso ? pc386_boot(p, iso, isosz, &ent[sel]) : pc386_boot_kernel(p, kernel, (int)klen, &ent[sel])) != 0){
        draw_stopped("This kernel will not boot.", p->notice, 0, 0);
        klog(KLOG_INFO, "retro: %s: %s", title, p->notice);
        keyboard_waitchar();
        goto out;
    }
    klog(KLOG_INFO, "retro: booting %s on the 386: %s %s, %u MB", title, ent[sel].path, ent[sel].args, memsz >> 20);

    for(;;){
        uint32_t t0 = host_ms(0);
        /* as many instructions as fit in about 20 ms, then the screen and
           the keyboard: the old builds' delays are loops, so speed is
           what makes them bearable */
        do { r = pc386_run(p, 250000); } while(r == X86_BUDGET && host_ms(0) - t0 < 20);
        if(!stopped && (r == X86_FAULT || r == X86_HALT || r == PC386_STOPPED)){
            char where[24];
            draw_386(p);
            if(r == X86_FAULT){
                strcpy(where, "at EIP ");
                hex(where + 7, p->cpu.fault_eip, 8);
                klog(KLOG_INFO, "retro: the 386 stopped %s: %s", where, p->cpu.fault_why);
            } else where[0] = 0;
            draw_stopped(r == X86_FAULT ? "The 386 stopped: it crashed." :
                         r == X86_HALT ? "It halted the CPU with interrupts off: it has finished." :
                         "The old PC has stopped.",
                         r == X86_FAULT ? p->cpu.fault_why : p->notice,
                         where[0] ? where : 0, 0);
            stopped = 1;
        } else if(!stopped) draw_386(p);

        {
            int k = keyboard_getchar();
            if(k == -1){
                kpoll_sleep_ms(stopped || r == X86_IDLE ? 10 : 1);
                continue;
            }
            if(stopped){
                if(k == 'r' || k == 'R'){
                    stopped = 0;
                    pc386_init(p, mem, memsz, &g_host386, 0);
                    if((iso ? pc386_boot(p, iso, isosz, &ent[sel]) : pc386_boot_kernel(p, kernel, (int)klen, &ent[sel])) != 0) break;
                    continue;
                }
                break;
            }
            if(k == LEAVE_KEY) break;
            if(k == 27 && chrome_take_close()) break;
            pc386_key(p, k);
        }
    }
out:
    kfree(p); kfree(mem);
}

/* an ISO, whole, from the HavenDOS disk or a stick, into memory */
static uint8_t *iso_load(uint32_t size){
    uint8_t *b;
    if(size > ISO_MAX){ toast_info("That CD image is over 48 MB; Retro PC reads a CD into memory"); return 0; }
    b = (uint8_t *)kmalloc(size ? size : 1);
    if(!b) toast_info("Not enough memory to read that CD image");
    return b;
}

/* its disk: FOO.IMG -> <dir>FOO.HDD */
static void hd_name(char *out, const char *dir, const char *path){
    int n = (int)strlen(path), dot = -1, start = 0;
    for(int i = 0; i < n; i++){ if(path[i] == '.') dot = i; if(path[i] == '/') start = i + 1; }
    if(!dir) start = 0;                    /* beside it: keep the folder */
    int end = dot > start ? dot : n;
    if(end - start > 60) end = start + 60;
    strcpy(out, dir ? dir : "");
    int o = (int)strlen(out);
    memcpy(out + o, path + start, (size_t)(end - start));
    out[o + end - start] = 0;
    strcat(out, ".HDD");
}

/* an image off the HavenDOS disk */
static void run_file(const char *path){
    char hdpath[72];
    uint8_t *buf;
    int got;
    if(is_iso(path)){
        /* its size: from the listing, or found by reading into a
           buffer that doubles until the whole file fits (`retro x.iso`) */
        uint32_t sz = 0;
        if(!strncmp(path, RETRO_DIR "/", strlen(RETRO_DIR "/")))
            for(int i = 0; i < g_ndisk; i++)
                if(!strcmp(g_disk[i], path + strlen(RETRO_DIR "/"))) sz = g_disk_size[i];
        if(sz){
            buf = iso_load(sz);
            if(!buf) return;
            got = vfs_read(path, (char *)buf, sz);
        } else {
            uint32_t cap = 8u * 1024u * 1024u;
            for(;;){
                buf = iso_load(cap);
                if(!buf) return;
                got = vfs_read(path, (char *)buf, cap);
                if(got < 0 || (uint32_t)got < cap || cap >= ISO_MAX) break;
                kfree(buf); cap *= 2;
            }
        }
        if(got > 0) run_386(path, 0, 0, 0, 0, buf, (uint32_t)got);
        else toast_info("Retro PC could not read that file");
        kfree(buf);
        return;
    }
    buf = (uint8_t *)kmalloc(FD_MAX);
    if(!buf){ toast_info("Retro PC needs about 3 MB of memory, and the heap has less free"); return; }
    got = vfs_read(path, (char *)buf, FD_MAX);
    if(got <= 0){
        kfree(buf);
        toast_info("Retro PC could not read that file");
        return;
    }
    hd_name(hdpath, 0, path);
    run_image(path, buf, (uint32_t)got, hdpath);
    kfree(buf);
}

/* an image off a USB stick; its disk in RETRO on the HavenDOS disk */
static void run_usb(const usb_img_t *u){
    char hdpath[72], title[32];
    uint8_t *buf;
    int got;
    if(is_iso(u->name)){
        buf = iso_load(u->size);
        if(!buf) return;
        got = usb_read(u, buf, u->size);
        strcpy(title, "stick: "); strcat(title, u->name);
        if(got > 0) run_386(title, 0, 0, 0, 0, buf, (uint32_t)got);
        else toast_info("Retro PC could not read that file off the stick -- is it still plugged in?");
        kfree(buf);
        return;
    }
    buf = (uint8_t *)kmalloc(FD_MAX);
    if(!buf){ toast_info("Retro PC needs about 3 MB of memory, and the heap has less free"); return; }
    got = usb_read(u, buf, FD_MAX);
    if(got <= 0){
        kfree(buf);
        toast_info(got == -2 ? "That image is bigger than a 2.88 MB floppy"
                             : "Retro PC could not read that file off the stick -- is it still plugged in?");
        return;
    }
    hd_name(hdpath, RETRO_DIR "/", u->name);
    strcpy(title, "stick: "); strcat(title, u->name);
    run_image(title, buf, (uint32_t)got, hdpath);
    kfree(buf);
}

/* ── the list ─────────────────────────────────────────────────────── */
/* v0.9.7.1: one list of all three sources, scrolling, so a stick with a
   dozen images on it cannot push the rows into the text below. */
#define LIST_Y   7
#define LIST_H  10

static const char *entry_text(int i, char *buf){
    if(i < RETRO_NIMAGES) return retro_images[i].title;
    i -= RETRO_NIMAGES;
    if(i < g_ndisk){ strcpy(buf, g_disk[i]); return buf; }
    i -= g_ndisk;
    strcpy(buf, g_usb[i].name); return buf;
}
static const char *entry_where(int i){
    if(i < RETRO_NIMAGES) return retro_images[i].multiboot ? "built in, 386" : "built in, 8086";
    if(i < RETRO_NIMAGES + g_ndisk) return "HavenDOS disk";
    {   /* the bus's own word, from fs/blkdev.c: "USB stick" */
        static char w[24];
        strncpy(w, blk_bus_name(g_usb[i - RETRO_NIMAGES - g_ndisk].vol.disk), 15); w[15] = 0;
        strcat(w, " stick");
        return w;
    }
}

static void rescan(void){
    g_ndisk = 0;
    vfs_list_dir(RETRO_DIR, disk_cb);
    usb_scan();
}

void app_retro(void){
    int sel = 0, top = 0;
    if(g_next[0]){
        char path[64];
        strcpy(path, g_next); g_next[0] = 0;
        run_file(path);
        return;
    }
    rescan();

    for(;;){
        int n = RETRO_NIMAGES + g_ndisk + g_nusb;
        char tb[40];
        if(sel >= n) sel = n - 1;
        if(sel < top) top = sel;
        if(sel >= top + LIST_H) top = sel - LIST_H + 1;
        vga_begin_frame();
        ui_kit_begin("Retro PC", "Up/Down choose   Enter boot   R look again   F4 leaves a running PC   Q quit");
        put(4, 3, "The old builds, on emulated PCs: an 8086 with a BIOS and a floppy for", VGA_LIGHT_GREY, VGA_BLACK);
        put(4, 4, "K-DOS, and a 32-bit 386 that loads BOOT OS the way GRUB did.", VGA_LIGHT_GREY, VGA_BLACK);
        ui_kit_heading(4, 6, "Images");
        for(int r = 0; r < LIST_H && top + r < n; r++){
            int i = top + r;
            ui_kit_row(6, LIST_Y + r, 40, sel == i, entry_text(i, tb));
            put(48, LIST_Y + r, entry_where(i), sel == i ? VGA_WHITE : VGA_DARK_GREY, VGA_BLACK);
        }
        if(top > 0)          put(46, LIST_Y, "^", VGA_DARK_GREY, VGA_BLACK);
        if(top + LIST_H < n) put(46, LIST_Y + LIST_H - 1, "v", VGA_DARK_GREY, VGA_BLACK);

        if(sel < RETRO_NIMAGES && retro_images[sel].multiboot){
            put(4, 19, retro_images[sel].about, VGA_WHITE, VGA_BLACK);
            put(4, 20, "A 32-bit kernel, with the boot menu its CD had. No disk of its own.", VGA_DARK_GREY, VGA_BLACK);
        } else if(sel < RETRO_NIMAGES){
            char hdp[32];
            strcpy(hdp, "Its hard disk: " RETRO_DIR "/"); strcat(hdp, retro_images[sel].id);
            put(4, 19, retro_images[sel].about, VGA_WHITE, VGA_BLACK);
            put(4, 20, hdp, VGA_DARK_GREY, VGA_BLACK);
            put(4 + (int)strlen(hdp), 20, ".HDD -- a file, never a real disk", VGA_DARK_GREY, VGA_BLACK);
        } else if(is_iso(entry_text(sel, tb))){
            put(4, 19, "A CD image: its boot menu is offered, and a 32-bit kernel on it boots on", VGA_WHITE, VGA_BLACK);
            put(4, 20, "the 386 -- read into memory, never written.", VGA_WHITE, VGA_BLACK);
        } else if(sel < RETRO_NIMAGES + g_ndisk){
            put(4, 19, "An image from the HavenDOS disk. Its hard disk is a .HDD file beside it.", VGA_WHITE, VGA_BLACK);
        } else {
            put(4, 19, "An image from a USB stick, read into memory to boot. Its hard disk is a", VGA_WHITE, VGA_BLACK);
            put(4, 20, ".HDD file in RETRO on the HavenDOS disk; nothing is written to the stick.", VGA_WHITE, VGA_BLACK);
        }
        put(4, 22, "Your own: .IMG floppies and .ISO CDs in a folder called RETRO on a USB", VGA_DARK_GREY, VGA_BLACK);
        put(4, 23, "stick; plug it in and press R.", VGA_DARK_GREY, VGA_BLACK);
        vga_end_frame();

        int k = keyboard_waitchar();
        if(k == 'q' || k == 'Q' || k == 27) return;
        if(k == 'r' || k == 'R'){ rescan(); continue; }
        if(k == KEY_UP && sel > 0) sel--;
        if(k == KEY_DOWN && sel < n - 1) sel++;
        if(k == '\n' || k == '\r'){
            if(sel < RETRO_NIMAGES && retro_images[sel].multiboot){
                run_386(retro_images[sel].title, retro_images[sel].data, retro_images[sel].size,
                        retro_images[sel].cfg, retro_images[sel].cfg_size, 0, 0);
            } else if(sel < RETRO_NIMAGES){
                char hdpath[24];
                strcpy(hdpath, RETRO_DIR "/"); strcat(hdpath, retro_images[sel].id); strcat(hdpath, ".HDD");
                run_image(retro_images[sel].title, retro_images[sel].data, retro_images[sel].size, hdpath);
            } else if(sel < RETRO_NIMAGES + g_ndisk){
                char path[32];
                strcpy(path, RETRO_DIR "/"); strcat(path, g_disk[sel - RETRO_NIMAGES]);
                run_file(path);
            } else {
                run_usb(&g_usb[sel - RETRO_NIMAGES - g_ndisk]);
            }
        }
    }
}
