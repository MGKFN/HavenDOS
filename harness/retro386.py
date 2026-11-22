#!/usr/bin/env python3
"""retro386.py -- v0.9.7.2: BOOT OS on Retro PC's 386, inside HavenDOS, in QEMU.

tests/test_emu386.c boots the three built-in BOOT OS kernels natively.
This is the same on the real kernel, with the image Kyan would use: a
CD image in RETRO on a USB stick.

  1. `retro` lists the built-in BOOT OS builds, as 386 images
  2. it lists BOOT52.ISO from RETRO on a FAT32 stick, as a CD on a stick
  3. Enter on the ISO: its own boot menu, read off the CD (grub.cfg)
  4. Enter on the text-desktop entry: BOOT OS 0.5.2 boots to its login
  5. bootos / 1234: its desktop
  6. F4 leaves, back to the list; the log says what was booted

The ISO is made here with xorriso from archive/retro/bootos-0.5.2.elf
and its grub.cfg, laid out as on the CD: /boot/bootos.elf and
/boot/grub/grub.cfg. Retro PC needs nothing else on it -- it does
GRUB's job itself.

This is slow: an x86 interpreter, inside a kernel, inside QEMU's own
emulation, running a boot animation that counts its delays in
instructions. Several minutes.

Run:  python3 harness/retro386.py [path/to.iso]   (foreground)
"""
import os
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
WORK = "/tmp/hd_retro386"
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1800:].replace("\n", "\n          "))
    return ok


def run(*a):
    subprocess.run(list(a), check=True, capture_output=True)


def boot_iso():
    d = os.path.join(WORK, "cd")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "boot", "grub"))
    shutil.copy(os.path.join(ROOT, "archive", "retro", "bootos-0.5.2.elf"), os.path.join(d, "boot", "bootos.elf"))
    shutil.copy(os.path.join(ROOT, "archive", "retro", "bootos-0.5.2.cfg"), os.path.join(d, "boot", "grub", "grub.cfg"))
    out = os.path.join(WORK, "BOOT52.ISO")
    run("xorriso", "-as", "mkisofs", "-quiet", "-R", "-o", out, d)
    return out


def retro_stick(cd):
    stick = make_stick(os.path.join(WORK, "stick.img"), mb=64, fat=32)
    part = os.path.join(WORK, "part.img")
    run("dd", "if=" + stick, "of=" + part, "bs=512", "skip=2048")
    run("mmd", "-i", part, "::RETRO")
    run("mcopy", "-i", part, cd, "::RETRO/BOOT52.ISO")
    run("dd", "if=" + part, "of=" + stick, "bs=512", "seek=2048", "conv=notrunc")
    return stick


def wait_for(q, text, limit):
    t0 = time.time()
    while time.time() - t0 < limit:
        s = q.screen()
        if text in s:
            return s, time.time() - t0
        time.sleep(3)
    return q.screen(), -1


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    for t in ("xorriso", "mkfs.vfat", "mcopy", "mmd", "dd"):
        if not shutil.which(t):
            sys.exit("needs " + t)
    print("Retro PC's 386: BOOT OS 0.5.2 from a CD image on a USB stick")
    os.makedirs(WORK, exist_ok=True)
    stick = retro_stick(boot_iso())
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, usbstick=stick) as q:
        boot_to_shell(q)
        q.line("retro"); time.sleep(5.0)
        s = q.screen()
        check("the list has BOOT OS 0.3.1, 0.5.0 and 0.5.2 built in, as 386 images",
              all(("BOOT OS %s" % v) in s for v in ("0.3.1", "0.5.0", "0.5.2")) and "built in, 386" in s, s)
        row = next((l for l in s.splitlines() if "BOOT52.ISO" in l), "")
        check("and BOOT52.ISO from RETRO on the stick", "BOOT52.ISO" in row and "stick" in row, s)

        for _ in range(4):
            q.key("down"); time.sleep(0.6)
        q.key("ret"); time.sleep(4.0)
        s = q.screen()
        check("Enter on the ISO: its own boot menu, off the CD",
              "Its boot menu" in s and "[Text Desktop]" in s and "Shell Mode" in s, s)

        q.key("ret")
        s, took = wait_for(q, "Password:", 600)
        check("BOOT OS 0.5.2 boots to its login screen (%ds)" % took, took >= 0 and "v0.5.2" in s, s)
        if took >= 0:
            for ch in "bootos":
                q.type(ch); time.sleep(0.6)
            q.key("ret"); time.sleep(1.5)
            for ch in "1234":
                q.type(ch); time.sleep(0.6)
            q.key("ret")
            s, took = wait_for(q, "Tab=Shell", 300)
            check("bootos / 1234: its desktop (%ds)" % took, took >= 0 and "Text Editor" in s, s)

        q.key("f4"); time.sleep(3.0)
        s = q.screen()
        check("F4 leaves the old PC, back to the list", "Retro PC" in s and "BOOT52.ISO" in s, s)
        q.type("q"); time.sleep(2.0)
        q.line("dmesg"); time.sleep(2.0)
        seen = ""
        for _ in range(40):
            s = q.screen(); seen += s
            if "retro: booting stick: BOOT52.ISO on the 386" in s or "-- more --" not in s:
                break
            q.key("spc"); time.sleep(0.8)
        check("the kernel log says it booted the stick's ISO on the 386",
              "retro: booting stick: BOOT52.ISO on the 386" in seen,
              "\n".join(l for l in seen.splitlines() if "retro" in l)[-600:])

    print("\n%d/%d" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
