#!/usr/bin/env python3
"""make_update_fixture.py -- build the EFI System Partition the update suite
runs against: one with a HavenDOS update staged on it, the way a person
would stage one from Windows with `mountvol`.

Same rule as make_esp_fixture.py and make_fat32_fixture.py: the volume is
formatted by mkfs.vfat and populated by mtools, so it is somebody else's
idea of a FAT32 volume rather than ours, and a suite that passes against it
has proved something about FAT32 rather than about fs/fat32.c's opinion of
itself.

The CRC in the manifest is computed with zlib.crc32 -- NOT with any HavenDOS
code.  fs/gpt.c's gpt_crc32() is the same CRC-32 (reflected 0xEDB88320), and
the suite asserts that the kernel's function agrees with Python's on the
real image.  A checksum only our own code can compute would be our code
agreeing with itself, which this project does not count as evidence.

Produces:

  tests/update_esp.img      48 MB FAT32 with a staged update on it
  tests/update_stick.img    48 MB exFAT with the SAME update staged
  tests/update_fixture.inc  sizes, CRCs and the manifest text, emitted by
                            the run that built the image

── WHY THERE IS AN exFAT ONE TOO, v0.8.8.2 ───────────────────────────

A USB stick of 32 GB or more is formatted exFAT by default, and until
v0.8.8.2 fs/update_src.c read FAT32 only -- so "USB discovery for
32 GB drives still broken for updating" was reported four releases
running and was this, every time.

The exFAT image is formatted by mkfs.exfat, populated through the FUSE
driver and judged by fsck.exfat, exactly as tools/make_exfat_fixture.py
does it: three other people's implementations, so a suite that passes
against it has proved something about exFAT rather than about
fs/exfat.c's opinion of itself.

IT CARRIES THE SAME MANIFEST AND THE SAME KERNEL BYTES as the FAT32
image, on purpose. The suite can then assert that the update produced
from an exFAT stick is byte-for-byte the one produced from a FAT32
stick -- which is the actual claim, and is stronger than "the exFAT
path returned 1".
"""
import os
import subprocess
import sys
import tempfile
import zlib

SECT = 512
VERSION = "0.8.9"          # deliberately ahead of any real release, so the
                           # suite's "is it newer" checks do not go stale
                           # the next time the tree's own version moves
KERNEL_NAME = "havendos.elf"
KERNEL_SIZE = 393_216      # 384 KB -- several clusters, several FAT entries


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write("make_update_fixture: failed: %s\n%s%s\n"
                         % (" ".join(cmd), r.stdout, r.stderr))
        sys.exit(1)
    return r.stdout


def need(tool):
    from shutil import which
    if which(tool) is None:
        sys.stderr.write(
            "make_update_fixture: %s is not installed.\n"
            "  The update suite tests against a volume this project did not\n"
            "  format, so the formatter is a build dependency:\n"
            "      apt-get install dosfstools mtools\n" % tool)
        sys.exit(1)


def body(seed, size):
    """The same deterministic rule the other fixtures use, with an ELF
    magic glued on the front -- the staged image has to look like a kernel
    because fs/update.c checks those four bytes before it writes."""
    out = bytearray(size)
    x = (seed * 2654435761) & 0xFFFFFFFF
    for i in range(size):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    out[0:4] = b"\x7fELF"
    return bytes(out)


def manifest_text(version, kernel, size, crc):
    """Written the way a person would write it, including the CRLF line
    endings Notepad produces, because that is where this file comes from."""
    return ("HAVENDOS-UPDATE 1\r\n"
            "# staged from Windows with mountvol\r\n"
            "version=%s\r\n"
            "size=%u\r\n"
            "crc32=%08x\r\n"
            "kernel=%s\r\n" % (version, size, crc, kernel))


def geometry(img):
    with open(img, "rb") as f:
        b = f.read(SECT)
    u16 = lambda o: b[o] | (b[o + 1] << 8)
    u32 = lambda o: (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24))
    g = {
        "bytes_per_sect":  u16(11),
        "sects_per_clust": b[13],
        "rsvd_sects":      u16(14),
        "num_fats":        b[16],
        "fat_sz":          u16(22) or u32(36),
        "total_sects":     u16(19) or u32(32),
        "root_clus":       u32(44),
    }
    g["cluster_bytes"] = g["sects_per_clust"] * g["bytes_per_sect"]
    return g


def build(img, mb, dirs, files, label="SYSTEM"):
    if os.path.exists(img):
        os.unlink(img)
    with open(img, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    # No partition table: this image IS the partition, the way an ESP is.
    run(["mkfs.vfat", "-F", "32", "-n", label, img])
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    for d in dirs:
        run(["mmd", "-i", img, "::" + d.lstrip("/")], env=env)
    with tempfile.TemporaryDirectory() as td:
        local = os.path.join(td, "f.bin")
        for p, data in files:
            with open(local, "wb") as f:
                f.write(data)
            run(["mcopy", "-i", img, "-o", local, "::" + p.lstrip("/")], env=env)


def cstr(s):
    """A C string literal, with CR and LF spelled out so the fixture is
    readable and so nobody's editor can silently normalise the line endings
    this file exists to exercise."""
    out = []
    for ch in s:
        if ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n\"\n    \"")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def emit(path, g, kernel, size, crc, text):
    L = []
    a = L.append
    a("/* GENERATED by tools/make_update_fixture.py -- do not edit.")
    a(" *")
    a(" * The manifest of tests/update_esp.img, emitted by the run that")
    a(" * built it.  UPD_FX_CRC is computed by python3's zlib.crc32, not by")
    a(" * any HavenDOS code: the suite asserts gpt_crc32() agrees with it.")
    a(" */")
    a("")
    a('#define UPD_FX_IMG      "tests/update_esp.img"')
    a('#define UPD_FX_DIR      "/EFI/HAVENDOS/update"')
    a('#define UPD_FX_MANIFEST "/EFI/HAVENDOS/update/update.inf"')
    a('#define UPD_FX_KERNEL   "/EFI/HAVENDOS/update/%s"' % kernel)
    a('#define UPD_FX_NAME     "%s"' % kernel)
    a('#define UPD_FX_VERSION  "%s"' % VERSION)
    a("#define UPD_FX_SIZE     %uu" % size)
    a("#define UPD_FX_CRC      0x%08Xu" % crc)
    a("#define UPD_FX_SEED     %u" % 1)
    a("#define UPD_FX_CLUSTER  %u" % g["cluster_bytes"])
    a("")
    a("/* The manifest exactly as it was written onto the volume, CRLF and")
    a("   all -- the suite parses this copy natively as well as reading the")
    a("   real one back off FAT32, and the two must agree. */")
    a("static const char UPD_FX_TEXT[] =")
    a("    " + cstr(text) + ";")
    a("")
    a("/* The deterministic body rule, so the suite can regenerate the")
    a("   staged kernel's bytes without the fixture shipping 384 KB of")
    a("   them.  Byte 0..3 are overwritten with ELF magic, as they are in")
    a("   the image. */")
    a("static void upd_fx_body(unsigned seed, unsigned char *out, unsigned n){")
    a("    unsigned x = (unsigned)(seed * 2654435761u);")
    a("    for(unsigned i = 0; i < n; i++){")
    a("        x = x * 1103515245u + 12345u;")
    a("        out[i] = (unsigned char)((x >> 16) & 0xFF);")
    a("    }")
    a("    if(n >= 4){ out[0]=0x7F; out[1]='E'; out[2]='L'; out[3]='F'; }")
    a("}")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def build_exfat(img, mb, dirs, files):
    """The same staged layout, on a volume mkfs.exfat wrote.

    Same shape as tools/make_exfat_fixture.py's build(): format with
    mkfs.exfat, populate through mount.exfat-fuse, and let fsck.exfat
    decide whether the result is a valid volume. If fsck refuses it
    this script fails, rather than handing the suite an image that
    only HavenDOS thinks is well formed.
    """
    for t in ("mkfs.exfat", "fsck.exfat", "mount.exfat-fuse", "losetup"):
        need(t)
    if os.path.exists(img):
        os.unlink(img)
    with open(img, "wb") as f:
        f.truncate(mb * 1024 * 1024)

    r = subprocess.run(["mkfs.exfat", "-n", "HAVENUPD", img],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("make_update_fixture: mkfs.exfat failed:\n" + r.stderr)

    r = subprocess.run(["losetup", "-f", "--show", img],
                       capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        sys.exit("make_update_fixture: losetup failed:\n" + r.stderr)
    loop = r.stdout.strip()
    mnt = img + ".mnt"
    os.makedirs(mnt, exist_ok=True)
    try:
        r = subprocess.run(["mount.exfat-fuse", loop, mnt],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("make_update_fixture: mount.exfat-fuse failed:\n"
                     + r.stderr)
        try:
            for d in dirs:
                os.makedirs(os.path.join(mnt, d.lstrip("/")), exist_ok=True)
            for path, data in files:
                full = os.path.join(mnt, path.lstrip("/"))
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "wb") as f:
                    f.write(data)
        finally:
            subprocess.run(["umount", mnt], capture_output=True, text=True)

        r = subprocess.run(["fsck.exfat", loop], capture_output=True,
                           text=True)
        if r.returncode != 0:
            sys.exit("make_update_fixture: fsck.exfat refused the image "
                     "this script just wrote:\n" + r.stdout + r.stderr)
        print("  fsck.exfat: " + r.stdout.strip().splitlines()[-1])
    finally:
        subprocess.run(["losetup", "-d", loop], capture_output=True,
                       text=True)
        try:
            os.rmdir(mnt)
        except OSError:
            pass


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "tests"
    for t in ("mkfs.vfat", "mcopy", "mmd"):
        need(t)

    kernel = body(1, KERNEL_SIZE)
    crc = zlib.crc32(kernel) & 0xFFFFFFFF
    text = manifest_text(VERSION, KERNEL_NAME, KERNEL_SIZE, crc)

    dirs = ["/EFI", "/EFI/BOOT", "/EFI/HAVENDOS", "/EFI/HAVENDOS/update",
            # A DIRECTORY sitting where a kernel would be, so the suite
            # can point a manifest at it. stat() succeeds for a
            # directory and its clusters are entry records, so a reader
            # that does not check fills the staging buffer with 32-byte
            # structures and checksums them. There was no directory in
            # the update folder before v0.8.8.2 and the injection for
            # that was therefore uncatchable.
            "/EFI/HAVENDOS/update/notakernel",
            "/EFI/Microsoft", "/EFI/Microsoft/Boot"]
    files = [
        ("/EFI/HAVENDOS/update/update.inf", text.encode("ascii")),
        ("/EFI/HAVENDOS/update/" + KERNEL_NAME, kernel),
        # A Windows file, so the suite can assert an update reads the volume
        # and changes nothing on it -- the ESP is mounted read only and the
        # claim is worth proving rather than asserting.
        ("/EFI/Microsoft/Boot/bootmgfw.efi", body(9, 65_536)),
        ("/EFI/BOOT/BOOTX64.EFI", body(7, 32_768)),
    ]

    img = os.path.join(out_dir, "update_esp.img")
    build(img, 48, dirs, files)
    g = geometry(img)
    emit(os.path.join(out_dir, "update_fixture.inc"),
         g, KERNEL_NAME, KERNEL_SIZE, crc, text)

    # The same update, on an exFAT stick. Same manifest, same kernel
    # bytes, so the suite can demand the two paths agree.
    stick = os.path.join(out_dir, "update_stick.img")
    build_exfat(stick, 48, dirs, files)
    with open(os.path.join(out_dir, "update_fixture.inc"), "a") as f:
        f.write("\n/* The exFAT stick carrying the SAME staged update.\n"
                "   v0.8.8.2: a 32 GB stick is exFAT by default, and the\n"
                "   update source read FAT32 only until this release. */\n")
        f.write('#define UPD_FX_STICK    "%s"\n' % stick)

    print("make_update_fixture: %s  %d MB, %u-byte clusters"
          % (img, os.path.getsize(img) // (1024 * 1024), g["cluster_bytes"]))
    print("make_update_fixture: staged %s v%s  %u bytes  crc32=%08x"
          % (KERNEL_NAME, VERSION, KERNEL_SIZE, crc))


if __name__ == "__main__":
    main()
