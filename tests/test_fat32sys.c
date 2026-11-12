/* tests/test_fat32sys.c -- the system volume, parked, borrowed and renamed
 *
 * v0.9.3 "Gaia". An install above 2047 MB is FAT32, so fs/fat32.c's one
 * mounted volume became the SYSTEM's -- while the ESP update source, the
 * `fat32` command, the installer and the uninstaller all go on borrowing
 * it. This suite stands two real mkfs.fat images in for the machine's own
 * volume and a USB stick, and checks the rules fs/fat32.c now keeps:
 *
 *   - a borrow parks the system volume; ending it brings the system back
 *   - fat32_unmount() never drops the system volume
 *   - a failed borrow leaves the system mounted
 *   - fat32_sys_begin()/_end() give the system ONE turn mid-borrow, and
 *     writes in that turn land on the system image and nowhere else
 *   - fs/sysvol.c, which fs/update.c writes kernels through, reaches the
 *     system volume while a stick is borrowed, with fs/fat16.c's return
 *     conventions
 *   - fat32_rename() moves entries, refuses what it must, and leaves a
 *     volume fsck.fat accepts
 *
 * Every verdict on what is on disk is fsck.fat's or mtools', not this
 * driver's opinion of its own work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kpollguard.inc"
#include "../fs/gpt.c"
#include "../fs/fat32.c"

/* fs/blkdev.c is not linked. Its four calls are answered from a whole-
   disk image when g_disk is open, which is how fat32_init()'s discovery
   is tested: a real GPT made by sgdisk, a real MBR written here, and
   volumes made by mkfs.fat at the partitions' offsets. */
static FILE *g_disk;
int blk_count(void){ return g_disk ? 1 : 0; }
int blk_scan(void){ return 0; }
int blk_boot_dev(void){ return g_disk ? 0 : -1; }
int blk_read(int d, uint64_t l, uint8_t *b){
    (void)d;
    if(!g_disk || fseek(g_disk, (long)(l * 512u), SEEK_SET)) return 0;
    return fread(b, 1, 512, g_disk) == 512;
}
int blk_write(int d, uint64_t l, const uint8_t *b){
    (void)d;
    if(!g_disk || fseek(g_disk, (long)(l * 512u), SEEK_SET)) return 0;
    return fwrite(b, 1, 512, g_disk) == 512;
}

/* fs/sysvol.c, against stubs for the FAT16 half and the VFS's answer */
static int g_v16 = 0, g_v32 = 1, g_f16_calls = 0;
int fat16_detected(void){ return g_v16; }
int fat16_exists_path(const char *p){ (void)p; g_f16_calls++; return 1; }
int fat16_read_path(const char *p, char *b, uint32_t m){ (void)p; (void)b; (void)m; g_f16_calls++; return 7; }
int fat16_write_path(const char *p, const char *d, uint32_t n){ (void)p; (void)d; g_f16_calls++; return (int)n; }
int fat16_delete_path(const char *p){ (void)p; g_f16_calls++; return 1; }
int vfs_using_fat16(void){ return g_v16; }
int vfs_using_fat32(void){ return g_v32; }
#include "../fs/sysvol.c"

static int pass, total;
static void ck(const char *what, int ok, long got){
    total++;
    if(ok){ pass++; return; }
    printf("    FAIL %-58s got %ld\n", what, got);
}
static void head(const char *s){ printf("\n  %s\n", s); }

/* ── two image files, one callback pair, the context says which ─────── */
static FILE *g_img[2];
static int img_rd(void *ctx, uint64_t lba, uint8_t *buf){
    FILE *f = g_img[(int)(long)ctx];
    if(fseek(f, (long)(lba * 512u), SEEK_SET)) return 0;
    return fread(buf, 1, 512, f) == 512;
}
static int img_wr(void *ctx, uint64_t lba, const uint8_t *buf){
    FILE *f = g_img[(int)(long)ctx];
    if(fseek(f, (long)(lba * 512u), SEEK_SET)) return 0;
    return fwrite(buf, 1, 512, f) == 512;
}
static int bad_rd(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx; (void)lba; memset(buf, 0, 512); return 1;   /* not a FAT */
}

#define SYS 0
#define USB 1
static const char *IMG[2] = { "/tmp/hd_f32s_sys.img", "/tmp/hd_f32s_usb.img" };

/* Different sizes on purpose: the two volumes' FATs and data areas then
   start in different places, so a mount field the park/swap code forgets
   to carry shows up as reading the wrong sectors. With identical images
   such a defect is invisible -- tools/check_fat32sys_suite.py found that. */
static int make_img(int which, const char *label, const char *cfg, int mb){
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "rm -f %s && dd if=/dev/zero of=%s bs=1M count=%d status=none && "
             "mkfs.fat -F 32 -n %s %s > /dev/null 2>&1 && "
             "printf '%s' > /tmp/hd_f32s_cfg.txt && "
             "mcopy -i %s /tmp/hd_f32s_cfg.txt ::/cfg.txt && "
             "mmd -i %s ::/boot",
             IMG[which], IMG[which], mb, label, IMG[which], cfg, IMG[which], IMG[which]);
    return system(cmd) == 0;
}
static int reopen(void){
    for(int i = 0; i < 2; i++){
        if(g_img[i]) fclose(g_img[i]);
        g_img[i] = fopen(IMG[i], "r+b");
        if(!g_img[i]) return 0;
    }
    return 1;
}
static int fsck_ok(int which){
    char cmd[256];
    fflush(g_img[which]);
    snprintf(cmd, sizeof cmd, "fsck.fat -n %s > /dev/null 2>&1", IMG[which]);
    return system(cmd) == 0;
}
/* mtools' copy of a file, compared with a string */
static int mtools_says(int which, const char *path, const char *want){
    char cmd[512];
    fflush(g_img[which]);
    snprintf(cmd, sizeof cmd,
             "mcopy -n -i %s ::%s /tmp/hd_f32s_out.txt > /dev/null 2>&1",
             IMG[which], path);
    if(system(cmd) != 0) return 0;
    FILE *f = fopen("/tmp/hd_f32s_out.txt", "rb");
    char b[256] = {0};
    size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
    if(f) fclose(f);
    return n == strlen(want) && memcmp(b, want, n) == 0;
}
static int mtools_has(int which, const char *path){
    char cmd[512];
    fflush(g_img[which]);
    snprintf(cmd, sizeof cmd, "mdir -i %s ::%s > /dev/null 2>&1", IMG[which], path);
    return system(cmd) == 0;
}
static int read_is(const char *path, const char *want){
    char b[256];
    int n = fat32_read(path, b, sizeof b - 1);
    if(n < 0) return 0;
    b[n] = 0;
    return strcmp(b, want) == 0;
}


/* ── fat32_init()'s discovery, against whole disks ────────────────────
   Each partition is 40 MB at a 1 MB-aligned start, formatted in place by
   mkfs.fat --offset, with a cfg.txt saying which one it is. */
#define DISK "/tmp/hd_f32s_disk.img"
static int fmt_at(uint32_t lba, const char *label, const char *cfg){
    char cmd[640];
    snprintf(cmd, sizeof cmd,
             "mkfs.fat -F 32 -n %s --offset=%u %s 81920 > /dev/null 2>&1 && "
             "printf '%s' > /tmp/hd_f32s_cfg.txt && "
             "mcopy -i %s@@%llu /tmp/hd_f32s_cfg.txt ::/cfg.txt",
             label, lba, DISK, cfg, DISK, (unsigned long long)lba * 512ull);
    return system(cmd) == 0;
}
static int blank_disk(int mb){
    char cmd[256];
    if(g_disk){ fclose(g_disk); g_disk = 0; }
    snprintf(cmd, sizeof cmd, "rm -f %s && dd if=/dev/zero of=%s bs=1M count=%d status=none",
             DISK, DISK, mb);
    return system(cmd) == 0;
}
static int open_disk(void){
    g_disk = fopen(DISK, "r+b");
    return g_disk != 0;
}
/* An MBR with up to four (type, lba, sectors) entries, written by hand. */
static int write_mbr(int n, const uint8_t *type, const uint32_t *lba, const uint32_t *cnt){
    uint8_t m[512];
    FILE *f = fopen(DISK, "r+b");
    if(!f) return 0;
    memset(m, 0, sizeof m);
    for(int i = 0; i < n; i++){
        uint8_t *e = m + 446 + 16 * i;
        e[4] = type[i];
        e[8]  = (uint8_t)lba[i];       e[9]  = (uint8_t)(lba[i] >> 8);
        e[10] = (uint8_t)(lba[i] >> 16); e[11] = (uint8_t)(lba[i] >> 24);
        e[12] = (uint8_t)cnt[i];       e[13] = (uint8_t)(cnt[i] >> 8);
        e[14] = (uint8_t)(cnt[i] >> 16); e[15] = (uint8_t)(cnt[i] >> 24);
    }
    m[510] = 0x55; m[511] = 0xAA;
    int ok = fwrite(m, 1, 512, f) == 512;
    fclose(f);
    return ok;
}
/* The HavenDOS type GUID is the ASCII bytes "HAVENDOS-PART-01"; as the
   mixed-endian text sgdisk reads, that is this. */
#define HAVEN_GUID "45564148-444E-534F-2D50-4152542D3031"
#define ESP_GUID   "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define MSDATA     "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

static void discovery(void){
    head("fat32_init on a GPT disk: our type GUID, never the ESP");
    ck("blank 160 MB disk", blank_disk(160), 0);
    ck("sgdisk: ESP, a Windows data partition, then HavenDOS",
       system("sgdisk -n 1:2048:+40M -t 1:" ESP_GUID
              " -n 2:83968:+40M -t 2:" MSDATA
              " -n 3:165888:+40M -t 3:" HAVEN_GUID
              " " DISK " > /dev/null 2>&1") == 0, 0);
    ck("format all three FAT32",
       fmt_at(2048, "EFI", "esp") && fmt_at(83968, "DATA", "windows-data")
       && fmt_at(165888, "HAVENDOS", "gpt-ours"), 0);
    ck("open the disk", open_disk(), 0);
    ck("fat32_init mounts something", fat32_init() == 1, 0);
    ck("and it is OURS -- not the ESP, not the data partition",
       read_is("/cfg.txt", "gpt-ours"), 0);
    ck("as the system volume", fat32_system_mounted() && !fat32_borrowed(), 0);

    head("a GPT disk with only an ESP and a data partition mounts nothing");
    ck("blank it", blank_disk(160), 0);
    ck("sgdisk: ESP and data only",
       system("sgdisk -n 1:2048:+40M -t 1:" ESP_GUID
              " -n 2:83968:+40M -t 2:" MSDATA
              " " DISK " > /dev/null 2>&1") == 0, 0);
    ck("format both", fmt_at(2048, "HAVENDOS", "esp-labelled-havendos")
                      && fmt_at(83968, "HAVENDOS", "data-labelled-havendos"), 0);
    ck("open", open_disk(), 0);
    ck("fat32_init finds nothing -- a label is not a type GUID",
       fat32_init() == 0, 0);
    ck("and has mounted nothing", fat32_detected() == 0, 0);

    head("an MBR disk: the volume labelled HAVENDOS wins");
    ck("blank 100 MB disk", blank_disk(100), 0);
    {
        uint8_t  t[2] = { 0x0C, 0x0C };
        uint32_t l[2] = { 2048, 83968 }, c[2] = { 81920, 81920 };
        ck("MBR: two FAT32 LBA partitions", write_mbr(2, t, l, c), 0);
    }
    ck("somebody's data first, ours second",
       fmt_at(2048, "DATA", "mbr-data") && fmt_at(83968, "HAVENDOS", "mbr-ours"), 0);
    ck("open", open_disk(), 0);
    ck("fat32_init mounts", fat32_init() == 1, 0);
    ck("ours, although it is second", read_is("/cfg.txt", "mbr-ours"), 0);

    head("an MBR disk with no HAVENDOS label: the old behaviour stands");
    ck("blank", blank_disk(100), 0);
    {
        uint8_t  t[1] = { 0x0B };
        uint32_t l[1] = { 2048 }, c[1] = { 81920 };
        ck("MBR: one FAT32 CHS partition", write_mbr(1, t, l, c), 0);
    }
    ck("format it, unlabelled as ours", fmt_at(2048, "OLDDISK", "mbr-legacy"), 0);
    ck("open", open_disk(), 0);
    ck("fat32_init still mounts it (no disk that mounted yesterday stops)",
       fat32_init() == 1 && read_is("/cfg.txt", "mbr-legacy"), 0);

    head("a FAT16 partition typed 0x0C is refused");
    ck("blank", blank_disk(100), 0);
    {
        uint8_t  t[1] = { 0x0C };
        uint32_t l[1] = { 2048 }, c[1] = { 81920 };
        ck("MBR says FAT32", write_mbr(1, t, l, c), 0);
    }
    ck("but it is FAT16",
       system("mkfs.fat -F 16 -n HAVENDOS --offset=2048 " DISK
              " 81920 > /dev/null 2>&1") == 0, 0);
    ck("open", open_disk(), 0);
    ck("fat32_init refuses it", fat32_init() == 0, 0);

    if(g_disk){ fclose(g_disk); g_disk = 0; }
}

int main(void){
    printf("The system volume: parked, borrowed, given a turn, renamed\n");
    printf("==========================================================\n");
    if(system("which mkfs.fat mcopy mdir fsck.fat sgdisk > /dev/null 2>&1") != 0){
        printf("  mkfs.fat/mtools/fsck.fat are not installed\n0/1 passed\n");
        return 1;
    }

    head("two real volumes: the system's own and a stick");
    ck("make the system image", make_img(SYS, "HAVENDOS", "system", 64), 0);
    ck("make the stick image",  make_img(USB, "STICK", "stick", 40), 0);
    ck("open both", reopen(), 0);
    {   /* and their geometry really does differ */
        uint8_t a[512], b[512];
        img_rd((void*)SYS, 0, a); img_rd((void*)USB, 0, b);
        ck("the two volumes' FATs are different sizes",
           memcmp(a + 36, b + 36, 4) != 0, 0);
    }

    head("the system volume is mounted as the system");
    ck("fat32_mount_system", fat32_mount_system(img_rd, img_wr, (void*)SYS, 0), 0);
    ck("it reads the system's file", read_is("/cfg.txt", "system"), 0);
    ck("it is the system", fat32_system_mounted() && !fat32_borrowed(), 0);

    head("a borrow parks it, and the slot reads the stick");
    ck("mount the stick", fat32_mount_dev(img_rd, img_wr, (void*)USB, 0), 0);
    ck("the slot reads the stick", read_is("/cfg.txt", "stick"), 0);
    ck("it says it is borrowed", fat32_borrowed() == 1, 0);
    ck("and the system is still there, parked", fat32_system_mounted() == 1, 0);
    ck("write a file on the stick", fat32_write("/s.txt", "on-stick", 8) == 1, 0);

    head("one turn for the system, mid-borrow");
    ck("sys_begin", fat32_sys_begin() == 1, 0);
    ck("the slot reads the SYSTEM now", read_is("/cfg.txt", "system"), 0);
    ck("write a file on the system",
       fat32_write("/boot/new.txt", "on-system", 9) == 1, 0);
    ck("the stick's file is not visible here", !fat32_exists("/s.txt"), 0);
    ck("a second begin inside the turn is harmless", fat32_sys_begin() == 1, 0);
    fat32_sys_end();
    ck("sys_end: the slot reads the stick again", read_is("/cfg.txt", "stick"), 0);
    ck("and the system's new file is not on the stick",
       !fat32_exists("/boot/new.txt"), 0);
    ck("the stick's own file survived the turn", read_is("/s.txt", "on-stick"), 0);
    fat32_sys_end();   /* a stray end is a no-op */
    ck("a stray sys_end changes nothing", read_is("/cfg.txt", "stick"), 0);

    head("ending the borrow brings the system back");
    fat32_unmount();
    ck("the slot reads the system", read_is("/cfg.txt", "system"), 0);
    ck("with the file written in its turn", read_is("/boot/new.txt", "on-system"), 0);
    ck("and it is not borrowed", !fat32_borrowed(), 0);
    fat32_unmount();
    ck("unmounting the SYSTEM is a no-op", read_is("/cfg.txt", "system"), 0);
    fat32_unmount();
    ck("twice", fat32_detected() == 1, 0);

    head("a borrow that fails leaves the system mounted");
    ck("mount a non-FAT", fat32_mount_dev(bad_rd, 0, 0, 0) == 0, 0);
    ck("the system is still in the slot", read_is("/cfg.txt", "system"), 0);
    ck("and the system is the system", !fat32_borrowed(), 0);
    fat32_unmount();     /* what every borrower does after a failure */
    ck("the borrower's tidy-up does not take it away",
       read_is("/cfg.txt", "system"), 0);

    head("a borrow opened during a system turn");
    ck("borrow the stick", fat32_mount_dev(img_rd, img_wr, (void*)USB, 0), 0);
    ck("begin a system turn", fat32_sys_begin() == 1, 0);
    ck("borrow the stick AGAIN from inside the turn",
       fat32_mount_dev(img_rd, img_wr, (void*)USB, 0) == 1, 0);
    ck("the slot reads the stick", read_is("/cfg.txt", "stick"), 0);
    fat32_unmount();
    ck("and ending it still gets the system back",
       read_is("/cfg.txt", "system"), 0);

    head("fs/sysvol.c: what fs/update.c writes a kernel through");
    {
        static char kern[70000], back[70000];
        for(unsigned i = 0; i < sizeof kern; i++) kern[i] = (char)(i * 7 + 3);
        kern[0] = 0x7F; kern[1] = 'E'; kern[2] = 'L'; kern[3] = 'F';
        ck("kind is FAT32", sysvol_kind() == SYSVOL_FAT32, sysvol_kind());
        ck("named FAT32", strcmp(sysvol_name(), "FAT32") == 0, 0);
        ck("borrow the stick, as the ESP update source does",
           fat32_mount_dev(img_rd, img_wr, (void*)USB, 0), 0);
        ck("sysvol_write returns the BYTE COUNT, like fat16_write_path",
           sysvol_write("/boot/havendos.elf", kern, sizeof kern) == (int)sizeof kern,
           0);
        ck("the stick is still in the slot afterwards", read_is("/cfg.txt", "stick"), 0);
        ck("and has no kernel on it", !fat32_exists("/boot/havendos.elf"), 0);
        ck("sysvol_exists sees the system's", sysvol_exists("/boot/havendos.elf") == 1, 0);
        ck("but not a directory as a file", sysvol_exists("/boot") == 0, 0);
        ck("sysvol_read returns the byte count",
           sysvol_read("/boot/havendos.elf", back, sizeof back) == (int)sizeof kern, 0);
        ck("of the same bytes", memcmp(kern, back, sizeof kern) == 0, 0);
        /* fs/update.c's verify reads exactly this: four bytes, the ELF
           magic, of a kernel megabytes long. Refusing it -- which the
           first version did -- fails every update on a FAT32 system. */
        memset(back, 0, 8);
        ck("a short read truncates, like fat16_read_path: update's verify",
           sysvol_read("/boot/havendos.elf", back, 4) == 4
           && back[0] == 0x7F && back[1] == 'E' && back[2] == 'L' && back[3] == 'F', 0);
        ck("a missing file is a failure",
           sysvol_read("/boot/nothere.elf", back, 4) < 0, 0);
        ck("write the backup", sysvol_write("/boot/havendos.bak", kern, 5000) == 5000, 0);
        ck("delete it", sysvol_delete("/boot/havendos.bak") == 1, 0);
        ck("and it is gone", sysvol_exists("/boot/havendos.bak") == 0, 0);
        fat32_unmount();
        ck("mtools reads the kernel off the SYSTEM image",
           mtools_has(SYS, "/boot/havendos.elf"), 0);
        ck("and not off the stick", !mtools_has(USB, "/boot/havendos.elf"), 0);
        ck("FAT16 untouched throughout", g_f16_calls == 0, g_f16_calls);

        g_v32 = 0; g_v16 = 1;
        ck("on a FAT16 system it is FAT16", sysvol_kind() == SYSVOL_FAT16, 0);
        ck("and goes to fs/fat16.c",
           sysvol_write("/boot/havendos.elf", kern, 10) == 10 && g_f16_calls == 1,
           g_f16_calls);
        g_v32 = 0; g_v16 = 0;
        ck("with nothing mounted it is none", sysvol_kind() == SYSVOL_NONE, 0);
        ck("and writes refuse", sysvol_write("/x", kern, 10) < 0, 0);
        ck("and exists says no", sysvol_exists("/x") == 0, 0);
        g_v32 = 1;
    }

    head("fat32_rename");
    ck("write /a.txt", fat32_write("/a.txt", "alpha", 5) == 1, 0);
    ck("rename it to /b.txt", fat32_rename("/a.txt", "/b.txt") == 1, 0);
    ck("the old name is gone", !fat32_exists("/a.txt"), 0);
    ck("the new name has the data", read_is("/b.txt", "alpha"), 0);
    ck("to a long name", fat32_rename("/b.txt", "/A Long Name For A File.txt") == 1, 0);
    ck("which reads back", read_is("/A Long Name For A File.txt", "alpha"), 0);
    ck("into a subdirectory",
       fat32_rename("/A Long Name For A File.txt", "/boot/moved.txt") == 1, 0);
    ck("which reads back", read_is("/boot/moved.txt", "alpha"), 0);
    ck("onto a name that exists is refused",
       fat32_rename("/boot/moved.txt", "/cfg.txt") == 0, 0);
    ck("and both files are intact",
       read_is("/boot/moved.txt", "alpha") && read_is("/cfg.txt", "system"), 0);
    ck("a missing source is refused", fat32_rename("/nope.txt", "/x.txt") == 0, 0);
    ck("into a missing directory is refused",
       fat32_rename("/cfg.txt", "/nodir/x.txt") == 0, 0);
    ck("the root is refused", fat32_rename("/", "/r") == 0, 0);
    ck("make /d1/inner", fat32_mkdir("/d1") && fat32_mkdir("/d1/inner"), 0);
    ck("a directory renames in place", fat32_rename("/d1", "/d2") == 1, 0);
    ck("its contents came with it", fat32_is_dir("/d2/inner"), 0);
    ck("a directory is refused across directories",
       fat32_rename("/d2/inner", "/boot/inner") == 0, 0);
    ck("\".\" is refused", fat32_rename("/d2/.", "/d3") == 0, 0);

    head("and the outside tools agree");
    fat32_flush();
    ck("fsck.fat: the system image is clean", fsck_ok(SYS), 0);
    ck("fsck.fat: the stick is clean", fsck_ok(USB), 0);
    ck("mtools: the stick's file", mtools_says(USB, "/s.txt", "on-stick"), 0);
    ck("mtools: the file from the system's turn",
       mtools_says(SYS, "/boot/new.txt", "on-system"), 0);
    ck("mtools: the renamed file", mtools_says(SYS, "/boot/moved.txt", "alpha"), 0);
    ck("mtools: the old names are gone",
       !mtools_has(SYS, "/a.txt") && !mtools_has(SYS, "/b.txt"), 0);
    ck("mtools: the renamed directory", mtools_has(SYS, "/d2/inner"), 0);

    fat32_unmount();
    discovery();

    printf("\n%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
