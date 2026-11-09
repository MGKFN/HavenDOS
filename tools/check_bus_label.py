#!/usr/bin/env python3
"""check_bus_label.py -- one function names a bus, and only one.

This check exists because the same bug shipped four times.

A disk is on one of four buses and a person needs to be told which.
Working that out is three lines, so every screen that needed it wrote
those three lines, and they disagreed:

  v0.8.3.1  the disk picker said "NVMe" and the confirmation screen --
            the one gated behind typing YES -- said "VirtIO". Three
            screens had worked it out independently and two had never
            heard of NVMe. Fixed by putting bus_name() "in one place".
  v0.8.5.3  that one place was a static function inside
            shell/iso_installer.c, so shell/desktop.c could not call it
            and had its own: `inst_disk_is_ata(i) ? "ATA/SATA" :
            "VirtIO"`. On a laptop with one NVMe disk the installer
            announced "(VirtIO)" on the confirmation screen. The welcome
            screen, meanwhile, tested two booleans and printed "WARNING:
            No disks found" directly above "1 VirtIO disk found -- will
            be auto-selected", both from the same scan.

A comment saying "everything asks here now" did not survive contact with
a new file. This does: the words themselves are the thing being guarded.

── v0.8.6: THE OWNER MOVED, AND WHY THAT WAS THE REAL PROBLEM ─────────

The owner was shell/iso_installer.c, and naming the bus in one place was
never the whole bug. When 0.8.6 came to add USB mass storage -- the fifth
bus -- a grep for every place that CHOOSES one found three sector
dispatch switches holding four, four and TWO buses: fs/fat32.c could not
see an NVMe or an AHCI disk at all. The label was guarded and the thing
the label described was not.

The owner is fs/blkdev.c now, and the words live one per bus in that
bus's own ops table -- a shape this check can guard and a C compiler can
see all of at once. "USB" joins the list in the same release that adds
it, rather than two releases after somebody notices.

tools/check_blk_dispatch.py is the other half: it guards the DISPATCH,
which is what this check could never see.

Run:  python3 tools/check_bus_label.py
"""
import os
import re
import sys

# Telling code from comments lives in tools/csource.py, once. This file
# used to do it with `line.split("/*")[0]`, which handles a comment
# opened and closed on one line and nothing else -- so in v0.8.6.1 it
# reported the word "USB" in shell/fat16cmd.c's opening paragraph, a
# comment explaining why the command exists. A false positive is not a
# checker being careful: it teaches whoever hits it to add an exemption,
# and an exemption added to silence a bug in the checker is one that
# will hide a real finding later.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import code_lines, has_reason                    # noqa: E402

# The one function allowed to produce these words, and the file it is in.
OWNER = "fs/blkdev.c"
WORDS = ("VirtIO", "NVMe", "SATA", "AHCI", "IDE", "ATA/SATA", "USB")

# Files that legitimately mention a bus for reasons other than labelling a
# disk for a person: driver banners for their own hardware, and the kernel
# log lines that name the controller they just brought up.
EXEMPT = {
    "drivers/nvme.c", "drivers/ahci.c", "drivers/ata.c", "drivers/virtio.c",
    "drivers/pci.c", "drivers/pci_bar.c", "drivers/uhci.c",
    "usb/xhci_hc.c", "usb/uhci_hc.c", "usb/core.c",
    "kernel/main.c",          # the boot log names each controller it finds
    "shell/lspci.c",          # describes PCI devices, not installer disks
    "shell/neofetch.c",       # reports the booted system's own disk
    "fs/fat16.c", "fs/install.c", "fs/update.c", "fs/update_src.c",
    # v0.8.6: "USB" is in WORDS now, so every file that names the BUS it
    # drives rather than labelling a disk has to be listed. These are
    # drivers and shells talking about USB hardware, not about a disk.
    "usb/msc.c", "usb/hub.c", "usb/hid.c", "shell/usb.c",
    "drivers/keyboard.c",     # "USB HID input disabled by nousbkbd"
    "fs/esp.c", "fs/blkdev.c",
    "shell/shell.c", "shell/help.c", "shell/man.c", "shell/legal.c",
    "shell/chat.c", "shell/packages.c", "shell/bpkg.c",
}

# A single line may opt out with `bus-label-ok: <reason>` in a comment on
# or just above it -- for a row that reports a DRIVER rather than labelling
# a disk. The reason is required.

def main():
    if not os.path.isdir("shell"):
        sys.stderr.write("run me from the source root\n")
        return 2

    bad = []
    checked = 0
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "isodir", "tests", "harness",
                                "tools", "manual", "docs", "rescue",
                                "__pycache__")]
        for fn in files:
            if not fn.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(root, fn), ".").replace("\\", "/")
            if rel == OWNER or rel in EXEMPT:
                continue
            checked += 1
            text = open(os.path.join(root, fn), encoding="utf-8",
                        errors="replace").read()
            lines = text.splitlines()
            stripped = code_lines(text)
            for n, line in enumerate(lines, 1):
                code = stripped[n - 1] if n <= len(stripped) else ""
                if not any(('"%s"' % w) in code or ('"%s ' % w) in code
                           for w in WORDS):
                    continue
                # Per-line opt-out, the same convention make_version.py uses
                # for its version literals. It must appear on the line or
                # within the six above it, WITH a reason after the colon --
                # a bare marker is how an exemption becomes a habit. Six,
                # because the reason is usually worth a sentence and a
                # four-line window silently missed one.
                window = "\n".join(lines[max(0, n - 7):n])
                if has_reason(window, "bus-label-ok"):
                    continue
                for w in WORDS:
                    if '"%s"' % w in code or '"%s ' % w in code:
                        bad.append((rel, n, w, line.strip()))

    if bad:
        print("check_bus_label: a bus name is written outside %s\n" % OWNER)
        for rel, n, w, line in bad:
            print("  %s:%d  %s" % (rel, n, w))
            print("      %s" % line[:72])
        print("\n  Use blk_bus_name(i) from include/blkdev.h.")
        print("  It is the only thing in this tree that turns a disk into one")
        print("  of those words, and it knows every bus, because each bus's")
        print("  word lives in that bus's own ops table in %s." % OWNER)
        print("  If this file genuinely names a bus for some other reason --")
        print("  a driver banner, a boot-log line -- add it to EXEMPT here")
        print("  and say why.")
        return 1

    print("check_bus_label: ok, %d files carry no bus-name literal; "
          "%s owns them" % (checked, OWNER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
