#!/usr/bin/env python3
"""check_fat32_suite.py -- prove tests/test_fat32.c can fail.

A test that cannot fail proves nothing.  tests/test_uhci.c was checked by
injecting 21 real UHCI bugs into the driver and confirming the suite caught
every one; three of its checks exist because that exercise found gaps.
tests/test_menugeom.c was checked the same way with four.  This is the same
exercise for FAT32, and it found gaps here too.

Each entry below is a mistake a real FAT32 implementation makes -- several
of them are mistakes that were made in this tree or in the fixture generator
while writing it.  The injection is a one-line textual edit to fs/fat32.c.
The suite must fail, crash, or hang on every one of them.

A CAUGHT result means the suite noticed.  A MISSED result means the suite
passed a driver with a known bug in it, and is a hole to go and fill.

Run:  python3 tools/check_fat32_suite.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

SRC = "fs/fat32.c"
TEST = "tests/test_fat32.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

# (label, what it breaks, find, replace)
BUGS = [
    ("fat entry not masked to 28 bits",
     "chains read through the reserved top nibble",
     "    *out = ld32(g_fatw + o) & CLUS_MASK;",
     "    *out = ld32(g_fatw + o);"),

    ("reserved top nibble zeroed on write",
     "bits that are not ours, destroyed",
     "    st32(g_fatw + o, (old & CLUS_RESERVED) | (val & CLUS_MASK));",
     "    st32(g_fatw + o, val & CLUS_MASK);"),

    ("BPB_FATSz32 read before FATSz16 (probe)",
     "offset 36 is the extended boot record on FAT12/16",
     "    uint32_t fsz  = ld16(bs + 22);\n    if(fsz == 0) fsz = ld32(bs + 36);",
     "    uint32_t fsz  = ld32(bs + 36);\n    if(fsz == 0) fsz = ld16(bs + 22);"),

    # The same field-order trap on the MOUNT path is deliberately not here.
    # fat32_mount_dev() will not mount anything fat_probe_type() has not
    # already called FAT32, and the spec requires BPB_FATSz16 to be zero on
    # a FAT32 volume, so on every volume that can reach those lines the two
    # orders give the same answer.  Injecting it produced a MISSED result
    # that was really "this is not a bug", which is worth saying rather than
    # writing a contrived test to chase.  What IS load-bearing is the gate
    # itself, so the suite checks that a FAT16 volume is refused a mount.

    ("FAT16/32 boundary off by one",
     "65525 clusters misread as FAT16",
     "    if(clusters < 65525) return FATTYPE_16;",
     "    if(clusters <= 65525) return FATTYPE_16;"),

    ("FAT12/16 boundary off by one",
     "4085 clusters misread as FAT12",
     "    if(clusters < 4085)  return FATTYPE_12;",
     "    if(clusters <= 4085)  return FATTYPE_12;"),

    ("end-of-chain compared against 0x0FFFFFFF",
     "0x0FFFFFF8..0x0FFFFFFE read as real clusters",
     "#define CLUS_EOC       0x0FFFFFF8u",
     "#define CLUS_EOC       0x0FFFFFFFu"),

    ("directory stops at the end of its first cluster",
     "the thing a FAT32 root directory stops being",
     "    c->sec = 0;\n    uint32_t next;\n    if(!fat_get(c->clus, &next)) return -1;",
     "    c->sec = 0;\n    uint32_t next;\n    c->clus = 0; return 0;\n    if(!fat_get(c->clus, &next)) return -1;"),

    ("NTRes case flags ignored",
     "readme.md is reported as README.MD",
     "    int lb = (d[DE_NTRES] & NTRES_LOWER_BASE) != 0;",
     "    int lb = 0;"),

    ("LFN checksum not verified",
     "an orphaned long name is attached to the wrong file",
     "        if(it->lvalid && it->lnext == 0 && lfn_sum(e + DE_NAME) == it->lsum){",
     "        if(it->lvalid && it->lnext == 0){"),

    ("LFN sequence order not verified",
     "slots accepted out of order give a scrambled name",
     "            if(n != it->lnext || e[DE_CRT_TENTH] != it->lsum){",
     "            if(0){"),

    ("LFN written in forward order",
     "every other implementation reads the name backwards",
     "    for(int s = nlfn; s >= 1; s--){",
     "    for(int s = 1; s <= nlfn; s++){"),

    ("LFN last-slot flag on the wrong slot",
     "0x40 marks the end of the run, not the start",
     "        e[0] = (uint8_t)(s | (s == nlfn ? 0x40 : 0));",
     "        e[0] = (uint8_t)(s | (s == 1 ? 0x40 : 0));"),

    ("LFN character offset table wrong",
     "thirteen characters per slot, at three disjoint runs",
     "static const uint8_t LFN_OFF[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };",
     "static const uint8_t LFN_OFF[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 26,28 };"),

    ("cluster-to-sector off by one",
     "every file reads one cluster early",
     "    return g_data_start + (c - 2) * g_sects_per_clust;",
     "    return g_data_start + (c - 1) * g_sects_per_clust;"),

    ("directory entry's high cluster word ignored",
     "FAT16's dirent layout applied to FAT32",
     "        out->clus = ((uint32_t)ld16(e + DE_CLUS_HI) << 16) | ld16(e + DE_CLUS_LO);",
     "        out->clus = ld16(e + DE_CLUS_LO);"),

    ("high cluster word not written",
     "the same, on the write side",
     "    st16(e + DE_CLUS_HI, (uint16_t)(clus >> 16));\n    st16(e + DE_CLUS_LO, (uint16_t)(clus & 0xFFFF));\n    st32(e + DE_SIZE, size);\n    /* Dates are left",
     "    st16(e + DE_CLUS_HI, 0);\n    st16(e + DE_CLUS_LO, (uint16_t)(clus & 0xFFFF));\n    st32(e + DE_SIZE, size);\n    /* Dates are left"),

    # Targeting the empty-basename guard rather than the leading-dot one.
    # Removing the leading-dot test alone changes nothing, because base==0
    # catches ".config" one line later -- the injection came back MISSED and
    # the honest reading was "that guard is redundant", not "the suite has a
    # hole".  This is the guard that does the work, and removing it
    # reproduces fs/fat16.c's dotfile bug exactly: a blank name field with a
    # three-letter extension.
    ("empty 8.3 basename accepted",
     "this IS the FAT16 dotfile bug, moved to FAT32",
     "    if(base == 0) return 0;",
     "    if(base == 0) { out11[0] = ' '; }"),

    ("short-name uniquifier always uses ~1",
     "two long names collapse onto one directory entry",
     "        tilde_83(n11, n);",
     "        tilde_83(n11, 1);"),

    ("only the first FAT is written",
     "the two copies disagree and fsck picks one",
     "    for(uint32_t i = 0; i < g_num_fats; i++)\n        if(!f32_wr(g_fat_start + i * g_fat_sz + g_fatw_sect, g_fatw)) return 0;",
     "    if(!f32_wr(g_fat_start + g_fatw_sect, g_fatw)) return 0;"),

    ("the old chain is not freed on overwrite",
     "clusters leak and fsck finds them lost",
     "        if(old && !chain_free(old)) return 0;",
     "        if(0 && old && !chain_free(old)) return 0;"),

    ("FSInfo never written back",
     "a stale free count is how a volume comes back wrong",
     "    if(!f32_wr(g_fsinfo_sect, g_sec)) return 0;\n    g_fsinfo_dirty = 0;",
     "    g_fsinfo_dirty = 0;"),

    ("FSInfo free count not decremented",
     "the number on disk drifts from the FAT",
     "        if(g_free_count >= n) g_free_count -= n;",
     "        if(g_free_count >= n) g_free_count -= 0;"),

    ("a read past end-of-file is not clipped",
     "the tail of the last cluster is returned as data",
     "    if(len > fsize - off) len = fsize - off;",
     "    ;"),

    ("clus_valid upper bound off by one",
     "the last cluster on the volume is unreachable",
     "    return c >= 2 && c <= g_clusters + 1;",
     "    return c >= 2 && c < g_clusters;"),

    ("the directory entry size field is not written",
     "every written file reads back as empty",
     "    st32(e + DE_SIZE, size);\n    /* Dates are left",
     "    st32(e + DE_SIZE, 0);\n    /* Dates are left"),

    ("deleted entries not skipped when scanning",
     "a deleted name is still found",
     "        if(first == DE_FREE){ it->lvalid = 0; it->lslots = 0; continue; }",
     "        if(first == DE_FREE){ it->lvalid = 0; it->lslots = 0; }"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tf32_inj")
    b = subprocess.run(CC + ["-o", exe, TEST],
                       capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    original = open(SRC, encoding="utf-8").read()

    print("Injecting %d real FAT32 bugs into %s.\n"
          "The suite must notice every one of them.\n" % (len(BUGS), SRC))

    # The clean tree has to pass first, or nothing below means anything.
    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2000:])
        return 2
    print("  baseline: the unmodified driver passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-46s INJECTION STALE (%d matches)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))
            st, _ = build_and_run(240)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung -- bounded by timeout)"
            else:
                verdict = "caught"
            print("  %2d. %-46s %s" % (i, label, verdict))
    finally:
        open(SRC, "w", encoding="utf-8").write(original)

    print()
    if stale:
        print("  %d injection(s) no longer match the source.  An injection that" % len(stale))
        print("  does not apply is a check that silently stopped running:")
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
    if missed:
        print("  %d bug(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s" % label)
            print("        %s" % why)
        print("\n  Each of these is a hole in tests/test_fat32.c.")
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
