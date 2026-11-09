#!/usr/bin/env python3
"""check_blk_suite.py -- prove tests/test_blkdev.c can fail.

fs/blkdev.c is 300 lines over an array and some function pointers, and
tests/test_blkdev.c passed 70 of 70 on its first run. By this tree's own
rule that is the least trustworthy result available -- and this layer is
below every write in the system, so a suite checking its own simulated
back-ends instead of the dispatch would be a green light over the code
that decides which disk a sector lands on.

Most of the defects below are not hypothetical. Each one is a mistake the
three switches this file replaced actually made, or one the shape of the
code invites:

  - "USB registers before the fixed disks" renumbers somebody's Windows
    NVMe between the installer's picker and the screen that asks them to
    type YES. That is the single property this layer exists to hold.
  - "the ATAPI skip is gone" is v0.8.5.3's: fs/fat16.c bound itself to
    q35's DVD-ROM, found no HavenDOS partition and sent an installed
    system back into its own installer.
  - "AHCI after legacy ATA" is the order the two old scans disagreed
    about, which is how the installer called a disk 0 that the filesystem
    did not.
  - "no read-only check in blk_write" removes the refusal that sits
    BELOW every caller, which is the whole reason it is there.
  - "the LBA48 choice is inverted" is the top-8-bit truncation that
    destroyed an MBR past 8 GiB in this project's own history.
  - "the bound is >" is an overrun of a 16-entry table.

Run:  python3 tools/check_blk_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "fs/blkdev.c"
TEST = "tests/test_blkdev.c"
CC = ["gcc", "-std=gnu99", "-Iinclude",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("USB registers before the fixed disks",
     "a stick renumbers every fixed disk, so the index on the "
     "confirmation screen and in the cached dry-run plan stops being the "
     "disk about to be written. The four-sectors guarantee is worth "
     "nothing if 'disk 1' can move onto somebody's Windows NVMe",
     """    /* 1 -- VirtIO */""",
     """    for(int i = 0; i < msc_count(); i++)
        blk_register(&blk_usb_ops, BLK_BUS_USB, i, msc_sectors(i), 0, 0);

    /* 1 -- VirtIO */"""),

    ("the ATAPI skip is gone",
     "v0.8.5.3: an optical drive is registered as a disk, fs/fat16.c "
     "binds itself to the DVD-ROM on q35's default controller, finds no "
     "HavenDOS partition and runs the installer on a machine that was "
     "installed to minutes earlier",
     """        if(ahci_is_atapi(i)) continue;
""",
     """"""),

    ("AHCI is registered after legacy ATA",
     "the order the installer's scan and fat16_init()'s probe disagreed "
     "about. On a machine where both answer, the legacy ports are a "
     "compatibility shim reaching at most two of the controller's ports",
     """    ahci_init();
    for(int i = 0; i < ahci_count(); i++){
        if(ahci_is_atapi(i)) continue;
        blk_register(&ahci_ops, BLK_BUS_AHCI, i, ahci_sectors(i), 0, 0);
    }

    /* 4 -- legacy ATA / SATA in compatibility mode */
    ata_init();
    if(ata_detected())
        blk_register(&ata_ops, BLK_BUS_ATA, 0, (uint64_t)ata_sectors(), 0, 0);
""",
     """    ata_init();
    if(ata_detected())
        blk_register(&ata_ops, BLK_BUS_ATA, 0, (uint64_t)ata_sectors(), 0, 0);

    ahci_init();
    for(int i = 0; i < ahci_count(); i++){
        if(ahci_is_atapi(i)) continue;
        blk_register(&ahci_ops, BLK_BUS_AHCI, i, ahci_sectors(i), 0, 0);
    }
"""),

    ("blk_write does not ask whether the disk is read-only",
     "the refusal that sits BELOW every caller and every screen is gone, "
     "so a future code path reaches a disk with no write function by "
     "forgetting to ask -- which is exactly how the installer's "
     "disk_is_readonly() had to be remembered for each new bus",
     """    if(blk_readonly(i))              return 0;
""",
     """"""),

    ("a bus with no write function is called writable",
     "the half of the read-only question that makes a NEW read-only bus "
     "safe by default rather than by diligence",
     """    if(!g_dev[i].ops->write) return 1;""",
     """    if(!g_dev[i].ops->write) return 0;"""),

    ("the per-disk refusal is ignored",
     "a writable bus can hold a disk that is not: an NVMe namespace with "
     "no write path for its block size, an AHCI port holding an optical "
     "drive, a stick with its write-protect tab across",
     """    if(g_dev[i].ops->readonly) return g_dev[i].ops->readonly(g_dev[i].unit) ? 1 : 0;
""",
     """"""),

    ("an out-of-range index is writable rather than read-only",
     "the safe default inverted. blk_write() asks blk_readonly() first, "
     "so this turns a bad index into a write through a zeroed ops table",
     """    if(i < 0 || i >= g_ndev) return 1;
    if(!g_dev[i].ops->write) return 1;""",
     """    if(i < 0 || i >= g_ndev) return 0;
    if(!g_dev[i].ops->write) return 1;"""),

    ("the LBA48 choice is inverted",
     "the top-8-bit truncation this project has already shipped: writes "
     "past 8 GiB wrapped onto sector 0 and 1 and destroyed the MBR after "
     "the first boot",
     """static int ata_rd(int unit, uint64_t lba, uint8_t *buf){
    (void)unit;
    if(ata_lba48()) return ata_read_sector48(lba, buf);
    return ata_read_sector((uint32_t)lba, buf);
}""",
     """static int ata_rd(int unit, uint64_t lba, uint8_t *buf){
    (void)unit;
    if(!ata_lba48()) return ata_read_sector48(lba, buf);
    return ata_read_sector((uint32_t)lba, buf);
}"""),

    ("the table bound is off by one",
     "a 16-entry table written at index 16: the next object in the file "
     "is whatever the linker put there",
     """    if(g_ndev >= BLK_MAX_DEV)     return -1;""",
     """    if(g_ndev > BLK_MAX_DEV)      return -1;"""),

    ("a disk with no read function is accepted",
     "an ops table missing its read is a disk every later call "
     "dereferences a null pointer through",
     """    if(!ops || !ops->read)        return -1;   /* a disk you cannot read
                                                  is not a disk */""",
     """    if(!ops)                      return -1;"""),

    ("blk_scan appends instead of replacing",
     "`disk rescan` after plugging a stick in doubles every disk, and "
     "the second copy of somebody's Windows disk has a different index "
     "from the one the picker showed",
     """int blk_scan(void){
    blk_reset();""",
     """int blk_scan(void){"""),

    ("the boot preference puts USB first",
     "a stick that happened to be plugged in becomes the volume an "
     "installed system mounts as its own, so the machine boots from its "
     "own disk and then reads somebody else's",
     """        BLK_BUS_VIRTIO, BLK_BUS_ATA, BLK_BUS_AHCI, BLK_BUS_NVME, BLK_BUS_USB""",
     """        BLK_BUS_USB, BLK_BUS_VIRTIO, BLK_BUS_ATA, BLK_BUS_AHCI, BLK_BUS_NVME"""),

    ("the boot preference drops NVMe",
     "v0.8.5.3's bug in the new layer: an installed system on an NVMe "
     "drive -- every laptop made this decade -- loads its kernel through "
     "GRUB and then cannot find the partition that kernel came from",
     """        BLK_BUS_VIRTIO, BLK_BUS_ATA, BLK_BUS_AHCI, BLK_BUS_NVME, BLK_BUS_USB""",
     """        BLK_BUS_VIRTIO, BLK_BUS_ATA, BLK_BUS_AHCI, BLK_BUS_USB"""),

    ("an empty table reports disk 0 as the boot disk",
     "-1 means there is no disk; 0 means there is one, and every caller "
     "goes on to read it",
     """    return -1;
}

/* ════════════════════════════════════════════════════════════════════
   The back-ends""",
     """    return 0;
}

/* ════════════════════════════════════════════════════════════════════
   The back-ends"""),

    ("nvme's 0-means-ok is passed through unchanged",
     "drivers/nvme.c returns 0 for success and this layer returns 1. "
     "Inverting it makes every NVMe read report failure -- or, worse, "
     "every failure report success",
     """static int nvme_rd1(int unit, uint64_t lba, uint8_t *buf){
    return nvme_read(unit, lba, 1, buf) == 0;
}""",
     """static int nvme_rd1(int unit, uint64_t lba, uint8_t *buf){
    return nvme_read(unit, lba, 1, buf);
}"""),

    ("the unit handed to a driver is the table index",
     "the driver's own index and this layer's are different numbers: an "
     "AHCI controller whose port 0 is a DVD has its disk at unit 1 and "
     "table index 0. This is the renumbering that made the picker and "
     "the confirmation screen name different disks",
     """    return g_dev[i].ops->read(g_dev[i].unit, lba, buf) ? 1 : 0;""",
     """    return g_dev[i].ops->read(i, lba, buf) ? 1 : 0;"""),

    ("the bus word comes from the table index rather than the bus",
     "every disk reports the first bus's name, which is the v0.8.3.1 bug "
     "verbatim: an NVMe drive announced as VirtIO on the screen whose "
     "whole job is to say what is about to be written",
     """    return g_dev[i].ops->name;""",
     """    return g_dev[0].ops->name;"""),

    ("an out-of-range index invents a bus",
     "a welcome screen counting two booleans printed 'No disks found' "
     "directly above '1 VirtIO disk found'. 'none' is what stops that",
     """    if(i < 0 || i >= g_ndev) return "none";
    return g_dev[i].ops->name;""",
     """    if(i < 0 || i >= g_ndev) return "VirtIO";
    return g_dev[i].ops->name;"""),

    ("a null buffer is passed to the driver",
     "the bounds check at the top of a read is the only thing between a "
     "caller's mistake and a driver dereferencing zero",
     """    if(i < 0 || i >= g_ndev || !buf) return 0;
    return g_dev[i].ops->read(g_dev[i].unit, lba, buf) ? 1 : 0;""",
     """    if(i < 0 || i >= g_ndev) return 0;
    return g_dev[i].ops->read(g_dev[i].unit, lba, buf) ? 1 : 0;"""),

    ("the USB ops table is not marked removable",
     "removable is what decides registration order, and a stick that is "
     "not marked as one has nothing keeping it behind the fixed disks",
     """const blk_ops_t blk_usb_ops = {
    "USB", msc_read_sector, msc_write_sector, msc_readonly, 1
};""",
     """const blk_ops_t blk_usb_ops = {
    "USB", msc_read_sector, msc_write_sector, msc_readonly, 0
};"""),
]

# Must NOT be caught. A comment, and a local variable renamed: no
# behaviour changes, so a suite that reports this as a failure is
# asserting something that is not behaviour -- or a damaging injection
# before it was not fully restored.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed, so a catch here means the loop is "
    "restoring badly or the suite is checking text",
    """int               blk_count(void)        { return g_ndev; }""",
    """/* how many disks the last scan registered */
int               blk_count(void)        { return g_ndev; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "blk", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        preamble="Proving tests/test_blkdev.c can fail.\n"
                 "fs/blkdev.c sits below every write in this system, and "
                 "the suite passed 70 of 70 first time."))
