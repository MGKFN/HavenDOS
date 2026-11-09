"""Capture HavenDOS screens from a running QEMU, as PNG."""
import sys, os, time
sys.path.insert(0, '/home/claude/havendos/harness')
from qemu import Qemu, kill_stale
from PIL import Image

OUT = "/home/claude/havendos/screenshots"
ISO = "/home/claude/havendos/HavenDOS-0.7.20-source/HavenDOS-0.8.0.iso"
DISK = "/home/claude/havendos/disk.img"
STICK = "/home/claude/havendos/usbstick.img"

_n = [0]

def shot(q, name, settle=1.2):
    """Capture the screen to screenshots/NN-name.png."""
    time.sleep(settle)
    _n[0] += 1
    ppm = "/tmp/_shot.ppm"
    q.shot(ppm)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
        time.sleep(0.1)
    im = Image.open(ppm).convert("RGB")
    path = os.path.join(OUT, "%02d-%s.png" % (_n[0], name))
    im.save(path)
    os.unlink(ppm)
    cols = len(im.getcolors(maxcolors=300000) or [])
    print("  %-34s %s  %d colours" % (name, im.size, cols))
    return path

def boot_shell(usbstick=STICK, disk=DISK, wait_grub=6):
    kill_stale()
    q = Qemu(ISO, disk=disk, usbstick=usbstick).start()
    time.sleep(wait_grub)
    return q

def cmd(q, line, settle=2.2):
    q.line(line)
    time.sleep(settle)
