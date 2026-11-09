/* ════════════════════════════════════════════════════════════════════
   shell/exfatcmd.c -- the `exfat` command.  v0.8.8.1.

   A USB stick over 32 GB ships formatted exFAT. Since v0.8.6.1 `fat32`
   has NAMED one rather than printing "none found" -- the honest thing
   to do with no driver, and it turned "the stick is not readable" into
   a question with an answer. fs/exfat.c is the answer.

   ── IT USES THE SAME PARTITION WALK ─────────────────────────────────

   fat_scan_volumes(FATTYPE_EXFAT, ...) -- the one in shell/fat32cmd.c,
   filtered. Not a second scan with its own disk indices: that is the
   three-installers bug, and v0.8.6 is the release that deleted three
   copies of a bus dispatch which had drifted in all three.

   ── AND IT IS READ-ONLY, WITH NOTHING TO GET WRONG ──────────────────

   include/exfat.h has no write function in it at all, so there is no
   NULL to pass and no flag to set. The partition most likely to come
   back from a scan belongs to somebody else's computer.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/fat32.h"
#include "../include/fat32vol.h"
#include "../include/exfat.h"

extern void ui_page_reset(void);
extern int  ui_page_line(void);

static void up(const char *s){ vga_puts(s); }

static void putu(uint32_t v, int width){
    char b[12]; int n = 0;
    if(v == 0) b[n++] = '0';
    while(v){ b[n++] = (char)('0' + v % 10); v /= 10; }
    while(width-- > n) vga_putchar(' ');
    while(n) vga_putchar(b[--n]);
}

/* Bytes, in the unit a person reads. Integer only -- there is no
   floating point in this kernel and a size printed as "1 GB" when it
   is 1.9 is a worse answer than "1945 MB". */
static void put_size(uint64_t bytes)
{
    if(bytes >= (1ull << 30)){ putu((uint32_t)(bytes >> 30), 0); up(" GB"); }
    else if(bytes >= (1ull << 20)){ putu((uint32_t)(bytes >> 20), 0); up(" MB"); }
    else if(bytes >= 1024){ putu((uint32_t)(bytes >> 10), 0); up(" KB"); }
    else { putu((uint32_t)bytes, 0); up(" B"); }
}

static fat32_vol_t g_vols[FAT32_MAX_VOLS];
static int         g_nvols;

static int scan(void){
    g_nvols = fat_scan_volumes(FATTYPE_EXFAT, g_vols, FAT32_MAX_VOLS);
    return g_nvols;
}

static void no_volumes(void)
{
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    up("No exFAT volume found.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("'disk' lists the disks, 'fat32' the FAT volumes and what it\n");
    up("refused. A stick plugged in after boot is noticed on its own\n");
    up("as of v0.8.8 if it is on a root port; behind a hub, type\n");
    up("'usb rescan'.\n");
}

/* "<n>:<path>", or just "<n>". Returns the volume number, -1 on a
   malformed argument -- which is a different answer from volume 0,
   and `exfat ls x` used to be volume 0 by accident of atoi(). */
static int split_vol(const char *s, const char **path_out)
{
    int n = 0, any = 0;
    if(path_out) *path_out = "";
    if(!s || !*s) return -1;
    while(*s >= '0' && *s <= '9'){ n = n * 10 + (*s - '0'); s++; any = 1; }
    if(!any) return -1;
    if(*s == ':'){ s++; if(path_out) *path_out = s; }
    else if(*s) return -1;
    return n;
}

static void show_volumes(void)
{
    up("exFAT volumes:\n\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  #  disk  first LBA     size     cluster\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for(int i = 0; i < g_nvols; i++){
        up("  "); putu((uint32_t)i, 1);
        up("   ");  putu((uint32_t)g_vols[i].disk, 2);
        up("   "); putu((uint32_t)g_vols[i].lba, 10);
        up("  ");
        if(exfat_vol_mount_ro(&g_vols[i])){
            put_size(exfat_volume_bytes());
            up("   ");
            putu(exfat_cluster_bytes(), 0);
            up(" B");
            exfat_unmount();
        } else {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            up("would not mount: ");
            up(exfat_why_name(exfat_why()));
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        up("\n");
    }
    up("\n  exfat ls  <n>[:<dir>]   a directory\n");
    up("  exfat cat <n>:<path>   a file\n");
    up("\n  Read-only. Writing exFAT needs the allocation bitmap kept\n");
    up("  in step with the FAT, and that is a release of its own.\n");
}

static void do_ls(const char *arg)
{
    const char *path = "";
    int v = split_vol(arg, &path);
    exfat_dir_t d;
    exfat_dirent_t e;
    int n = 0, dirs = 0;

    if(v < 0 || v >= g_nvols){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("No such volume. 'exfat' lists them.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    if(!exfat_vol_mount_ro(&g_vols[v])){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("Could not mount: "); up(exfat_why_name(exfat_why())); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    {
        int r = exfat_opendir(path, &d);
        if(r != EXFAT_OK){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            up("Cannot list it: "); up(exfat_why_name(-r)); up("\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            exfat_unmount();
            return;
        }
    }
    ui_page_reset();
    while(exfat_readdir(&d, &e) == 1){
        if(e.is_dir){
            vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            up("  <DIR>        ");
            dirs++;
        } else {
            vga_set_color(VGA_WHITE, VGA_BLACK);
            up("  ");
            put_size(e.size);
            for(int k = (int)strlen("  ") ; k < 10; k++) vga_putchar(' ');
            up("   ");
        }
        up(e.name);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("\n");
        n++;
        if(ui_page_line()) break;
    }
    up("\n  ");
    putu((uint32_t)n, 0);
    up(n == 1 ? " entry" : " entries");
    if(dirs){ up(", "); putu((uint32_t)dirs, 0); up(" of them directories"); }
    up("\n");
    exfat_unmount();
}

static void do_cat(const char *arg)
{
    const char *path = "";
    int v = split_vol(arg, &path);
    static uint8_t buf[512];
    exfat_dirent_t st;
    uint64_t off = 0;
    int r;

    if(v < 0 || v >= g_nvols){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("No such volume. 'exfat' lists them.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    if(!path[0]){
        up("Which file?  exfat cat <n>:<path>\n");
        return;
    }
    if(!exfat_vol_mount_ro(&g_vols[v])){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("Could not mount: "); up(exfat_why_name(exfat_why())); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    r = exfat_stat(path, &st);
    if(r != EXFAT_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("Cannot read it: "); up(exfat_why_name(-r)); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        exfat_unmount();
        return;
    }
    ui_page_reset();
    while(off < st.size){
        int got = exfat_read(path, off, buf, sizeof buf);
        if(got <= 0) break;
        for(int i = 0; i < got; i++){
            char c = (char)buf[i];
            /* A binary file on screen is a scrambled terminal. Printable
               and newline through; everything else as a dot, which is
               what `hex` is for. */
            if(c == '\n'){ vga_putchar('\n'); if(ui_page_line()) { off = st.size; break; } }
            else if(c == '\t' || (c >= 32 && c < 127)) vga_putchar(c);
            else vga_putchar('.');
        }
        off += (uint64_t)got;
    }
    up("\n");
    exfat_unmount();
}

void cmd_exfat(const char *arg1, const char *arg2)
{
    if(!arg1 || !arg1[0]){
        if(scan() == 0){ no_volumes(); return; }
        show_volumes();
        return;
    }
    if(!strcmp(arg1, "ls")){ scan(); do_ls(arg2); return; }
    if(!strcmp(arg1, "cat")){ scan(); do_cat(arg2); return; }

    /* An argument this command does not know. v0.8.6.6's lesson: a
       command that quietly does something else when it does not
       understand you is worse than one that refuses, and it cost a
       round trip to the laptop. */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    up("exfat: no such option: "); up(arg1); up("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("  exfat                  every exFAT volume the kernel can see\n");
    up("  exfat ls  <n>[:<dir>]  a directory on volume n\n");
    up("  exfat cat <n>:<path>   a file on volume n\n");
}
