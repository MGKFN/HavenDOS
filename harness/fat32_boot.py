#!/usr/bin/env python3
"""fat32_boot.py -- boot the ISO and prove the FAT32 driver works in the kernel.

`make test` compiles fs/fat32.c natively and runs it against an image made
by mkfs.vfat.  That proves the filesystem logic.  It does NOT prove that the
driver works when it is reached through the real block layer, on a real
boot, with the real disk scan in front of it -- and the three-installers bug
is what happens when nobody checks that seam: a disk layer that worked
perfectly, called with an index from a different enumeration.

So this boots the ISO in Shell Mode with a FAT32 disk attached and runs the
`fat32` command, which is the whole stack end to end: PCI, ATA, the unified
disk scan, the partition walk, fat_probe_type(), the mount, the directory
iterator and the LFN reassembly.

Usage:
    python3 harness/fat32_boot.py [iso] [disk-image]

Defaults to havendos.iso and tests/fat32.img, which `make` and `make test`
produce.  Read-only throughout: the command has no write path, and the ISO
boots live, which mounts nothing.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu as Q

ISO = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
DISK = sys.argv[2] if len(sys.argv) > 2 else "tests/fat32.img"

# Shell Mode is the second entry in isodir/boot/grub/grub.cfg.  The menu
# cannot be read back -- GRUB draws it with its own font, not the kernel's
# 8x8 one, so the framebuffer decoder returns noise -- so it is driven blind
# by counting menuentry lines, exactly as the handoff says to.
SHELL_MODE_INDEX = 1

fails = []


def check(what, ok, detail=""):
    print("  %-58s %s" % (what, "ok" if ok else "FAIL"))
    if not ok:
        fails.append((what, detail))


def main():
    for p in (ISO, DISK):
        if not os.path.exists(p):
            sys.exit("missing %s -- run `make` and `make test` first" % p)

    Q.kill_stale()
    print("Booting %s with %s attached on IDE\n" % (ISO, DISK))

    with Q.Qemu(iso=ISO, disk=DISK, boot="d", mem=256) as q:
        time.sleep(3)
        for _ in range(SHELL_MODE_INDEX):
            q.key("down")
            time.sleep(0.3)
        q.key("ret")

        s = q.wait_for("HavenDOS", timeout=120)
        check("the kernel boots and draws", s is not None, s or "")
        if s is None:
            return 1

        # A live boot must mount nothing -- adding a FAT32 driver is exactly
        # what makes this worth asserting, because an ESP is FAT32 and sits
        # first on any UEFI disk.
        q.line("disk")
        time.sleep(2)
        s = q.screen()
        check("live media reports no mounted partition",
              "RAMFS" in s or "live" in s.lower(), s)

        q.line("fat32")
        # wait_for returns the moment the heading appears, which is before
        # the table under it has been drawn.  Re-read once it has settled --
        # asserting against the first frame that matched is how a passing
        # check turns into a flaky one.
        q.wait_for("FAT32 volumes", timeout=60)
        time.sleep(2)
        s = q.screen()
        print("\n----- fat32 -----")
        print(s.rstrip())
        print("-----------------\n")
        check("the FAT32 partition on the attached disk is found",
              "mbr type 0x0c" in s.lower(), s)
        check("and its BPB label is read off the volume",
              "HAVENFIX" in s, s)
        check("no volume was reported that is not FAT32",
              "none found" not in s, s)

        q.line("fat32 ls 0")
        time.sleep(3)
        s = q.screen()
        print("----- fat32 ls 0 -----")
        print(s.rstrip())
        print("----------------------\n")
        # The three that matter: a long name reassembled from LFN slots, a
        # dotfile that is NOT a blank basename, and a subdirectory.
        check("a long file name is reassembled",
              "A Long File Name.txt" in s, s)
        check("a dotfile keeps its leading dot and full name",
              ".config" in s, s)
        check("a subdirectory is listed as one", "EFI" in s, s)

        q.line("fat32 ls 0:/EFI/BOOT")
        time.sleep(3)
        s = q.screen()
        print("----- fat32 ls 0:/EFI/BOOT -----")
        print(s.rstrip())
        print("--------------------------------\n")
        check("a nested directory walks", "BOOTX64.EFI" in s.upper(), s)

        q.line("fat32 cat 0:/HELLO.TXT")
        time.sleep(3)
        s = q.screen()
        check("a file reads back through the kernel",
              "Hello from mkfs.vfat" in s, s)

        # And the disk is untouched by all of that.
    import hashlib
    h = hashlib.md5(open(DISK, "rb").read()).hexdigest()
    print()
    return h


if __name__ == "__main__":
    import hashlib
    before = hashlib.md5(open(DISK, "rb").read()).hexdigest() \
        if os.path.exists(DISK) else None
    rc = main()
    after = hashlib.md5(open(DISK, "rb").read()).hexdigest()
    check("the attached disk is byte-identical afterwards",
          before == after, "%s -> %s" % (before, after))
    print()
    if fails:
        print("%d check(s) failed" % len(fails))
        for w, d in fails:
            print("\n  FAIL %s" % w)
            if d:
                print("\n".join("       " + l for l in str(d).splitlines()[:25]))
        sys.exit(1)
    print("all checks passed")
