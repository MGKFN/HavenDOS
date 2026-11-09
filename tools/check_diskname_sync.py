#!/usr/bin/env python3
"""check_diskname_sync.py -- the suite must link the real function, not a copy.

── WHAT THIS CHECKED BEFORE v0.8.6 ───────────────────────────────────

tests/test_diskname.c could not include shell/iso_installer.c: that file
is three thousand lines of VGA drawing, keyboard waits and disk I/O, and
the two functions worth testing sat behind all of it. So the suite
carried a copy, and this script compared the two byte for byte after
normalising whitespace.

The reason it had to exist is the tree's own rule: a test that holds its
own copy of the function it is testing proves nothing about the
function -- it tests the copy, passes for ever, and goes green while the
real one rots. Exactly the failure this project had twice with the bus
label itself.

── WHY IT CHECKS SOMETHING ELSE NOW ──────────────────────────────────

A build check guarding a copy is a lesser answer than not having a copy.

v0.8.6 moved the disk table out of shell/iso_installer.c into
fs/blkdev.c, which is pure logic over function pointers and compiles
natively. tests/test_diskname.c LINKS it. There is nothing left to drift,
so this script's job changes from comparing two bodies to making sure the
second body does not come back:

  1. shell/iso_installer.c must NOT define bus_name() or a disk table of
     its own again. The two "fix it in one place" releases both put the
     one place inside that file, where nothing else could reach it, and
     the third copy appearing there is the exact regression to catch.

  2. tests/test_diskname.c must not define bus_name()/blk_bus_name()
     either -- that would be the copy returning -- and it must include
     fs/blkdev.c, which is what makes the suite's result mean anything.

  3. The bus WORDS in the suite's simulated ops tables must match the
     words in fs/blkdev.c's real ones. The suite states them a second
     time on purpose, so its assertions are written against the
     specification rather than against whatever the kernel says today --
     but then the two have to be checked, or a typo in the kernel's table
     becomes a pair of files agreeing with each other, which is precisely
     the trap the old copy fell into.

tools/check_bus_label.py guards where the words may appear.
tools/check_blk_dispatch.py guards where a sector may be dispatched.
This guards the suite.

Run:  python3 tools/check_diskname_sync.py
"""
import os
import re
import sys

OWNER = "fs/blkdev.c"
OLD   = "shell/iso_installer.c"
TEST  = "tests/test_diskname.c"

# The bus words, and the BLK_BUS_* kind each belongs to. Both files have
# to agree with this table, which is the specification of what a bus is
# called -- stated here so neither file is the authority for the other.
WORDS = {
    "BLK_BUS_ATA":    "IDE",
    "BLK_BUS_AHCI":   "SATA",
    "BLK_BUS_NVME":   "NVMe",
    "BLK_BUS_VIRTIO": "VirtIO",
    "BLK_BUS_USB":    "USB",
}

# A bus name appears in an ops table as its first initialiser:
#     static const blk_ops_t nvme_ops = { "NVMe", ... };
OPS = re.compile(r'blk_ops_t\s+\w+\s*=\s*\{\s*"([^"]+)"')


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def defines(path, name):
    """Does this file DEFINE a function of that name (not just call it)?"""
    text = strip_comments(open(path, encoding="utf-8").read())
    return re.search(
        r"^[A-Za-z_][A-Za-z0-9_ \t\*]*\b%s\s*\([^;{]*\)\s*\{" % name,
        text, re.M) is not None


def main():
    if not os.path.isdir("shell"):
        sys.stderr.write("run me from the source root\n")
        return 2
    for p in (OWNER, TEST):
        if not os.path.exists(p):
            print("check_diskname_sync: %s is missing" % p)
            return 1

    bad = 0

    # 1 ── the copy must not come back to the installer
    for name in ("bus_name", "disk_is_readonly_impl"):
        if defines(OLD, name):
            print("check_diskname_sync: %s defines %s() again.\n" % (OLD, name))
            print("  That is where v0.8.3.1 and v0.8.5.4 both put the 'one")
            print("  place', and both times nothing else could reach it: in")
            print("  v0.8.5.3 shell/desktop.c had its own and announced a")
            print("  SATA disk as VirtIO on the screen gated behind typing")
            print("  YES. %s owns this now." % OWNER)
            bad = 1

    # 2 ── the suite must link the real thing
    test_text = open(TEST, encoding="utf-8").read()
    if "fs/blkdev.c" not in test_text:
        print("check_diskname_sync: %s does not include %s.\n" % (TEST, OWNER))
        print("  The suite has to compile the shipping function, or a green")
        print("  run says nothing about the code on the laptop.")
        bad = 1
    for name in ("bus_name", "blk_bus_name"):
        if defines(TEST, name):
            print("check_diskname_sync: %s defines %s() itself.\n"
                  % (TEST, name))
            print("  That is the copy this check used to police, and the")
            print("  reason it is gone is that a copy tests itself. Link")
            print("  %s instead." % OWNER)
            bad = 1

    # 3 ── the words in both ops tables must match the table above
    owner_words = set(OPS.findall(strip_comments(
        open(OWNER, encoding="utf-8").read())))
    test_words = set(OPS.findall(strip_comments(test_text)))
    want = set(WORDS.values())

    missing = want - owner_words
    if missing:
        print("check_diskname_sync: %s has no ops table naming %s\n"
              % (OWNER, ", ".join(sorted(missing))))
        print("  Every bus's word lives in that bus's own ops table. A bus")
        print("  with no word is a disk the installer calls 'none'.")
        bad = 1
    extra = owner_words - want
    if extra:
        print("check_diskname_sync: %s names a bus this check does not know "
              "about: %s\n" % (OWNER, ", ".join(sorted(extra))))
        print("  Add it to WORDS here and to %s's simulated buses, so the" % TEST)
        print("  suite covers it. A sixth bus that no suite knows about is")
        print("  how the fifth one nearly shipped untested.")
        bad = 1
    missing = want - test_words
    if missing:
        print("check_diskname_sync: %s does not simulate %s\n"
              % (TEST, ", ".join(sorted(missing))))
        print("  The suite needs one simulated bus per real bus, or a bus")
        print("  ships with its name checked by nothing.")
        bad = 1

    if bad:
        return 1

    print("check_diskname_sync: ok, %s links %s, no copies, %d bus names "
          "agree" % (TEST, OWNER, len(want)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
