#!/usr/bin/env python3
"""Refuse to ship an ISO whose kernel disagrees with its version marker.

An ISO carries the version twice: as text in /version.txt, which the update
manager reads to decide whether an upgrade is on offer, and compiled into
/boot/havendos.elf, which is what the machine actually reports once it boots.

Those two came from different halves of the build. version.txt is GENERATED
from include/version.h on every build. The kernel is COMPILED, and until
v0.7.20 nothing in the Makefile made a version change recompile anything.

So a version bump produced an ISO that ANNOUNCED v0.7.20 and CONTAINED
v0.7.19. The update manager then behaved perfectly: it read v0.7.20, offered
the upgrade, copied the kernel, verified the write, and reported success --
and the machine rebooted into the old version. Nothing in the process was
capable of noticing, because every part of it did its own job correctly.

This is the part that notices. It runs at the end of every ISO build.

    python3 tools/check_iso_version.py havendos.iso
"""
import os
import re
import subprocess
import sys
import tempfile


def extract(iso, out):
    """Unpack the ISO. xorriso first, then 7z, then a loopback mount."""
    for cmd in (["xorriso", "-osirrox", "on", "-indev", iso, "-extract", "/", out],
                ["7z", "x", "-y", "-o" + out, iso]):
        try:
            r = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=180)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0 and os.path.exists(os.path.join(out, "version.txt")):
            return True
    return False


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
    if not os.path.exists(iso):
        print("check_iso_version: no %s to check" % iso)
        return 0

    tmp = tempfile.mkdtemp(prefix="isover")
    if not extract(iso, tmp):
        # Not fatal: a machine without xorriso or 7z can still build. Say so
        # loudly rather than implying the check passed.
        print("check_iso_version: WARNING - cannot unpack %s, version NOT "
              "verified (install xorriso or p7zip)" % iso)
        return 0

    vpath = os.path.join(tmp, "version.txt")
    kpath = os.path.join(tmp, "boot", "havendos.elf")
    try:
        with open(vpath, encoding="utf-8", errors="replace") as f:
            marker = f.read().strip().lstrip("vV")
    except OSError:
        print("check_iso_version: %s has no version.txt" % iso)
        return 1
    try:
        with open(kpath, "rb") as f:
            kernel = f.read()
    except OSError:
        print("check_iso_version: %s has no /boot/havendos.elf" % iso)
        return 1

    # The kernel embeds its version as a plain string. Check the marker is in
    # there, byte for byte, with a word boundary so 0.7.2 does not match
    # inside 0.7.20.
    present = re.search(rb"(?<![\d.])" + re.escape(marker.encode())
                        + rb"(?![\d.])", kernel) is not None

    # For the failure message, only HavenDOS-shaped versions. A plain
    # \d+\.\d+\.\d+ sweep also finds every IP address and library version in
    # the binary, which buries the answer in noise.
    found = sorted(set(m.decode() for m in
                       re.findall(rb"(?<![\d.])0\.\d+\.\d+(?![\d.])", kernel)))
    if present:
        print("check_iso_version: ok, kernel and version.txt both say %s"
              % marker)
        return 0

    print("")
    print("=" * 62)
    print("  BUILD REFUSED: the ISO and its kernel disagree")
    print("=" * 62)
    print("  %s says:        %s" % (os.path.basename(vpath), marker))
    print("  the kernel contains: %s"
          % (", ".join(found) if found else "no version string at all"))
    print("")
    print("  An ISO like this passes every check the update manager makes")
    print("  and still leaves the machine on the old version: it reads the")
    print("  marker, installs the kernel, and the kernel is the old one.")
    print("")
    print("  Usually this means the kernel was not recompiled after")
    print("  include/version.h changed. Try:")
    print("")
    print("      make clean && make")
    print("")
    return 1


if __name__ == "__main__":
    sys.exit(main())
