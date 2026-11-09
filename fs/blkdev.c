/* ════════════════════════════════════════════════════════════════════
   fs/blkdev.c -- the one place that knows a disk is on a bus.

   include/blkdev.h carries the argument for why this file exists and
   what belongs in it. The short version: a grep for "every place that
   chooses a bus" -- this tree's own rule when adding one -- found three
   sector-dispatch switches holding four, four and TWO buses, two
   releases after "the installer had one block layer and the filesystem
   had another" was declared fixed.

   What is here: the table, the dispatch, the per-disk refusal, the bus
   word, the registration order, and the boot-volume preference. Nothing
   above a sector: no partitions, no GPT, no cache.

   This file is compiled natively by tests/test_blkdev.c and
   tests/test_diskname.c, which is why the back-end probes are the only
   part of it that touches hardware and they sit behind BLK_NO_HW.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/kpoll.h"
#include "blkdev.h"
#include "string.h"

static blk_dev_t g_dev[BLK_MAX_DEV];
static int       g_ndev;

/* ════════════════════════════════════════════════════════════════════
   The table
   ════════════════════════════════════════════════════════════════════ */

void blk_reset(void){
    memset(g_dev, 0, sizeof g_dev);
    g_ndev = 0;
}

int blk_register(const blk_ops_t *ops, uint8_t kind, int unit,
                 uint64_t sectors, uint8_t pci_bus, uint8_t pci_dev)
{
    if(!ops || !ops->read)        return -1;   /* a disk you cannot read
                                                  is not a disk */
    if(g_ndev >= BLK_MAX_DEV)     return -1;
    g_dev[g_ndev].ops     = ops;
    g_dev[g_ndev].kind    = kind;
    g_dev[g_ndev].unit    = unit;
    g_dev[g_ndev].sectors = sectors;
    g_dev[g_ndev].pci_bus = pci_bus;
    g_dev[g_ndev].pci_dev = pci_dev;
    return g_ndev++;
}

int               blk_count(void)        { return g_ndev; }
const blk_dev_t  *blk_dev(int i)         { return (i>=0 && i<g_ndev) ? &g_dev[i] : 0; }
uint64_t          blk_sectors(int i)     { return (i>=0 && i<g_ndev) ? g_dev[i].sectors : 0; }
uint8_t           blk_kind(int i)        { return (i>=0 && i<g_ndev) ? g_dev[i].kind : BLK_BUS_NONE; }
int               blk_unit(int i)        { return (i>=0 && i<g_ndev) ? g_dev[i].unit : -1; }
uint8_t           blk_pci_bus(int i)     { return (i>=0 && i<g_ndev) ? g_dev[i].pci_bus : 0; }
uint8_t           blk_pci_dev(int i)     { return (i>=0 && i<g_ndev) ? g_dev[i].pci_dev : 0; }

int blk_removable(int i){
    if(i < 0 || i >= g_ndev) return 0;
    return g_dev[i].ops->removable ? 1 : 0;
}

/* A disk is read-only if its bus has no write function, or if the bus
   says this particular disk cannot be written.

   Those are two questions and they used to be one: the installer's
   disk_is_readonly() switched on the bus TYPE to ask the per-disk
   question, so every bus added had to be remembered in it or silently
   became writable. A bus with no write() is refused here without the
   back-end having to say anything, which is what makes a new read-only
   bus safe by default rather than by diligence. */
int blk_readonly(int i){
    if(i < 0 || i >= g_ndev) return 1;
    if(!g_dev[i].ops->write) return 1;
    if(g_dev[i].ops->readonly) return g_dev[i].ops->readonly(g_dev[i].unit) ? 1 : 0;
    return 0;
}

/* ── v0.8.8: bracketed for kernel_poll(), as a wrapper ───────────────
 *
 * The sector buffers under this layer are module-level and shared, and
 * the thing kernel_poll() can reach from here is worse than a torn
 * buffer: the USB hot-plug client calls blk_scan(), which REBUILDS this
 * file's table.  A poll that arrived between the bounds check and the
 * dispatch below would be renumbering g_dev[] while an index into it
 * was live on the stack.
 *
 * Wrapper rather than enter/leave in the body, so no return path can be
 * missed; tools/check_kpoll_guard.py holds the list. */
static int blk_read_inner(int i, uint64_t lba, uint8_t *buf){
    if(i < 0 || i >= g_ndev || !buf) return 0;
    return g_dev[i].ops->read(g_dev[i].unit, lba, buf) ? 1 : 0;
}

int blk_read(int i, uint64_t lba, uint8_t *buf){
    int r;
    kpoll_enter();
    r = blk_read_inner(i, lba, buf);
    kpoll_leave();
    return r;
}

static int blk_write_inner(int i, uint64_t lba, const uint8_t *buf){
    if(i < 0 || i >= g_ndev || !buf) return 0;
    /* Below every caller and every screen: a code path cannot reach a
       disk with no write function by forgetting to ask. */
    if(blk_readonly(i))              return 0;
    return g_dev[i].ops->write(g_dev[i].unit, lba, buf) ? 1 : 0;
}

int blk_write(int i, uint64_t lba, const uint8_t *buf){
    int r;
    kpoll_enter();
    r = blk_write_inner(i, lba, buf);
    kpoll_leave();
    return r;
}

/* ── the bus word ─────────────────────────────────────────────────────
   Out of range returns "none" rather than inventing a bus for a disk
   that is not there. A welcome screen that tested two booleans used to
   print "No disks found" directly above "1 VirtIO disk found". */
const char *blk_bus_name(int i){
    if(i < 0 || i >= g_ndev) return "none";
    return g_dev[i].ops->name;
}

void blk_bus_counts(int *ide, int *sata, int *nvme, int *virtio, int *usb){
    int a=0,s=0,n=0,v=0,u=0;
    for(int i=0;i<g_ndev;i++){
        switch(g_dev[i].kind){
            case BLK_BUS_ATA:    a++; break;
            case BLK_BUS_AHCI:   s++; break;
            case BLK_BUS_NVME:   n++; break;
            case BLK_BUS_VIRTIO: v++; break;
            case BLK_BUS_USB:    u++; break;
            default: break;
        }
    }
    if(ide)    *ide    = a;
    if(sata)   *sata   = s;
    if(nvme)   *nvme   = n;
    if(virtio) *virtio = v;
    if(usb)    *usb    = u;
}

int blk_find(uint8_t kind, int unit){
    for(int i=0;i<g_ndev;i++)
        if(g_dev[i].kind == kind && g_dev[i].unit == unit) return i;
    return -1;
}

/* ════════════════════════════════════════════════════════════════════
   Which disk is this machine's own
   ════════════════════════════════════════════════════════════════════

   fat16_init() and fat32_init() both asked this and both answered it
   themselves. fat16's order was VirtIO, ATA, AHCI, NVMe; the installer's
   registration order is VirtIO, NVMe, AHCI, ATA. On a machine where both
   the NVMe and the legacy ATA ports answer, the installer called the
   NVMe disk 0 and the filesystem bound itself to the ATA one, and
   nothing on any screen could tell you.

   The orders are not a duplicate to collapse -- registration order is
   identity and this is a preference -- but there is one of each, and
   this is the one.

   The preference, oldest-interface-first, is fat16_init()'s and the
   argument is its:

     VirtIO   a machine that has VirtIO has nothing else.
     ATA      legacy ports answering means firmware chose compatibility
              mode, where the AHCI registers may not be there at all.
     AHCI     reached exactly when the other two are not, which is every
              modern laptop.
     NVMe     same reason as AHCI, one step further on.

   USB is last and that is not a detail. An installed HavenDOS boots from
   a fixed disk; a stick that was plugged in before the kernel got here
   must not become the volume the system mounts as its own. It is in the
   list at all because a machine can boot HavenDOS from a USB disk and
   then needs to find it -- but only when nothing fixed answered. */
int blk_boot_dev(void){
    static const uint8_t pref[] = {
        BLK_BUS_VIRTIO, BLK_BUS_ATA, BLK_BUS_AHCI, BLK_BUS_NVME, BLK_BUS_USB
    };
    for(unsigned p = 0; p < sizeof pref / sizeof pref[0]; p++)
        for(int i = 0; i < g_ndev; i++)
            if(g_dev[i].kind == pref[p]) return i;
    return -1;
}

/* ════════════════════════════════════════════════════════════════════
   The back-ends
   ════════════════════════════════════════════════════════════════════

   One ops table per bus. The bus word lives in it, which is what makes
   tools/check_bus_label.py able to guard the words themselves rather
   than guard a comment saying everything asks in one place -- a comment
   that did not survive contact with a new file, twice.

   BLK_NO_HW is defined by the suites, which link this file for the
   table, the dispatch and the words and have no drivers to call. */
#ifndef BLK_NO_HW

/* ── VirtIO ───────────────────────────────────────────────────────── */
extern int      virtio_scan_drives(void);
extern uint64_t virtio_drive_sectors(int idx);
extern uint8_t  virtio_drive_bus(int idx);
extern uint8_t  virtio_drive_dev(int idx);
extern int      virtio_drive_read (int idx, uint64_t sector, uint8_t *buf);
extern int      virtio_drive_write(int idx, uint64_t sector, const uint8_t *buf);

static const blk_ops_t virtio_ops = {
    "VirtIO", virtio_drive_read, virtio_drive_write, 0, 0
};

/* ── NVMe ─────────────────────────────────────────────────────────── */
extern int      nvme_count(void);
extern uint64_t nvme_sectors(int i);
extern int      nvme_writable(int idx);
extern int      nvme_read (int idx, uint64_t lba, uint32_t count, void *buf);
extern int      nvme_write(int idx, uint64_t lba, uint32_t count, const void *buf);

static int nvme_rd1(int unit, uint64_t lba, uint8_t *buf){
    return nvme_read(unit, lba, 1, buf) == 0;
}
static int nvme_wr1(int unit, uint64_t lba, const uint8_t *buf){
    return nvme_write(unit, lba, 1, buf) == 0;
}
/* A namespace whose block size this driver has no write path for. The
   dangerous case is a 4096-byte-block namespace, where writing one
   512-byte sector means reading the block first or destroying the other
   seven -- so the refusal is the driver's own answer, not a guess. */
static int nvme_ro(int unit){ return !nvme_writable(unit); }

static const blk_ops_t nvme_ops = {
    "NVMe", nvme_rd1, nvme_wr1, nvme_ro, 0
};

/* ── AHCI ─────────────────────────────────────────────────────────── */
extern int      ahci_init(void);
extern int      ahci_count(void);
extern uint64_t ahci_sectors(int idx);
extern int      ahci_is_atapi(int idx);
extern int      ahci_read_sector (int idx, uint64_t lba, uint8_t *buf);
extern int      ahci_write_sector(int idx, uint64_t lba, const uint8_t *buf);

/* Unreachable by construction -- blk_scan() does not register an ATAPI
   port at all -- and here anyway, because a future scan that stops
   skipping them must not make a DVD-ROM writable as a side effect. */
static int ahci_ro(int unit){ return ahci_is_atapi(unit); }

static const blk_ops_t ahci_ops = {
    "SATA", ahci_read_sector, ahci_write_sector, ahci_ro, 0
};

/* ── legacy ATA / SATA in compatibility mode ──────────────────────── */
extern void     ata_init(void);
extern int      ata_detected(void);
extern uint32_t ata_sectors(void);
extern int      ata_lba48(void);
extern int      ata_read_sector   (uint32_t lba, uint8_t *buf);
extern int      ata_write_sector  (uint32_t lba, const uint8_t *buf);
extern int      ata_read_sector48 (uint64_t lba, uint8_t *buf);
extern int      ata_write_sector48(uint64_t lba, const uint8_t *buf);

/* One drive, so the unit is ignored. The LBA48 choice is made here
   rather than by each caller, which is how a 48-bit address used to get
   truncated to 28 bits in one copy of this line and not another. */
static int ata_rd(int unit, uint64_t lba, uint8_t *buf){
    (void)unit;
    if(ata_lba48()) return ata_read_sector48(lba, buf);
    return ata_read_sector((uint32_t)lba, buf);
}
static int ata_wr(int unit, uint64_t lba, const uint8_t *buf){
    (void)unit;
    if(ata_lba48()) return ata_write_sector48(lba, buf);
    return ata_write_sector((uint32_t)lba, buf);
}

static const blk_ops_t ata_ops = {
    "IDE", ata_rd, ata_wr, 0, 0
};

/* ── USB mass storage ─────────────────────────────────────────────── */
extern int      msc_count(void);
extern uint64_t msc_sectors(int idx);
extern int      msc_readonly(int idx);
extern int      msc_read_sector (int idx, uint64_t lba, uint8_t *buf);
extern int      msc_write_sector(int idx, uint64_t lba, const uint8_t *buf);

/* removable = 1. The only field in this file that protects somebody's
   Windows partition: see the note in include/blkdev.h on why a stick
   must register after every fixed disk. */
const blk_ops_t blk_usb_ops = {
    "USB", msc_read_sector, msc_write_sector, msc_readonly, 1
};

/* ════════════════════════════════════════════════════════════════════
   blk_scan -- registration order, in one function
   ════════════════════════════════════════════════════════════════════

   This order is identity: it is what the installer's picker shows, what
   fat32_vol_t::disk stores, and what a person reads off the screen that
   asks them to type YES. Changing it renumbers disks.

   Fixed buses first, every removable one after, because a stick that
   registers before a fixed disk SHIFTS that disk's index -- and the
   index is how the confirmation screen and the cached dry-run plan name
   the disk they are about to write to. tests/test_blkdev.c holds that
   property as a check. */
int blk_scan(void){
    blk_reset();

    /* 1 -- VirtIO */
    { int n = virtio_scan_drives();
      for(int i = 0; i < n; i++)
          blk_register(&virtio_ops, BLK_BUS_VIRTIO, i,
                       virtio_drive_sectors(i),
                       virtio_drive_bus(i), virtio_drive_dev(i)); }

    /* 2 -- NVMe. Registered writable now: the blanket refusal that used
       to protect somebody's Windows install is gone, because what
       protects it is that the only installer allowed to write to a disk
       with a partition table is the one that ADDS to it. */
    for(int i = 0; i < nvme_count(); i++)
        blk_register(&nvme_ops, BLK_BUS_NVME, i, nvme_sectors(i), 0, 0);

    /* 3 -- AHCI, before legacy ATA: on a machine where both answer, the
       AHCI registers are the real interface and the legacy ports are a
       compatibility shim reaching at most two of the controller's ports.
       An ATAPI port is not registered -- an optical drive is not a disk,
       and fs/fat16.c used to bind itself to the DVD-ROM on q35 and then
       report the machine unformatted. */
    ahci_init();
    for(int i = 0; i < ahci_count(); i++){
        if(ahci_is_atapi(i)) continue;
        blk_register(&ahci_ops, BLK_BUS_AHCI, i, ahci_sectors(i), 0, 0);
    }

    /* 4 -- legacy ATA / SATA in compatibility mode */
    ata_init();
    if(ata_detected())
        blk_register(&ata_ops, BLK_BUS_ATA, 0, (uint64_t)ata_sectors(), 0, 0);

    /* 5 -- USB mass storage, LAST, and the comment above says why. */
    for(int i = 0; i < msc_count(); i++)
        blk_register(&blk_usb_ops, BLK_BUS_USB, i, msc_sectors(i), 0, 0);

    return g_ndev;
}

#endif /* BLK_NO_HW */
