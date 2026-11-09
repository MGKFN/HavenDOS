#!/usr/bin/env python3
"""make_iso_manifest.py -- write isodir/update.inf beside the kernel.

From v0.8.4 the CD-ROM update source reads a manifest too, so an update
applied from a disc gets the same checksum verification an update applied
from the EFI System Partition gets.  An ISO built before 0.8.4 has no
/update.inf, and fs/update_src.c synthesises one from /version.txt and the
directory entry instead -- with have_crc left at zero, because there is no
checksum to be had from one of those and saying otherwise would be
inventing output.

The CRC is zlib's, which is the reflected-0xEDB88320 CRC-32 that
fs/gpt.c's gpt_crc32() computes, so the value here can be checked by
anybody with python3 and no HavenDOS source.

    python3 tools/make_iso_manifest.py isodir/boot/havendos.elf \\
                                       include/version.h isodir/update.inf
"""
import os
import re
import sys
import zlib


def read_version(header):
    src = open(header, encoding="utf-8").read()
    m = re.search(r'^#define\s+HAVENDOS_VER_NUM\s+"([^"]+)"', src, re.M)
    if not m:
        sys.exit("make_iso_manifest: no HAVENDOS_VER_NUM in %s" % header)
    return m.group(1)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: make_iso_manifest.py <kernel.elf> <version.h> <out.inf>")
    kernel, header, out = sys.argv[1], sys.argv[2], sys.argv[3]

    data = open(kernel, "rb").read()
    if data[:4] != b"\x7fELF":
        sys.exit("make_iso_manifest: %s is not an ELF -- refusing to describe it"
                 % kernel)

    ver = read_version(header)
    crc = zlib.crc32(data) & 0xFFFFFFFF

    # LF line endings here, CRLF when a person writes one on Windows; the
    # parser tolerates both and tests/test_update.c proves it.
    text = ("HAVENDOS-UPDATE 1\n"
            "# Written by tools/make_iso_manifest.py at build time.\n"
            "version=%s\n"
            "size=%u\n"
            "crc32=%08x\n"
            "kernel=%s\n" % (ver, len(data), crc, os.path.basename(kernel)))
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write(text)

    print("make_iso_manifest: %s -> v%s, %u bytes, crc32=%08x"
          % (out, ver, len(data), crc))


if __name__ == "__main__":
    main()
