#!/usr/bin/env python3
"""check_fat16fmt_suite.py -- prove tests/test_fat16fmt.c can fail.

tests/test_fat16fmt.c passed on its first run. That is the least
trustworthy result a suite can produce, and this tree has the receipts:
the FAT32 suite reported 168/168 while missing ten of twenty-six injected
bugs, and the ESP suite reported 74/74 while missing eight of sixteen,
because its entire verify-after-write path was dead code on a working
disk.

The bugs below are the ones a formatter actually ships. The first is not
hypothetical -- it is what fs/install.c did, on every install, to every
partition over 64 MB, for every release up to this one.

Run:  python3 tools/check_fat16fmt_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/fat16fmt.c"
TEST = "tests/test_fat16fmt.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    # ── the geometry ────────────────────────────────────────────────
    ("the cluster count is never checked against the FAT16 maximum",
     "THE SHIPPED BUG: 524215 clusters in a filesystem that holds 65524",
     "        if(clusters > FAT16_MAX_CLUSTERS) continue;   /* needs FAT32     */",
     "        ;"),

    ("the cluster count is never checked against the minimum",
     "a 1 MB partition formatted as FAT16 is really FAT12, and is misread",
     "        if(clusters < FAT16_MIN_CLUSTERS) continue;   /* FAT12 territory */",
     "        ;"),

    ("the FAT is sized without the two reserved entries",
     "entries 0 and 1 are the media byte and the chain terminator, not clusters",
     "            need = ((clusters + 2u) * 2u + SECT - 1u) / SECT;",
     "            need = (clusters * 2u + SECT - 1u) / SECT;"),

    ("64 KB clusters are allowed back in",
     "over the specification's 32 KB cap, and zero in a reader's 16-bit field",
     "    static const uint8_t spc_table[] = { 1, 2, 4, 8, 16, 32, 64 };",
     "    static const uint8_t spc_table[] = { 1, 2, 4, 8, 16, 32, 64, 128 };"),

    ("the largest cluster size is tried first, not the smallest",
     "half the partition spent on slack, and nothing anywhere complains",
     "    static const uint8_t spc_table[] = { 1, 2, 4, 8, 16, 32, 64 };",
     "    static const uint8_t spc_table[] = { 64, 32, 16, 8, 4, 2, 1 };"),

    ("the FAT size is guessed once instead of iterated to a fixed point",
     "the first guess is one sector, and every number after it is wrong",
     "            if(need == spf) break;\n"
     "            spf = need;",
     "            spf = need; break;"),

    ("the root directory is left out of the data area's start",
     "the first file is written over the root directory that indexes it",
     "        g->data_start      = g->root_start + root_sects;",
     "        g->data_start      = g->root_start;"),

    ("the second FAT is not counted in the overhead",
     "the data area starts two FATs early and overlaps the one it skipped",
     "            overhead = (uint32_t)rsvd + (uint32_t)nfats * spf + root_sects;",
     "            overhead = (uint32_t)rsvd + spf + root_sects;"),

    # ── the written volume ──────────────────────────────────────────
    ("only the first FAT is written",
     "fsck compares them; a reader that trusts the second finds zeroes",
     "            if(g->num_fats >= 2 && !wr(ctx, g->fat2_start + i, src)) return 0;",
     "            ;"),

    ("the FAT's reserved entries are never written",
     "entry 0 carries the media byte and entry 1 terminates every chain",
     "        first[0] = 0xF8; first[1] = 0xFF;          /* entry 0: media     */\n"
     "        first[2] = 0xFF; first[3] = 0xFF;          /* entry 1: EOC       */",
     "        ;"),

    ("the media byte in the BPB disagrees with the one in the FAT",
     "two fields that must match, and a check that notices when they do not",
     "    buf[21] = 0xF8;                                /* fixed disk         */",
     "    buf[21] = 0xF0;"),

    ("the boot sector has no 0x55 0xAA signature",
     "nothing will mount it, and the installer would not have noticed",
     "    buf[510] = 0x55; buf[511] = 0xAA;",
     "    ;"),

    ("the total-sectors count goes in the 16-bit field whatever the size",
     "a 1 GB volume described as however many sectors fit in 65535",
     "    if(g->part_sectors < 0x10000u){",
     "    if(1){"),

    ("both total-sectors fields are set",
     "ambiguous by specification, and the ambiguity that broke the FAT32 fixture",
     "    } else {\n"
     "        buf[32] = (uint8_t)(g->part_sectors       & 0xFF);",
     "    }\n"
     "    {\n"
     "        buf[32] = (uint8_t)(g->part_sectors       & 0xFF);"),

    ("sectors-per-FAT in the BPB is the old hardcoded 128",
     "the exact field whose disagreement with the volume size is the bug",
     "    buf[22] = (uint8_t)(g->sects_per_fat & 0xFF);\n"
     "    buf[23] = (uint8_t)((g->sects_per_fat >> 8) & 0xFF);",
     "    buf[22] = 128; buf[23] = 0;"),

    ("sectors-per-cluster in the BPB is the old hardcoded 4",
     "the geometry is computed correctly and then not written down",
     "    buf[13] = g->sects_per_clust;",
     "    buf[13] = 4;"),

    ("hidden sectors is always 2048",
     "right for the whole-disk installer, wrong for every GPT gap",
     "    buf[28] = (uint8_t)(hidden_sects        & 0xFF);\n"
     "    buf[29] = (uint8_t)((hidden_sects >> 8) & 0xFF);\n"
     "    buf[30] = (uint8_t)((hidden_sects >> 16)& 0xFF);\n"
     "    buf[31] = (uint8_t)((hidden_sects >> 24)& 0xFF);",
     "    buf[28] = 0x00; buf[29] = 0x08; buf[30] = 0; buf[31] = 0;"),

    ("the volume label entry is not written to the root directory",
     "GRUB's search --label finds nothing and the menu falls through",
     "    buf[11] = 0x08;                                /* ATTR_VOLUME_ID     */\n"
     "    if(!wr(ctx, g->root_start, buf)) return 0;",
     "    ;"),

    ("the root directory's remaining sectors are left as they were",
     "whatever was in that partition before reads back as directory entries",
     "    for(i = 1; i < g->root_sects; i++)\n"
     "        if(!wr(ctx, g->root_start + i, buf)) return 0;",
     "    ;"),

    ("a failed write is reported as success",
     "the one thing a formatter must never do quietly",
     "        if(!wr(ctx, g->fat1_start + i, src)) return 0;",
     "        wr(ctx, g->fat1_start + i, src);"),

    ("the geometry is used without being checked for existing",
     "format with a zeroed struct and the arithmetic divides by zero",
     "    if(!wr || !g || !g->sects_per_clust || !g->sects_per_fat) return 0;",
     "    if(!g) return 0;"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tf16f_inj")
    b = subprocess.run(CC + ["-o", exe, TEST], capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    original = open(SRC, encoding="utf-8").read()

    print("Injecting %d real formatter bugs into %s.\n"
          "The first one is not hypothetical: it is what shipped.\n" % (len(BUGS), SRC))

    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified formatter passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-60s INJECTION STALE (%d)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))
            st, _ = build_and_run(300)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"
            print("  %2d. %-60s %s" % (i, label, verdict))
    finally:
        open(SRC, "w", encoding="utf-8").write(original)

    print()
    if stale:
        print("  %d injection(s) no longer match the source:" % len(stale))
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
    if missed:
        print("  %d bug(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s\n        %s" % (label, why))
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
