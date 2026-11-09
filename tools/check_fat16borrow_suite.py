#!/usr/bin/env python3
"""check_fat16borrow_suite.py -- prove tests/test_fat16borrow.c can fail.

v0.8.6.3 gave fs/fat16.c a way to WRITE to a volume it does not own, so
that `fat16 cp` can put a file on a USB stick. Everything that keeps
that safe is a refusal in one function, and the one ordering constraint
is in another.

── WHY THIS HARNESS IS NOT OPTIONAL ──────────────────────────────────

tests/test_fat16borrow.c passed 53/53 the first time it ran, which by
this tree's own rule is the least trustworthy result available. Every
suite in v0.8.6 that passed first time turned out to have holes, and
they were all the same hole: checking the OUTCOME instead of the wire.

The defects below are the ones that matter, and most of them do not
look like bugs:

  * A flush on the wrong side of the device switch in fat16_return()
    writes the STICK's allocation table over the system volume's. The
    copy still succeeds. Nothing warns. The machine boots until the
    next time something walks that FAT.
  * A missing blk_removable() check turns `fat16 cp` into a write to
    whatever disk the volume list happens to number -- which on the
    laptop this project is aimed at is the disk holding Windows.
  * A g_borrow_rw that is set by the READ-ONLY borrow, or not cleared
    on return, makes every later borrow writable. The read-only path
    keeps working, so nothing on screen changes.

None of those produce an error message, and none of them fail a boot.
A suite is the only thing that can see them, which is why it has to be
shown capable of failing.

Run:  python3 tools/check_fat16borrow_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC   = "fs/fat16.c"
TEST  = "tests/test_fat16borrow.c"
# fs/gpt.c is linked rather than stubbed: fs/fat16.c asks it whether a
# disk is GPT before falling back to the MBR, and a stub answering "no"
# would be a different program from the one that ships.
EXTRA = ["fs/gpt.c"]
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "-Wno-implicit-function-declaration"]

BUGS = [
    # ── the ordering. This is the one. ───────────────────────────────
    ("the borrowed FAT is flushed AFTER the device switches back",
     "the stick's allocation table is written over the system volume's. "
     "The copy reports success, nothing warns, and an installed "
     "HavenDOS is destroyed in a way that only shows up at the next "
     "boot that walks that FAT",
     """    if(g_borrow_rw && fat_dirty){
        if(!fat_flush())
            klog(KLOG_WARN, "FAT16: the borrowed volume's FAT would not "
                            "write back -- files copied to it may be "
                            "incomplete");
    }
    fat_dirty   = 0;

    g_borrowed  = 0;
    g_borrow_rw = 0;
    g_fat16_dev = g_saved_dev;
    g_part_lba  = g_saved_lba;""",
     """    g_borrowed  = 0;
    g_fat16_dev = g_saved_dev;
    g_part_lba  = g_saved_lba;
    if(g_borrow_rw && fat_dirty){
        if(!fat_flush())
            klog(KLOG_WARN, "FAT16: the borrowed volume's FAT would not "
                            "write back -- files copied to it may be "
                            "incomplete");
    }
    fat_dirty   = 0;
    g_borrow_rw = 0;"""),

    # ── the refusals ─────────────────────────────────────────────────
    ("a disk that is not removable is no longer refused",
     "`fat16 cp 0:/X` becomes a write to the machine's own disk, "
     "through a command whose help text says it copies to a stick",
     """    if(!blk_removable(dev)){
        klog(KLOG_WARN, "FAT16: refusing to write to %s disk %d -- a "
                        "writable borrow is for removable media only",
             blk_bus_name(dev), dev);
        return 0;
    }""",
     """    (void)0;"""),

    ("the boot disk is no longer refused",
     "the system's own volume is reached through the VFS, which knows "
     "about the mounted filesystem; a borrow does not. Two refusals "
     "exist here rather than one on purpose",
     """    if(dev == blk_boot_dev()){
        klog(KLOG_WARN, "FAT16: disk %d is this system's own boot disk "
                        "-- not borrowed for writing", dev);
        return 0;
    }""",
     """    (void)0;"""),

    ("a read-only disk is no longer refused",
     "a stick with its write-protect switch on, or a disk whose bus "
     "has no write function, is borrowed for writing and every write "
     "fails one layer further down than it should",
     """    if(blk_readonly(dev)){
        klog(KLOG_WARN, "FAT16: %s disk %d is read-only",
             blk_bus_name(dev), dev);
        return 0;
    }""",
     """    (void)0;"""),

    ("a second borrow is permitted while one is held",
     "g_saved_dev is overwritten by the second borrow, so the system's "
     "own volume is never remounted and every later vfs_read() reads "
     "the stick. BOTH guards go: fat16_borrow_rw() and borrow_core() "
     "each have one, so removing either alone changes nothing and no "
     "suite can see it. This harness reported exactly that, and the "
     "answer is to inject the defect that is real",
     """static int borrow_core(int dev, uint64_t part_lba, int writable)
{
    if(g_borrowed) return 0;            /* one at a time */""",
     """static int borrow_core(int dev, uint64_t part_lba, int writable)
{"""),

    # ── the flag itself ──────────────────────────────────────────────
    ("the READ-ONLY borrow is writable too",
     "the whole point of two entry points is that one of them cannot "
     "write. `fat16 cat` on somebody's stick becomes able to modify it",
     """int fat16_borrow(int dev, uint64_t part_lba)
{
    return borrow_core(dev, part_lba, 0);
}""",
     """int fat16_borrow(int dev, uint64_t part_lba)
{
    return borrow_core(dev, part_lba, 1);
}"""),

    ("a borrow no longer refuses writes at all",
     "v0.8.6's behaviour before the flag existed: fat16_raw_write() "
     "reaches blk_write() during a read-only borrow",
     """    if(g_borrowed && !g_borrow_rw) return 0;
    return blk_write(g_fat16_dev, lba, buf);""",
     """    return blk_write(g_fat16_dev, lba, buf);"""),

    # ── NOT INJECTED, and the reasons are worth keeping ──────────────
    #
    # `g_borrow_rw = 0` in fat16_return() was injected out and the
    # suite did not notice -- correctly, again. Every reader of
    # g_borrow_rw is gated on g_borrowed first:
    #
    #     fat16_raw_write()    if(g_borrowed && !g_borrow_rw)
    #     fat16_borrowed_rw()  return g_borrowed && g_borrow_rw;
    #     fat16_return()       returns early unless g_borrowed
    #
    # and the only place that sets g_borrowed is borrow_core(), which
    # sets g_borrow_rw in the same breath. So a stale flag can never be
    # read: the next borrow overwrites it before anything looks. The
    # clear stays in the source as hygiene and is not injected here,
    # because an injection that cannot express a reachable defect
    # reports a catch the suite did not earn -- which is what
    # check_xhci_suite.py's first loop did eleven times over.
    #
    #
    # `fat_loaded = 0` in borrow_core() was injected out in the first
    # run of this harness and the suite did not notice -- correctly.
    # mount_at_part_lba() sets fat_loaded = 0 and reloads the FAT
    # itself, so that line is belt-and-braces for the window where the
    # mount fails early, and NO suite can distinguish its absence.
    #
    # It is kept in the source and not injected here. The alternative
    # -- deleting working safety code so a harness reports a clean run
    # -- is the harness grading the code instead of the other way
    # round.

    ("the saved device is never restored",
     "the driver stays pointed at the stick after fat16_return(), and "
     "the system volume is gone until a reboot",
     """    g_fat16_dev = g_saved_dev;
    g_part_lba  = g_saved_lba;
    fat16_ready = 0;""",
     """    fat16_ready = 0;"""),
]

HARMLESS = (
    "a comment is added to fs/fat16.c (must not be caught)",
    "nothing about the code changed, so a catch here means the "
    "restore is broken or the suite is matching text rather than "
    "behaviour",
    """int fat16_borrowed(void){ return g_borrowed; }""",
    """/* Is a borrow outstanding, of either kind? */
int fat16_borrowed(void){ return g_borrowed; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "f16borrow", SRC, TEST, CC + EXTRA, BUGS, HARMLESS, timeout=90,
        preamble="The writable borrow in fs/fat16.c. Half of these "
                 "defects leave `fat16 cp` working perfectly and "
                 "destroy the system volume instead."))
