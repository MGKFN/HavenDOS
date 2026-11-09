#pragma once
#include "types.h"

/* ── The block layer: ONE place that knows a disk is on a bus ─────────
 *
 * ── WHY THIS EXISTS ──────────────────────────────────────────────────
 *
 * v0.8.3.1 shipped three bugs that were one mistake: "the installer had
 * one block layer and the filesystem had another, and only one of them
 * got taught about NVMe." The fix that release made was to write the bus
 * LABEL in one place. The label was never the whole problem.
 *
 * When 0.8.6 came to add USB mass storage -- the fifth bus -- a grep for
 * "every place that chooses one", which is this tree's own rule, found
 * THREE sector-dispatch switches, and they did not agree:
 *
 *   shell/iso_installer.c  disk_read/disk_write    VirtIO ATA AHCI NVMe
 *   fs/fat16.c             fat16_raw_read/write    VirtIO ATA AHCI NVMe
 *   fs/fat32.c             local_read/local_write  VirtIO ATA
 *
 * So fat32_init() -- the entry point for "a FAT32 partition on the disk
 * the machine already knows how to read" -- could not see an NVMe or an
 * AHCI disk AT ALL. That is the 0.8.3.1 bug, still present, in the third
 * copy, two releases after it was declared fixed. Four releases of
 * being careful did not work; adding a fifth bus to three switches would
 * have been the same bet a fourth time.
 *
 * Worse, the two SCAN orders disagreed as well:
 *
 *   the installer   VirtIO, NVMe, AHCI, ATA
 *   fat16_init()    VirtIO, ATA, AHCI, NVMe
 *
 * On a machine where both NVMe and the legacy ATA ports answer, the
 * installer calls the NVMe disk 0 and the filesystem binds itself to the
 * ATA one. Nothing on screen could tell you.
 *
 * ── WHAT IS ONE THING AND WHAT IS TWO ────────────────────────────────
 *
 * Those two orders are not a duplicate to be collapsed. They answer
 * different questions and both answers are needed:
 *
 *   REGISTRATION ORDER is identity. blk_index() is what appears on the
 *   installer's picker, what fat32_vol_t::disk stores, and what a person
 *   reads off a screen before typing YES. Changing it renumbers disks.
 *
 *   BOOT PREFERENCE is "which of these disks is this machine's own",
 *   which fat16_init() asks and the installer never does.
 *
 * They live in two functions here, each saying why its order is what it
 * is, rather than in two files that did not know about each other.
 *
 * ── WHY USB REGISTERS LAST, AND WHY THAT IS A SAFETY PROPERTY ────────
 *
 * A removable disk that registers before the fixed ones RENUMBERS THEM.
 * The installer's confirmation screen names a disk by index; so does the
 * dry-run plan cached by plan_disks(). If plugging a stick in between the
 * picker and the confirmation could shift "disk 1" from the spare SATA
 * drive onto somebody's Windows NVMe, the four-sectors guarantee this
 * release ships on would be worth nothing.
 *
 * So blk_scan() registers every fixed bus before any removable one, and
 * tests/test_blkdev.c checks that a stick arriving does not move a fixed
 * disk's index. It is a one-line property and it protects the only thing
 * in this system that cannot be undone.
 *
 * ── WHERE THE LINE IS ────────────────────────────────────────────────
 *
 * This layer knows: there are disks, each is on a bus, each has a sector
 * count, some can be written and some cannot, and each bus has a name.
 * It does NOT know what a partition is. Partition tables, GPT entries and
 * install planning stay above it, in shell/iso_installer.c and fs/gpt.c,
 * which are this layer's callers like everything else.
 *
 * It holds no buffers and no cache. A read is a read.
 */

/* ── bus kinds ────────────────────────────────────────────────────────
 * The numbering is new; nothing persists it. Do not write it to a disk.
 * BLK_BUS_NONE is 0 so a zeroed table entry is not a bus. */
#define BLK_BUS_NONE    0
#define BLK_BUS_VIRTIO  1
#define BLK_BUS_NVME    2
#define BLK_BUS_AHCI    3
#define BLK_BUS_ATA     4
#define BLK_BUS_USB     5

#define BLK_MAX_DEV    16   /* was MAX_DISKS 8; a hub of sticks fits now */

/* ── a back-end ───────────────────────────────────────────────────────
 *
 * One of these per bus, not per disk. `unit` is the driver's own index
 * into whatever it enumerated -- an NVMe namespace, an AHCI port, a USB
 * mass-storage device -- and this layer never interprets it.
 *
 * `name` is the bus word shown to a person, and it lives HERE because
 * that makes it one string per bus in one table per bus, which is the
 * form tools/check_bus_label.py can actually guard. A back-end with no
 * write function is a read-only bus, which is different from a read-only
 * DISK on a writable bus -- see readonly() below.
 */
typedef struct {
    const char *name;        /* "NVMe", "SATA", ... shown to a person     */

    int (*read )(int unit, uint64_t lba, uint8_t *buf);
    int (*write)(int unit, uint64_t lba, const uint8_t *buf);  /* or 0    */

    /* Per-disk refusal, for a bus where some disks can be written and
       some cannot: an NVMe namespace with no write path for its block
       size, an AHCI port holding an optical drive, a USB stick with its
       write-protect tab across. Return 1 to refuse. May be 0, meaning
       "every disk on this bus is writable if write() exists".

       This is separate from write()==0 because the two were conflated
       once already: the installer's disk_is_readonly() switched on the
       bus type to ask a per-disk question, so every new bus had to be
       added to it or silently became writable. */
    int (*readonly)(int unit);

    /* Removable. Decides registration ORDER, not permission: see the
       note above on why a stick must never renumber a fixed disk. */
    int removable;
} blk_ops_t;

typedef struct {
    const blk_ops_t *ops;
    uint8_t  kind;           /* BLK_BUS_*                                 */
    int      unit;           /* the back-end's own index                  */
    uint64_t sectors;
    uint8_t  pci_bus, pci_dev;   /* for a display line; 0 if not on PCI   */
} blk_dev_t;

/* ── the registry ─────────────────────────────────────────────────────  */

/* Probe every bus and rebuild the table. Returns the number of disks.
 * Calls each back-end's own init, in the order registration order is
 * decided -- which is in this function and nowhere else.
 *
 * Safe to call again: it clears the table first, so a `disk rescan` after
 * plugging a stick in does not leave the old entries behind. */
int blk_scan(void);

/* Register one disk. Called by the back-ends' probes from inside
 * blk_scan(), and by usb/msc.c when a device arrives after the scan.
 * Returns the new index, or -1 if the table is full.
 *
 * A back-end that calls this with a removable ops table AFTER a fixed
 * disk has registered is fine -- that is the normal order. One that does
 * it the other way round is the renumbering bug, and blk_scan() is
 * written so it cannot. */
int blk_register(const blk_ops_t *ops, uint8_t kind, int unit,
                 uint64_t sectors, uint8_t pci_bus, uint8_t pci_dev);

/* Forget every disk. For a rescan, and for a test that wants a known
 * table. Does not touch the hardware. */
void blk_reset(void);

int               blk_count(void);
const blk_dev_t  *blk_dev(int i);          /* 0 if out of range           */
uint64_t          blk_sectors(int i);
uint8_t           blk_kind(int i);
int               blk_unit(int i);
uint8_t           blk_pci_bus(int i);
uint8_t           blk_pci_dev(int i);

/* 1 on success, 0 on failure -- the convention every caller of the three
 * old switches already used, so a value that reaches a caller means what
 * it used to mean. */
int blk_read (int i, uint64_t lba, uint8_t *buf);

/* Every write in the system funnels through here, so the read-only
 * refusal is BELOW every caller: a future code path cannot reach a disk
 * with no write function by forgetting to ask. Returns 0 for a refused
 * disk, which is the same answer as a failed write, deliberately -- a
 * caller that cannot tell them apart must not proceed either way. */
int blk_write(int i, uint64_t lba, const uint8_t *buf);

int blk_readonly(int i);
int blk_removable(int i);

/* The bus this disk is on, as a word to show a person: "IDE", "SATA",
 * "NVMe", "VirtIO" or "USB". THE ONLY PLACE IN THIS TREE that produces
 * one -- tools/check_bus_label.py fails the build if any other file
 * writes one as a literal.
 *
 * Out of range returns "none" rather than inventing a bus for a disk
 * that is not there, which is what a welcome screen counting two
 * booleans used to do. */
const char *blk_bus_name(int i);

/* How many disks are on each bus, for a summary line that has to say
 * what was found without listing it: a screen that tests two booleans
 * cannot describe a machine with only NVMe. Pass 0 for any you do not
 * need. */
void blk_bus_counts(int *ide, int *sata, int *nvme, int *virtio, int *usb);

/* The first disk of a kind, or -1. For a driver that enumerated
 * something and needs the index this layer gave it. */
int blk_find(uint8_t kind, int unit);

/* ── "which disk is this machine's own" ───────────────────────────────
 *
 * fat16_init() and fat32_init() both ask this and both used to answer it
 * themselves, with different orders. It is one question, so it has one
 * answer, and the order is argued for where it is written.
 *
 * Returns an index into this table, or -1 if there is no disk at all. */
int blk_boot_dev(void);

/* ── for the suites ───────────────────────────────────────────────────
 * tests/test_blkdev.c links this file and registers simulated disks
 * through blk_register() with its own ops tables, so the table, the
 * dispatch, the refusals and the bus words under test are the real ones
 * and not a copy. tests/test_diskname.c does the same -- it used to
 * carry a copy of bus_name() kept in step by a build check, which is
 * what you need when the function you want is walled in behind VGA and
 * keyboard code. It is not any more. */
