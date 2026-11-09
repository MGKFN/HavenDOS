/*
 * fat16cmd.c -- find and inspect FAT16 volumes on any disk, and copy a
 * file onto one.
 *
 * ── WHY THIS EXISTS ──────────────────────────────────────────────────
 *
 * Somebody typed `fat16` on the HP after v0.8.6 and got
 *
 *     error: 'fat16' command not found
 *
 * The USB stick was registered, `disk` listed it as
 * "USB disk 0 : Kingston DataTraveler 3.0", and `fat32` could read a
 * FAT32 volume on it -- but a FAT16 one had no command at all. The
 * 0.8.6 handoff listed that as a known gap and called it "a command
 * rather than a driver". This is the command.
 *
 *   fat16                  every FAT16 volume the kernel can see
 *   fat16 ls  <n>          the root directory of volume n
 *   fat16 cat <n>:<path>   a file on volume n
 *   fat16 cp <src> <n>:<dst>   copy a file from this system onto one
 *
 * The n:path form is because exec_cmd() passes a command two arguments
 * and not three. Widening it for one command would touch every dispatch
 * line in shell.c, and house rule one is that new commands do not go
 * editing shell.c's brace structure.
 *
 * ── v0.8.6.3: ONE OF THESE WRITES, AND IT USES A DIFFERENT DOOR ──────
 *
 * `fat16 cp` copies a file from this system's own volume onto a volume
 * on a stick. It is the only thing in this file that writes, and it
 * does not get there by passing a flag: fs/fat16.c has a second entry
 * point, fat16_borrow_rw(), which refuses a disk that is not removable,
 * refuses the boot disk, refuses a read-only disk or volume, and
 * refuses a FAT bigger than the cache.
 *
 * The refusals live in the driver because that is the only place that
 * cannot be bypassed by a caller who forgets. What this file adds is
 * the ORDER, which matters just as much:
 *
 *   read the source file FIRST, into a buffer, while the system's own
 *   volume is still mounted -- then borrow, then write, then return.
 *
 * Borrowing displaces the system volume. A `cp` that borrowed first and
 * then tried to read its source would read the source path off the
 * STICK, and on a stick that happened to have a file of that name it
 * would copy that file onto itself and report success.
 *
 * ── READ ONLY AT THE DRIVER, NOT BY DISCIPLINE ───────────────────────
 *
 * fs/fat16.c holds ONE volume -- a device index, a partition LBA, a BPB
 * and a cached FAT -- so looking at a stick means displacing the
 * system's own volume and putting it back. fat16_borrow() sets a flag
 * that makes fat16_raw_write() refuse before it reaches a sector, and
 * fat16_return() re-mounts the system's volume from its own disk.
 *
 * That is the same shape as fs/fat32.c's mount_ro/release pair and the
 * same reason: the volume most likely to turn up on this screen belongs
 * to somebody else, and a refusal that depends on every caller
 * remembering is not a refusal.
 *
 * ── AND THE PARTITION WALK IS NOT A SECOND COPY ──────────────────────
 *
 * fat_scan_volumes() is shell/fat32cmd.c's walk, filtered by FAT type.
 * Writing a second one here would be the shape of bug v0.8.6 spent its
 * whole release deleting: three files that each knew how to find a
 * thing, and only some of them taught about a new case.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/fat16.h"
#include "../include/fat32.h"
#include "../include/fat32vol.h"
#include "../include/blkdev.h"

extern void utoa(uint32_t, char*, int);

#define MAX_VOLS FAT32_MAX_VOLS

static fat32_vol_t g_vols[MAX_VOLS];
static int         g_nvols;

static void putu(uint32_t v, int width){
    char s[12]; utoa(v, s, 10);
    int l = (int)strlen(s);
    vga_puts(s);
    while(l++ < width) vga_puts(" ");
}

static int scan(void){
    g_nvols = fat_scan_volumes(FATTYPE_16, g_vols, MAX_VOLS);
    return g_nvols;
}

/* Borrow the driver for volume n. Pair EVERY success with release(). */
static int borrow(int n){
    if(n < 0 || n >= g_nvols) return 0;
    return fat16_borrow(g_vols[n].disk, g_vols[n].lba);
}
/* The writable borrow, for `fat16 cp` and nothing else. Every refusal
   that makes this safe is inside fat16_borrow_rw(); this only passes the
   volume on. */
static int borrow_rw(int n){
    if(n < 0 || n >= g_nvols) return 0;
    return fat16_borrow_rw(g_vols[n].disk, g_vols[n].lba);
}
static void release(void){ fat16_return(); }

/* ── the listing ────────────────────────────────────────────────── */
static int  g_listed;
static void list_cb(const char *name, int is_dir, uint32_t size){
    g_listed++;
    vga_set_color(is_dir ? VGA_LIGHT_CYAN : VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts("    ");
    vga_puts(is_dir ? "<DIR>  " : "       ");
    if(!is_dir) putu(size, 10); else vga_puts("          ");
    vga_puts(name);
    vga_puts("\n");
}

/* Split "3" or "3:/BOOT/X.TXT" into a volume number and a path.
   Returns the volume number, or -1 if the digits are not digits. */
static int split_vol(const char *s, const char **path_out){
    int v = 0, any = 0;
    *path_out = 0;
    if(!s || !*s) return -1;
    for(; *s; s++){
        if(*s == ':'){ *path_out = s + 1; break; }
        if(*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        any = 1;
    }
    return any ? v : -1;
}

static void no_volume(int n){
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("  volume ");
    putu((uint32_t)n, 0);
    vga_puts(" is not there -- run 'fat16' for the list\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

static void list_volumes(void){
    int n = scan();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("FAT16 volumes\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts("  disks scanned: ");
    putu((uint32_t)blk_count(), 0);
    vga_puts("\n");

    if(n == 0){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  none found\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        /* 'fat32' prints every partition it examined and what each one
           turned out to be, which is the screen that answers "then what
           IS on my stick". Pointed at rather than printed twice. */
        vga_puts("  'fat32' lists every partition it looked at and what\n"
                 "  each one is -- exFAT and NTFS have no driver here.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("   #  disk  start LBA   size     source          bus\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for(int i = 0; i < n; i++){
        fat32_vol_t *v = &g_vols[i];
        vga_puts("  ");
        putu((uint32_t)i, 4);
        putu((uint32_t)v->disk, 6);
        putu((uint32_t)v->lba, 12);
        {   /* MB with a shift: no 64-bit division in the kernel */
            uint32_t mb = (uint32_t)(v->sectors >> 11);
            putu(mb, 4); vga_puts("MB  ");
        }
        if(v->from_gpt)
            vga_puts(v->is_esp ? "GPT ESP         " : "GPT entry       ");
        else {
            char h[8]; utoa(v->mbr_type, h, 16);
            vga_puts("MBR type 0x");
            if(v->mbr_type < 16) vga_puts("0");
            vga_puts(h);
            vga_puts((v->mbr_type==0x04 || v->mbr_type==0x06 ||
                      v->mbr_type==0x0E) ? "   " : " ? ");
        }
        /* The bus, from the one place in the tree that names one. */
        vga_puts(blk_bus_name(v->disk));
        vga_puts("\n");
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  a '?' marks a type byte that does not say FAT16 on a volume\n"
             "  that is -- the cluster count decides, not the byte\n");
    vga_puts("  fat16 ls  <n>           the root directory  (read-only)\n");
    vga_puts("  fat16 cat <n>:<path>    read a file         (read-only)\n");
    vga_puts("  fat16 cp <src> <n>:<p>  copy onto removable media only\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

static void do_ls(const char *arg){
    const char *path = 0;
    int n = split_vol(arg, &path);

    scan();
    if(n < 0 || n >= g_nvols){ no_volume(n < 0 ? 0 : n); return; }

    if(!borrow(n)){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  volume ");
        putu((uint32_t)n, 0);
        vga_puts(" would not mount\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("  /  on volume ");
    putu((uint32_t)n, 0);
    vga_puts("  (read-only)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    if(path && *path){
        /* Stated rather than silently ignored. fs/fat16.c's public
           listing walks the ROOT directory only, and a command that
           took a path and showed the root anyway would be lying about
           which directory is on screen. */
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  (a subdirectory path is not listed yet -- this is the\n"
                 "   root. 'fat16 cat' does take a path.)\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    g_listed = 0;
    fat16_list(list_cb);
    release();

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  ");
    putu((uint32_t)g_listed, 0);
    vga_puts(g_listed == 1 ? " entry\n" : " entries\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* One file's worth. 8 KB, because this is a look-at-it command and a
   kernel with a 255 KB heap should not hand a whole file a buffer. */
#define CAT_MAX 8192
static char g_cat[CAT_MAX];

static void do_cat(const char *arg){
    const char *path = 0;
    int n = split_vol(arg, &path);
    int r;

    scan();
    if(n < 0 || n >= g_nvols){ no_volume(n < 0 ? 0 : n); return; }
    if(!path || !*path){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  usage: fat16 cat <n>:<path>\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    if(!borrow(n)){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  volume ");
        putu((uint32_t)n, 0);
        vga_puts(" would not mount\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    r = fat16_read(path, g_cat, CAT_MAX - 1);
    release();          /* BEFORE printing: the volume is not needed to
                           show what was already read, and holding
                           somebody else's disk open across a screenful
                           of output is holding it longer than needed */

    if(r < 0){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  not found on that volume: ");
        vga_puts(path);
        vga_puts("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    if(r >= CAT_MAX - 1){
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  (first 8 KB)\n");
    }
    g_cat[r < 0 ? 0 : r] = 0;
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts(g_cat);
    if(r > 0 && g_cat[r-1] != '\n') vga_puts("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}


/* ── fat16 cp <src> <n>:<dst> ────────────────────────────────────────
   Copy a file from this system's own volume onto a volume on a stick.

   THE BUFFER comes from the physical allocator for the length of the
   copy and goes straight back, which is how fs/update.c stages an 8 MB
   kernel without reserving 8 MB of BSS at every boot. 4 MB is enough
   for havendos.elf -- staging an update onto a stick is the reason this
   command exists -- and a file that does not fit is REFUSED rather than
   truncated. A short file written under a full file's name is a wrong
   answer presented as a right one.

   THE ORDER is read, borrow, write, return. See the note at the top of
   this file for what borrowing first would do. */
#define CP_MAX (4u * 1024u * 1024u)
#define CP_BLOCKS (CP_MAX / 4096u)
extern void *pmm_alloc_contig(uint32_t nblocks);
extern void  pmm_free_contig(void *p, uint32_t nblocks);

static void cp_err(const char *msg){
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_puts("  "); vga_puts(msg); vga_puts("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

static void do_cp(const char *args)
{
    char src[128];
    const char *dst_arg, *dst;
    int  v, i, got, wrote;
    char *buf;

    /* exec_cmd() hands this command arg2 as the REST OF THE LINE, so
       both operands arrive as one string and are split here. */
    if(!args || !*args){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  fat16 cp <src> <n>:<dst>\n");
        vga_puts("  e.g. fat16 cp /boot/havendos.elf 1:/HAVENDOS.ELF\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    for(i = 0; args[i] && args[i] != ' ' && i < (int)sizeof src - 1; i++)
        src[i] = args[i];
    src[i] = 0;
    if(args[i] != ' '){ cp_err("needs a destination: <n>:<path>"); return; }
    while(args[i] == ' ') i++;
    dst_arg = args + i;

    v = split_vol(dst_arg, &dst);
    if(v < 0 || !dst || !*dst){
        cp_err("the destination must look like 1:/NAME.EXT");
        return;
    }

    if(!scan()){ cp_err("no FAT16 volumes found"); return; }
    if(v >= g_nvols){ no_volume(v); return; }

    /* Printed here only so the message is useful. fat16_borrow_rw() is
       what refuses, and it refuses whatever this prints. */
    if(!blk_removable(g_vols[v].disk)){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  volume ");
        putu((uint32_t)v, 0);
        vga_puts(" is on ");
        vga_puts(blk_bus_name(g_vols[v].disk));
        vga_puts(" disk ");
        putu((uint32_t)g_vols[v].disk, 0);
        vga_puts(", not removable\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        cp_err("this copies onto removable media only");
        return;
    }

    /* ── 1. read the source, system volume still mounted ───────────── */
    if(!fat16_exists_path(src)){
        cp_err("no such file on this system");
        return;
    }
    buf = (char *)pmm_alloc_contig(CP_BLOCKS);
    if(!buf){ cp_err("not enough contiguous memory for the copy"); return; }

    got = fat16_read_path(src, buf, CP_MAX);
    if(got < 0){
        pmm_free_contig(buf, CP_BLOCKS);
        cp_err("that file would not read");
        return;
    }
    if((uint32_t)got >= CP_MAX){
        /* Equal to the buffer means it MAY be longer, and nothing here
           can tell that from an exact fit. Refused. */
        pmm_free_contig(buf, CP_BLOCKS);
        cp_err("that file is too big for this command, 4 MB limit");
        return;
    }

    /* ── 2. borrow for writing ─────────────────────────────────────── */
    if(!borrow_rw(v)){
        pmm_free_contig(buf, CP_BLOCKS);
        cp_err("that volume would not mount for writing");
        return;
    }

    /* ── 3. write, 4. return -- in that order, and always both ─────── */
    wrote = fat16_write_path(dst, buf, (uint32_t)got);
    release();
    pmm_free_contig(buf, CP_BLOCKS);

    /* fat16_write_path() returns the NUMBER OF BYTES WRITTEN. Treating
       it as 0/1 cost v0.7.5 a release, so it is compared with the
       length rather than tested for truth. */
    if(wrote != got){
        cp_err("the copy did not complete -- what is on the stick is "
               "not what was asked for");
        return;
    }
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts("  copied ");
    putu((uint32_t)got, 0);
    vga_puts(" bytes to ");
    putu((uint32_t)v, 0);
    vga_puts(dst);
    vga_puts("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_fat16(const char *arg1, const char *arg2)
{
    if(!arg1 || !*arg1){ list_volumes(); return; }
    if(!strcmp(arg1, "ls")){  do_ls(arg2);  return; }
    if(!strcmp(arg1, "cat")){ do_cat(arg2); return; }
    if(!strcmp(arg1, "cp")){  do_cp(arg2);  return; }

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("  fat16                   list FAT16 volumes\n");
    vga_puts("  fat16 ls  <n>           the root directory  (read-only)\n");
    vga_puts("  fat16 cat <n>:<path>    read a file         (read-only)\n");
    vga_puts("  fat16 cp <src> <n>:<p>  copy a file onto a stick\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}
