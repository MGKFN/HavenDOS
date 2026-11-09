/* ════════════════════════════════════════════════════════════════════
   test_fat16borrow.c -- the writable borrow, and whose disk the writes
   land on.

   ── WHY THIS SUITE EXISTS ────────────────────────────────────────────

   v0.8.6.3 added `fat16 cp`, which copies a file from this system's own
   volume onto a volume on a USB stick. fs/fat16.c holds ONE volume, so
   doing that means pointing the driver at the stick, writing, and
   pointing it back -- and the driver's whole FAT lives in a 128 KB
   cache that is flushed when dirty.

   There is therefore a single line of ordering between "a file appears
   on the stick" and "the stick's allocation table is written over the
   system volume's, destroying an installed HavenDOS". The flush has to
   happen while g_fat16_dev still names the stick, and fat_dirty has to
   be clear before it names the system disk again.

   A comment saying so is not evidence. So this suite gives the kernel
   TWO disks, both with real FAT16 volumes made by this tree's own
   formatter, and COUNTS THE WRITES PER DISK. The assertion that matters
   is not "the file arrived" -- it is "disk 0 was not written to", which
   is a fact about sectors and not about outcomes.

   That distinction is the one the v0.8.6 injection harnesses found ten
   holes of in my own suites: checking the outcome instead of the wire.

   ── WHAT THE TWO DISKS ARE ───────────────────────────────────────────

     RAM 0   VirtIO, FIXED      the system volume, what fat16_init()
                                mounts and what must not be touched
     RAM 1   NVMe,   FIXED      a fixed disk that is NOT the boot disk
     RAM 2   USB,    REMOVABLE  the stick

   ── RAM INDEX IS NOT BLOCK INDEX, AND THIS SUITE LEARNED IT ──────────

   The write counters are per RAM DISK, which never moves. The kernel's
   own numbering does: blk_scan() registers every fixed bus before any
   removable one, so the stick is block device 1 with two disks on the
   bus and block device 2 with three. The first version of this suite
   hardcoded 1 and silently tested the wrong disk the moment a third
   appeared.

   So nothing here writes a block index as a literal. bdev() asks
   blk_find() for the real one, which is also the only way a suite can
   notice if that registration order ever changes.

   BLKSTUB_USB_OWN tells tests/blkbus.inc to leave the USB stubs to this
   file, since every other suite that includes it wants no stick at
   all.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* -Iinclude makes <string.h> the kernel's own, so this file declares
   nothing from it -- fs/fat16.c includes that header itself. The host
   memset/memcpy come in through it. tests/test_fat16fmt.c says the same
   thing at more length. */

#include "../include/fat16fmt.h"

/* ── two RAM disks, with a write counter each ─────────────────────────
   Sized to hold a small FAT16 volume and nothing more. A write outside
   either one is a bug in the test, not in the kernel, so it is counted
   separately and asserted to be zero. */
#define DISK_SECTS   40960u          /* 20 MB */
#define PART_LBA     2048u           /* both volumes start here        */

#define NDISK 3

static uint8_t  *g_disk[NDISK];
static unsigned  g_writes[NDISK];    /* sectors written, per disk      */
static unsigned  g_reads[NDISK];
static unsigned  g_oob;              /* writes outside any disk        */
static int       g_usb_present = 1;
static int       g_usb_ro;           /* the stick says read-only       */
static int       g_virtio_present = 1;
static int       g_nvme_present;     /* a FIXED disk that is not boot  */

/* ── A STICK THAT FAILS PART WAY THROUGH A WRITE ──────────────────────
   Set g_fail_after to N and the Nth write to that disk, and every one
   after it, fails. This is how a `cp` reaches fat16_return() with the
   FAT cache still DIRTY -- which is the only state in which the flush
   ordering in fat16_return() is observable at all, and it is the state
   a yanked stick produces. Without it the inline flush in
   fat16_write() has already cleaned the cache and the flush in
   fat16_return() is a no-op whichever side of the device switch it
   sits on. tools/check_fat16borrow_suite.py found that out. */
static int      g_fail_disk = -1;
static unsigned g_fail_after;

static int ram_read(int d, uint64_t lba, uint8_t *buf){
    if(d < 0 || d >= NDISK || !g_disk[d] || lba >= DISK_SECTS) return 0;
    g_reads[d]++;
    memcpy(buf, g_disk[d] + lba * 512u, 512);
    return 1;
}
static int ram_write(int d, uint64_t lba, const uint8_t *buf){
    if(d < 0 || d >= NDISK || !g_disk[d] || lba >= DISK_SECTS){
        g_oob++; return 0;
    }
    if(d == g_fail_disk && g_writes[d] >= g_fail_after){
        g_writes[d]++;               /* counted: it was attempted      */
        return 0;
    }
    g_writes[d]++;
    memcpy(g_disk[d] + lba * 512u, buf, 512);
    return 1;
}

/* ── disk 0: VirtIO, the system's own, and the boot device ────────── */
int virtio_blk_ready(void){ return g_virtio_present; }
int virtio_blk_read (uint64_t lba, uint8_t *buf){ return ram_read (0, lba, buf); }
int virtio_blk_write(uint64_t lba, const uint8_t *buf){ return ram_write(0, lba, buf); }
int virtio_drive_count(void){ return g_virtio_present ? 1 : 0; }

/* ── disk 2: NVMe. FIXED, and NOT the boot device ──────────────────────
   blk_boot_dev() prefers VirtIO, so with disk 0 present this one is a
   fixed disk that is not the boot disk -- the only arrangement in which
   the blk_removable() refusal can be told apart from the
   blk_boot_dev() one. With both guarding the same disk, removing
   either changes nothing, which is exactly what the injection harness
   reported. */
int nvme_count(void){ return g_nvme_present ? 1 : 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i; if(n != 1) return -1;
    return ram_read(1, l, (uint8_t *)b) ? 0 : -1;
}
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i; if(n != 1) return -1;
    return ram_write(1, l, (const uint8_t *)b) ? 0 : -1;
}

/* ── RAM 2: USB, removable. blkbus.inc leaves these to us ─────────── */
#define BLKSTUB_USB_OWN 1
int      msc_count(void){ return g_usb_present ? 1 : 0; }
uint64_t msc_sectors(int i){ (void)i; return DISK_SECTS; }
int      msc_readonly(int i){ (void)i; return g_usb_ro; }
int msc_read_sector(int i, uint64_t l, uint8_t *b){
    (void)i; return ram_read(2, l, b);
}
int msc_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i; return ram_write(2, l, b);
}

/* ── the buses that are not here ──────────────────────────────────── */
int ata_lba48(void){ return 0; }
int ata_present(void){ return 0; }
int ata_identify(void){ return 0; }
int ata_read_sector (uint32_t lba, uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_write_sector(uint32_t lba, const uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_read_sector48 (uint64_t lba, uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_write_sector48(uint64_t lba, const uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_detected(void){ return 0; }
int ahci_count(void){ return 0; }
int ahci_is_atapi(int p){ (void)p; return 1; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }

/* fs/fat16.c prints progress through the console, and reports a volume
   that did not come back through klog(). Both swallowed: the suite's
   own output is the readable thing.

   The refusals under test say WHY in the log, and this suite asserts
   the refusal rather than its wording -- one that matched the message
   would fail on a reworded sentence and pass on a missing refusal. */
static void vga_printf(const char *fmt, ...){ (void)fmt; }
static void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

#include "blkbus.inc"                /* the rest of blk_scan(), + blkdev.c */
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/fat16fmt.c"
#include "../fs/fat16.c"
/* fs/gpt.c is LINKED, not stubbed: fs/fat16.c asks it whether a disk is
   GPT before falling back to the MBR, and a stub answering "no" would
   be testing a different program from the one that ships. The two disks
   here are MBR, so the real gpt_present() says no for the real reason. */

/* ── the harness ──────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* fat16_format() writes through a callback. Both volumes are formatted
   through the same RAM disks the kernel reads, so the volumes under
   test are made by the shipping formatter rather than by a fixture
   somebody typed out. */
static int fmt_disk;
static int fmt_wr(void *ctx, uint32_t lba, const uint8_t *buf){
    (void)ctx;
    return ram_write(fmt_disk, PART_LBA + lba, buf);
}

static int make_volume(int disk)
{
    fat16_geom_t g;
    if(!fat16_geom(DISK_SECTS - PART_LBA, &g)) return 0;
    fmt_disk = disk;
    return fat16_format(fmt_wr, NULL, &g, PART_LBA, "HAVENDOS");
}

/* An MBR on each disk, so the partition is findable the way the kernel
   finds one, rather than by this file telling it where to look. */
static void write_mbr(int disk)
{
    uint8_t mbr[512];
    uint8_t *e = mbr + 446;
    memset(mbr, 0, sizeof mbr);
    e[4] = 0x06;                                  /* FAT16 type byte   */
    e[8]  = (uint8_t)(PART_LBA & 0xFF);
    e[9]  = (uint8_t)((PART_LBA >> 8) & 0xFF);
    e[10] = (uint8_t)((PART_LBA >> 16) & 0xFF);
    e[11] = (uint8_t)((PART_LBA >> 24) & 0xFF);
    {
        uint32_t cnt = DISK_SECTS - PART_LBA;
        e[12] = (uint8_t)(cnt & 0xFF);
        e[13] = (uint8_t)((cnt >> 8) & 0xFF);
        e[14] = (uint8_t)((cnt >> 16) & 0xFF);
        e[15] = (uint8_t)((cnt >> 24) & 0xFF);
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    ram_write(disk, 0, mbr);
}

/* The kernel's block index for a bus, asked rather than assumed. */
static int bdev(uint8_t kind){ return blk_find(kind, 0); }
#define SYSDEV   bdev(BLK_BUS_VIRTIO)
#define STICK    bdev(BLK_BUS_USB)
#define FIXED2   bdev(BLK_BUS_NVME)
/* ...and the RAM disk each one is backed by, for the write counters. */
#define RAM_SYS   0
#define RAM_FIXED 1
#define RAM_STICK 2

static void zero_counters(void){
    int i;
    for(i = 0; i < NDISK; i++){ g_writes[i] = 0; g_reads[i] = 0; }
    g_oob = 0;
}

int main(void)
{
    static const char PAYLOAD[] =
        "the bytes that must end up on the stick and nowhere else\n";
    int i;

    for(i = 0; i < NDISK; i++){
        g_disk[i] = calloc(DISK_SECTS, 512);
        if(!g_disk[i]){ printf("  SKIP  out of memory\n"); return 0; }
    }

    printf("the writable borrow, and whose disk the writes land on\n");
    printf("=====================================================\n");

    /* ── setup ─────────────────────────────────────────────────────── */
    head("two disks, each with a FAT16 volume made by our own formatter");
    write_mbr(RAM_SYS); write_mbr(RAM_STICK);
    ck("the system disk is formatted", make_volume(RAM_SYS), 0);
    ck("the stick is formatted",        make_volume(RAM_STICK), 0);

    blk_reset();
    ck("the block layer finds both", blk_scan() == 2, blk_count());
    ck("the system disk is the fixed one", !blk_removable(SYSDEV), 0);
    ck("the stick is the removable one",    blk_removable(STICK), 0);
    ck("...and the stick registered LAST", STICK > SYSDEV, STICK);
    ck("...and the fixed disk is the boot device",
       blk_boot_dev() == SYSDEV, blk_boot_dev());

    ck("the system volume mounts", fat16_init() == 1, 0);
    ck("...and it is the system disk", fat16_dev() == SYSDEV, fat16_dev());
    ck("nothing is borrowed yet", !fat16_borrowed(), 0);

    /* ── the refusals, before anything is written ─────────────────── */
    head("a writable borrow of a disk that is not removable is REFUSED");
    zero_counters();
    ck("the boot disk is refused",
       fat16_borrow_rw(SYSDEV, PART_LBA) == 0, 0);
    ck("...and no borrow is left outstanding", !fat16_borrowed(), 0);
    ck("...and NOT ONE SECTOR of it was written",
       g_writes[RAM_SYS] == 0, (long)g_writes[RAM_SYS]);
    ck("...and the system volume is still mounted on it",
       fat16_dev() == SYSDEV, fat16_dev());

    head("a FIXED disk that is not the boot disk is refused TOO");
    /* Two refusals guard this, not one, and with both of them pointed
       at the same disk neither can be told from the other -- removing
       either changed nothing and the harness said so. So: a third disk,
       fixed, which blk_boot_dev() does not choose. Only blk_removable()
       stands between this and a write. */
    g_nvme_present = 1;
    write_mbr(RAM_FIXED);
    ck("the second fixed disk is formatted", make_volume(RAM_FIXED), 0);
    blk_reset();
    ck("the block layer finds three disks", blk_scan() == 3, blk_count());
    ck("the new disk is fixed",       !blk_removable(FIXED2), 0);
    ck("...and is NOT the boot disk", blk_boot_dev() != FIXED2,
       blk_boot_dev());
    ck("...and the stick was pushed past it, as removable disks are",
       STICK > FIXED2, STICK);
    ck("the system volume still mounts", fat16_init() == 1, 0);
    zero_counters();
    ck("a writable borrow of the fixed non-boot disk is refused",
       fat16_borrow_rw(FIXED2, PART_LBA) == 0, 0);
    ck("...and not one sector of it was written",
       g_writes[RAM_FIXED] == 0, (long)g_writes[RAM_FIXED]);
    ck("...and nothing is left borrowed", !fat16_borrowed(), 0);
    g_nvme_present = 0;
    blk_reset(); blk_scan();
    ck("the system volume mounts again", fat16_init() == 1, 0);

    head("...and the BOOT disk is refused even when it is removable");
    /* With no fixed disk at all, blk_boot_dev() picks the stick. It is
       removable, so blk_removable() lets it through and only the
       blk_boot_dev() refusal is left standing. That is the arrangement
       in which THAT guard can be told apart. */
    g_virtio_present = 0;
    blk_reset();
    ck("only the stick is on the bus", blk_scan() == 1, blk_count());
    ck("...so it is the boot device", blk_boot_dev() == STICK,
       blk_boot_dev());
    ck("...and it is removable", blk_removable(STICK), 0);
    zero_counters();
    ck("a writable borrow of it is refused ANYWAY",
       fat16_borrow_rw(STICK, PART_LBA) == 0, 0);
    ck("...and not one sector of it was written",
       g_writes[RAM_STICK] == 0, (long)g_writes[RAM_STICK]);
    g_virtio_present = 1;
    blk_reset(); blk_scan();
    ck("both disks are back", blk_count() == 2, blk_count());
    ck("the system volume mounts again", fat16_init() == 1, 0);

    head("a read-only stick is refused");
    g_usb_ro = 1;
    blk_reset(); blk_scan();
    zero_counters();
    ck("the stick reports read-only", blk_readonly(STICK), 0);
    ck("...so a writable borrow is refused",
       fat16_borrow_rw(STICK, PART_LBA) == 0, 0);
    ck("...and nothing was written to it", g_writes[RAM_STICK] == 0,
       (long)g_writes[RAM_STICK]);
    g_usb_ro = 0;
    blk_reset(); blk_scan();
    ck("the system volume still mounts afterwards", fat16_init() == 1, 0);

    head("a READ-ONLY borrow still refuses writes, as it did in 0.8.6.1");
    zero_counters();
    ck("the stick borrows read-only", fat16_borrow(STICK, PART_LBA) == 1, 0);
    ck("...and reports itself borrowed", fat16_borrowed(), 0);
    ck("...but NOT as writable", !fat16_borrowed_rw(), 0);
    ck("a write through it is refused",
       fat16_write_path("/RO.TXT", PAYLOAD, sizeof PAYLOAD - 1) <= 0, 0);
    ck("...and no sector of the stick was written", g_writes[RAM_STICK] == 0,
       (long)g_writes[RAM_STICK]);
    fat16_return();
    ck("...and none of the system disk either", g_writes[RAM_SYS] == 0,
       (long)g_writes[RAM_SYS]);

    /* ── the copy, and the only question that matters ─────────────── */
    head("a writable borrow of the stick writes to the STICK");
    zero_counters();
    ck("the stick borrows for writing", fat16_borrow_rw(STICK, PART_LBA) == 1, 0);
    ck("...and says so", fat16_borrowed() && fat16_borrowed_rw(), 0);
    ck("...and the driver is pointed at the stick", fat16_dev() == STICK,
       fat16_dev());
    {
        int w = fat16_write_path("/HD.TXT", PAYLOAD, sizeof PAYLOAD - 1);
        ck("the write reports the number of bytes written",
           w == (int)(sizeof PAYLOAD - 1), w);
    }
    ck("the stick was written to", g_writes[RAM_STICK] > 0, (long)g_writes[RAM_STICK]);

    head("AND NOT ONE SECTOR OF THE SYSTEM DISK, AT ANY POINT");
    /* The whole suite is here. A flush on the wrong side of the device
       switch in fat16_return() writes the stick's FAT over disk 0's,
       and nothing about that fails, warns, or is visible until the
       machine next boots. */
    ck("the system disk untouched while the borrow was held", g_writes[RAM_SYS] == 0,
       (long)g_writes[RAM_SYS]);
    fat16_return();
    ck("the system disk untouched by the RETURN as well", g_writes[RAM_SYS] == 0,
       (long)g_writes[RAM_SYS]);
    ck("no write went outside either disk", g_oob == 0, (long)g_oob);

    head("A STICK THAT DIES MID-COPY DOES NOT TAKE THE SYSTEM WITH IT");
    /* THE REASON THIS SUITE EXISTS.
       fat16_write() flushes the FAT as it goes, so after a successful
       copy the cache is clean and the flush in fat16_return() has
       nothing to do -- on either side of the device switch. The
       ordering is only observable when the cache is still DIRTY at
       return, which is what a stick pulled out mid-write produces.

       So: fail the stick's writes part way through, then return, then
       ask whose disk was written to. With the flush on the wrong side
       of the switch, the stick's allocation table lands on the system
       volume and the only evidence is a boot that stops working. */
    zero_counters();
    ck("the stick borrows for writing", fat16_borrow_rw(STICK, PART_LBA) == 1, 0);
    g_fail_disk  = RAM_STICK;
    g_fail_after = 3;                 /* a few sectors in, then dead   */
    {
        int w = fat16_write_path("/DEAD.TXT", PAYLOAD, sizeof PAYLOAD - 1);
        ck("the write does not claim success",
           w != (int)(sizeof PAYLOAD - 1), w);
    }
    ck("the stick was written to, as far as it got", g_writes[RAM_STICK] > 0,
       (long)g_writes[RAM_STICK]);
    ck("and the system disk was NOT", g_writes[RAM_SYS] == 0, (long)g_writes[RAM_SYS]);
    fat16_return();
    g_fail_disk = -1;
    ck("AND THE RETURN DID NOT WRITE TO THE SYSTEM DISK EITHER",
       g_writes[RAM_SYS] == 0, (long)g_writes[RAM_SYS]);
    ck("no write went outside any disk", g_oob == 0, (long)g_oob);
    ck("the system volume came back", fat16_dev() == SYSDEV && fat16_detected(),
       fat16_dev());
    ck("...and the dead stick's file is not on it",
       !fat16_exists_path("/DEAD.TXT"), 0);

    head("and a READ-ONLY borrow after a writable one is still read-only");
    /* g_borrow_rw not being cleared on return is invisible to
       fat16_borrowed_rw(), which also tests g_borrowed -- so the way to
       see it is to take a read-only borrow afterwards and try to write
       through it. The harness found this one too. */
    zero_counters();
    ck("the stick borrows read-only", fat16_borrow(STICK, PART_LBA) == 1, 0);
    ck("...and does not report itself writable", !fat16_borrowed_rw(), 0);
    ck("a write through it is REFUSED",
       fat16_write_path("/AFTER.TXT", PAYLOAD, sizeof PAYLOAD - 1) <= 0, 0);
    ck("...and not one sector of the stick was written", g_writes[RAM_STICK] == 0,
       (long)g_writes[RAM_STICK]);
    fat16_return();
    ck("...nor of the system disk", g_writes[RAM_SYS] == 0, (long)g_writes[RAM_SYS]);

    head("and the system's own volume came back");
    ck("the driver is pointed at the system disk again", fat16_dev() == SYSDEV,
       fat16_dev());
    ck("nothing is borrowed", !fat16_borrowed() && !fat16_borrowed_rw(), 0);
    ck("the system volume is readable", fat16_detected(), 0);
    {
        /* The copied file is on the STICK, so it must NOT be visible on
           the system volume now that the borrow is back. A `cp` that
           quietly wrote to the wrong volume would show up right here. */
        ck("the copied file is NOT on the system volume",
           !fat16_exists_path("/HD.TXT"), 0);
    }

    head("and the file really is on the stick");
    {
        char back[128];
        int  r;
        ck("it borrows again", fat16_borrow(STICK, PART_LBA) == 1, 0);
        r = fat16_read_path("/HD.TXT", back, sizeof back);
        ck("the file is there", r == (int)(sizeof PAYLOAD - 1), r);
        ck("...with the bytes that were written",
           r > 0 && !memcmp(back, PAYLOAD, (unsigned)r), 0);
        fat16_return();
    }

    head("a write while the system volume is mounted still works");
    /* The guard is on BORROWED volumes, not on writing. A refusal that
       also broke the machine's own filesystem would pass every check
       above. */
    zero_counters();
    {
        int w = fat16_write_path("/OWN.TXT", PAYLOAD, sizeof PAYLOAD - 1);
        ck("the system volume accepts a write",
           w == (int)(sizeof PAYLOAD - 1), w);
        ck("...and it landed on the system disk", g_writes[RAM_SYS] > 0, (long)g_writes[RAM_SYS]);
        ck("...and not on the stick", g_writes[RAM_STICK] == 0, (long)g_writes[RAM_STICK]);
        ck("...and it reads back", fat16_exists_path("/OWN.TXT"), 0);
    }

    head("one borrow at a time");
    ck("a writable borrow succeeds", fat16_borrow_rw(STICK, PART_LBA) == 1, 0);
    ck("...a second is refused", fat16_borrow_rw(STICK, PART_LBA) == 0, 0);
    ck("...and so is a read-only one", fat16_borrow(STICK, PART_LBA) == 0, 0);
    ck("...and the first is still the one that is held",
       fat16_borrowed_rw() && fat16_dev() == STICK, fat16_dev());
    fat16_return();
    ck("returning once is enough", !fat16_borrowed(), 0);
    fat16_return();          /* idempotent */
    ck("returning twice does not fault or re-borrow",
       !fat16_borrowed() && fat16_dev() == SYSDEV, fat16_dev());

    head("a negative or absent disk is refused");
    ck("disk -1", fat16_borrow_rw(-1, PART_LBA) == 0, 0);
    ck("a disk index past the table", fat16_borrow_rw(7, PART_LBA) == 0, 0);
    ck("...and nothing is left borrowed", !fat16_borrowed(), 0);

    printf("\n%d/%d passed\n", total - fails, total);
    for(i = 0; i < NDISK; i++) free(g_disk[i]);
    return fails ? 1 : 0;
}
