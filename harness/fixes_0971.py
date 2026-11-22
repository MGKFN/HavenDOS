#!/usr/bin/env python3
"""fixes_0971.py -- 0.9.7.1: your own images in Retro PC, and `cp` of a big file.

Kyan, 22 Nov: "how do i add my own imgs???". 0.9.7 looked in RETRO on
the HavenDOS disk only, and on the HP nothing could put a file there:
the Shell's `cp` copied 4 KB of anything, and Files cannot see a stick.

  1. `>>` past 4 KB keeps what was there (it overwrote it), and `cp` of
     a file bigger than 4 KB copies all of it
  2. `retro` lists an image from RETRO on a FAT32 USB stick, as on a stick
  3. Enter on it boots it: K-DOS's login screen, read from the stick
  4. the kernel log says it booted the stick's image

The stick is a partitioned FAT32 image with RETRO/MYDOS.IMG on it, a
copy of archive/retro/kdos-beta-1.1b.img -- Windows makes exactly that
when a folder is dragged onto a stick.

Run:  python3 harness/fixes_0971.py [path/to.iso]   (foreground)
"""
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402
from usb_storage import make_stick                      # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
WORK = "/tmp/hd_fixes_0971"
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1600:].replace("\n", "\n          "))
    return ok


def retro_stick():
    os.makedirs(WORK, exist_ok=True)
    stick = make_stick(os.path.join(WORK, "stick.img"), mb=64, fat=32)
    part = os.path.join(WORK, "part.img")
    subprocess.run(["dd", "if=" + stick, "of=" + part, "bs=512", "skip=2048"],
                   check=True, capture_output=True)
    subprocess.run(["mmd", "-i", part, "::RETRO"], check=True, capture_output=True)
    subprocess.run(["mcopy", "-i", part, os.path.join(ROOT, "archive", "retro", "kdos-beta-1.1b.img"),
                    "::RETRO/MYDOS.IMG"], check=True, capture_output=True)
    subprocess.run(["dd", "if=" + part, "of=" + stick, "bs=512", "seek=2048", "conv=notrunc"],
                   check=True, capture_output=True)
    return stick


def copied(s, name):
    m = re.findall(r"Copied \S+ -> \S*%s \((\d+) bytes\)" % re.escape(name), s)
    return int(m[-1]) if m else -1


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    for t in ("mkfs.vfat", "mcopy", "mmd", "dd"):
        if not shutil.which(t):
            sys.exit("needs " + t)
    print("0.9.7.1: Retro PC images off a USB stick; `cp` of a big file")
    stick = retro_stick()
    Q.kill_stale()
    # without the stick: a live session with a FAT32 stick plugged in
    # writes elsewhere, and this is about the Shell, not the stick
    with Q.Qemu(iso=ISO, mem=256) as q:
        boot_to_shell(q)

        line = "x" * 40         # the prompt line stops at the screen's edge
        for i in range(110):
            q.line("echo %03d%s >> BIG.TXT" % (i, line)); time.sleep(0.15)
        q.line("cls"); time.sleep(1.5)
        q.line("wc BIG.TXT"); time.sleep(1.5)
        s = q.screen()
        m = re.findall(r"Lines: (\d+)", s)
        check("`>>` 110 times: all 110 lines (4.8 KB) are in the file", m and m[-1] == "110",
              "\n".join(l for l in s.splitlines() if l.strip())[-500:])
        q.line("cp BIG.TXT BIG2.TXT"); time.sleep(2.0)
        q.line("cp BIG2.TXT BIG3.TXT"); time.sleep(2.0)
        s = q.screen()
        a, b = copied(s, "BIG2.TXT"), copied(s, "BIG3.TXT")
        check("`cp` copies a file over 4 KB whole (%d bytes)" % a, a > 4096, s[-900:])
        check("and a copy of the copy is the same size (%d)" % b, b == a and a > 0, s[-900:])
        q.line("cls"); time.sleep(1.5)


    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, usbstick=stick) as q:
        boot_to_shell(q)
        q.line("retro"); time.sleep(4.0)
        s = q.screen()
        check("`retro` lists MYDOS.IMG from RETRO on the stick",
              "MYDOS.IMG" in s and "stick" in s, s)
        lines = s.splitlines()
        row = next((l for l in lines if "MYDOS.IMG" in l), "")
        check("and says it is on a USB stick", "USB stick" in row, row)

        # past the built-in images (one row each, "built in, ..."): since
        # 0.9.7.2 there are four, K-DOS and three BOOT OS
        for _ in range(s.count("built in")):
            q.key("down"); time.sleep(0.8)
        q.key("ret"); time.sleep(4.0)
        s = q.screen()
        check("Enter boots it off the stick: K-DOS's login screen",
              "K-DOS-Beta-1.1b SECURITY" in s and "User:" in s, s)
        q.key("f4"); time.sleep(2.0)
        q.type("q"); time.sleep(2.0)

        q.line("dmesg"); time.sleep(2.0)
        seen = ""
        for _ in range(30):
            s = q.screen(); seen += s
            if "retro: booting stick: MYDOS.IMG" in s or "-- more --" not in s:
                break
            q.key("spc"); time.sleep(0.8)
        check("the kernel log names the stick's image, and its disk in RETRO",
              "retro: booting stick: MYDOS.IMG" in seen and "hard disk RETRO/M" in seen,
              "\n".join(l for l in seen.splitlines() if "retro" in l)[-600:])

    print("\n%d/%d" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
