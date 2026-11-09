/*
 * fat32cmd.c -- find and inspect FAT32 volumes, READ ONLY.
 *
 * Phase A gave HavenDOS a FAT32 driver.  This is how you see it working on
 * a machine rather than in a test, and it is the thing to run first on the
 * HP: it says whether the ESP the installer will one day write into is
 * visible, readable, and shaped the way Phase B assumes.
 *
 * THE MOUNT HERE IS READ-ONLY, AT THE DRIVER.
 * fat32_mount_dev() is handed a NULL write function, so every write entry
 * point in fs/fat32.c refuses before it reaches a sector -- not because
 * this command is careful, but because there is no write path wired in for
 * it to be careless with.  That matters more than usual: the partition most
 * likely to turn up here is an EFI System Partition, which belongs to
 * whatever else is installed on the machine.  Handoff section 13b exists
 * because of exactly that partition, and the whole point of this command is
 * to look at it on somebody's real laptop.
 *
 * The partition type byte is NOT trusted.  Every candidate's boot sector is
 * read and its cluster count counted, because 0x0B and 0x0C are a hint and
 * real media lies about them routinely -- which is also why a volume whose
 * type byte says something else entirely still shows up here if it is in
 * fact FAT32.
 *
 *   fat32                  every FAT32 volume the kernel can see
 *   fat32 ls  <n>          the root directory of volume n
 *   fat32 ls  <n>:<dir>    a directory on volume n
 *   fat32 cat <n>:<path>   a file on volume n
 *
 * The n:path form is because exec_cmd() passes a command two arguments and
 * not three.  Widening it for one command would touch every dispatch line
 * in shell.c, and house rule one is that new commands do not go editing
 * shell.c's brace structure.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/fat32.h"
#include "../include/fat32vol.h"
#include "../include/gpt.h"

/* The unified disk layer from shell/iso_installer.c.  Every disk the
   installer can see arrives through these, which is the same set this
   command walks -- one scan, one answer, rather than a second copy that
   drifts.  (Two copies of a disk scan is the three-installers bug.) */
extern int      inst_disk_scan(void);
extern int      inst_disk_count(void);
extern int      inst_disk_read(int idx, uint64_t lba, uint8_t *buf);
extern uint64_t inst_disk_sectors(int idx);
extern int      inst_disk_is_ata(int idx);
extern int      inst_disk_is_nvme(int idx);
extern int      inst_disk_has_gpt(int idx);
extern int      inst_disk_gpt_parts(int idx, gpt_part_t *out, int max);

extern int  vfs_using_fat32(void);
extern void vfs_refresh_fat32(void);

#define MAX_VOLS FAT32_MAX_VOLS

static fat32_vol_t g_vols[MAX_VOLS];
static int         g_nvols;

/* The block seam: ctx is the disk index.  inst_disk_read() already returns
   1 for success, which is the convention fs/fat32.c uses, so nothing is
   translated here and there is no polarity to get backwards. */
static int vol_read(void *ctx, uint64_t lba, uint8_t *buf){
    return inst_disk_read((int)(long)ctx, lba, buf);
}

/* Pull the label out of a FAT32 boot sector.
   This is BPB_VolLab at offset 71, which is a CONVENIENCE COPY.  The name a
   FAT volume actually carries is a root-directory entry with attribute
   0x08, and the two are allowed to disagree -- 0.8 Beta shipped an
   installer that wrote this field and not that entry, so `search --label
   HAVENDOS` failed on every disk it produced.  Labelled as the BPB copy on
   screen for that reason. */
static void bpb_label(const uint8_t *bs, char *out){
    int j = 0;
    for(int i = 0; i < 11; i++){
        char c = (char)bs[71 + i];
        if(c == ' ' && j == 0) continue;
        out[j++] = c;
    }
    while(j > 0 && out[j-1] == ' ') j--;
    out[j] = 0;
}

/* ── the partitions this kernel cannot read ──────────────────────────
   v0.8.6.1. Every partition examined and rejected, with what it turned
   out to be.

   `fat32` used to print "none found" and stop. On the HP that was the
   answer for a 64 GB USB stick formatted exFAT -- so "this kernel
   cannot read that filesystem" looked exactly like "this kernel cannot
   see the disk", which is the question somebody staring at the screen
   is actually asking. The installer's drive picker learned the same
   lesson one release earlier: "Refused" taught the user nothing; the
   reason tells them whether anything they could do would change it. */
#define MAX_SKIP 12
typedef struct {
    int      disk;
    uint64_t lba;
    uint64_t sectors;
    int      from_gpt;
    uint8_t  mbr_type;
    const char *what;      /* fat_fs_name(), or why it was not read */
} skipped_t;
static skipped_t g_skip[MAX_SKIP];
static int       g_nskip;

/* Which FAT type this walk is collecting. FATTYPE_NONE means all of
   them. Set by the entry points below and never anywhere else. */
static int       g_want = FATTYPE_32;

static void note_skip(int disk, uint64_t lba, uint64_t sectors,
                      int from_gpt, uint8_t mbr_type, const char *what)
{
    if(g_nskip >= MAX_SKIP) return;
    g_skip[g_nskip].disk     = disk;
    g_skip[g_nskip].lba      = lba;
    g_skip[g_nskip].sectors  = sectors;
    g_skip[g_nskip].from_gpt = from_gpt;
    g_skip[g_nskip].mbr_type = mbr_type;
    g_skip[g_nskip].what     = what;
    g_nskip++;
}

/* Is the volume at this LBA a FAT32 one?  Record it if so, and record
   what it was instead if not. */
static void consider(int disk, uint64_t lba, uint64_t sectors,
                     int from_gpt, int is_esp, uint8_t mbr_type)
{
    uint8_t bs[512];
    if(g_nvols >= MAX_VOLS) return;
    if(!lba) return;
    if(!inst_disk_read(disk, lba, bs)){
        /* A partition whose first sector will not read is worth saying
           out loud: it is a disk problem, not a filesystem one. */
        note_skip(disk, lba, sectors, from_gpt, mbr_type,
                  "could not be read");
        return;
    }
    {   int t = fat_probe_type(bs);
        /* Recorded by TYPE rather than filtered to FAT32 here, so one
           partition walk serves `fat32` and `fat16` both. g_want picks
           which ones the caller gets. A FAT16 volume is still listed as
           skipped for `fat32`, with its type as the reason, because
           "there is a FAT16 volume here, use the other command" is the
           answer somebody wants. */
        if(t != FATTYPE_12 && t != FATTYPE_16 && t != FATTYPE_32){
            /* ── v0.8.8.1: exFAT, through the same walk ────────────
               An exFAT volume has no BPB to count, so fat_probe_type()
               says NONE for it and always will. fat_fs_name() already
               identified it by the signature at offset 3 -- that is
               what v0.8.6.1 added so `fat32` could say what it had
               found instead of "none found".
               Now that there is a reader, `exfat` asks for them
               through this same walk. A second partition scan with
               its own indices is the three-installers bug. */
            const char *nm = fat_fs_name(bs);
            if(g_want == FATTYPE_EXFAT && nm[0] == 'e' && nm[1] == 'x'){
                fat32_vol_t *ev = &g_vols[g_nvols++];
                ev->disk = disk; ev->lba = lba; ev->sectors = sectors;
                ev->from_gpt = from_gpt; ev->is_esp = is_esp;
                ev->mbr_type = mbr_type;
                ev->fattype = FATTYPE_EXFAT;
                ev->label[0] = 0;
                return;
            }
            note_skip(disk, lba, sectors, from_gpt, mbr_type, nm);
            return;
        }
        if(g_want != FATTYPE_NONE && t != g_want){
            note_skip(disk, lba, sectors, from_gpt, mbr_type,
                      fat_fs_name(bs));
            return;
        }
        g_vols[g_nvols].fattype = (uint8_t)t;
    }
    fat32_vol_t *v = &g_vols[g_nvols++];
    v->disk = disk; v->lba = lba; v->sectors = sectors;
    v->from_gpt = from_gpt; v->is_esp = is_esp; v->mbr_type = mbr_type;
    bpb_label(bs, v->label);
}

static uint32_t rd32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8)
         | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* Walk every disk's partition table and collect the FAT32 volumes. */
static int scan_into_g(void){
    g_nvols = 0;
    g_nskip = 0;
    int nd = inst_disk_scan();
    for(int d = 0; d < nd && g_nvols < MAX_VOLS; d++){
        if(inst_disk_has_gpt(d)){
            gpt_part_t p[GPT_MAX_PARTS];
            int n = inst_disk_gpt_parts(d, p, GPT_MAX_PARTS);
            for(int i = 0; i < n && g_nvols < MAX_VOLS; i++){
                uint64_t cnt = (p[i].last_lba >= p[i].first_lba)
                             ? (p[i].last_lba - p[i].first_lba + 1) : 0;
                consider(d, p[i].first_lba, cnt, 1, p[i].is_esp, 0);
            }
            continue;
        }
        uint8_t mbr[512];
        if(!inst_disk_read(d, 0, mbr)) continue;
        if(!(mbr[510] == 0x55 && mbr[511] == 0xAA)) continue;
        for(int i = 0; i < 4 && g_nvols < MAX_VOLS; i++){
            const uint8_t *e = mbr + 446 + i*16;
            uint32_t lba = rd32(e + 8), cnt = rd32(e + 12);
            if(!lba || !cnt) continue;
            /* 0x0B and 0x0C are the FAT32 type bytes, and every candidate
               is probed regardless of what its byte says.  A partition
               labelled 0x07 that is really FAT32 still belongs on this list;
               the byte is a hint, the cluster count is the answer. */
            consider(d, lba, cnt, 0, 0, e[4]);
        }
    }
    return g_nvols;
}

/* ── the public half, for Phase B (include/fat32vol.h) ──────────── */
/* One walk, filtered. `want` is FATTYPE_16, FATTYPE_32, or FATTYPE_NONE
   for every FAT volume on the machine. */
int fat_scan_volumes(int want, fat32_vol_t *out, int max){
    int n;
    g_want = want;
    n = scan_into_g();
    g_want = FATTYPE_32;          /* back to the default for every other
                                     caller, so a borrowed filter cannot
                                     leak into the next scan */
    if(out && max > 0){
        if(n > max) n = max;
        for(int i = 0; i < n; i++) out[i] = g_vols[i];
    }
    return n;
}

/* Every exFAT volume, same walk, same shape as fat32_scan_volumes().
   fs/update_src.c tries FAT32 and then this, and should not have to
   know that one of them spells its filter differently. v0.8.8.2. */
int exfat_scan_volumes(fat32_vol_t *out, int max){
    return fat_scan_volumes(FATTYPE_EXFAT, out, max);
}

int fat32_scan_volumes(fat32_vol_t *out, int max){
    int n;
    g_want = FATTYPE_32;
    n = scan_into_g();
    if(out && max > 0){
        if(n > max) n = max;
        for(int i = 0; i < n; i++) out[i] = g_vols[i];
    }
    return n;
}

/* Read-only by construction: the NULL write function means fs/fat32.c
   refuses every write before it reaches a sector.  The partition on the
   other end of this is usually somebody else's. */
/* ── v0.8.8.1 ─────────────────────────────────────────────────────
   The exFAT counterpart, through the same disk seam. There is no
   write function to pass: include/exfat.h has none, so the refusal
   is in the interface rather than in the discipline of the caller --
   which is a step beyond fat32_vol_mount_ro()'s NULL. */
int exfat_vol_mount_ro(const fat32_vol_t *v){
    extern int exfat_mount_dev(int (*)(void*, uint64_t, uint8_t*),
                               void *, uint64_t);
    if(!v) return 0;
    return exfat_mount_dev(vol_read, (void*)(long)v->disk, v->lba);
}

int fat32_vol_mount_ro(const fat32_vol_t *v){
    if(!v) return 0;
    return fat32_mount_dev(vol_read, 0, (void*)(long)v->disk, v->lba);
}

void fat32_vol_release(void){
    fat32_unmount();
    /* Put back whatever the VFS had mounted.  Without this, inspecting a
       volume on a system whose own filesystem is FAT32 would leave every
       later vfs_read() talking to nothing. */
    if(vfs_using_fat32()) vfs_refresh_fat32();
}

/* ── the command's own shorthands ───────────────────────────────── */
static int scan_volumes(void){ return scan_into_g(); }
static int vol_mount(int n){
    if(n < 0 || n >= g_nvols) return 0;
    return fat32_vol_mount_ro(&g_vols[n]);
}
static void vol_release(void){ fat32_vol_release(); }

static void putu(uint32_t v, int width){
    char s[12]; utoa(v, s, 10);
    int l = (int)strlen(s);
    vga_puts(s);
    while(l++ < width) vga_puts(" ");
}

/* The partitions that were looked at and are not FAT32, with what each
   one is. Printed under the volume list -- or instead of it, when there
   are no FAT32 volumes at all and the honest answer is "there are
   partitions here and this kernel cannot read any of them". */
static void show_skipped(void)
{
    int i;
    if(g_nskip == 0) return;

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("\n  partitions this kernel does not read:\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for(i = 0; i < g_nskip; i++){
        skipped_t *k = &g_skip[i];
        vga_puts("      disk ");
        putu((uint32_t)k->disk, 3);
        putu((uint32_t)k->lba, 12);
        {   uint32_t mb = (uint32_t)(k->sectors >> 11);
            putu(mb, 6); vga_puts("MB  "); }
        if(k->from_gpt) vga_puts("GPT       ");
        else {
            char h[8]; utoa(k->mbr_type, h, 16);
            vga_puts("MBR 0x");
            if(k->mbr_type < 16) vga_puts("0");
            vga_puts(h);
            vga_puts("  ");
        }
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts(k->what);
        vga_puts("\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  exFAT and NTFS have no driver here. A FAT16 one is read\n"
             "  with 'fat16' rather than this command.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

static int   g_listed;
static void  list_cb(const char *name, int is_dir, uint32_t size){
    g_listed++;
    vga_set_color(is_dir ? VGA_LIGHT_CYAN : VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts("    ");
    vga_puts(is_dir ? "<DIR>  " : "       ");
    if(!is_dir){ putu(size, 10); } else vga_puts("          ");
    vga_puts(name);
    vga_puts("\n");
}

/* Split "3" or "3:/EFI/BOOT" into a volume number and a path.
   Returns the volume number, or -1 if the digits are not digits. */
static int split_vol(const char *s, const char **path_out){
    int v = 0, any = 0;
    *path_out = 0;
    if(!s || !*s) return -1;
    for(; *s; s++){
        if(*s == ':'){ *path_out = s + 1; break; }
        if(*s < '0' || *s > '9') return -1;
        v = v*10 + (*s - '0');
        any = 1;
    }
    if(!any) return -1;
    if(*path_out && !**path_out) *path_out = 0;   /* "3:" is just "3" */
    return v;
}

void cmd_fat32(const char *arg1, const char *arg2)
{
    const char *path = 0;
    vga_set_color(VGA_WHITE, VGA_BLACK);

    if(arg1 && !strcmp(arg1, "ls")){
        int n = split_vol(arg2, &path);
        if(scan_volumes() == 0 || n < 0 || n >= g_nvols){
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("  no such volume -- run 'fat32' for the list\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return;
        }
        if(!vol_mount(n)){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_puts("  that volume would not mount\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            vol_release();
            return;
        }
        const char *dir = path ? path : "/";
        vga_puts("  "); vga_puts(dir); vga_puts("  on volume ");
        putu((uint32_t)n, 0); vga_puts("  (read-only)\n");
        g_listed = 0;
        fat32_list_dir(dir, list_cb);
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("  ");
        putu((uint32_t)g_listed, 0);
        vga_puts(" entries\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vol_release();
        return;
    }

    if(arg1 && !strcmp(arg1, "cat")){
        static char buf[2048];
        int n = split_vol(arg2, &path);
        if(scan_volumes() == 0 || n < 0 || n >= g_nvols || !path){
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("  usage: fat32 cat <volume>:<path>\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return;
        }
        if(!vol_mount(n)){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_puts("  that volume would not mount\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            vol_release();
            return;
        }
        int got = fat32_read(path, buf, sizeof buf - 1);
        if(got < 0){
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("  not found on that volume\n");
        } else {
            buf[got] = 0;
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            vga_puts(buf);
            if(got == (int)sizeof buf - 1){
                vga_set_color(VGA_DARK_GREY, VGA_BLACK);
                vga_puts("\n  -- truncated at 2047 bytes --\n");
            }
        }
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vol_release();
        return;
    }

    /* The listing. */
    int n = scan_volumes();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("FAT32 volumes\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  disks scanned: ");
    putu((uint32_t)inst_disk_count(), 0);
    vga_puts("\n");
    if(n == 0){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("  none found\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        if(g_nskip == 0)
            vga_puts("  and no partition table entry to look at either -- so\n"
                     "  the disk itself may not be being read. Check 'disk'\n"
                     "  and 'lspci' before suspecting this driver.\n");
        else
            vga_puts("  but partitions WERE found and examined; what each\n"
                     "  one turned out to be is listed below.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        show_skipped();
        return;
    }
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("   #  disk  start LBA   size     source          BPB label\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for(int i = 0; i < n; i++){
        fat32_vol_t *v = &g_vols[i];
        vga_puts("  ");
        putu((uint32_t)i, 4);
        putu((uint32_t)v->disk, 6);
        putu((uint32_t)v->lba, 12);
        {   /* MB, computed with a shift: no 64-bit division in the kernel */
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
            vga_puts((v->mbr_type==0x0B || v->mbr_type==0x0C) ? "   " : " ? ");
        }
        vga_puts(v->label);
        vga_puts("\n");
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  a '?' marks a type byte that does not say FAT32 on a volume\n"
             "  that is -- the cluster count decides, not the byte\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    show_skipped();
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("  fat32 ls  <n>[:dir]     list a directory   (read-only)\n");
    vga_puts("  fat32 cat <n>:<path>    read a file        (read-only)\n");
    vga_puts("  nothing here writes to a disk\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}
