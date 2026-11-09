#!/usr/bin/env python3
"""make_exfat_fixture.py -- build the image the exFAT suite runs against.

WHY AN IMAGE AND NOT A HAND-BUILT STRUCTURE

A 64 GB USB stick ships formatted exFAT. v0.8.6.1 taught `fat32` to NAME
one rather than print "none found" -- which was the honest thing to do
with no driver -- and from then until v0.8.8.1 that is all HavenDOS
could do with one: say what it was and refuse it. "USB drives still not
always recognised", 31 October, and this is what it was.

So the suite has to prove a reader against a volume SOMEBODY ELSE
wrote. The same rule the FAT32 fixture is built on, and for a sharper
reason here: exFAT's directory entries are a SET -- a 0x85 file entry,
a 0xC0 stream extension and one or more 0xC1 name entries, with a
checksum over the whole set -- and a fixture built by this script from
the same reading of the specification as the driver would agree with
the driver perfectly and prove nothing. tests/test_usbmux.c passed 79
of 79 against a header the phone has never used, for exactly that
reason.

WHAT IT PRODUCES

  tests/exfat.img          64 MB, formatted by mkfs.exfat and populated
                           by the FUSE exFAT driver -- i.e. by two other
                           people's implementations
  tests/exfat_fixture.inc  the manifest: every file, its bytes or its
                           CRC, and the geometry mkfs ACTUALLY chose,
                           read back off the image rather than assumed

AND IT IS CHECKED BY fsck.exfat BEFORE IT IS SHIPPED TO THE SUITE. An
image this script believes is good is not evidence; one exfatprogs
calls clean is. If fsck refuses it, this script fails rather than
writing a fixture, because a suite that passes against a corrupt
volume is worse than no suite.

NEEDS: exfatprogs (mkfs.exfat, fsck.exfat), exfat-fuse
(mount.exfat-fuse), losetup and a writable /dev/loop. All four are in
Debian/Ubuntu; the Makefile target says so when one is missing rather
than producing an empty fixture.
"""
import os
import subprocess
import sys
import zlib
import struct

SECT = 512
IMG_MB = 64


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def need(tool):
    r = run(["sh", "-c", "command -v " + tool])
    if r.returncode != 0:
        sys.exit("make_exfat_fixture: %s is not installed.\n"
                 "  apt-get install -y exfatprogs exfat-fuse" % tool)


# ── the contents ────────────────────────────────────────────────────
#
# Each one is here because it exercises something a reader gets wrong:
#
#   README.TXT   a short file inside one cluster, read byte for byte
#   BIG.BIN      100000 bytes over several clusters: the cluster WALK,
#                which on exFAT may be a FAT chain or a contiguous run
#                depending on the NoFatChain bit, and a reader that
#                handles only one of the two works on about half of all
#                real volumes
#   SPARSE.DAT   3 MB of zeroes, long enough to cross whatever cluster
#                size mkfs picks for a 64 MB volume several times over
#   DOCS/...     a subdirectory, and a file two levels down
#   the long name  a name needing THREE 0xC1 entries (15 UTF-16 chars
#                each), which is where a reader that assumes one name
#                entry stops being right
def seeded(size, seed):
    out = bytearray(size)
    x = seed & 0xFFFFFFFF
    for i in range(size):
        x = (1103515245 * x + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


LONGNAME = "a-long-file-name-that-needs-several-name-entries.txt"

FILES = [
    ("README.TXT", b"HavenDOS exFAT fixture\nline two\n"),
    ("BIG.BIN", seeded(100000, 0x5EED)),
    ("SPARSE.DAT", b"\0" * 3000000),
    ("DOCS/NOTES.TXT", b"a file in a subdirectory\n"),
    ("DOCS/DEEP/NESTED.TXT", b"two levels down\n"),
    ("DOCS/" + LONGNAME, b"three name entries\n"),
    ("FRAG.BIN", seeded(2500000, 0xF1A6)),
]
DIRS = ["DOCS", "DOCS/DEEP"]

# ── AND ONE FILE THAT IS NOT CONTIGUOUS ─────────────────────────────
#
# The first version of this fixture produced "3 contiguous, 0
# FAT-chained", and the suite's check said "at least one is
# contiguous" and passed. Half the driver -- the FAT chain walk --
# was never executed, which is the EXACT hazard fs/exfat.c's header
# is about: a reader that follows the FAT works on a fragmented
# volume and walks garbage on a fresh one, and a fresh volume is all
# this fixture had.
#
# A writer sets NoFatChain for anything laid down in one go, so a
# fragmented file has to be MADE: fill the volume with blocks, punch
# a hole in the middle, and write a file too big for the hole. What
# comes out is a real fragmented file, allocated by somebody else's
# driver, with NoFatChain clear because it genuinely is not one run.
#
# It is done between the directories and the files above so that the
# holes are still there when FRAG.BIN is written.
FRAG_NAME = "FRAG.BIN"
FILLER_MB = 6
HOLE_AT = 2

# A directory that cannot fit in one cluster, and a file that is
# deleted but whose entries remain. Both exist because an injection
# harness proved the suite could not see their absence.
MANY_DIR = "MANY"
MANY_N   = 200
ZOMBIE   = "ZOMBIE.TMP"


def build(img):
    if os.path.exists(img):
        os.unlink(img)
    with open(img, "wb") as f:
        f.truncate(IMG_MB * 1024 * 1024)

    r = run(["mkfs.exfat", "-n", "HAVENTEST", img])
    if r.returncode != 0:
        sys.exit("make_exfat_fixture: mkfs.exfat failed:\n" + r.stderr)

    # A loop device, because the FUSE driver wants a block device.
    r = run(["losetup", "-f", "--show", img])
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit("make_exfat_fixture: losetup failed:\n" + r.stderr)
    loop = r.stdout.strip()
    mnt = img + ".mnt"
    os.makedirs(mnt, exist_ok=True)
    try:
        r = run(["mount.exfat-fuse", loop, mnt])
        if r.returncode != 0:
            sys.exit("make_exfat_fixture: mount.exfat-fuse failed:\n"
                     + r.stderr)
        try:
            for d in DIRS:
                os.makedirs(os.path.join(mnt, d), exist_ok=True)

            # Lay down filler, punch a hole, and let FRAG.BIN land in
            # it -- see the note on FRAG_NAME.
            for i in range(FILLER_MB):
                with open(os.path.join(mnt, "FILL%02d.TMP" % i), "wb") as f:
                    f.write(seeded(1024 * 1024, 0x1000 + i))
            os.unlink(os.path.join(mnt, "FILL%02d.TMP" % HOLE_AT))
            os.unlink(os.path.join(mnt, "FILL%02d.TMP" % (HOLE_AT + 2)))

            for name, data in FILES:
                with open(os.path.join(mnt, name), "wb") as f:
                    f.write(data)

            # The filler goes; the hole it left does not.
            for i in range(FILLER_MB):
                p = os.path.join(mnt, "FILL%02d.TMP" % i)
                if os.path.exists(p):
                    os.unlink(p)

            # ── A DIRECTORY BIGGER THAN ONE CLUSTER ──────────────────
            # A cluster here is 4096 bytes, which is 128 directory
            # entries, which is about forty files with ordinary names.
            # Every directory in the first version of this fixture fit
            # in one, so the code that follows a DIRECTORY's own FAT
            # chain never ran and an injection deleting it was MISSED.
            os.makedirs(os.path.join(mnt, MANY_DIR), exist_ok=True)
            for i in range(MANY_N):
                with open(os.path.join(mnt, MANY_DIR,
                                       "entry-%03d.txt" % i), "wb") as f:
                    f.write(b"%d\n" % i)

            # ── AND AN ENTRY SET THAT IS DELETED BUT STILL THERE ─────
            # Written LAST and removed, so its entries stay in the
            # directory with the in-use bit cleared rather than being
            # overwritten by something else. Without one, a reader
            # that ignores that bit lists exactly the same files and
            # the suite cannot tell.
            z = os.path.join(mnt, ZOMBIE)
            with open(z, "wb") as f:
                f.write(b"deleted before the image was closed\n")
            os.unlink(z)
        finally:
            run(["umount", mnt])

        # THE JUDGE. Not this script's opinion of the image.
        r = run(["fsck.exfat", loop])
        if r.returncode != 0:
            sys.exit("make_exfat_fixture: fsck.exfat refused the image "
                     "this script just wrote:\n" + r.stdout + r.stderr)
        print("  fsck.exfat: " + r.stdout.strip().splitlines()[-1])
    finally:
        run(["losetup", "-d", loop])
        try:
            os.rmdir(mnt)
        except OSError:
            pass


def geometry(img):
    """Read back what mkfs.exfat ACTUALLY chose.

    A suite that asserts the driver parsed the parameters this script
    passed to mkfs proves the driver agrees with this script. One that
    asserts it parsed what is in the boot sector on disk proves the
    driver agrees with mkfs.exfat, which is the claim worth making.
    """
    with open(img, "rb") as f:
        bs = f.read(512)
    g = {
        "fat_offset":    struct.unpack_from("<I", bs, 80)[0],
        "fat_length":    struct.unpack_from("<I", bs, 84)[0],
        "heap_offset":   struct.unpack_from("<I", bs, 88)[0],
        "cluster_count": struct.unpack_from("<I", bs, 92)[0],
        "root_cluster":  struct.unpack_from("<I", bs, 96)[0],
        "bps_shift":     bs[108],
        "spc_shift":     bs[109],
        "num_fats":      bs[110],
        "vol_length":    struct.unpack_from("<Q", bs, 72)[0],
    }
    if bs[3:11] != b"EXFAT   ":
        sys.exit("make_exfat_fixture: the image mkfs.exfat wrote does "
                 "not carry the exFAT signature")
    return g


def c_bytes(data):
    return ",".join(str(b) for b in data)


def emit(path, g, files):
    a = []
    w = a.append
    w("/* GENERATED by tools/make_exfat_fixture.py -- do not edit.")
    w("")
    w("   The geometry below was READ BACK off tests/exfat.img after")
    w("   mkfs.exfat wrote it, not passed to mkfs by this script. A")
    w("   suite that asserts the driver agrees with what we asked for")
    w("   proves only that two of our own files agree; one that asserts")
    w("   it agrees with what is on the disk proves it agrees with")
    w("   exfatprogs. */")
    w("")
    for k, v in sorted(g.items()):
        w("#define EXFIX_%-14s %uu" % (k.upper(), v))
    w("")
    w("typedef struct {")
    w("    const char    *path;      /* as the driver is asked for it */")
    w("    unsigned long  size;")
    w("    unsigned long  crc;       /* zlib crc32 of the contents    */")
    w("    const unsigned char *body;/* small files only, else 0      */")
    w("    unsigned long  body_len;")
    w("} exfix_file_t;")
    w("")
    for i, (name, data) in enumerate(files):
        if len(data) <= 256:
            w("static const unsigned char exfix_body_%d[] = { %s };"
              % (i, c_bytes(data)))
    w("")
    w("static const exfix_file_t EXFIX_FILES[] = {")
    for i, (name, data) in enumerate(files):
        if len(data) <= 256:
            w('    { "%s", %u, 0x%08xul, exfix_body_%d, %u },'
              % (name, len(data), zlib.crc32(data) & 0xFFFFFFFF, i,
                 len(data)))
        else:
            w('    { "%s", %u, 0x%08xul, 0, 0 },'
              % (name, len(data), zlib.crc32(data) & 0xFFFFFFFF))
    w("};")
    w("#define EXFIX_NFILES ((int)(sizeof(EXFIX_FILES)/sizeof(EXFIX_FILES[0])))")
    w("")
    w('#define EXFIX_LONGNAME "%s"' % LONGNAME)
    w("")
    w('#define EXFIX_MANY_DIR "%s"' % MANY_DIR)
    w("#define EXFIX_MANY_N   %d" % MANY_N)
    w('#define EXFIX_ZOMBIE   "%s"' % ZOMBIE)
    w("")
    open(path, "w", encoding="utf-8").write("\n".join(a) + "\n")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests"
    for t in ("mkfs.exfat", "fsck.exfat", "mount.exfat-fuse", "losetup"):
        need(t)
    img = os.path.join(out, "exfat.img")
    print("make_exfat_fixture: %s" % img)
    build(img)
    g = geometry(img)
    print("  geometry: %u-byte sectors, %u sectors/cluster, root at "
          "cluster %u, %u clusters"
          % (1 << g["bps_shift"], 1 << g["spc_shift"],
             g["root_cluster"], g["cluster_count"]))
    emit(os.path.join(out, "exfat_fixture.inc"), g, FILES)
    print("  manifest: %s  (%d files)"
          % (os.path.join(out, "exfat_fixture.inc"), len(FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
