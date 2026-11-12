#!/usr/bin/env python3
"""selfhost.py -- an installed HavenDOS installs HavenDOS.

v0.9.4 "Prometheus". RM-1003 asked for "a system that can install
itself". The kernel an installed system runs is the pass-1 build, which
carries no image to install, so `install` on one used to refuse. It now
copies its own /boot/havendos.elf, read off the system volume.

This proves it on QEMU, with no ISO anywhere after the first step:

  1. harness/wholedisk_fat32.py installs a 6 GB FAT32 system (disk A)
  2. disk A boots its own "shell mode" entry, with a blank USB stick B
  3. `install`: the welcome screen says it is copying this system's
     kernel; disk A is marked "running from this disk" and refused;
     disk B is installed whole-disk
  4. disk B's /boot/havendos.elf is byte-for-byte disk A's, per mtools
  5. disk B boots ALONE to the first-boot wizard

Run:  python3 harness/selfhost.py [iso]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu import Qemu, kill_stale                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
A = "/tmp/hd_wd32.img"            # what wholedisk_fat32.py leaves behind
B = "/tmp/hd_selfhost_b.img"
B_GB = 3
MENU_SHELL = 1                     # the installed grub.cfg: 0 desktop, 1 shell

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
    print("  [1] disk A: a FAT32 install made from the ISO")
    r = subprocess.run([sys.executable, os.path.join(HERE, "wholedisk_fat32.py"), ISO],
                       capture_output=True, text=True)
    ck("wholedisk_fat32.py made and booted disk A", r.returncode == 0,
       r.stdout[-400:])
    if r.returncode != 0:
        return 1
    # wholedisk_fat32.py booted it once, into the setup wizard, and powered
    # it off there; nothing it did is needed here.

    if os.path.exists(B):
        os.unlink(B)
    with open(B, "wb") as f:
        f.truncate(B_GB * 1024 * 1024 * 1024)

    print("\n  [2] disk A boots its shell entry, with blank disk B beside it")
    kill_stale()
    q = None
    hit = None
    # The installed menu counts down from FIVE seconds (fs/install.c's
    # grub.cfg), so a key sent after a fixed sleep lands on a kernel that
    # is already booting the default entry -- this harness's first run.
    # HOME is pressed every 0.4 s from the start instead: it stops the
    # countdown the moment GRUB is up and cannot move past entry 0, so
    # the one DOWN after it is exact.
    for attempt in range(3):
        if q: q.stop(); kill_stale()
        # Disk B is a USB stick. drivers/ata.c operates only the first IDE
        # drive it finds (it logs the others), so a second IDE disk is
        # invisible -- and on the HP, the realistic target for a copy is
        # exactly this: a stick beside the NVMe the system runs from.
        q = Qemu(None, disk=A, mem=512, boot="c", usbstick=B).start()
        for _ in range(20):
            time.sleep(0.4)
            q.key("home")
        for _ in range(MENU_SHELL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        hit, s = wait_any(q, ["Shell", "Terms of Service"], 90)
        if hit == "Shell":
            break
    try:
        ck("disk A reached its shell", hit == "Shell", s[:400])
        time.sleep(1)
        q.line("install")
        hit, s = wait_any(q, ["Welcome", "cannot install"], 30)
        ck("`install` opens the installer, not a refusal", hit == "Welcome", s[:600])
        ck("which says it copies this system's own kernel",
           "this system's own kernel" in s, s[:900])
        hit = None
        for _ in range(6):                    # see wholedisk_fat32.py
            time.sleep(2)
            q.key("ret")
            hit, s = wait_any(q, ["running from this disk"], 15)
            if hit:
                break
        ck("disk A is marked as the disk it is running from", hit is not None,
           s[:900])
        q.key("ret"); time.sleep(3)           # try to pick disk A anyway
        s = q.screen()
        ck("and picking it is refused", "running from" in s, s[:900])
        time.sleep(2)
        q.key("down"); time.sleep(0.5)
        q.key("ret"); time.sleep(2)           # disk B: whole-disk confirm
        s = q.screen()
        ck("disk B (the stick) offers a FAT32 whole-disk install",
           "FAT32 (type 0x0C)" in s, s[:900])
        q.line("YES")
        hit, s = wait_any(q, ["successfully", "Failed", "failed"], 300)
        ck("the install onto disk B completes", hit == "successfully", s[:600])
        ck("and does not reboot the running system",
           "keeps running" in s, s[:600])
    finally:
        q.stop()

    print("\n  [3] disk B, by tools that are not ours")
    off = 2048 * 512
    mbr = open(B, "rb").read(512)
    ck("MBR type 0x0C", mbr[446 + 4] == 0x0C, hex(mbr[446 + 4]))
    ck("fsck.fat accepts it", sh(f"fsck.fat -n {B}@@{off}").returncode == 0 or
       sh(f"dd if={B} of=/tmp/hd_sh_b.img bs=1M skip=1 conv=sparse status=none"
          f" && fsck.fat -n /tmp/hd_sh_b.img").returncode == 0)
    ra = sh(f"mcopy -n -i {A}@@{off} ::/boot/havendos.elf /tmp/hd_sh_ka.elf")
    rb = sh(f"mcopy -n -i {B}@@{off} ::/boot/havendos.elf /tmp/hd_sh_kb.elf")
    ck("both disks hold a kernel", ra.returncode == 0 and rb.returncode == 0)
    ck("and disk B's is byte-for-byte disk A's",
       sh("cmp -s /tmp/hd_sh_ka.elf /tmp/hd_sh_kb.elf").returncode == 0)

    print("\n  [4] disk B, booted alone (as the machine's disk)")
    kill_stale()
    q = Qemu(None, disk=B, mem=512, boot="c").start()
    try:
        hit, s = wait_any(q, ["Terms of Service", "Installer", "admin@havendos"], 180)
        ck("disk B boots to the first-boot wizard",
           hit == "Terms of Service", s[:400])
    finally:
        q.stop()

    print()
    if fails:
        print(f"{len(fails)} of {total[0]} check(s) failed")
        return 1
    print(f"all {total[0]} checks passed -- an installed HavenDOS installed "
          "HavenDOS, and the copy boots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
