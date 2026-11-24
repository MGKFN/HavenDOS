#!/usr/bin/env python3
"""two_gaps_0979.py -- the installer on a disk with two free spaces.

Kyan, 24 Nov: his laptop had 34 GB free that he was keeping for macOS and
2 GB he had made for HavenDOS. 0.9.7.8 took the FIRST free space on the
disk -- the 34 GB one -- and made 32 GB of it HavenDOS, without asking.

This builds that disk (shrunk: a 34 GB space would make the image huge for
nothing, so 6 GB stands for it) -- ESP, MSR, "C:", [6 GB free], a partition,
[2 GB free], recovery -- boots the ISO, and drives the installer:

  * the disk list says the free space is chosen next;
  * "Choose the Free Space" lists both, the 2 GB one highlighted;
  * Enter, YES: the install goes into the 2 GB space;
  * sgdisk afterwards: the 6 GB space is still free and whole, every
    partition that was there is where it was.

Run:  python3 harness/two_gaps_0979.py [path/to.iso]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
IMG = "/tmp/hd_twogaps.img"
GB = 16
MENU_INSTALL = 3
fails, total = [], [0]


def ck(what, ok, detail=""):
    total[0] += 1
    print(("    ok   " if ok else "    FAIL ") + what
          + ("" if ok or not detail else "\n         " + str(detail)[:900].replace("\n", "\n         ")))
    if not ok:
        fails.append(what)


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True)


def parts():
    out = {}
    for line in sh("sgdisk -p %s" % IMG).stdout.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0].isdigit():
            out[int(p[0])] = (int(p[1]), int(p[2]), " ".join(p[6:]))
    return out


def gaps(pt):
    edges = sorted((a, b) for a, b, _ in pt.values())
    g = [(edges[i][1] + 1, edges[i + 1][0] - 1) for i in range(len(edges) - 1)
         if edges[i + 1][0] - 1 >= edges[i][1] + 1]
    return [(lo, hi) for lo, hi in g if hi - lo > 64 * 2048]


def build():
    if os.path.exists(IMG):
        os.unlink(IMG)
    # ESP 260M, MSR 16M, C: 5G, [6G free], "Other OS" 1G, [2G free],
    # recovery 512M at the very end -- absolute sectors, so there is no
    # third free space at the tail.
    rec_end = 30975999
    with open(IMG, "wb") as f:
        f.truncate((rec_end + 1 + 2048) * 512)
    r = sh("sgdisk -o "
           "-n 1:2048:+260M -t 1:ef00 -c 1:'EFI system partition' "
           "-n 2:0:+16M -t 2:0c01 -c 2:'Microsoft reserved partition' "
           "-n 3:567296:11053055 -t 3:0700 -c 3:'Basic data partition' "
           "-n 4:23635968:25733119 -t 4:0700 -c 4:'Other OS' "
           "-n 5:29927424:%d -t 5:2700 -c 5:'Basic data partition' %s" % (rec_end, IMG))
    if r.returncode:
        sys.exit("sgdisk: " + r.stderr)
    e0, e1, _ = parts()[1]
    sh("dd if=/dev/zero of=/tmp/hd_tg_esp.img bs=512 count=%d status=none" % (e1 - e0 + 1))
    sh("mkfs.vfat -F 32 -n EFI /tmp/hd_tg_esp.img")
    for d in ("::/EFI", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot", "::/EFI/BOOT"):
        sh("mmd -i /tmp/hd_tg_esp.img %s" % d)
    sh("head -c 1800000 /dev/urandom > /tmp/hd_tg_bm.bin")
    sh("mcopy -i /tmp/hd_tg_esp.img /tmp/hd_tg_bm.bin ::/EFI/Microsoft/Boot/bootmgfw.efi")
    sh("dd if=/tmp/hd_tg_esp.img of=%s bs=512 seek=%d conv=notrunc status=none" % (IMG, e0))


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    print("two free spaces: the installer asks, and starts on the small one")
    build()
    before = parts()
    g = gaps(before)
    for lo, hi in g:
        print("    free  %10d..%-10d  %d MB" % (lo, hi, (hi - lo + 1) // 2048))
    ck("the disk has two free spaces, the big one first",
       len(g) == 2 and (g[0][1] - g[0][0]) > (g[1][1] - g[1][0]), g)
    big, small = g[0], g[1]

    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=512, boot="d", machine="pc", usb=False,
                extra=["-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % IMG]) as q:
        time.sleep(4)
        for _ in range(MENU_INSTALL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        ck("the installer came up", q.wait_for("Installer", 180) is not None, q.screen())
        time.sleep(2)
        q.key("ret")                                        # welcome -> disks
        s = q.wait_for("Select Target Disk", 30) or q.screen()
        ck("the disk list says the free space is chosen next",
           "space chosen next" in s, s)
        q.key("ret")
        s = q.wait_for("Choose the Free Space", 30) or q.screen()
        ck("it asks which free space", "Choose the Free Space" in s, s)
        q.shot("/tmp/hd_tg_choose.ppm")            # for the install guide
        ck("listing both, with their sizes", "6.0 GB" in s and "2.0 GB" in s, s)
        lines = [l for l in s.splitlines() if l.strip().startswith(">")]
        ck("the 2 GB one is highlighted to start with",
           len(lines) == 1 and "2.0 GB" in lines[0], lines)
        ck("and it says what HavenDOS would use of it",
           "HavenDOS would use about 2." in s or "HavenDOS would use about 1" in s, s)
        q.key("ret")
        s = q.wait_for("What will be written", 30) or q.screen()
        ck("the plan is for the 2 GB space, formatted FAT32",
           "formatted FAT32" in s and ("(20" in s or "(19" in s), s)
        q.key("ret")
        s = q.wait_for("Nothing is deleted", 30) or q.screen()
        q.type("YES"); time.sleep(0.4); q.key("ret")
        hit = None
        for _ in range(60):
            time.sleep(3)
            s = q.screen()
            if "Complete" in s or "Failed" in s:
                hit = s; break
        ck("the install completed", hit is not None and "Complete" in hit, s)

    after = parts()
    new = [n for n in after if n not in before]
    ck("one partition was added", len(new) == 1, after)
    if new:
        a, b, _ = after[new[0]]
        ck("it is inside the 2 GB space", small[0] <= a and b <= small[1], (a, b, small))
    ck("every partition that was there is where it was",
       all(after.get(n, (0, 0, ""))[:2] == v[:2] for n, v in before.items()), after)
    g2 = gaps(after)
    ck("and the 6 GB space is still free, whole", big in g2, g2)
    ck("sgdisk finds no problems", "No problems found" in sh("sgdisk -v %s" % IMG).stdout)
    print("\n%d/%d passed" % (total[0] - len(fails), total[0]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
