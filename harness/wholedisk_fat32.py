#!/usr/bin/env python3
"""wholedisk_fat32.py -- the whole-disk installer, on a disk over 2 GB.

v0.9.3 "Gaia". Until this release the whole-disk installer made a 2047 MB
FAT16 partition on any disk, whatever its size. It now makes FAT32 above
2047 MB, up to 32 GB, and three things outside the C suites have to agree
for that to be a machine that boots:

  - GRUB's i386-pc core image, embedded at sector 1 by our installer, has
    to read a FAT32 volume and find /boot/grub on it;
  - the menu's `search --label HAVENDOS` has to find the FAT32 label;
  - and the installed kernel has to mount its own FAT32 volume and then
    NOT decide it is uninstalled -- kernel/main.c tested
    `!vfs_using_fat16()` for that until 0.9.3, which would have put every
    FAT32 install in front of the installer on its first boot.

So: a blank sparse disk, SeaBIOS, "Install to Disk", the YES prompt; then
sfdisk-free checks with fsck.fat and mtools; then boot the DISK with no
ISO, and wait for the first-boot wizard -- which only an installed system
runs -- and then the shell, and ask `disk` what it mounted.

Run:  python3 harness/wholedisk_fat32.py [iso] [--gb N]
      --gb 1 exercises the FAT16 path the same way.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu import Qemu, kill_stale                            # noqa: E402

ROOT = os.path.dirname(HERE)
args = sys.argv[1:]
GB = 6
if "--gb" in args:
    i = args.index("--gb"); GB = int(args[i + 1]); del args[i:i + 2]
ISO = args[0] if args else os.path.join(ROOT, "havendos.iso")
IMG = "/tmp/hd_wd32.img"
MENU_INSTALL = 3          # "Install to Disk", counted in isodir/boot/grub/grub.cfg

fails = []
total = [0]


def ck(what, ok, detail=""):
    total[0] += 1
    if ok:
        print(f"    ok   {what}")
    else:
        fails.append(what)
        print(f"    FAIL {what}" + (f"\n         {detail}" if detail else ""))


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def wait_any(q, needles, timeout):
    t0 = time.time(); s = ""
    while time.time() - t0 < timeout:
        s = q.screen()
        for n in needles:
            if n in s:
                return n, s
        time.sleep(1.0)
    return None, s


def main():
    disk_sectors = GB * 1024 * 1024 * 1024 // 512
    part = min(disk_sectors - 2048, 67108864) // 2048 * 2048
    want = 32 if part > 4192256 else 16
    print(f"Whole-disk install onto a blank {GB} GB disk: expecting a "
          f"{part // 2048} MB FAT{want} partition.\n")

    if os.path.exists(IMG):
        os.unlink(IMG)
    with open(IMG, "wb") as f:
        f.truncate(GB * 1024 * 1024 * 1024)

    kill_stale()
    print("  [1] the ISO, Install to Disk")
    # GRUB draws its menu in graphics, which q.screen() cannot read, so
    # the menu is driven blind. A cold first boot reaches it later than a
    # warm one, and keys sent before it is up are lost and the default
    # entry boots -- that was this harness's first run. So: try, look for
    # the installer, and if the desktop came up instead, power-cycle and
    # try again with a longer wait.
    q = None
    hit = None
    for wait in (8, 14, 20):
        if q: q.stop(); kill_stale()
        q = Qemu(ISO, disk=IMG, mem=512).start()
        time.sleep(wait)
        for _ in range(MENU_INSTALL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        hit, s = wait_any(q, ["Installer", "Welcome to"], 90)
        if hit: break
    try:
        ck("the installer came up", hit is not None, s[:300])
        q.key("ret"); time.sleep(2)                  # welcome -> picker
        q.key("ret"); time.sleep(2)                  # the only disk
        s = q.screen()
        ck(f"the layout preview says FAT{want}",
           f"FAT{want} (type 0x{'0C' if want == 32 else '0E'})" in s, s[:900])
        ck("and names the size it will use",
           f"{part // 2048} MB" in s, s[:900])
        q.line("YES")
        hit, s = wait_any(q, ["successfully", "Failed", "failed"], 300)
        ck("the install completed", hit == "successfully", s[:600])
    finally:
        q.stop()

    print("\n  [2] the disk, by tools that are not ours")
    mbr = open(IMG, "rb").read(512)
    ck(f"MBR partition type 0x{'0C' if want == 32 else '0E'}",
       mbr[446 + 4] == (0x0C if want == 32 else 0x0E), hex(mbr[446 + 4]))
    cnt = int.from_bytes(mbr[446 + 12:446 + 16], "little")
    ck("the entry is the expected size", cnt == part, f"{cnt} sectors")
    off = 2048 * 512
    r = sh(f"fsck.fat -n {IMG}@@{off} 2>&1 || "
           f"(dd if={IMG} of=/tmp/hd_wd32_p.img bs=1M skip=1 count={part // 2048} "
           f"conv=sparse status=none && fsck.fat -n /tmp/hd_wd32_p.img)")
    ck("fsck.fat accepts the volume", r.returncode == 0, (r.stdout + r.stderr)[-300:])
    ck("it is labelled HAVENDOS",
       "HAVENDOS" in sh(f"mlabel -i {IMG}@@{off} -s ::").stdout)
    ck("the kernel is there",
       sh(f"mdir -i {IMG}@@{off} ::/boot/havendos.elf").returncode == 0)
    ck("and the menu",
       sh(f"mcopy -n -i {IMG}@@{off} ::/boot/grub/grub.cfg /tmp/hd_wd32_cfg.txt"
          " && grep -q 'label HAVENDOS' /tmp/hd_wd32_cfg.txt").returncode == 0)
    bpb = open(IMG, "rb")
    bpb.seek(off); b = bpb.read(512); bpb.close()
    ck(f"the BPB says FAT{want}",
       (b[82:87] == b"FAT32") if want == 32 else (b[54:59] == b"FAT16"),
       repr(b[82:90] if want == 32 else b[54:62]))

    print("\n  [3] the disk, booted, no ISO")
    kill_stale()
    q = Qemu(None, disk=IMG, mem=512, boot="c").start()
    try:
        hit, s = wait_any(q, ["Terms of Service", "Installer",
                              "install ISO", "admin@havendos"], 180)
        ck("GRUB found the volume and the kernel booted from it",
           hit is not None, s[:400])
        ck("the installed system runs its first-boot wizard, not the installer",
           hit == "Terms of Service", s[:400])
    finally:
        q.stop()

    print()
    if fails:
        print(f"{len(fails)} of {total[0]} check(s) failed")
        return 1
    print(f"all {total[0]} checks passed -- a {GB} GB disk installs FAT{want} "
          "and boots it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
