#!/usr/bin/env python3
"""check_blk_dispatch.py -- one file chooses the bus a sector goes to.

── WHY THIS EXISTS ────────────────────────────────────────────────────

tools/check_bus_label.py has guarded the bus NAME since v0.8.3.1, after
the installer's picker said NVMe and its confirmation screen said VirtIO.
The label was the symptom. Nothing guarded the thing the label described.

So when v0.8.6 came to add USB mass storage -- the fifth bus -- a grep
for "every place that chooses one", which is this tree's own rule when
adding a bus, found THREE sector-dispatch switches:

    shell/iso_installer.c   disk_read/disk_write     VirtIO ATA AHCI NVMe
    fs/fat16.c              fat16_raw_read/write     VirtIO ATA AHCI NVMe
    fs/fat32.c              local_read/local_write   VirtIO ATA

fs/fat32.c could not see an NVMe or an AHCI disk AT ALL -- which is to
say fat32_init() returned "no disk" on every laptop made this decade --
two releases after "the installer had one block layer and the filesystem
had another, and only one of them got taught about NVMe" was written down
as the lesson of v0.8.3.1. The lesson was recorded in a comment. The
third copy was never looked at.

Their SCAN ORDERS disagreed too: the installer registered VirtIO, NVMe,
AHCI, ATA and fs/fat16.c probed VirtIO, ATA, AHCI, NVMe, so on a machine
where both the NVMe and the legacy ATA ports answer, the installer called
the NVMe disk 0 and the filesystem bound itself to the ATA one. Nothing
on any screen could tell you.

Four releases of being careful did not work. One grep does.

── WHAT IT REFUSES ───────────────────────────────────────────────────

A call to any driver's raw sector function from outside fs/blkdev.c and
outside the driver that owns it. Those functions are the dispatch: a file
that calls one has chosen a bus, and only one file is allowed to.

Everything above the block layer calls blk_read()/blk_write() and names
no bus at all -- which is the whole point of the layer, and the thing
three separate switches got wrong.

A line may opt out with `blk-dispatch-ok: <reason>` in a comment on it or
within the six above, WITH a reason. A bare marker is how an exemption
becomes a habit.

Run:  python3 tools/check_blk_dispatch.py
"""
import os
import re
import sys

OWNER = "fs/blkdev.c"

# The raw per-bus sector functions. Calling one of these IS choosing a bus.
FUNCS = (
    "virtio_blk_read", "virtio_blk_write",
    "virtio_drive_read", "virtio_drive_write",
    "nvme_read", "nvme_write",
    "ahci_read_sector", "ahci_write_sector",
    "ata_read_sector", "ata_write_sector",
    "ata_read_sector48", "ata_write_sector48",
    "msc_read_sector", "msc_write_sector",
)

# A driver may call its own, and a few files are the drivers themselves or
# sit below the block layer entirely.
EXEMPT = {
    OWNER,
    "drivers/virtio.c", "drivers/nvme.c", "drivers/ahci.c",
    "drivers/ata.c", "drivers/atapi.c",
    "usb/msc.c",
    # fs/fat12.c is the FAT12 floppy driver and predates every bus but
    # ATA. It is ATA-only by design -- a floppy is not on a hub and not
    # on NVMe -- and porting it is not what 0.8.6 is for. Listed rather
    # than left to trip this check, so the exception is a decision and
    # not an oversight.
    "fs/fat12.c",
}

# What counts as a reason after the marker lives in
# tools/csource.py, beside the comment-stripping, for the reason
# its own header gives: all three checks tested it with `\s*\S`,
# and in C the character after a bare marker is the `*` of the
# `*/` that closes the comment. A bare marker silenced all three.

# Telling code from comments lives in tools/csource.py, once. The first
# version of this file did it here, and tools/check_bus_label.py did it
# differently and worse -- which is how a word in a file-header comment
# came to be reported as a bus label in v0.8.6.1.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import code_lines, has_reason                    # noqa: E402


def main():
    if not os.path.isdir("shell"):
        sys.stderr.write("run me from the source root\n")
        return 2
    if not os.path.exists(OWNER):
        print("check_blk_dispatch: %s is missing" % OWNER)
        return 1

    call = re.compile(r"\b(%s)\s*\(" % "|".join(FUNCS))
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
            if rel in EXEMPT:
                continue
            checked += 1
            raw = open(os.path.join(root, fn), encoding="utf-8",
                       errors="replace").read()
            lines = raw.splitlines()
            code = code_lines(raw, strip_externs=True)
            for n, line in enumerate(lines, 1):
                code_line = code[n - 1] if n <= len(code) else ""
                m = call.search(code_line)
                if not m:
                    continue
                window = "\n".join(lines[max(0, n - 7):n])
                if has_reason(window, "blk-dispatch-ok"):
                    continue
                bad.append((rel, n, m.group(1), line.strip()))

    if bad:
        print("check_blk_dispatch: a bus is chosen outside %s\n" % OWNER)
        for rel, n, f, line in bad:
            print("  %s:%d  %s()" % (rel, n, f))
            print("      %s" % line[:72])
        print("\n  Use blk_read(i, lba, buf) / blk_write(i, lba, buf) from")
        print("  include/blkdev.h. They take a disk index, not a bus, and")
        print("  the read-only refusal is below every caller so a new code")
        print("  path cannot reach an unwritable disk by forgetting to ask.")
        print("\n  Three files used to do this independently and one of them")
        print("  had never heard of NVMe or AHCI. If this call genuinely")
        print("  belongs below the block layer, add the file to EXEMPT here")
        print("  and say why, or mark the line `blk-dispatch-ok: <reason>`.")
        return 1

    print("check_blk_dispatch: ok, %d files choose no bus; %s owns the "
          "dispatch for %d raw entry points"
          % (checked, OWNER, len(FUNCS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
