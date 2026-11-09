#!/usr/bin/env python3
"""gptwrite_real.py -- run fs/gpt_write.c against a partition table that
sgdisk built, and have sgdisk tell us what it thinks afterwards.

tests/test_gptwrite.c checks the writer against a table this project also
wrote. That proves it is self-consistent, which is not the claim anybody
needs before pointing it at their own laptop. The claim that matters is
that a REAL partition tool, which knows the specification and did not
learn it from us, looks at the disk afterwards and finds nothing wrong.

So this:

  1. builds a sparse 512 GB image and has **sgdisk** lay down a Windows 11
     partition layout on it -- ESP, Microsoft Reserved, a ~510 GB basic
     data volume and a recovery partition -- leaving a 1 GB gap where
     Disk Management would leave one after you shrink C:
  2. records the SHA-256 of every byte of the image
  3. runs the real fs/gpt_write.c against that file, adding a partition in
     the gap
  4. asks **sgdisk --verify** and **parted print** what they make of it
  5. checks that every one of the original partitions still has exactly
     the type, start and end sgdisk gave it
  6. checks that the only bytes that changed anywhere in 512 GB are the
     four sectors the writer said it would touch

Step 6 is the one worth reading twice. It is not "the partitions look
fine": it is a byte-level diff of the entire disk against its own
before-image.

    python3 harness/gptwrite_real.py
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SECT = 512
DISK_SECTORS = 1073741824          # 512 GB
IMG = "/tmp/hd_gptreal.img"

fails = []


def check(what, ok, detail=""):
    print("  %-62s %s" % (what, "ok" if ok else "FAIL"))
    if not ok:
        fails.append((what, detail))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def need(tool):
    if shutil.which(tool) is None:
        sys.exit("gptwrite_real: %s is not installed (apt-get install gdisk parted)"
                 % tool)


# ── the C harness that drives the real driver against the file ──────────
HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define _FILE_OFFSET_BITS 64

void *memset(void *d, int c, uint32_t n){
    unsigned char *p=(unsigned char*)d; while(n--) *p++=(unsigned char)c; return d; }
void *memcpy(void *d, const void *s, uint32_t n){
    unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s;
    while(n--) *a++=*b++; return d; }
int memcmp(const void *a, const void *b, uint32_t n){
    const unsigned char *x=a,*y=b; while(n--){ if(*x!=*y) return (int)*x-(int)*y; x++; y++; } return 0; }

#include "../fs/gpt.c"
#include "../fs/gpt_write.c"

static FILE *g_f;
static int rd(void *ctx, uint64_t lba, void *buf){
    (void)ctx;
    if(fseeko(g_f, (off_t)lba*512, SEEK_SET)) return -1;
    size_t n = fread(buf, 1, 512, g_f);
    if(n == 0){ memset(buf, 0, 512); return 0; }   /* sparse tail reads as zeros */
    if(n < 512) memset((char*)buf+n, 0, (uint32_t)(512-n));
    return 0;
}
static int wr(void *ctx, uint64_t lba, const void *buf){
    (void)ctx;
    if(fseeko(g_f, (off_t)lba*512, SEEK_SET)) return -1;
    if(fwrite(buf, 1, 512, g_f) != 512) return -1;
    fflush(g_f);
    return 0;
}

int main(int argc, char **argv){
    if(argc < 4){ fprintf(stderr, "usage: %s img first last [--dry]\n", argv[0]); return 2; }
    g_f = fopen(argv[1], "r+b");
    if(!g_f){ perror("open"); return 2; }
    uint64_t a = strtoull(argv[2], 0, 10), b = strtoull(argv[3], 0, 10);
    int dry = (argc > 4);
    uint64_t w[8]; int nw = 0, idx = -1;
    int r = gpt_add_partition(rd, dry ? 0 : wr, 0, a, b,
                              GPT_TYPE_HAVENDOS, "HavenDOS", &idx, w, &nw);
    printf("result %d %s\n", r, gpt_write_strerror(r));
    printf("slot %d\n", idx);
    printf("writes %d\n", nw);
    for(int i=0;i<nw;i++) printf("lba %llu\n", (unsigned long long)w[i]);
    fclose(g_f);
    return r == GPTW_OK ? 0 : 1;
}
"""


def sector_hashes(path, lbas):
    """SHA-256 of specific sectors."""
    out = {}
    with open(path, "rb") as f:
        for l in lbas:
            f.seek(l * SECT)
            out[l] = hashlib.sha256(f.read(SECT)).hexdigest()
    return out


def whole_image_sectors(path):
    """Every ALLOCATED sector of the sparse file, as {lba: sha256}.

    Walked with SEEK_DATA/SEEK_HOLE rather than by reading 512 GB a sector
    at a time. That is not only faster: it is a stronger check. A sparse
    file only allocates a region once something has written to it, so this
    finds every byte the disk has ever been given -- and a write to a
    sector that was previously a hole shows up just as loudly as a change
    to one that held data.
    """
    out = {}
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY)
    try:
        f = os.fdopen(os.dup(fd), "rb")
        pos = 0
        while pos < size:
            try:
                data = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break                      # no more data extents
            if data >= size:
                break
            try:
                hole = os.lseek(fd, data, os.SEEK_HOLE)
            except OSError:
                hole = size
            if hole > size:
                hole = size
            lba = data // SECT
            f.seek(lba * SECT)
            n = (hole - lba * SECT + SECT - 1) // SECT
            for i in range(n):
                b = f.read(SECT)
                if not b:
                    break
                if any(b):
                    out[lba + i] = hashlib.sha256(b).hexdigest()
            pos = hole
        f.close()
    finally:
        os.close(fd)
    return out


def main():
    for t in ("sgdisk", "parted"):
        need(t)

    print("Running fs/gpt_write.c against a table sgdisk built,")
    print("then asking sgdisk and parted what they think of the result.\n")

    # ── 1. a sparse 512 GB image, partitioned by sgdisk ─────────────
    if os.path.exists(IMG):
        os.unlink(IMG)
    with open(IMG, "wb") as f:
        f.truncate(DISK_SECTORS * SECT)

    # Windows 11 layout, with a 1 GB hole before recovery.
    esp = (2048, 616447)
    msr = (616448, 648191)
    win = (648192, 1069543423)
    gap = (1069543424, 1071640575)          # 1 GB, where we will install
    rec = (1071640576, 1072689151)

    r = run(["sgdisk",
             "-n", "1:%d:%d" % esp, "-t", "1:ef00", "-c", "1:EFI system partition",
             "-n", "2:%d:%d" % msr, "-t", "2:0c01", "-c", "2:Microsoft reserved partition",
             "-n", "3:%d:%d" % win, "-t", "3:0700", "-c", "3:Basic data partition",
             "-n", "4:%d:%d" % rec, "-t", "4:2700", "-c", "4:Basic data partition",
             IMG])
    check("sgdisk lays down a Windows-shaped GPT", r.returncode == 0,
          r.stdout + r.stderr)
    if r.returncode != 0:
        return

    before_tbl = run(["sgdisk", "-p", IMG]).stdout
    check("sgdisk verifies its own table",
          "No problems found" in run(["sgdisk", "--verify", IMG]).stdout, "")

    # ── 2. the before-image ─────────────────────────────────────────
    before = whole_image_sectors(IMG)
    print("     (%d non-empty sectors in the before-image)" % len(before))

    # ── 3. build and run the real driver ────────────────────────────
    src = os.path.join(tempfile.gettempdir(), "hd_gptreal.c")
    exe = os.path.join(tempfile.gettempdir(), "hd_gptreal")
    open(src, "w").write(HARNESS)
    b = run(["gcc", "-std=gnu99", "-fno-builtin", "-D_FILE_OFFSET_BITS=64",
             "-I" + os.path.join(ROOT, "include"),
             "-Wno-builtin-declaration-mismatch",
             "-o", exe, src], cwd=HERE)
    check("the driver compiles against the real image", b.returncode == 0,
          b.stderr)
    if b.returncode != 0:
        return

    # dry run first -- it must change nothing at all
    d = run([exe, IMG, str(gap[0]), str(gap[1]), "--dry"])
    check("a dry run reports it would succeed", "result 0 ok" in d.stdout, d.stdout)
    check("and the dry run changed not one byte",
          whole_image_sectors(IMG) == before, "")

    out = run([exe, IMG, str(gap[0]), str(gap[1])])
    check("the real write succeeds", out.returncode == 0, out.stdout + out.stderr)
    written = [int(m) for m in re.findall(r"^lba (\d+)$", out.stdout, re.M)]
    check("it wrote exactly four sectors", len(written) == 4, str(written))
    print("     wrote sectors: %s" % ", ".join(str(x) for x in written))

    # ── 4. the independent opinions ─────────────────────────────────
    v = run(["sgdisk", "--verify", IMG])
    check("sgdisk --verify finds no problems",
          "No problems found" in v.stdout, v.stdout + v.stderr)

    p = run(["parted", "-s", IMG, "print"])
    check("parted reads the table without complaint",
          p.returncode == 0 and "Error" not in p.stderr, p.stdout + p.stderr)
    check("parted sees five partitions",
          len(re.findall(r"^\s*\d+\s", p.stdout, re.M)) == 5, p.stdout)

    # ── 5. every original partition identical, per sgdisk ───────────
    after_tbl = run(["sgdisk", "-p", IMG]).stdout

    def rows(t):
        out = {}
        for line in t.splitlines():
            m = re.match(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+(\S+)", line)
            if m:
                out[int(m.group(1))] = (int(m.group(2)), int(m.group(3)),
                                        m.group(4))
        return out

    rb, ra = rows(before_tbl), rows(after_tbl)
    check("sgdisk listed the four original partitions before", len(rb) == 4,
          before_tbl)
    check("and five after", len(ra) == 5, after_tbl)
    same = all(n in ra and ra[n] == rb[n] for n in rb)
    check("every original partition has the same start, end and type",
          same, "before=%r\nafter=%r" % (rb, ra))
    check("ours is partition 5 at the gap",
          5 in ra and ra[5][0] == gap[0] and ra[5][1] == gap[1],
          str(ra.get(5)))

    # ── 6. the byte-level diff of the whole 512 GB ──────────────────
    after = whole_image_sectors(IMG)
    allowed = set(written)
    changed = [l for l in set(before) | set(after)
               if before.get(l) != after.get(l)]
    unexpected = sorted(set(changed) - allowed)
    check("the ONLY sectors that changed in 512 GB are those four",
          not unexpected,
          "also changed: %s" % unexpected[:12])
    print("     (%d non-empty sectors after; %d changed, all permitted)"
          % (len(after), len(changed)))

    # ── and the refusals, against the REAL partition bounds ─────────
    #
    # Using the LBAs we asked sgdisk for would be a mistake: sgdisk aligns
    # partitions to 2048-sector boundaries, so the numbers on the command
    # line are not necessarily the numbers on the disk. The first draft of
    # this harness did exactly that and reported a false failure -- the
    # "overlapping" range it tried was really in the alignment gap sgdisk
    # had left, and the driver was right to accept it. Read the bounds
    # back and test against those.
    print()
    w_a, w_b, _ = ra[3]          # the basic data volume, as it really is
    r_a, r_b, _ = ra[4]          # recovery, as it really is
    h_a, h_b, _ = ra[5]          # ours

    before_ref = whole_image_sectors(IMG)
    for what, a, b2, want in [
        ("a range inside the data volume", w_a + 100, w_a + 1000, 8),
        ("a range on the data volume's last sector", w_b, w_b + 10, 8),
        ("a range on recovery's first sector", r_a - 10, r_a, 8),
        ("a range covering our own new partition", h_a, h_b, 8),
        ("a range past the usable area", DISK_SECTORS - 20, DISK_SECTORS - 5, 7),
        ("a backwards range", w_b, w_a, 11),
    ]:
        rr = run([exe, IMG, str(a), str(b2)])
        m = re.search(r"result (-?\d+)", rr.stdout)
        got = int(m.group(1)) if m else 999
        check("refused: " + what, got == -want,
              "wanted -%d, got %d\n%s" % (want, got, rr.stdout))

    check("and not one of the refusals changed a byte",
          whole_image_sectors(IMG) == before_ref, "")

    v2 = run(["sgdisk", "--verify", IMG])
    check("sgdisk still finds no problems after all of that",
          "No problems found" in v2.stdout, v2.stdout)


if __name__ == "__main__":
    main()
    print()
    if fails:
        print("%d check(s) failed" % len(fails))
        for w, d in fails:
            print("\n  FAIL %s" % w)
            if d:
                print("\n".join("       " + l for l in str(d).splitlines()[:20]))
        sys.exit(1)
    print("all checks passed -- sgdisk and parted agree the disk is intact")
