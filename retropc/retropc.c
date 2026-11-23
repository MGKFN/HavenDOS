/* ════════════════════════════════════════════════════════════════════
   retropc/retropc.c -- Retro PC, the program.  v0.9.7.5.

   shell/retro.c, moved out of the kernel: the same list, the same two
   PCs and the same rules, drawn into a 640x400 picture that a host
   puts in a window. See retropc.h for how the files divide.

     retropc                 the list: the images built in, then any
                             .IMG or .ISO in the RETRO folder beside
                             the program
     retropc <file>          boots that file; F4 then quits
     retropc --boot <ID>     boots a built-in image by its id (KDOS11B,
                             BOOTOS31, BOOTOS50, BOOTOS52)
     retropc --mem <MB>      the 386's memory (16 by default)

   THE KEYS. Everything goes to the old PC except F4, which leaves it,
   exactly as inside HavenDOS. Closing the window leaves too, and
   still saves the hard disk.

   ITS HARD DISK is RETRO/<ID>.HDD beside the program for a built-in
   image, and the image's own name with .HDD beside it for a file --
   the same names and the same 504 KB as on HavenDOS, so a disk can be
   carried either way. Written when you leave, and only if the old
   system wrote to it.

   SOMETHING THE OLD PROGRAM ASKED FOR AND DID NOT GET is a notice:
   the window's title says it, and stderr logs it. A notice is never
   drawn over the old PC's own screen.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "retropc.h"
#include "emu86.h"
#include "pcbios.h"
#include "pc386.h"
#include "keys.h"
#include "retro_images.h"
#include "version.h"
#define RP_VERSION HAVENDOS_VER_NUM

void rp_font_build(unsigned char *f);

#define LEAVE_KEY   KEY_F4
#define FD_MAX      2949120u              /* a 2.88 MB floppy         */
#define ISO_MAX     (1024u * 1024u * 1024u)
#define FRAME_MS    14                    /* instructions per frame, in time */
#define CHUNK_86    20000u
#define CHUNK_386   250000u

uint32_t rp_frame_cap = 40000000u;        /* host_null lowers this     */
int      rp_dump;

static unsigned char g_font[256 * 16];
static uint32_t      g_mem386 = 16u * 1024u * 1024u;
static int           g_direct;            /* started on a file: F4 quits */
static int           g_quit;              /* the window was closed      */

/* ── the screen: 80x25 cells, as a PC's text mode holds them ─────── */
static uint8_t  g_cells[80 * 25 * 2];
static int      g_cx, g_cy, g_cursor;
static int      g_blink = 1;          /* attribute bit 7: blink, or a bright background */
static uint32_t g_px[RP_W * RP_H], g_shown[RP_W * RP_H];
static int      g_shown_ok;

static const uint32_t g_pal[16] = {      /* the VGA's 16 text colours */
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGREY,
       DGREY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE };

static void cell(int x, int y, int ch, int fg, int bg){
    if(x < 0 || x >= 80 || y < 0 || y >= 25) return;
    g_cells[(y * 80 + x) * 2] = (uint8_t)ch;
    g_cells[(y * 80 + x) * 2 + 1] = (uint8_t)((bg << 4) | fg);
}
static void put(int x, int y, const char *s, int fg, int bg){
    for(; *s && x < 80; s++, x++) cell(x, y, (unsigned char)*s, fg, bg);
}
static void fill(int x, int y, int w, int h, int bg){
    for(int r = 0; r < h; r++) for(int c = 0; c < w; c++) cell(x + c, y + r, ' ', WHITE, bg);
}
static void clear(void){ fill(0, 0, 80, 25, BLACK); g_cursor = 0; g_blink = 1; }

/* Cells into pixels, and to the window if anything changed. The blink
   bit blinks, as on a VGA: QEMU's screens, which the K-DOS suite was
   checked against, do the same. The cursor is the VGA's: the bottom
   two rows of its cell, every 16 frames. */
static void present(void){
    uint32_t ms = host_ms();
    int cur_on = (ms / 267) & 1, blink_on = (ms / 533) & 1;
    for(int y = 0; y < 25; y++)
        for(int x = 0; x < 80; x++){
            uint8_t ch = g_cells[(y * 80 + x) * 2], at = g_cells[(y * 80 + x) * 2 + 1];
            uint32_t fg = g_pal[at & 15], bg = g_pal[(at >> 4) & (g_blink ? 7 : 15)];
            const unsigned char *gl = g_font + ch * 16;
            int hide = g_blink && (at & 0x80) && !blink_on;
            for(int r = 0; r < 16; r++){
                unsigned bits = hide ? 0 : gl[r];
                if(g_cursor && x == g_cx && y == g_cy && r >= 14 && cur_on) bits = 0xFF;
                uint32_t *o = g_px + (y * 16 + r) * RP_W + x * 8;
                for(int b = 0; b < 8; b++) o[b] = (bits & (0x80 >> b)) ? fg : bg;
            }
        }
    if(g_shown_ok && !memcmp(g_px, g_shown, sizeof g_px)) return;
    memcpy(g_shown, g_px, sizeof g_px);
    g_shown_ok = 1;
    host_present(g_px);
}

void rp_screen_text(char *out){
    for(int y = 0; y < 25; y++){
        int n = 0;
        for(int x = 0; x < 80; x++){
            uint8_t c = g_cells[(y * 80 + x) * 2];
            out[y * 81 + x] = (char)(c >= 0x20 && c < 0x7F ? c : (c ? '.' : ' '));
        }
        for(n = 79; n >= 0 && out[y * 81 + n] == ' '; n--) ;
        for(int x = n + 1; x < 80; x++) out[y * 81 + x] = ' ';
        out[y * 81 + 80] = '\n';
    }
    out[25 * 81] = 0;
}

static void dump_screen(void){
    char t[25 * 81 + 1];
    if(!rp_dump) return;
    rp_screen_text(t);
    /* trailing spaces off each line, for a readable diff */
    for(char *l = t; *l; ){
        char *e = strchr(l, '\n'), *s = e;
        while(s > l && s[-1] == ' ') s--;
        fwrite(l, 1, (size_t)(s - l), stdout); putchar('\n');
        l = e + 1;
    }
    fflush(stdout);
}

/* Waits for a key, keeping the window alive. RP_KEY_QUIT sets g_quit. */
static int wait_key(void){
    for(;;){
        int k = host_key();
        if(k == RP_KEY_QUIT){ g_quit = 1; return k; }
        if(k != RP_KEY_NONE) return k;
        present();
        host_sleep(15);
    }
}

static void say(const char *what, const char *msg){
    char t[200];
    fprintf(stderr, "retropc: %s\n", msg);
    snprintf(t, sizeof t, "Retro PC - %s - %s", what, msg);
    host_title(t);
}
static void title_for(const char *what){
    char t[160];
    snprintf(t, sizeof t, "Retro PC - %s - F4 %s", what, g_direct ? "quits" : "goes back to the list");
    host_title(t);
}

/* ── files ────────────────────────────────────────────────────────── */
static char g_retro_dir[1024];             /* <data dir>/RETRO        */

static void path_join(char *out, size_t max, const char *a, const char *b){
    size_t n = strlen(a);
    if(n && (a[n - 1] == '/' || a[n - 1] == '\\')) snprintf(out, max, "%s%s", a, b);
    else snprintf(out, max, "%s%c%s", a, plat_sep(), b);
}

/* The whole of a file, into a buffer of its own. Bytes, or -1. */
static long read_file(const char *path, uint8_t **out, uint32_t max){
    FILE *f = fopen(path, "rb");
    long n;
    *out = 0;
    if(!f) return -1;
    if(fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)){ fclose(f); return -1; }
    if((uint32_t)n > max){ fclose(f); return -2; }
    *out = (uint8_t *)malloc(n ? (size_t)n : 1);
    if(!*out){ fclose(f); return -1; }
    if(fread(*out, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(*out); *out = 0; return -1; }
    fclose(f);
    return n;
}

static int write_file(const char *path, const uint8_t *b, uint32_t n){
    FILE *f = fopen(path, "wb");
    int ok;
    if(!f) return -1;
    ok = fwrite(b, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : -1;
}

static int ends_with(const char *n, const char *ext){
    size_t l = strlen(n);
    if(l <= 4 || n[l - 4] != '.') return 0;
    for(int i = 0; i < 3; i++){
        char c = n[l - 3 + i];
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        if(c != ext[i]) return 0;
    }
    return 1;
}
static int is_iso(const char *n){ return ends_with(n, "ISO"); }
static int is_image(const char *n){ return ends_with(n, "IMG") || ends_with(n, "IMA") || is_iso(n); }

/* FOO.IMG -> FOO.HDD, beside it */
static void hd_beside(char *out, size_t max, const char *path){
    const char *dot = strrchr(path, '.'), *sl1 = strrchr(path, '/'), *sl2 = strrchr(path, '\\');
    const char *sl = sl1 > sl2 ? sl1 : sl2;
    size_t n = (dot && (!sl || dot > sl)) ? (size_t)(dot - path) : strlen(path);
    if(n + 5 > max) n = max - 5;
    memcpy(out, path, n);
    strcpy(out + n, ".HDD");
}

static const char *base_name(const char *p){
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

/* ── the host side of the PCs ─────────────────────────────────────── */
static uint32_t pc_ms(void *c){ (void)c; return host_ms(); }
static void pc_rtc(void *c, int *y, int *mo, int *d, int *h, int *mi, int *s){
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    (void)c;
    *y = tm->tm_year + 1900; *mo = tm->tm_mon + 1; *d = tm->tm_mday;
    *h = tm->tm_hour; *mi = tm->tm_min; *s = tm->tm_sec;
}
static const pc_host_t    g_host   = { pc_ms, pc_rtc };
static const pc386_host_t g_host386 = { pc_ms, pc_rtc };

static void hex(char *o, uint32_t v, int digits){
    static const char h[] = "0123456789ABCDEF";
    for(int i = digits - 1; i >= 0; i--){ o[i] = h[v & 15]; v >>= 4; }
    o[digits] = 0;
}

/* a box over the old PC's screen, for when it has stopped */
static void draw_stopped(const char *title, const char *l1, const char *l2, const char *l3){
    fill(8, 8, 64, 9, DGREY);
    fill(9, 9, 62, 7, BLACK);
    put(11, 9, title, LCYAN, BLACK);
    put(11, 11, l1 ? l1 : "", WHITE, BLACK);
    if(l2) put(11, 12, l2, LGREY, BLACK);
    if(l3) put(11, 13, l3, LGREY, BLACK);
    put(11, 15, g_direct ? "R restarts it.   Any other key quits."
                         : "R restarts it.   Any other key goes back to the list.", DGREY, BLACK);
    g_cursor = 0;
}

/* Keys waiting, to the old PC. 1 if it is time to leave. */
static int feed_keys(void (*to)(void *, int), void *p, int *stopped, int *restart){
    for(;;){
        int k = host_key();
        if(k == RP_KEY_NONE) return 0;
        if(k == RP_KEY_QUIT){ g_quit = 1; return 1; }
        if(k == RP_KEY_DROP) continue;            /* the list takes drops */
        if(*stopped){
            if(k == 'r' || k == 'R'){ *restart = 1; return 0; }
            return 1;
        }
        if(k == LEAVE_KEY) return 1;
        to(p, k);
    }
}
static void key86(void *p, int k){ pc_key((pc_t *)p, k); }
static void key386(void *p, int k){ pc386_key((pc386_t *)p, k); }

/* ── the 8086 PC: a floppy ────────────────────────────────────────── */
static void show_pc(const pc_t *p){
    pc_screen_t s;
    pc_screen(p, &s);
    for(int y = 0; y < 25; y++)
        for(int x = 0; x < 80; x++){
            uint8_t ch = 0, at = 0x07;
            if(y < s.rows && x < s.cols){
                ch = s.cells[(y * s.cols + x) * 2];
                at = s.cells[(y * s.cols + x) * 2 + 1];
            }
            g_cells[(y * 80 + x) * 2] = ch;
            g_cells[(y * 80 + x) * 2 + 1] = at;
        }
    g_cursor = s.cursor_on && s.cx < 80 && s.cy < 25;
    g_cx = s.cx; g_cy = s.cy;
    g_blink = 1;
    if(!s.text){
        fill(14, 11, 52, 3, DGREY);
        put(16, 12, "It switched to a graphics mode Retro PC cannot show.", WHITE, DGREY);
    }
}

static void run_floppy(const char *title, const uint8_t *img, uint32_t size, const char *hdpath){
    static uint8_t mem[EMU86_MEM], hd[PC_HD_BYTES];
    static pc_t p;
    uint32_t fdsz = size > PC_FD_BYTES ? FD_MAX : PC_FD_BYTES;
    uint8_t *fd, *old = 0;
    uint32_t notices = 0;
    int r = EMU_BUDGET, stopped = 0, restart = 0;
    long n;

    if(size > FD_MAX){ say(title, "bigger than a 2.88 MB floppy; the 8086 PC boots floppies"); wait_key(); return; }
    fd = (uint8_t *)calloc(1, fdsz);
    if(!fd){ say(title, "not enough memory"); return; }
    memcpy(fd, img, size);
    memset(hd, 0, sizeof hd);
    n = read_file(hdpath, &old, PC_HD_BYTES);
    if(n == (long)PC_HD_BYTES) memcpy(hd, old, PC_HD_BYTES);
    free(old);

    pc_init(&p, mem, &g_host, 0, fd, fdsz, size, hd, PC_HD_BYTES);
    fprintf(stderr, "retropc: booting %s (%u bytes), hard disk %s\n", title, size, hdpath);
    title_for(title);
    if(pc_boot(&p) != 0){
        clear(); draw_stopped("This image will not boot.", p.notice, 0, 0);
        present(); wait_key();
        free(fd); return;
    }

    for(;;){
        if(!stopped){
            uint32_t t0 = host_ms(), ran = 0;
            do { r = pc_run(&p, CHUNK_86); ran += CHUNK_86; }
            while(r == EMU_BUDGET && host_ms() - t0 < FRAME_MS && ran < rp_frame_cap);
            if(p.notices != notices){ notices = p.notices; say(title, p.notice); }
            show_pc(&p);
            if(r == EMU_FAULT || r == EMU_HALT || r == PC_STOPPED){
                char where[40] = "", b[3][3];
                if(r == EMU_FAULT){
                    hex(where, p.cpu.fault_cs, 4); where[4] = ':'; hex(where + 5, p.cpu.fault_ip, 4);
                    for(int i = 0; i < 3; i++) hex(b[i], p.cpu.fault_op[i], 2);
                    strcat(where, "  bytes "); strcat(where, b[0]); strcat(where, " ");
                    strcat(where, b[1]); strcat(where, " "); strcat(where, b[2]);
                    fprintf(stderr, "retropc: stopped at %s: %s\n", where, p.cpu.fault_why);
                }
                draw_stopped(r == EMU_FAULT ? "The old PC stopped: it used an instruction" : "The old PC has stopped.",
                             r == EMU_FAULT ? p.cpu.fault_why :
                             r == EMU_HALT ? "It halted the CPU with interrupts off: it has finished." : p.notice,
                             r == EMU_FAULT ? "This PC is an 80186 with the 386's 16-bit additions." : 0,
                             where[0] ? where : 0);
                stopped = 1;
            }
        }
        present();
        if(feed_keys(key86, &p, &stopped, &restart)) break;
        if(restart){
            restart = 0; stopped = 0;
            memset(fd, 0, fdsz); memcpy(fd, img, size);
            if(pc_boot(&p) != 0) break;
            continue;
        }
        host_sleep(stopped || r == EMU_WAIT || r == EMU_IDLE ? 15 : 1);
    }
    if(p.hd_dirty){
        if(write_file(hdpath, hd, PC_HD_BYTES) == 0)
            fprintf(stderr, "retropc: saved %s\n", hdpath);
        else {
            /* say it: a pretend disk that silently is not saved is the
               kind of failure that only shows up next time */
            say(title, "could not save the old PC's hard disk");
            fprintf(stderr, "retropc: could not write %s\n", hdpath);
        }
    }
    free(fd);
}

/* ── the 386 PC: a Multiboot kernel ───────────────────────────────── */
static void show_386(const pc386_t *p){
    pc386_screen_t s;
    pc386_screen(p, &s);
    memcpy(g_cells, s.cells, sizeof g_cells);
    g_blink = (p->attr[0x10] & 0x08) != 0;
    g_cursor = s.cursor_on && !s.graphics && s.cx < 80 && s.cy < 25;
    g_cx = s.cx; g_cy = s.cy;
    if(s.graphics){
        fill(14, 11, 52, 3, DGREY);
        put(16, 12, "It switched to a graphics mode Retro PC cannot show yet.", WHITE, DGREY);
    }
}

static int is_gfx_entry(const pc386_entry_t *e){
    return strstr(e->args, "gfx=mode13") || strstr(e->args, "gfx=vesa");
}

/* GRUB's menu, as GRUB would draw it. The entry to boot, or -1. */
static int pick_entry(const char *title, pc386_entry_t *e, int n){
    int sel = 0;
    for(int i = 0; i < n; i++)
        if(!e[i].mb2 && strstr(e[i].args, "gfx=text") && !strstr(e[i].args, "mode=safe")){ sel = i; break; }
    host_title("Retro PC - choose an entry");
    for(;;){
        clear();
        fill(0, 0, 80, 1, BLUE);
        put(2, 0, "Retro PC", WHITE, BLUE);
        put(4, 3, title, WHITE, BLACK);
        put(4, 4, "Its boot menu, as GRUB showed it:", LGREY, BLACK);
        for(int i = 0; i < n && i < 12; i++){
            char row[80];
            int on = sel == i;
            snprintf(row, sizeof row, " %-60.60s", e[i].title[0] ? e[i].title : e[i].path);
            fill(4, 6 + i, 72, 1, on ? CYAN : BLACK);
            put(4, 6 + i, row, on ? BLACK : LGREY, on ? CYAN : BLACK);
            if(e[i].mb2) put(68, 6 + i, "64-bit", on ? BLACK : DGREY, on ? CYAN : BLACK);
            else if(is_gfx_entry(&e[i])) put(66, 6 + i, "graphics", on ? BLACK : DGREY, on ? CYAN : BLACK);
        }
        put(4, 19, e[sel].mb2 ? "A 64-bit kernel: Retro PC's 386 cannot run it yet." :
                   is_gfx_entry(&e[sel]) ? "Graphics entries boot, but the screen is not shown yet: choose a text one." :
                   "A 32-bit kernel, loaded the way GRUB loads it, on an emulated 386.", DGREY, BLACK);
        put(4, 21, "Inside: F4 leaves. Old builds count their own delays, so a boot", DGREY, BLACK);
        put(4, 22, "animation runs at the speed of this emulator, not of the old PC.", DGREY, BLACK);
        fill(0, 24, 80, 1, BLUE);
        put(2, 24, "Up/Down choose   Enter boot   Esc back", WHITE, BLUE);
        present();
        {
            int k = wait_key();
            if(k == RP_KEY_QUIT || k == 27 || k == 'q' || k == 'Q') return -1;
            if(k == KEY_UP && sel > 0) sel--;
            if(k == KEY_DOWN && sel < n - 1 && sel < 11) sel++;
            if(k == '\n') return sel;
        }
    }
}

static void run_386(const char *title, const uint8_t *kernel, uint32_t klen,
                    const char *cfg, uint32_t cfglen, const uint8_t *iso, uint32_t isosz){
    pc386_entry_t ent[PC386_ENTRY_MAX];
    static pc386_t p;
    const char *why = "";
    int n, sel, r = X86_BUDGET, stopped = 0, restart = 0;
    uint8_t *mem;

    n = iso ? pc386_iso_menu(iso, isosz, ent, PC386_ENTRY_MAX, &why)
            : pc386_cfg_menu(cfg, (int)cfglen, ent, PC386_ENTRY_MAX, &why);
    if(n <= 0){
        clear();
        put(4, 4, title, WHITE, BLACK);
        put(4, 6, "This will not boot here:", LGREY, BLACK);
        put(6, 7, why, YELLOW, BLACK);
        put(4, 9, "Any key goes back.", DGREY, BLACK);
        fprintf(stderr, "retropc: %s: %s\n", title, why);
        present(); wait_key();
        return;
    }
again:
    sel = pick_entry(title, ent, n);
    if(sel < 0) return;
    if(ent[sel].mb2){ say(title, "a 64-bit kernel: Retro PC's 386 cannot run it yet"); goto again; }

    mem = (uint8_t *)malloc(g_mem386);
    if(!mem){ say(title, "not enough memory for the 386"); return; }
    pc386_init(&p, mem, g_mem386, &g_host386, 0);
    if((iso ? pc386_boot(&p, iso, isosz, &ent[sel]) : pc386_boot_kernel(&p, kernel, (int)klen, &ent[sel])) != 0){
        clear(); draw_stopped("This kernel will not boot.", p.notice, 0, 0);
        fprintf(stderr, "retropc: %s: %s\n", title, p.notice);
        present(); wait_key();
        free(mem); return;
    }
    fprintf(stderr, "retropc: booting %s on the 386: %s %s, %u MB\n", title, ent[sel].path, ent[sel].args, g_mem386 >> 20);
    title_for(title);

    for(;;){
        if(!stopped){
            uint32_t t0 = host_ms(), ran = 0;
            /* as much as fits in a frame: the old builds' delays are
               loops, so speed is what makes them bearable */
            do { r = pc386_run(&p, CHUNK_386); ran += CHUNK_386; }
            while(r == X86_BUDGET && host_ms() - t0 < FRAME_MS && ran < rp_frame_cap);
            show_386(&p);
            if(r == X86_FAULT || r == X86_HALT || r == PC386_STOPPED){
                char where[24] = "";
                if(r == X86_FAULT){
                    strcpy(where, "at EIP "); hex(where + 7, p.cpu.fault_eip, 8);
                    fprintf(stderr, "retropc: the 386 stopped %s: %s\n", where, p.cpu.fault_why);
                }
                draw_stopped(r == X86_FAULT ? "The 386 stopped: it crashed." :
                             r == X86_HALT ? "It halted the CPU with interrupts off: it has finished." :
                             "The old PC has stopped.",
                             r == X86_FAULT ? p.cpu.fault_why : p.notice, where[0] ? where : 0, 0);
                stopped = 1;
            }
        }
        present();
        if(feed_keys(key386, &p, &stopped, &restart)) break;
        if(restart){
            restart = 0; stopped = 0;
            pc386_init(&p, mem, g_mem386, &g_host386, 0);
            if((iso ? pc386_boot(&p, iso, isosz, &ent[sel]) : pc386_boot_kernel(&p, kernel, (int)klen, &ent[sel])) != 0) break;
            continue;
        }
        host_sleep(stopped || r == X86_IDLE ? 10 : 1);
    }
    free(mem);
}

/* ── one image, wherever it came from ─────────────────────────────── */
static void run_builtin(int i){
    const retro_image_t *im = &retro_images[i];
    if(im->multiboot){
        run_386(im->title, im->data, im->size, im->cfg, im->cfg_size, 0, 0);
    } else {
        char hdpath[1100], name[16];
        plat_mkdir(g_retro_dir);
        snprintf(name, sizeof name, "%s.HDD", im->id);
        path_join(hdpath, sizeof hdpath, g_retro_dir, name);
        run_floppy(im->title, im->data, im->size, hdpath);
    }
}

static void run_file(const char *path){
    uint8_t *buf;
    long got = read_file(path, &buf, is_iso(path) ? ISO_MAX : FD_MAX);
    if(got < 0){
        char m[300];
        snprintf(m, sizeof m, got == -2 ? (is_iso(path) ? "over 1 GB, too big to read into memory"
                                                         : "bigger than a 2.88 MB floppy")
                                        : "could not read the file");
        clear();
        put(4, 4, base_name(path), WHITE, BLACK);
        put(4, 6, m, YELLOW, BLACK);
        put(4, 8, "Any key goes on.", DGREY, BLACK);
        say(base_name(path), m);
        present(); wait_key();
        return;
    }
    if(is_iso(path) || (got >= 0x8006 && !memcmp(buf + 0x8001, "CD001", 5)))
        run_386(base_name(path), 0, 0, 0, 0, buf, (uint32_t)got);
    else {
        char hdpath[1100];
        hd_beside(hdpath, sizeof hdpath, path);
        run_floppy(base_name(path), buf, (uint32_t)got, hdpath);
    }
    free(buf);
}

/* ── the list ─────────────────────────────────────────────────────── */
#define DISK_MAX 64
static char     g_disk[DISK_MAX][256];
static uint64_t g_disk_size[DISK_MAX];
static int      g_ndisk;

static void list_cb(const char *name, uint64_t size, void *ctx){
    (void)ctx;
    if(g_ndisk >= DISK_MAX || !is_image(name) || strlen(name) >= 256) return;
    strcpy(g_disk[g_ndisk], name);
    g_disk_size[g_ndisk] = size;
    g_ndisk++;
}
static void rescan(void){
    g_ndisk = 0;
    plat_list(g_retro_dir, list_cb, 0);
}

#define LIST_Y  7
#define LIST_H 10

static void menu(void){
    int sel = 0, top = 0;
    rescan();
    for(;;){
        int n = RETRO_NIMAGES + g_ndisk;
        char row[90];
        if(sel >= n) sel = n - 1;
        if(sel < 0) sel = 0;
        if(sel < top) top = sel;
        if(sel >= top + LIST_H) top = sel - LIST_H + 1;
        host_title("Retro PC");
        clear();
        fill(0, 0, 80, 1, BLUE);
        put(2, 0, "Retro PC", WHITE, BLUE);
        put(56, 0, "Tech Haven Studios", LCYAN, BLUE);
        put(4, 2, "The old builds, on emulated PCs: an 8086 with a BIOS and a floppy for", LGREY, BLACK);
        put(4, 3, "K-DOS, and a 32-bit 386 that loads BOOT OS the way GRUB did.", LGREY, BLACK);
        put(4, 5, "Images", LCYAN, BLACK);
        for(int r = 0; r < LIST_H && top + r < n; r++){
            int i = top + r, on = sel == i;
            const char *where;
            if(i < RETRO_NIMAGES){
                snprintf(row, sizeof row, " %-38.38s", retro_images[i].title);
                where = retro_images[i].multiboot ? "built in, 386" : "built in, 8086";
            } else {
                snprintf(row, sizeof row, " %-38.38s", g_disk[i - RETRO_NIMAGES]);
                where = is_iso(g_disk[i - RETRO_NIMAGES]) ? "RETRO folder, CD" : "RETRO folder";
            }
            fill(6, LIST_Y + r, 40, 1, on ? CYAN : BLACK);
            put(6, LIST_Y + r, row, on ? BLACK : LGREY, on ? CYAN : BLACK);
            put(48, LIST_Y + r, where, on ? WHITE : DGREY, BLACK);
        }
        if(top > 0)          put(46, LIST_Y, "^", DGREY, BLACK);
        if(top + LIST_H < n) put(46, LIST_Y + LIST_H - 1, "v", DGREY, BLACK);

        if(sel < RETRO_NIMAGES){
            put(4, 18, retro_images[sel].about, WHITE, BLACK);
            if(retro_images[sel].multiboot)
                put(4, 19, "A 32-bit kernel, with the boot menu its CD had. No disk of its own.", DGREY, BLACK);
            else {
                snprintf(row, sizeof row, "Its hard disk: RETRO%c%s.HDD -- a file, never a real disk", plat_sep(), retro_images[sel].id);
                put(4, 19, row, DGREY, BLACK);
            }
        } else if(is_iso(g_disk[sel - RETRO_NIMAGES])){
            put(4, 18, "A CD image: its boot menu is offered, and a 32-bit kernel on it boots", WHITE, BLACK);
            put(4, 19, "on the 386 -- read into memory, never written.", WHITE, BLACK);
        } else {
            put(4, 18, "A floppy image from the RETRO folder. Its hard disk is a .HDD file", WHITE, BLACK);
            put(4, 19, "beside it.", WHITE, BLACK);
        }
        put(4, 21, "Your own: .IMG floppies and .ISO CDs in the RETRO folder beside this", DGREY, BLACK);
        snprintf(row, sizeof row, "program (%.60s), or dropped on the window.", g_retro_dir);
        put(4, 22, row, DGREY, BLACK);
        fill(0, 24, 80, 1, BLUE);
        put(2, 24, "Up/Down choose  Enter boot  O open a file  R look again  Q quit", WHITE, BLUE);
        present();

        int k = wait_key();
        if(k == RP_KEY_QUIT || k == 'q' || k == 'Q' || k == 27) return;
        if(k == 'r' || k == 'R'){ rescan(); continue; }
        if(k == KEY_UP && sel > 0) sel--;
        if(k == KEY_DOWN && sel < n - 1) sel++;
        if(k == KEY_PGUP){ sel -= LIST_H; if(sel < 0) sel = 0; }
        if(k == KEY_PGDN){ sel += LIST_H; if(sel > n - 1) sel = n - 1; }
        if(k == 'o' || k == 'O'){
            char p[1024];
            int got = host_pick_file(p, sizeof p);
            if(got == 0) run_file(p);
            else if(got < 0) say("Open", "no file chooser here: put the file in the RETRO folder, or give it on the command line");
        }
        if(k == RP_KEY_DROP && host_dropped()) run_file(host_dropped());
        if(k == '\n'){
            if(sel < RETRO_NIMAGES) run_builtin(sel);
            else {
                char p[1300];
                path_join(p, sizeof p, g_retro_dir, g_disk[sel - RETRO_NIMAGES]);
                run_file(p);
            }
        }
        if(g_quit) return;
    }
}

static void usage(void){
    fprintf(stderr,
        "Retro PC -- Tech Haven Studios' old builds on emulated PCs (HavenDOS %s)\n"
        "  retropc                the list\n"
        "  retropc <file>         boot a floppy (.IMG) or a CD (.ISO)\n"
        "  retropc --boot <ID>    boot a built-in image:", RP_VERSION);
    for(int i = 0; i < RETRO_NIMAGES; i++) fprintf(stderr, " %s", retro_images[i].id);
    fprintf(stderr, "\n  retropc --mem <MB>     the 386's memory, 4 to 256 (16)\n"
                    "Inside an old PC, F4 leaves.\n");
}

int rp_main(int argc, char **argv){
    const char *file = 0, *boot = 0;
    rp_font_build(g_font);
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--boot") && i + 1 < argc) boot = argv[++i];
        else if(!strcmp(argv[i], "--mem") && i + 1 < argc){
            int mb = atoi(argv[++i]);
            if(mb < 4 || mb > 256){ usage(); return 2; }
            g_mem386 = (uint32_t)mb << 20;
        }
        else if(!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "/?")){ usage(); return 0; }
        else if(!strcmp(argv[i], "--version")){ printf("Retro PC, HavenDOS %s\n", RP_VERSION); return 0; }
        else if(argv[i][0] == '-' && argv[i][1] == '-'){ usage(); return 2; }
        else file = argv[i];
    }
    path_join(g_retro_dir, sizeof g_retro_dir, plat_data_dir(), "RETRO");
    if(boot){
        int i;
        for(i = 0; i < RETRO_NIMAGES; i++) if(!strcmp(retro_images[i].id, boot)) break;
        if(i == RETRO_NIMAGES){ fprintf(stderr, "retropc: no built-in image %s\n", boot); usage(); return 2; }
        if(host_open("Retro PC") != 0) return 1;
        g_direct = 1;
        run_builtin(i);
    } else if(file){
        if(host_open("Retro PC") != 0) return 1;
        g_direct = 1;
        run_file(file);
    } else {
        if(host_open("Retro PC") != 0) return 1;
        menu();
    }
    if(g_quit) dump_screen();
    host_close();
    return 0;
}
