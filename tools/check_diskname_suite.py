#!/usr/bin/env python3
"""check_diskname_suite.py -- prove tests/test_diskname.c can fail.

The bus label is four lines of switch and it has been wrong in a shipped
release four times. A suite over four lines looks unfalsifiable, which is
exactly the kind of suite that quietly tests nothing: every one of these
bugs is one character, and a suite that checks only "NVMe says NVMe"
catches none of the interesting ones.

── v0.8.6: ONE COPY, SO ONE INJECTION SITE ────────────────────────────

Until this release the injections went into BOTH copies at once -- the
real function in shell/iso_installer.c and the hand-kept copy inside
tests/test_diskname.c -- because tools/check_diskname_sync.py held the
two identical and damaging only one would have been caught by that
instead, which proves nothing about the suite.

The copy existed because the function was walled in behind three
thousand lines of VGA drawing and keyboard waits. v0.8.6 moved the disk
table into fs/blkdev.c, which compiles on its own, and the suite links
it. So there is one place to damage, and a green run here is a statement
about the code that runs on the laptop.

The words themselves now live one per bus in that bus's ops table, which
is why the injections below change a table entry rather than a switch
arm. Each one is still a bug that has shipped, or is one character from
one that did.

Run:  python3 tools/check_diskname_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "fs/blkdev.c"
TEST = "tests/test_diskname.c"
CC = ["gcc", "-std=gnu99", "-Iinclude",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("AHCI is named VirtIO",
     "the v0.8.5.3 bug exactly: a SATA disk announced as VirtIO on the "
     "screen that asks for YES",
     '''    "SATA", ahci_read_sector, ahci_write_sector, ahci_ro, 0''',
     '''    "VirtIO", ahci_read_sector, ahci_write_sector, ahci_ro, 0'''),

    ("NVMe is named VirtIO",
     "the v0.8.3.1 bug: the picker said NVMe and the confirmation said "
     "VirtIO",
     '''    "NVMe", nvme_rd1, nvme_wr1, nvme_ro, 0''',
     '''    "VirtIO", nvme_rd1, nvme_wr1, nvme_ro, 0'''),

    ("legacy ATA is named SATA",
     "an IDE disk reported as SATA -- different controller, different "
     "write path, same screen",
     '''    "IDE", ata_rd, ata_wr, 0, 0''',
     '''    "SATA", ata_rd, ata_wr, 0, 0'''),

    ("a USB stick is named VirtIO",
     "the fifth bus, one release after the fourth one shipped under the "
     "wrong name twice",
     '''    "USB", msc_read_sector, msc_write_sector, msc_readonly, 1''',
     '''    "VirtIO", msc_read_sector, msc_write_sector, msc_readonly, 1'''),

    ("AHCI and NVMe are swapped",
     "both names exist, both are wrong, and every individual name still "
     "appears somewhere on screen -- which is what a suite checking "
     "'some disk says NVMe' would pass",
     '''static const blk_ops_t nvme_ops = {
    "NVMe", nvme_rd1, nvme_wr1, nvme_ro, 0
};''',
     '''static const blk_ops_t nvme_ops = {
    "SATA", nvme_rd1, nvme_wr1, nvme_ro, 0
};'''),

    ("a disk index past the end falls through to the first disk's bus",
     "this is how a welcome screen printed '1 VirtIO disk found' for a "
     "machine whose disk was NVMe -- it named a disk that was not there",
     '''    if(i < 0 || i >= g_ndev) return "none";''',
     '''    if(i < 0 || i >= g_ndev) i = 0;'''),

    ("a negative index reads in front of the array",
     "g_dev[-1] is whatever the linker put there, read as an ops "
     "pointer and dereferenced",
     '''    if(i < 0 || i >= g_ndev) return "none";
    return g_dev[i].ops->name;''',
     '''    if(i >= g_ndev) return "none";
    return g_dev[i].ops->name;'''),

    ("the counts are taken from the table index, not the bus",
     "every disk counts as the first one's bus, so a machine with an "
     "NVMe disk and a stick reports two NVMe disks",
     '''        switch(g_dev[i].kind){''',
     '''        switch(g_dev[0].kind){'''),

    ("the USB count is folded into VirtIO",
     "a stick counted as a VirtIO disk. Both numbers are then wrong and "
     "the total is still right, which is what makes a summary line look "
     "healthy",
     '''            case BLK_BUS_USB:    u++; break;''',
     '''            case BLK_BUS_USB:    v++; break;'''),

    ("a count the caller did not ask for is written anyway",
     "a null pointer written through. Every caller passes 0 for the "
     "counts it does not need, which is most of them",
     '''    if(usb)    *usb    = u;''',
     '''    *usb = u;'''),

    ("the counts are never zeroed, so they accumulate",
     "a second call doubles every number, and the installer's welcome "
     "screen calls this on every redraw",
     '''    int a=0,s=0,n=0,v=0,u=0;''',
     '''    static int a,s,n,v,u;'''),

    ("an unknown bus kind counts as VirtIO",
     "the default: arm that answered VirtIO for a disk nobody had "
     "taught it about, which is the shape of all four shipped bugs",
     '''            default: break;''',
     '''            default: v++; break;'''),
]

HARMLESS = (
    "a comment is added (must not be caught)",
    "nothing about the code changed, so a catch here means the loop is "
    "restoring badly or the suite is checking text",
    '''const char *blk_bus_name(int i){''',
    '''/* the only place in this tree that turns a disk into one of those words */
const char *blk_bus_name(int i){''')

if __name__ == "__main__":
    sys.exit(inject.run(
        "diskname", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        # The suite states each bus's word a SECOND time, in its own
        # simulated ops tables, so its assertions are written against the
        # specification rather than against whatever fs/blkdev.c says
        # today. That is deliberate and it means the suite cannot notice
        # a typo in the kernel's table -- check_diskname_sync.py is what
        # notices that, by comparing both files against the table of
        # words it holds. The pair is: the suite covers the logic, the
        # check covers the text.
        checks=[["python3", "tools/check_diskname_sync.py"]],
        preamble="Proving tests/test_diskname.c can fail.\n"
                 "Four shipped releases named a disk for the wrong bus, "
                 "on screens that ask for YES."))
