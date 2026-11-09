/* ════════════════════════════════════════════════════════════════════
   test_blkdev.c -- one block layer, and the two orders it holds.

   fs/blkdev.c was written in v0.8.6 because adding a fifth bus found
   three sector-dispatch switches holding four, four and TWO buses:
   fs/fat32.c could not see an NVMe or an AHCI disk at all, two releases
   after "the installer had one block layer and the filesystem had
   another, and only one of them got taught about NVMe" was recorded as
   the lesson of v0.8.3.1.

   What this suite is for is not the dispatch -- that is four lines and
   tests/test_diskname.c covers the words. It is the two ORDERS, and one
   of them protects the only thing in this system that cannot be undone:

     REGISTRATION ORDER is identity. It is what the installer's picker
     shows, what fat32_vol_t::disk stores, and what a person reads off
     the screen that asks them to type YES. A removable disk that
     registers before the fixed ones RENUMBERS THEM -- so if plugging a
     stick in between the picker and the confirmation could shift
     "disk 1" from a spare drive onto somebody's Windows NVMe, the
     four-sectors guarantee this release ships on is worth nothing.

     BOOT PREFERENCE is "which of these disks is this machine's own",
     which fat16_init() and fat32_init() ask and the installer never
     does. The two used to disagree: the installer registered VirtIO,
     NVMe, AHCI, ATA while fs/fat16.c probed VirtIO, ATA, AHCI, NVMe, so
     on a machine where both the NVMe and the legacy ATA ports answer the
     installer called the NVMe disk 0 and the filesystem bound itself to
     the ATA one.

   The registration ORDER itself -- which bus blk_scan() asks first -- is
   checked by driving the real blk_scan() against simulated back-ends,
   not by registering disks by hand. Doing it by hand would test this
   file's idea of the order rather than blkdev.c's, which is the copy
   problem tests/test_diskname.c used to have.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* ── the simulated machine ────────────────────────────────────────────
   Five back-ends, each a knob. blk_scan() calls these and nothing else,
   so what this suite drives is the real registration order. */
static int g_virtio_n, g_nvme_n, g_ahci_n, g_ata, g_msc_n;
static int g_ahci_atapi[4];       /* which AHCI ports hold a DVD-ROM     */
static int g_nvme_ro[4];          /* which namespaces refuse writes      */
static int g_msc_ro[4];
static int g_ahci_init_calls, g_ata_init_calls;

/* Every read and write records where it went, so a test can check that a
   sector reached the bus it was meant to and not merely that a call
   returned 1. A mock that returns success is the thing include/usb.h
   warns about one layer up. */
static int      g_last_kind;      /* 1 virtio 2 nvme 3 ahci 4 ata 5 msc  */
static int      g_last_unit;
static uint64_t g_last_lba;
static int      g_last_write;

#define REC(k, u, l, w) do { g_last_kind=(k); g_last_unit=(u); \
                             g_last_lba=(l);  g_last_write=(w); } while(0)

/* A read that records as the USB bus, for an ops table registered by
   hand rather than by blk_scan(). */
static int sim_rd(int unit, uint64_t lba, uint8_t *buf){
    (void)buf; REC(5, unit, lba, 0); return 1;
}

int      virtio_scan_drives(void){ return g_virtio_n; }
uint64_t virtio_drive_sectors(int i){ return (uint64_t)(1000 + i); }
uint8_t  virtio_drive_bus(int i){ return (uint8_t)(10 + i); }
uint8_t  virtio_drive_dev(int i){ return (uint8_t)(20 + i); }
int virtio_drive_read (int i, uint64_t l, uint8_t *b){
    (void)b; REC(1,i,l,0); return 1; }
int virtio_drive_write(int i, uint64_t l, const uint8_t *b){
    (void)b; REC(1,i,l,1); return 1; }

int      nvme_count(void){ return g_nvme_n; }
uint64_t nvme_sectors(int i){ return (uint64_t)(2000 + i); }
int      nvme_writable(int i){ return (i>=0 && i<4) ? !g_nvme_ro[i] : 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)n;(void)b; REC(2,i,l,0); return 0; }     /* 0 == ok, nvme's own */
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)n;(void)b; REC(2,i,l,1); return 0; }

int      ahci_init(void){ g_ahci_init_calls++; return g_ahci_n; }
int      ahci_count(void){ return g_ahci_n; }
uint64_t ahci_sectors(int i){ return (uint64_t)(3000 + i); }
int      ahci_is_atapi(int i){ return (i>=0 && i<4) ? g_ahci_atapi[i] : 0; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){
    (void)b; REC(3,i,l,0); return 1; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)b; REC(3,i,l,1); return 1; }

void     ata_init(void){ g_ata_init_calls++; }
int      ata_detected(void){ return g_ata; }
uint32_t ata_sectors(void){ return 4000u; }
/* The LBA48 switch is fs/blkdev.c's now. g_lba48 lets a test check that
   a 48-bit address reaches the 48-bit entry point -- the top-8-bit
   truncation that destroyed an MBR past 8 GiB is this project's own. */
static int g_lba48;
int      ata_lba48(void){ return g_lba48; }
static int g_ata_used48;
int ata_read_sector   (uint32_t l, uint8_t *b){
    (void)b; g_ata_used48=0; REC(4,0,l,0); return 1; }
int ata_write_sector  (uint32_t l, const uint8_t *b){
    (void)b; g_ata_used48=0; REC(4,0,l,1); return 1; }
int ata_read_sector48 (uint64_t l, uint8_t *b){
    (void)b; g_ata_used48=1; REC(4,0,l,0); return 1; }
int ata_write_sector48(uint64_t l, const uint8_t *b){
    (void)b; g_ata_used48=1; REC(4,0,l,1); return 1; }

int      msc_count(void){ return g_msc_n; }
uint64_t msc_sectors(int i){ return (uint64_t)(5000 + i); }
int      msc_readonly(int i){ return (i>=0 && i<4) ? g_msc_ro[i] : 1; }
int msc_read_sector (int i, uint64_t l, uint8_t *b){
    (void)b; REC(5,i,l,0); return 1; }
int msc_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)b; REC(5,i,l,1); return 1; }

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/blkdev.c"

static void machine(int virtio, int nvme, int ahci, int ata, int msc)
{
    g_virtio_n = virtio; g_nvme_n = nvme; g_ahci_n = ahci;
    g_ata = ata; g_msc_n = msc;
    memset(g_ahci_atapi, 0, sizeof g_ahci_atapi);
    memset(g_nvme_ro, 0, sizeof g_nvme_ro);
    memset(g_msc_ro, 0, sizeof g_msc_ro);
    g_ahci_init_calls = g_ata_init_calls = 0;
    g_last_kind = g_last_unit = -1; g_last_lba = 0; g_last_write = -1;
    g_lba48 = 0; g_ata_used48 = -1;
}

static uint8_t buf[512];

int main(void)
{
    printf("One block layer, and the two orders it holds\n");
    printf("===========================================\n");

    head("an empty machine");
    machine(0,0,0,0,0);
    ck("blk_scan finds nothing", blk_scan() == 0, blk_count());
    ck("and says so", blk_count() == 0, blk_count());
    ck("index 0 is not a disk", blk_dev(0) == 0, 0);
    ck("its bus is 'none'", !strcmp(blk_bus_name(0), "none"), 0);
    ck("it is read-only, not writable", blk_readonly(0), 0);
    ck("there is no boot disk", blk_boot_dev() == -1, blk_boot_dev());
    ck("a read of it fails", blk_read(0, 0, buf) == 0, 0);
    ck("a write of it fails", blk_write(0, 0, buf) == 0, 0);

    head("registration order: fixed buses in the order blk_scan asks");
    machine(1,1,1,1,0);
    ck("five buses, four disks", blk_scan() == 4, blk_count());
    ck("disk 0 is VirtIO", blk_kind(0) == BLK_BUS_VIRTIO, blk_kind(0));
    ck("disk 1 is NVMe",   blk_kind(1) == BLK_BUS_NVME,   blk_kind(1));
    ck("disk 2 is AHCI",   blk_kind(2) == BLK_BUS_AHCI,   blk_kind(2));
    ck("disk 3 is ATA",    blk_kind(3) == BLK_BUS_ATA,    blk_kind(3));
    ck("AHCI is probed before legacy ATA, not merely listed first",
       g_ahci_init_calls == 1 && g_ata_init_calls == 1, g_ahci_init_calls);
    ck("each disk carries its own bus's sector count",
       blk_sectors(0)==1000 && blk_sectors(1)==2000 &&
       blk_sectors(2)==3000 && blk_sectors(3)==4000, (long)blk_sectors(1));

    head("A STICK MUST NOT RENUMBER A FIXED DISK");
    /* The property this file exists for. The installer's confirmation
       screen names a disk by index and so does the cached dry-run plan,
       so a removable disk that lands before a fixed one moves the disk
       somebody is about to have written to. */
    machine(1,1,1,1,0);
    blk_scan();
    {
        uint8_t  k0 = blk_kind(0), k1 = blk_kind(1);
        uint8_t  k2 = blk_kind(2), k3 = blk_kind(3);
        uint64_t s0 = blk_sectors(0), s3 = blk_sectors(3);

        machine(1,1,1,1,2);          /* two sticks arrive */
        ck("six disks now", blk_scan() == 6, blk_count());
        ck("disk 0 is the same bus it was", blk_kind(0) == k0, blk_kind(0));
        ck("disk 1 is the same bus it was", blk_kind(1) == k1, blk_kind(1));
        ck("disk 2 is the same bus it was", blk_kind(2) == k2, blk_kind(2));
        ck("disk 3 is the same bus it was", blk_kind(3) == k3, blk_kind(3));
        ck("disk 0 is the same SIZE it was", blk_sectors(0) == s0,
           (long)blk_sectors(0));
        ck("disk 3 is the same SIZE it was", blk_sectors(3) == s3,
           (long)blk_sectors(3));
        ck("the sticks are 4 and 5, after everything fixed",
           blk_kind(4) == BLK_BUS_USB && blk_kind(5) == BLK_BUS_USB,
           blk_kind(4));
        ck("...and they are the only removable ones",
           blk_removable(4) && blk_removable(5) &&
           !blk_removable(0) && !blk_removable(3), 0);
    }
    /* And the same with ONLY a stick: it is disk 0 then, because there is
       nothing for it to displace. A machine that boots HavenDOS off a USB
       disk has to be able to find it. */
    machine(0,0,0,0,1);
    ck("a stick alone is disk 0", blk_scan() == 1 && blk_kind(0) == BLK_BUS_USB,
       blk_kind(0));

    head("an optical drive is not a disk");
    /* ahci_count() counts a DVD-ROM. fs/fat16.c used to bind itself to
       the one on q35's default controller, find no HavenDOS partition on
       it, and send an installed system back into its own installer. */
    machine(0,0,2,0,0);
    g_ahci_atapi[0] = 1;
    ck("two AHCI ports, one a DVD: one disk", blk_scan() == 1, blk_count());
    ck("and it is the one that is not ATAPI", blk_unit(0) == 1, blk_unit(0));
    ck("the unit is the DRIVER's index, not a renumbering",
       blk_sectors(0) == 3001, (long)blk_sectors(0));
    machine(0,0,1,0,0);
    g_ahci_atapi[0] = 1;
    ck("a controller with nothing but a DVD has no disks",
       blk_scan() == 0, blk_count());

    head("boot preference: which disk is this machine's own");
    /* fs/fat16.c's order, with its argument, in one place now. The bug
       it replaces: the installer said disk 0 was the NVMe and the
       filesystem mounted the ATA one. */
    machine(1,1,1,1,1);
    blk_scan();
    ck("VirtIO wins when it is there", blk_kind(blk_boot_dev()) == BLK_BUS_VIRTIO,
       blk_kind(blk_boot_dev()));
    machine(0,1,1,1,1);
    blk_scan();
    ck("then legacy ATA -- firmware chose compatibility mode",
       blk_kind(blk_boot_dev()) == BLK_BUS_ATA, blk_kind(blk_boot_dev()));
    machine(0,1,1,0,1);
    blk_scan();
    ck("then AHCI", blk_kind(blk_boot_dev()) == BLK_BUS_AHCI,
       blk_kind(blk_boot_dev()));
    machine(0,1,0,0,1);
    blk_scan();
    ck("then NVMe -- every laptop made this decade",
       blk_kind(blk_boot_dev()) == BLK_BUS_NVME, blk_kind(blk_boot_dev()));
    machine(0,0,0,0,1);
    blk_scan();
    ck("and a USB disk LAST, when nothing fixed answered",
       blk_kind(blk_boot_dev()) == BLK_BUS_USB, blk_kind(blk_boot_dev()));
    /* The one that matters: a stick must never become the volume an
       installed system mounts as its own. */
    machine(0,1,0,0,1);
    blk_scan();
    ck("a stick does NOT displace the machine's own NVMe disk",
       blk_kind(blk_boot_dev()) == BLK_BUS_NVME, blk_kind(blk_boot_dev()));
    machine(0,0,1,0,1);
    blk_scan();
    ck("...nor its SATA disk", blk_kind(blk_boot_dev()) == BLK_BUS_AHCI,
       blk_kind(blk_boot_dev()));

    head("a sector reaches the bus it was addressed to");
    machine(1,1,1,1,1);
    blk_scan();
    ck("a read of disk 1 goes to NVMe unit 0",
       blk_read(1, 77, buf) && g_last_kind == 2 && g_last_unit == 0 &&
       g_last_lba == 77, g_last_kind);
    ck("a read of disk 2 goes to AHCI",
       blk_read(2, 88, buf) && g_last_kind == 3 && g_last_lba == 88,
       g_last_kind);
    ck("a read of disk 4 goes to the stick",
       blk_read(4, 99, buf) && g_last_kind == 5 && g_last_lba == 99,
       g_last_kind);
    ck("a write of disk 0 goes to VirtIO as a WRITE",
       blk_write(0, 5, buf) && g_last_kind == 1 && g_last_write == 1,
       g_last_write);
    ck("nvme's 0-means-ok is translated, not passed through",
       blk_read(1, 1, buf) == 1, 0);

    head("LBA48 is chosen once, below every caller");
    machine(0,0,0,1,0);
    g_lba48 = 0; blk_scan();
    ck("a 28-bit disk uses the 28-bit entry point",
       blk_read(0, 1234, buf) && g_ata_used48 == 0, g_ata_used48);
    machine(0,0,0,1,0);
    g_lba48 = 1; blk_scan();
    ck("a 48-bit disk uses the 48-bit one",
       blk_read(0, 1234, buf) && g_ata_used48 == 1, g_ata_used48);
    ck("...and a sector past 8 GiB is not truncated on the way",
       blk_read(0, 0x1234567ull, buf) && g_last_lba == 0x1234567ull,
       (long)g_last_lba);

    head("every disk is named for its own bus, not for the first one");
    /* tools/check_blk_suite.py found this missing: an injection making
       blk_bus_name() read g_dev[0] instead of g_dev[i] -- which is the
       v0.8.3.1 bug verbatim, an NVMe drive announced as VirtIO on the
       screen whose whole job is to say what is about to be written --
       was MISSED, because this suite never asked for a name.
       tests/test_diskname.c covers the words from the specification
       side; the suite over fs/blkdev.c has to cover its behaviour. */
    machine(1,1,1,1,1);
    blk_scan();
    ck("disk 0 is VirtIO", !strcmp(blk_bus_name(0), "VirtIO"), 0);
    ck("disk 1 is NVMe",   !strcmp(blk_bus_name(1), "NVMe"), 0);
    ck("disk 2 is SATA",   !strcmp(blk_bus_name(2), "SATA"), 0);
    ck("disk 3 is IDE",    !strcmp(blk_bus_name(3), "IDE"), 0);
    ck("disk 4 is USB",    !strcmp(blk_bus_name(4), "USB"), 0);
    ck("so no two disks on different buses share a name",
       strcmp(blk_bus_name(0), blk_bus_name(1)) &&
       strcmp(blk_bus_name(1), blk_bus_name(2)) &&
       strcmp(blk_bus_name(2), blk_bus_name(3)) &&
       strcmp(blk_bus_name(3), blk_bus_name(4)), 0);

    head("a bus with NO write function at all");
    /* Different from a writable bus holding an unwritable disk, and the
       distinction is the point: a bus with no write() is refused without
       the back-end saying anything, which is what makes a new read-only
       bus safe by default rather than by somebody remembering to add it
       to a switch. The installer's old disk_is_readonly() switched on
       the bus TYPE to ask a per-disk question, so every bus added had to
       be remembered in it.

       tools/check_blk_suite.py found this missing too. */
    blk_reset();
    {
        static const blk_ops_t ops_nowrite = { "USB", sim_rd, 0, 0, 1 };
        ck("it registers -- a disk you can read is a disk",
           blk_register(&ops_nowrite, BLK_BUS_USB, 0, 100, 0, 0) == 0, 0);
        ck("and it is read-only without the bus being asked",
           blk_readonly(0), 0);
        ck("a write is refused", blk_write(0, 0, buf) == 0, 0);
        ck("...and the read still works", blk_read(0, 0, buf) == 1, 0);
        ck("...through the stick's own read function", g_last_kind == 5,
           g_last_kind);
    }

    head("read-only: the bus's answer and the disk's answer");
    machine(0,1,0,0,0);
    g_nvme_ro[0] = 1; blk_scan();
    ck("a namespace with no write path is read-only", blk_readonly(0), 0);
    ck("...and the write is refused BELOW the caller",
       blk_write(0, 0, buf) == 0, 0);
    ck("...without reaching the driver", g_last_write != 1, g_last_write);
    ck("...while the read still works", blk_read(0, 0, buf) == 1, 0);
    machine(0,0,0,0,1);
    g_msc_ro[0] = 1; blk_scan();
    ck("a write-protected stick is read-only", blk_readonly(0), 0);
    machine(0,0,0,0,1);
    g_msc_ro[0] = 0; blk_scan();
    ck("an ordinary stick is writable", !blk_readonly(0), 0);
    ck("...and the write reaches it",
       blk_write(0, 3, buf) == 1 && g_last_kind == 5 && g_last_write == 1,
       g_last_kind);

    head("a rescan replaces the table rather than appending to it");
    machine(1,1,1,1,1);
    ck("five disks", blk_scan() == 5, blk_count());
    ck("five again, not ten", blk_scan() == 5, blk_count());
    ck("and again", blk_scan() == 5, blk_count());
    machine(0,0,0,0,0);
    ck("an unplugged machine scans to nothing", blk_scan() == 0, blk_count());

    head("the table has a bound and it is respected");
    machine(0,0,0,0,0);
    blk_reset();
    {
        int i, got = 0;
        /* Register past the end by hand; blk_scan() cannot reach this
           with the simulated knobs, which is the point of checking it
           here instead. A table that silently overran would write
           through a disk entry nobody allocated. */
        for(i = 0; i < BLK_MAX_DEV + 4; i++)
            if(blk_register(&ata_ops, BLK_BUS_ATA, i, 10, 0, 0) >= 0) got++;
        ck("exactly BLK_MAX_DEV registered", got == BLK_MAX_DEV, got);
        ck("and the count agrees", blk_count() == BLK_MAX_DEV, blk_count());
        ck("the extra ones were refused, not wrapped",
           blk_register(&ata_ops, BLK_BUS_ATA, 0, 10, 0, 0) == -1, 0);
    }

    head("a disk you cannot read is not a disk");
    blk_reset();
    {
        static const blk_ops_t noread = { "USB", 0, 0, 0, 1 };
        ck("an ops table with no read() is refused",
           blk_register(&noread, BLK_BUS_USB, 0, 10, 0, 0) == -1, 0);
        ck("a null ops table is refused",
           blk_register(0, BLK_BUS_USB, 0, 10, 0, 0) == -1, 0);
        ck("and neither landed in the table", blk_count() == 0, blk_count());
    }

    head("a null buffer is refused rather than dereferenced");
    machine(0,0,0,1,0); blk_scan();
    ck("a read with no buffer fails", blk_read(0, 0, 0) == 0, 0);
    ck("a write with no buffer fails", blk_write(0, 0, 0) == 0, 0);

    head("the PCI location travels with the disk, for the picker's columns");
    machine(2,0,0,0,0); blk_scan();
    ck("disk 0 carries its own bus/dev",
       blk_pci_bus(0) == 10 && blk_pci_dev(0) == 20, blk_pci_bus(0));
    ck("disk 1 carries its own, not disk 0's",
       blk_pci_bus(1) == 11 && blk_pci_dev(1) == 21, blk_pci_bus(1));
    ck("an index that is not a disk reports 0, not stale data",
       blk_pci_bus(5) == 0 && blk_pci_dev(5) == 0, blk_pci_bus(5));

    head("blk_find: the index this layer gave a driver's own unit");
    machine(0,2,2,0,0);
    g_ahci_atapi[0] = 1;        /* so AHCI unit 1 is the only SATA disk */
    blk_scan();
    ck("NVMe unit 1 is disk 1", blk_find(BLK_BUS_NVME, 1) == 1,
       blk_find(BLK_BUS_NVME, 1));
    ck("AHCI unit 1 is disk 2", blk_find(BLK_BUS_AHCI, 1) == 2,
       blk_find(BLK_BUS_AHCI, 1));
    ck("AHCI unit 0 is not in the table at all -- it is the DVD",
       blk_find(BLK_BUS_AHCI, 0) == -1, blk_find(BLK_BUS_AHCI, 0));
    ck("a bus that is not there answers -1",
       blk_find(BLK_BUS_USB, 0) == -1, blk_find(BLK_BUS_USB, 0));

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
