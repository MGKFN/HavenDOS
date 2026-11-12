/* ================================================================
   tests/test_fat16gpt.c -- can the installed system find itself?
   ================================================================
   fs/fat16.c mounted the volume at sector 2048, always, because that is
   where the whole-disk installer puts a partition and there was no other
   kind of install.

   The additive installer puts one wherever the free space on a GPT disk
   is. On the 8 GB test disk in harness/uefi_dualboot.py that was sector
   11053056, and what happened was this: the install completed, every
   sector outside the new partition was byte-identical, the firmware ran
   the loader, GRUB ran, the kernel started -- and fat16_init() looked at
   sector 2048, found somebody else's partition, returned 0, and
   kernel/main.c read "no FAT16 partition" as "this machine needs
   installing" and launched the installer.

   Every native suite was green. The machine booted. It booted the
   installer, on a machine that had just been installed.

   So this suite is the native version of that question: put a HavenDOS
   partition somewhere other than 2048, and see whether fat16_init() finds
   it -- and, as much as it matters, see that it refuses to find things
   that are not ours.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MAXSEC   65536
#define MAXWLOG  4096

#include "types.h"
#include "gpt.h"
#include "gpt_write.h"
#include "fat16fmt.h"

static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/gpt.c"
#include "../fs/gpt_write.c"
#include "gptdisk.inc"
#include "../fs/fat16fmt.c"

/* ── the block layer fs/fat16.c reaches for ─────────────────────────── */
static int g_ata = 1;
int virtio_blk_ready(void){ return 0; }
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int ata_detected(void){ return g_ata; }
int ata_lba48(void){ return 1; }
int ata_read_sector   (uint32_t l, uint8_t *b){ return d_read(0,l,b)==0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ return d_write(0,l,b)==0; }
/* NVMe is a bus fs/fat16.c can read from since the installed-system
   boot failed on a real NVMe laptop. These suites drive the other buses,
   so the calls only have to link -- nvme_count() returning 0 keeps the
   detection order landing where each suite already expects. */
int nvme_count(void){ return 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int ata_read_sector48 (uint64_t l, uint8_t *b){ return d_read(0,l,b)==0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ return d_write(0,l,b)==0; }

/* fs/fat16.c reaches AHCI now -- a machine whose SATA controller is in
   AHCI mode (every laptop) could otherwise be installed to and then not
   read itself back. These suites use neither bus; the stubs resolve the
   symbols and report no controller, so fat16 falls through to the one
   they do simulate. */
int ahci_count(void){ return 0; }
int ahci_is_atapi(int i){ (void)i; return 0; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){ (void)i;(void)l;(void)b; return 0; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){ (void)i;(void)l;(void)b; return 0; }


/* fs/fat16.c prints progress through the console. Swallowed here rather
   than echoed: the suite's own output is the readable thing, and a driver
   that is chatty on a real boot should not be chatty in a test. */
static void vga_printf(const char *fmt, ...){ (void)fmt; }

/* v0.8.6: the bus selection fs/fat16.c and fs/fat32.c used to do
   themselves is fs/blkdev.c's now -- one block layer under what were
   three switches holding four, four and TWO buses. tests/blkbus.inc
   supplies the remaining back-end entry points blk_scan() calls, in
   terms of the stubs above, and pulls in the real fs/blkdev.c. */
#include "blkbus.inc"


/* v0.8.6.1: fs/fat16.c reports through klog() when the system's own
   volume does not come back after a borrow -- an unmounted filesystem
   with nothing on screen to explain it is worse than a noisy one.
   Swallowed here; the suite's own output is the readable thing. */
static void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

#include "../fs/fat16.c"

/* ── writing a real HavenDOS volume into the model ──────────────────── */
static uint64_t g_fmt_base;
static int fmt_wr(void *ctx, uint32_t part_lba, const uint8_t *buf){
    (void)ctx; return d_write(0, g_fmt_base + part_lba, buf) == 0;
}

static const uint8_t T_HAVEN[16] = {
    'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'
};
/* Microsoft basic data, for the partition that must NOT be mounted. */
static const uint8_t T_MSDATA[16] = {
    0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,
    0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7
};

static int dwr(void *ctx, uint64_t lba, const void *buf){
    (void)ctx; return d_write(0, lba, buf);
}
static int drd(void *ctx, uint64_t lba, void *buf){
    (void)ctx; return d_read(0, lba, buf);
}

/* Add a partition of the given type to the fixture's free space and put a
   FAT16 volume in it. Returns its first LBA, or 0. */
static uint64_t add_volume(const uint8_t type[16], uint64_t sectors,
                           const char *label)
{
    gpt_gap_t gap;
    uint64_t first, last;
    fat16_geom_t g;
    int entry = -1, nw = 0;
    uint64_t written[8];

    if(gpt_find_free(drd, 0, sectors, &gap) != GPTW_OK) return 0;
    first = (gap.first_lba + 2047) / 2048 * 2048;
    last  = first + sectors - 1;
    if(last > gap.last_lba) return 0;
    if(gpt_add_partition(drd, dwr, 0, first, last, type, "HavenDOS",
                         &entry, written, &nw) != GPTW_OK) return 0;
    if(!fat16_geom((uint32_t)sectors, &g)) return 0;
    g_fmt_base = first;
    if(!fat16_format(fmt_wr, 0, &g, (uint32_t)first, label)) return 0;
    return first;
}

#define DISK_SECTORS (8ull * 1024 * 1024 * 1024 / 512)

int main(void)
{
    printf("Finding the HavenDOS volume on a GPT disk\n");
    printf("=========================================\n");

    head("the old behaviour still works: a volume at sector 2048");
    {
        /* No GPT at all -- an MBR disk with our volume at 2048, which is
           every disk the whole-disk installer has ever produced. If this
           regresses, every existing installation stops booting. */
        fat16_geom_t g;
        build_disk(DISK_SECTORS);
        memset(find(1)->b, 0, 512);                  /* remove the GPT    */
        memset(find(DISK_SECTORS - 1)->b, 0, 512);
        { uint8_t *m = make(0)->b;                   /* a real MBR entry  */
          memset(m, 0, 512);
          m[446+4] = 0x0E;                           /* FAT16 LBA         */
          p32(m+446+8, 2048); p32(m+446+12, 262144);
          m[510]=0x55; m[511]=0xAA; }
        ck("geometry for a 128 MB volume", fat16_geom(262144u, &g), 0);
        g_fmt_base = 2048;
        ck("format it at 2048",
           fat16_format(fmt_wr, 0, &g, 2048u, "HAVENDOS"), 0);
        ck("fat16_init mounts it", fat16_init() == 1, 0);
        ck("from sector 2048, as it always did",
           g_part_lba == 2048, (long)g_part_lba);
        ck("and reports itself detected", fat16_detected(), 0);
    }

    head("the bug: a volume somewhere else on a GPT disk");
    {
        uint64_t where;
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("a HavenDOS partition was added to the free space", where != 0,
           (long)where);
        ck("and it is nowhere near 2048", where > 100000, (long)where);
        ck("fat16_init finds it", fat16_init() == 1, 0);
        ck("at the partition's own first LBA",
           g_part_lba == where, (long)g_part_lba);
        printf("      mounted from LBA %llu\n", (unsigned long long)g_part_lba);
    }

    head("and it reads what the installer wrote there");
    {
        /* Not just "it mounted": the volume label is what GRUB searches
           for, and a mount that found the wrong partition would still
           report success. */
        uint8_t sec[512];
        ck("the boot sector at the mounted base is a FAT16 BPB",
           d_read(0, g_part_lba, sec) == 0
           && sec[510] == 0x55 && sec[511] == 0xAA
           && sec[54]=='F' && sec[55]=='A' && sec[56]=='T', 0);
        ck("with the OEM name fs/fat16fmt.c writes",
           sec[3]=='H'&&sec[4]=='A'&&sec[5]=='V'&&sec[6]=='E'&&sec[7]=='N', 0);
    }

    head("it does NOT mount a partition that is not ours");
    {
        /* The dangerous failure in the other direction: a disk with a
           FAT16 data partition on it that is somebody's, mounted because
           it is FAT16 and we were looking for FAT16. Writing a
           /havendos.txt into it would be the install damaging data it was
           supposed to leave alone. */
        uint64_t where;
        build_disk(DISK_SECTORS);
        where = add_volume(T_MSDATA, 262144u, "MYSTUFF");
        ck("a Microsoft-typed FAT16 partition was added", where != 0,
           (long)where);
        ck("fat16_init refuses to mount it", fat16_init() == 0, 0);
        ck("and did not move the base off its default",
           g_part_lba == 2048, (long)g_part_lba);
    }

    head("nor an EFI System Partition, whatever is planted in it");
    {
        /* Belt and braces: an ESP is FAT32 and would fail the BPB checks
           anyway, but it is the one partition on the disk where a mistake
           cannot be undone, so fs/fat16.c skips it by type rather than by
           luck. A partition with the ESP type GUID is added and a
           HavenDOS-looking FAT16 is planted inside it, so the only thing
           that can keep it unmounted is the skip.

           The first version of this planted the volume in the fixture's
           own ESP -- which starts at sector 2048, where the legacy
           direct-read path looks before the GPT is consulted at all. So
           it mounted, and the test was reporting on a path it was not
           testing. The ESP under test is a second one, in free space. */
        uint64_t where;
        build_disk(DISK_SECTORS);
        where = add_volume(T_ESP, 262144u, "HAVENDOS");
        ck("a second ESP-typed partition was added", where != 0, (long)where);
        ck("it is not at 2048, so the legacy path cannot claim it",
           where > 100000, (long)where);
        ck("fat16_init does not mount it", fat16_init() == 0, 0);
        ck("and left the base at its default", g_part_lba == 2048,
           (long)g_part_lba);
    }

    head("a partition of the right type with nothing in it is refused");
    {
        /* The type GUID says it is ours. There is no filesystem inside it
           -- an install that was interrupted between adding the partition
           and formatting it. Mounting that would read zeroes as a BPB and
           compute a layout out of them. */
        gpt_gap_t gap;
        uint64_t first;
        int entry = -1, nw = 0;
        uint64_t written[8];
        build_disk(DISK_SECTORS);
        ck("free space found", gpt_find_free(drd, 0, 262144u, &gap) == GPTW_OK, 0);
        first = (gap.first_lba + 2047) / 2048 * 2048;
        ck("a HavenDOS partition with no filesystem in it",
           gpt_add_partition(drd, dwr, 0, first, first + 262143u, T_HAVEN,
                             "HavenDOS", &entry, written, &nw) == GPTW_OK, 0);
        ck("fat16_init refuses it", fat16_init() == 0, 0);
        ck("and left the base alone", g_part_lba == 2048, (long)g_part_lba);
    }

    head("our type, our OEM name, and no valid BPB -- still refused");
    {
        /* Everything the search looks for except a filesystem: an
           interrupted format, or a partition whose first sector was
           damaged. fat16_init() re-checks the BPB after the search picks a
           base, and this is what asserts that it does. */
        uint64_t where;
        uint8_t sec[512];
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("a HavenDOS volume exists", where != 0, (long)where);
        ck("read its boot sector", d_read(0, where, sec) == 0, 0);
        sec[510] = 0; sec[511] = 0;               /* no boot signature */
        ck("remove its boot signature", d_write(0, where, sec) == 0, 0);
        ck("fat16_init refuses it", fat16_init() == 0, 0);

        ck("re-read it", d_read(0, where, sec) == 0, 0);
        sec[510] = 0x55; sec[511] = 0xAA;
        sec[11] = 0x00; sec[12] = 0x04;           /* 1024 bytes per sector */
        ck("restore the signature but break bytes-per-sector",
           d_write(0, where, sec) == 0, 0);
        ck("fat16_init refuses that too", fat16_init() == 0, 0);
    }

    head("a FAT16 that is not one of ours is refused");
    {
        /* Right type GUID, valid FAT16 BPB, somebody else's OEM name --
           the type GUID reused, or a partition we created and someone
           reformatted. The OEM name fs/fat16fmt.c writes is the second
           signature, and it is the one that distinguishes "a FAT16" from
           "our FAT16". */
        uint64_t where;
        uint8_t sec[512];
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("a HavenDOS volume exists", where != 0, (long)where);
        ck("read its boot sector", d_read(0, where, sec) == 0, 0);
        memcpy(sec + 3, "MSDOS5.0", 8);           /* reformatted elsewhere */
        ck("rewrite it with another OEM name", d_write(0, where, sec) == 0, 0);
        ck("fat16_init refuses it", fat16_init() == 0, 0);
    }

    head("and writes go where the reads came from");
    {
        /* The subtlest failure available here: the search finds the
           partition, the mount reads from it, and the WRITE path still
           uses the default base -- so a file written by the installed
           system lands at sector 2048, inside whatever is there. It
           mounts, it reads, it looks correct, and it corrupts another
           partition on the first write. */
        uint64_t where;
        unsigned char before2048[512], after2048[512];
        const char *msg = "HavenDOS wrote this\n";
        char back[64];
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("a HavenDOS volume away from 2048", where > 100000, (long)where);
        ck("it mounts", fat16_init() == 1, 0);

        ck("snapshot sector 2048 first", d_read(0, 2048, before2048) == 0, 0);
        g_logging = 1; g_nwlog = 0;
        /* fat16_write returns the byte count, not a boolean. */
        ck("write a file through the driver",
           fat16_write("TEST.TXT", msg, 20) == 20,
           fat16_write("TEST.TXT", msg, 20));
        ck("and read it back", fat16_read("TEST.TXT", back, sizeof back) == 20,
           fat16_read("TEST.TXT", back, sizeof back));
        g_logging = 0;
        ck("with the right contents", memcmp(back, msg, 20) == 0, 0);

        ck("sector 2048 is untouched",
           d_read(0, 2048, after2048) == 0
           && memcmp(before2048, after2048, 512) == 0, 0);

        /* and every write landed inside the partition */
        {
            int outside = 0, i;
            unsigned lim = (unsigned)g_nwlog;
            if(lim > MAXWLOG) lim = MAXWLOG;
            for(i = 0; i < (int)lim; i++)
                if(g_wlog[i] < where || g_wlog[i] > where + 262143u) outside++;
            ck("every sector written is inside the partition",
               outside == 0, outside);
        }
    }

    head("v0.9.3: dotfiles get a legal 8.3 name, and old ones are found");
    {
        /* ".shell_history" was a BLANK name with extension SHE: fsck.fat
           calls that a bad short name and renames it FSCK0000.000, and
           Windows' chkdsk does the same -- to .accounts too. Found by
           harness/esp_update.py running fsck.fat on an installed volume. */
        uint64_t where;
        char back[64];
        uint32_t rs;
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("mount a HavenDOS volume", where != 0 && fat16_init() == 1, 0);
        ck("write .shell_history",
           fat16_write(".shell_history", "ls\nver\n", 7) == 7, 0);
        rs = (uint32_t)(where + root_start);
        {
            uint8_t d[512];
            int found = 0, blank = 0;
            d_read(0, rs, d);
            for(int i = 0; i < 512; i += 32){
                if(memcmp(d + i, "_SHELL_H   ", 11) == 0) found = 1;
                if(d[i] == ' ' && d[i + 8] != ' ') blank = 1;
            }
            ck("its entry is _SHELL_H", found, 0);
            ck("and no entry has a blank name", !blank, 0);
        }
        ck("it reads back by its dotted name",
           fat16_read(".shell_history", back, sizeof back) == 7
           && memcmp(back, "ls\nver\n", 7) == 0, 0);
        ck(".bpkg_db and .bpkg are two files, not one",
           fat16_write(".bpkg_db", "db", 2) == 2
           && fat16_write(".bpkg", "pk", 2) == 2
           && fat16_read(".bpkg_db", back, sizeof back) == 2
           && memcmp(back, "db", 2) == 0, 0);

        /* An entry as a pre-0.9.3 system left it: blank name, ACC. */
        {
            uint8_t d[512];
            int slot = -1;
            ck("write .accounts", fat16_write(".accounts", "admin:x", 7) == 7, 0);
            d_read(0, rs, d);
            for(int i = 0; i < 512; i += 32)
                if(memcmp(d + i, "_ACCOUNT", 8) == 0) slot = i;
            ck("find its entry", slot >= 0, slot);
            if(slot >= 0){
                memcpy(d + slot, "        ACC", 11);
                d_write(0, rs, d);
            }
            ck("remount, as the next boot would", fat16_init() == 1, 0);
            ck("the old-style .accounts is still found",
               fat16_read(".accounts", back, sizeof back) == 7
               && memcmp(back, "admin:x", 7) == 0, 0);
            d_read(0, rs, d);
            ck("and its entry was renamed in place to _ACCOUNT",
               slot >= 0 && memcmp(d + slot, "_ACCOUNT   ", 11) == 0, 0);
        }
        ck("\".\" and \"..\" are not dotfiles",
           is_dotfile(".x") && !is_dotfile(".") && !is_dotfile("..")
           && !is_dotfile("a.b"), 0);
    }

    head("a disk with no GPT and no MBR entry mounts nothing");
    {
        build_disk(DISK_SECTORS);
        memset(find(1)->b, 0, 512);
        memset(find(DISK_SECTORS - 1)->b, 0, 512);
        memset(make(0)->b, 0, 512);
        ck("fat16_init returns 0", fat16_init() == 0, 0);
        ck("rather than mounting whatever is at 2048",
           !fat16_detected(), 0);
    }

    head("no disk at all");
    {
        build_disk(DISK_SECTORS);
        g_ata = 0;
        ck("fat16_init returns 0 with no drive", fat16_init() == 0, 0);
        g_ata = 1;
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
