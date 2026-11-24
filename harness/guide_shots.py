#!/usr/bin/env python3
"""guide_shots.py -- the screenshots in the install guide (THS-IG-1001).

Takes them from the release itself, so the guide shows the menu as it is:

  grub-iso.png    the USB stick's boot menu, 9 s in (BIOS, 1920x1080)
  grub-disk.png   the installed system's menu, from the disk that
                  harness/uefi_dualboot.py leaves at /tmp/hd_dualboot.img
                  (run that first), 4 s in -- before its 8 s timeout

The installer's own screens (picker.png, confirm.png) are the ones
uefi_dualboot.py saves as /tmp/hd_db_02_picker.png and _04_confirm.png.

Run:  python3 harness/guide_shots.py [path/to.iso]
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                            # noqa: E402

ROOT = os.path.dirname(HERE)
ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
IMG = os.path.join(ROOT, "docs", "install-guide", "img")
DISK = "/tmp/hd_dualboot.img"
CODE, VARS = "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"


def png(src, dst):
    from PIL import Image
    Image.open(src).convert("RGB").save(dst)
    print("  wrote", dst)


def main():
    os.makedirs(IMG, exist_ok=True)
    Q.kill_stale()
    q = Q.Qemu(ISO, disk=None, usb=False).start()
    try:
        time.sleep(9)
        q.hmp("screendump /tmp/gs_iso.ppm"); time.sleep(0.5)
    finally:
        q.stop()
    png("/tmp/gs_iso.ppm", os.path.join(IMG, "grub-iso.png"))

    if os.path.exists("/tmp/hd_tg_choose.ppm"):     # harness/two_gaps_0979.py
        png("/tmp/hd_tg_choose.ppm", os.path.join(IMG, "choose-space.png"))
    for n in ("02_picker", "04_confirm"):
        src = "/tmp/hd_db_%s.png" % n
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(IMG, n.split("_")[1] + ".png"))

    if not os.path.exists(DISK):
        print("  no %s -- run harness/uefi_dualboot.py for grub-disk.png" % DISK)
        return 0
    shutil.copyfile(VARS, "/tmp/gs_vars.fd")
    Q.kill_stale()
    with Q.Qemu(iso=None, mem=512, gfx=True, binary="qemu-system-x86_64",
                machine="pc",
                extra=["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % CODE,
                       "-drive", "if=pflash,format=raw,unit=1,file=/tmp/gs_vars.fd",
                       "-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % DISK]
                ) as q:
        # OVMF, then GRUB; the menu's timeout is 8 s, so look early and
        # stop the countdown with a key that moves nothing (Up on entry 0
        # is not where it starts: default is Windows, entry 1 -- Up moves
        # the bar to HavenDOS, which is the picture the guide wants).
        for _ in range(40):
            time.sleep(1)
            q.hmp("screendump /tmp/gs_disk.ppm")
            from PIL import Image
            im = Image.open("/tmp/gs_disk.ppm")
            if im.size[0] >= 1024 and im.getpixel((im.size[0] // 10 + 4, im.size[1] // 8)) != (0, 0, 0):
                break
        q.key("up"); time.sleep(1.5)
        q.hmp("screendump /tmp/gs_disk.ppm"); time.sleep(0.5)
    png("/tmp/gs_disk.ppm", os.path.join(IMG, "grub-disk.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
