/* Can an installed system on an NVMe drive read itself back?
 *
 * It could not. fs/fat16.c knew VirtIO, ATA and AHCI; there was no NVMe
 * case, so fat16_raw_read() fell through to the VirtIO reader, which
 * answers nothing on a machine that has no VirtIO. fat16_init() returned
 * 0, kernel/main.c read that as "this machine has no HavenDOS partition"
 * and ran the installer -- on a laptop that had been installed to
 * successfully ten minutes earlier.
 *
 * Nothing caught it because no test and no harness ever BOOTED an
 * installed system from NVMe. The installer was exercised on NVMe and
 * passed, because the installer has its own block layer; the filesystem
 * has another one, and only that second one was missing the bus.
 *
 * So this suite is the other half of test_fat16gpt.c: the same fixture
 * and the same questions, with the model disk reachable ONLY through
 * nvme_read/nvme_write and every other bus reporting nothing -- which is
 * what a laptop made this decade actually looks like.
 */
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
static int g_ata = 0;
int virtio_blk_ready(void){ return 0; }
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int ata_detected(void){ return g_ata; }
int ata_lba48(void){ return 1; }
int ata_read_sector   (uint32_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
/* THIS suite is the NVMe one: the model disk is reached only through
   these, and every other bus reports nothing. That is a modern laptop --
   no VirtIO, no legacy IDE, no AHCI controller, one NVMe namespace. */
int nvme_count(void){ return 1; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i; if(n != 1) return -1; return d_read (0, l, (uint8_t *)b); }
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i; if(n != 1) return -1; return d_write(0, l, (const uint8_t *)b); }
int ata_read_sector48 (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }

/* fs/fat16.c reaches AHCI now -- a machine whose SATA controller is in
   AHCI mode (every laptop) could otherwise be installed to and then not
   read itself back. These suites use neither bus; the stubs resolve the
   symbols and report no controller, so fat16 falls through to the one
   they do simulate. */
static int g_ahci_n = 0, g_ahci_atapi = 1;
int ahci_count(void){ return g_ahci_n; }
int ahci_is_atapi(int i){ (void)i; return g_ahci_atapi; }
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
    printf("Reading an installed volume back over NVMe\n");
    printf("=========================================\n");

    head("the bus is NVMe and nothing else answers");
    {
        build_disk(DISK_SECTORS);
        ck("no VirtIO", virtio_blk_ready() == 0, 0);
        ck("no legacy IDE", ata_detected() == 0, 0);
        ck("no AHCI controller", ahci_count() == 0, 0);
        ck("one NVMe namespace", nvme_count() == 1, 0);
    }

    head("a HavenDOS volume in GPT free space, as the installer leaves it");
    {
        uint64_t where;
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("the partition was added", where != 0, (long)where);
        ck("fat16_init mounts it over NVMe", fat16_init() == 1, 0);
        ck("at the partition's own first LBA",
           g_part_lba == where, (long)g_part_lba);
        ck("and reports itself detected", fat16_detected(), 0);
        /* v0.8.6: through the shipping accessor and the shipping bus
           kind, not a file-local variable. fs/fat16.c used to carry its
           own FAT16_BUS_* constants and its own probe order; both are
           fs/blkdev.c's now, which is also what made fs/fat32.c's
           two-bus blindness visible. Asserting through fat16_dev() and
           blk_kind() means this check goes through the same path the
           `disk` command does. */
        ck("the bus it chose is NVMe",
           blk_kind(fat16_dev()) == BLK_BUS_NVME,
           (long)blk_kind(fat16_dev()));
        ck("...and it is named NVMe on screen",
           !strcmp(blk_bus_name(fat16_dev()), "NVMe"), 0);
        printf("      mounted from LBA %llu over NVMe\n",
               (unsigned long long)g_part_lba);
    }

    head("an MBR volume at 2048 over NVMe, the whole-disk install");
    {
        fat16_geom_t g;
        build_disk(DISK_SECTORS);
        memset(find(1)->b, 0, 512);
        memset(find(DISK_SECTORS - 1)->b, 0, 512);
        { uint8_t *m = make(0)->b;
          memset(m, 0, 512);
          m[446+4] = 0x0E;
          p32(m+446+8, 2048); p32(m+446+12, 262144);
          m[510]=0x55; m[511]=0xAA; }
        ck("geometry", fat16_geom(262144u, &g), 0);
        g_fmt_base = 2048;
        ck("format at 2048", fat16_format(fmt_wr, 0, &g, 2048u, "HAVENDOS"), 0);
        ck("fat16_init mounts it over NVMe", fat16_init() == 1, 0);
        ck("from sector 2048", g_part_lba == 2048, (long)g_part_lba);
    }

    head("an optical drive must not shadow the NVMe disk");
    {
        uint64_t where;
        build_disk(DISK_SECTORS);
        where = add_volume(T_HAVEN, 262144u, "HAVENDOS");
        ck("the partition is there", where != 0, (long)where);
        /* A DVD-ROM on the AHCI controller. ahci_count() counts it, which
           is what used to bind the filesystem to an optical drive and send
           an installed system back into its own installer. */
        g_ahci_n = 1; g_ahci_atapi = 1;
        /* The hardware changed, so the table is rebuilt -- which is what
           a boot does once. Without this the DVD-ROM would not be in
           fs/blkdev.c's table at all and the check below would pass
           without testing anything, which is the shape of the 621
           silently-absent checks v0.8.5.6 found. */
        blk_scan();
        ck("AHCI reports one device", ahci_count() == 1, 0);
        ck("and the block layer did not register it",
           blk_count() == 1, (long)blk_count());
        ck("and it is ATAPI", ahci_is_atapi(0) == 1, 0);
        ck("fat16_init still mounts over NVMe", fat16_init() == 1, 0);
        ck("the bus it chose is NVMe, not the DVD",
           blk_kind(fat16_dev()) == BLK_BUS_NVME,
           (long)blk_kind(fat16_dev()));
        ck("at the right LBA", g_part_lba == where, (long)g_part_lba);
        g_ahci_n = 0;
        blk_scan();
    }

    head("a disk with no HavenDOS volume is still refused");
    {
        build_disk(DISK_SECTORS);
        ck("nothing to mount", fat16_init() == 0, 0);
        ck("and it does not claim to be detected", !fat16_detected(), 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
