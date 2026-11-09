#!/usr/bin/env python3
"""usb_storage.py -- can HavenDOS read a file off a USB stick?

This is 0.8.6 "Argo"'s acceptance check, and it is an end-to-end one on
purpose: every layer it crosses has had a bug that the layer's own unit
suite could not see.

  usb/msc.c      Bulk-Only Transport and SCSI over it
  usb/hub.c      the stick is BEHIND A HUB, which is where a stick in
                 the side of a laptop usually is
  usb/core.c     the route string that addresses a device behind one
  usb/xhci_hc.c  the controller, with a route string in its slot context
                 for the first time
  fs/blkdev.c    the stick registered as a block device, after every
                 fixed disk
  fs/fat32.c     a partition table read off it, and a file read out

What it proves, and what it cannot, stated up front because this
project's own history is of claims that outran their evidence:

  CAN    the whole path works against QEMU's usb-storage device, on a
         root port and behind a hub, on xHCI and on UHCI.
  CANNOT that it works on the HP. QEMU's xHCI is not the HP's, and
         0.8.5 shipped THREE releases of fixes that QEMU could not have
         found: a 64-bit BAR truncated, a blocking interrupt poll, and
         an 8042 read loop with no I/O delay. The handoff lists this as
         the one thing only the laptop can answer.
  CANNOT anything about a stick that is not FAT. Most sticks over 32 GB
         are exFAT and this kernel has no exFAT driver.

── WHY A HUB IS THE DEFAULT AND NOT AN EXTRA CASE ────────────────────

0.8.5 walked root ports only and judged it enough, correctly, for a
keyboard: the HP's own listing shows devices on root ports 2 and 9, both
internal. A stick is not internal. The 20 October handoff named a hub as
the first suspect if a stick did not appear, so the hub case is the one
that runs first here.

Run:  python3 harness/usb_storage.py [xhci|uhci|both]
"""
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu import Qemu, kill_stale            # noqa: E402

ISO  = os.path.join(SRC, "havendos.iso")
WORK = "/tmp/hd_usbstor"

# GRUB entry 1 is Shell Mode. isodir/boot/grub/grub.cfg says so and
# three other harnesses index the same list.
SHELL_ENTRY  = 1
BOOT_SECONDS = 45
GATE_PRESSES = 5

# The text written onto the stick and read back out of it. Distinctive
# on purpose: it must come off the DISK and not out of this kernel, and a
# generic string could have come from anywhere on the screen.
MARKER = "HavenDOS reached this file over Bulk-Only Transport."

total = 0
fails = 0


def ck(what, ok, detail=""):
    global total, fails
    total += 1
    if ok:
        print("    ok   %s" % what, flush=True)
    else:
        fails += 1
        print("    FAIL %-52s %s" % (what, detail), flush=True)


def head(s):
    print("\n  %s" % s, flush=True)


def need(*tools):
    missing = [t for t in tools
               if subprocess.run(["which", t], capture_output=True).returncode]
    if missing:
        print("  SKIP  this harness needs %s" % ", ".join(missing))
        return False
    return True


def make_stick(path, mb=512, fat=32):
    """A stick with a partition table on it, because real ones have one.

    The first version of this harness made a bare filesystem -- a
    "superfloppy" -- and `fat32` correctly reported no volumes, because
    fs/fat32vol.c looks for PARTITION ENTRIES. That is right: a volume
    found without one would be a volume found by guessing. So the image
    is partitioned, with an 0x0C (FAT32 LBA) entry at the usual 2048.
    """
    os.makedirs(WORK, exist_ok=True)
    total_sz = mb * 1024 * 1024
    start = 2048
    count = total_sz // 512 - start

    with open(path, "wb") as f:
        f.truncate(total_sz)

    mbr = bytearray(512)
    p = 446
    mbr[p + 0] = 0x80
    mbr[p + 1:p + 4] = bytes([0xFE, 0xFF, 0xFF])
    mbr[p + 4] = 0x0C if fat == 32 else 0x0E
    mbr[p + 5:p + 8] = bytes([0xFE, 0xFF, 0xFF])
    struct.pack_into("<I", mbr, p + 8, start)
    struct.pack_into("<I", mbr, p + 12, count)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    with open(path, "r+b") as f:
        f.write(bytes(mbr))

    part = os.path.join(WORK, "part.img")
    note = os.path.join(WORK, "note.txt")
    with open(note, "w") as f:
        f.write(MARKER + "\n")
    subprocess.run(["dd", "if=/dev/zero", "of=" + part, "bs=1M",
                    "count=%d" % (mb - 1)], check=True,
                   capture_output=True)
    subprocess.run(["mkfs.vfat", "-F", str(fat), "-n", "ARGO", part],
                   check=True, capture_output=True)
    subprocess.run(["mcopy", "-i", part, note, "::ARGO.TXT"], check=True,
                   capture_output=True)
    subprocess.run(["mmd", "-i", part, "::DOCS"], check=True,
                   capture_output=True)
    subprocess.run(["mcopy", "-i", part, note, "::DOCS/NOTES.TXT"],
                   check=True, capture_output=True)
    subprocess.run(["dd", "if=" + part, "of=" + path, "bs=512",
                    "seek=%d" % start, "conv=notrunc"], check=True,
                   capture_output=True)
    return path


def qemu_for(controller, stick, hub):
    """The device arguments. Two controllers, so the route string is
    exercised against a back-end that uses it and one for which a hub is
    transparent -- include/usb.h's argument for two implementations, one
    layer up."""
    extra = []
    if controller == "xhci":
        extra += ["-device", "qemu-xhci,id=xhci"]
        bus = "xhci.0"
    else:
        # -usb is what puts the UHCI at 00:01.2.
        bus = "usb-bus.0"
    extra += ["-drive", "if=none,id=stick,file=%s,format=raw" % stick]
    if hub:
        extra += ["-device", "usb-hub,bus=%s,port=2,id=hub1" % bus]
        extra += ["-device",
                  "usb-storage,drive=stick,bus=%s,port=2.3,removable=on" % bus]
    else:
        extra += ["-device",
                  "usb-storage,drive=stick,bus=%s,removable=on" % bus]
    return extra, (controller == "uhci")


def run_case(controller, hub, stick):
    label = "%s, %s" % (controller, "behind a hub" if hub else "on a root port")
    head(label)
    kill_stale()
    extra, want_usb = qemu_for(controller, stick, hub)
    q = Qemu(ISO, machine="q35", usb=want_usb, sock="/tmp/hd_usbstor.sock",
             extra=extra).start()
    try:
        time.sleep(7)
        for _ in range(SHELL_ENTRY):
            q.key("down")
            time.sleep(0.4)
        q.key("ret")
        time.sleep(BOOT_SECONDS)
        for _ in range(GATE_PRESSES):
            if q.proc.poll() is not None:
                break
            q.key("ret")
            time.sleep(2)
        if q.proc.poll() is not None:
            ck("the machine stayed up", False, "(qemu exited during boot)")
            return

        # The boot log, which is where the driver says what it found.
        # Paged with SPACE only: the shell's pagers eat every other key
        # and `q` quits the shell to the desktop.
        log = read_cmd(q, "dmesg", pages=40)
        ck("the driver brought the stick up",
           "USB MSC" in log, "(no USB MSC line in dmesg)")
        ck("...and read its capacity off it",
           "512-byte blocks" in log, "(no block size reported)")

        bus = read_cmd(q, "usb -vv", pages=40)
        ck("the bus lists a mass-storage interface",
           "08/06/50" in bus, "(no Bulk-Only SCSI interface)")
        if hub:
            ck("the hub is listed as a hub", "09/00/00" in bus)
            ck("the stick is recorded as behind it",
               "behind the hub" in bus, "(no hub parentage printed)")
            # THE CHECK THE ROUTE STRING NEEDS. Port 2 of the hub is
            # port NUMBER 3, so a tier-1 device's route is 3 -- and a
            # wrong route is a valid address to a different socket, with
            # no error anywhere to say so.
            ck("...at route 3, the hub port one-based",
               "route 00003" in bus, "(route string wrong or absent)")
            ck("...on tier 1", "tier 1" in bus)

        disk = read_cmd(q, "disk", pages=20)
        ck("`disk` lists it as a USB disk",
           "USB disk 0" in disk, "(not in the disk list)")
        ck("...and as writable, not read-only by accident",
           "writable" in disk, "(reported read-only)")

        vols = read_cmd(q, "fat32", pages=20)
        ck("its partition table was read",
           "MBR type 0x0c" in vols or "MBR type 0x0C" in vols,
           "(no FAT32 volume found)")
        ck("...and the volume label came off the disk",
           "ARGO" in vols, "(label not read)")

        ls = read_cmd(q, "fat32 ls 0", pages=20)
        ck("the root directory lists the file",
           "ARGO.TXT" in ls, "(file not listed)")
        ck("...and the subdirectory", "DOCS" in ls)

        # THE ACCEPTANCE CHECK. Everything above could pass on a stick
        # whose descriptors were read and whose data never was.
        cat = read_cmd(q, "fat32 cat 0:ARGO.TXT", pages=20)
        ck("A FILE IS READ OFF THE STICK",
           MARKER.split(" over ")[0] in cat,
           "(the file's contents never arrived)")

        sub = read_cmd(q, "fat32 ls 0:DOCS", pages=20)
        ck("a subdirectory on it is readable too",
           "NOTES.TXT" in sub, "(subdirectory unreadable)")
    finally:
        try:
            q.stop()
        except Exception:
            pass
        kill_stale()


def read_cmd(q, cmd, pages=6):
    """Type a command, page through it, return everything that was on
    screen on the way.

    Paged with SPACE only, and paged until the pager says it is done
    rather than a fixed number of times. The fixed count was wrong: the
    boot log is long enough that ten pages did not reach the end of it,
    so the driver's own "USB MSC" line -- which is near the bottom --
    was never on any screen this function looked at, and two checks
    failed on a run where everything had worked.

    SPACE only is not a preference. Every other key is a command name at
    a prompt, and `q` in particular quits the shell to the desktop.

    The key is "spc". It is QEMU's own QKeyCode name and the whole of
    harness/qemu.py's _MAP uses it; "space" is not a QKeyCode, so
    send-key with it does nothing at all. The first version of this file
    used "space" and therefore never paged: every capture was the same
    first screen ten times over, and two checks failed on a run where
    the driver had worked perfectly. A key name that is silently ignored
    is worse than one that raises.
    """
    q.key("ret")
    time.sleep(1)
    q.line(cmd)
    time.sleep(4)
    out = []
    for _ in range(pages):
        screen = q.fbtext()
        out += screen
        if not any("-- more --" in l for l in screen):
            break
        time.sleep(0.8)
        q.key("spc")
    out += q.fbtext()
    # The run of spaces left a blank line at the prompt; Enter discards
    # it, so the next command starts from a clean one.
    q.key("ret")
    return "\n".join(out)


def main():
    which = (sys.argv[1] if len(sys.argv) > 1 else "both").lower()
    if not os.path.exists(ISO):
        print("no %s -- run make first" % ISO)
        return 2
    if not need("qemu-system-i386", "mkfs.vfat", "mcopy", "mmd", "dd"):
        return 0

    print("Reading a file off a USB stick")
    print("==============================")
    stick = make_stick(os.path.join(WORK, "stick.img"))
    print("  stick: 512 MB, MBR, one FAT32 partition at LBA 2048")
    print("  file:  /ARGO.TXT and /DOCS/NOTES.TXT\n")

    # The hub case first: it is where a stick in the side of a laptop
    # actually is, and 0.8.5 could not see it at all.
    if which in ("xhci", "both"):
        run_case("xhci", True, stick)
        run_case("xhci", False, stick)
    if which in ("uhci", "both"):
        run_case("uhci", True, stick)
        run_case("uhci", False, stick)

    print("\n%d/%d passed" % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
