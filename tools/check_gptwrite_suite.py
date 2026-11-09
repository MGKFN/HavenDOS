#!/usr/bin/env python3
"""check_gptwrite_suite.py -- prove tests/test_gptwrite.c can fail.

This is the most important injection harness in the tree, because
fs/gpt_write.c is the only code in HavenDOS that writes to a partition
table somebody's other operating system is living in. Everything else that
goes wrong costs a scratch image. This costs a Windows install.

So the bugs below are not plausible-looking edits. Every one of them is a
mistake a real partition editor has shipped at some point: a table rewritten
wholesale instead of one entry, a CRC computed over the wrong length, a
checksum field not zeroed before it is checksummed, a 64-bit LBA truncated
to 32, an overlap test that is off by one at the boundary. Each is applied
to the driver in turn, and the suite must notice every single one.

A MISSED result means the suite passed a partition writer with a known bug
in it. Treat that as a release blocker, not a to-do.

Run:  python3 tools/check_gptwrite_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/gpt_write.c"
TEST = "tests/test_gptwrite.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude",
      "-Wno-builtin-declaration-mismatch"]

# (label, what it breaks, find, replace)
# The byte-order helpers in fs/gpt_write.c are named gw_ld32/gw_st64 and so
# on, prefixed when that file had to share a translation unit with
# fs/fat32.c -- which has helpers of the same names -- for
# tests/test_installadd.c. An injection that still said ld32 matched
# nothing and was reported STALE rather than MISSED, which is the harness
# working: a rename silently turning an injection into a no-op is how a
# harness stops testing anything.
BUGS = [
    ("the whole entry array is rewritten, not one sector",
     "the mistake this entire file exists to be incapable of",
     "    const uint8_t *esec = g.ent + (uint64_t)ent_sec * SECT;\n"
     "    if(wr(ctx, wlist[0], esec) != 0) return GPTW_E_WRITE;",
     "    const uint8_t *esec = g.ent + (uint64_t)ent_sec * SECT;\n"
     "    for(uint32_t s=0;s<g.ent_sectors;s++)\n"
     "        if(wr(ctx, g.bent_lba+s, g.ent+s*SECT) != 0) return GPTW_E_WRITE;\n"
     "    if(wr(ctx, wlist[0], esec) != 0) return GPTW_E_WRITE;"),

    ("the entry-array CRC is not recomputed",
     "every other implementation then treats the table as corrupt",
     "    uint32_t new_ent_crc = gpt_crc32(g.ent, g.n_ent * g.ent_size);",
     "    uint32_t new_ent_crc = gw_ld32(g.hdr + 88);"),

    ("the entry CRC covers the wrong length",
     "a CRC over the sector count rather than the entry count",
     "    uint32_t new_ent_crc = gpt_crc32(g.ent, g.n_ent * g.ent_size);",
     "    uint32_t new_ent_crc = gpt_crc32(g.ent, g.ent_sectors * SECT);"),

    ("the header CRC field is not zeroed before checksumming",
     "the spec's one counter-intuitive rule, and it is always this one",
     "    gw_st32(ph + 16, 0); gw_st32(ph + 16, gpt_crc32(ph, gw_ld32(ph + 12)));",
     "    gw_st32(ph + 16, gpt_crc32(ph, gw_ld32(ph + 12)));"),

    ("the primary header CRC is not recomputed at all",
     "firmware rejects the table and may silently fall back to the backup",
     "    gw_st32(ph + 16, 0); gw_st32(ph + 16, gpt_crc32(ph, gw_ld32(ph + 12)));",
     "    ;"),

    ("the backup header CRC is not recomputed",
     "the two copies disagree and a repair tool picks one",
     "    gw_st32(bh + 16, 0); gw_st32(bh + 16, gpt_crc32(bh, gw_ld32(bh + 12)));",
     "    ;"),

    ("the primary is written before the backup",
     "a power cut then leaves the copy firmware reads half-updated",
     "    wlist[nw++] = g.bent_lba + ent_sec;\n"
     "    wlist[nw++] = g.alt_lba;\n"
     "    wlist[nw++] = g.ent_lba  + ent_sec;\n"
     "    wlist[nw++] = g.my_lba;",
     "    wlist[nw++] = g.ent_lba  + ent_sec;\n"
     "    wlist[nw++] = g.my_lba;\n"
     "    wlist[nw++] = g.bent_lba + ent_sec;\n"
     "    wlist[nw++] = g.alt_lba;"),

    ("the backup copy is never updated",
     "Windows' own repair then reverts the change, or worse trusts it",
     "    if(wr(ctx, wlist[0], esec) != 0) return GPTW_E_WRITE;\n"
     "    if(wr(ctx, wlist[1], bh)   != 0) return GPTW_E_WRITE;",
     "    ;"),

    ("overlap uses < where it must use <=",
     "a partition starting exactly where another ends is an overlap",
     "        if(first_lba <= b && a <= last_lba) return GPTW_E_OVERLAP;",
     "        if(first_lba < b && a < last_lba) return GPTW_E_OVERLAP;"),

    ("overlap only checks the start of the new range",
     "a range that begins in free space and runs into NTFS",
     "        if(first_lba <= b && a <= last_lba) return GPTW_E_OVERLAP;",
     "        if(first_lba >= a && first_lba <= b) return GPTW_E_OVERLAP;"),

    ("the upper geometry bound is not checked",
     "a partition written over the backup GPT itself",
     "    if(first_lba < g.first_usable || last_lba > g.last_usable)",
     "    if(first_lba < g.first_usable || last_lba > 0xFFFFFFFFFFFFFFFFULL)"),

    ("slot 0 is claimed instead of the first free one",
     "this overwrites the ESP's entry, which is partition one",
     "        if(!entry_used(&g, i)){ slot = i; have_slot = 1; break; }",
     "        if(i >= 0){ slot = 0; have_slot = 1; break; }"),

    ("a used entry is detected by its first byte only",
     "a type GUID beginning 0x00 then reads as a free slot",
     "    for(int k = 0; k < 16; k++) if(e[k]) return 1;",
     "    if(e[0]) return 1;"),

    ("the backup header LBA is truncated to 32 bits",
     "on a 4 TB disk that lands in the middle of somebody's data",
     "    wlist[nw++] = g.alt_lba;",
     "    wlist[nw++] = (uint64_t)(uint32_t)g.alt_lba;"),

    ("the partition start is truncated to 32 bits",
     "the same bug one field along",
     "        gw_st64(e + 32, first_lba);",
     "        gw_st64(e + 32, (uint64_t)(uint32_t)first_lba);"),

    ("the primary header CRC is not verified before writing",
     "a table we cannot read correctly is one we must not modify",
     "    if(!hdr_crc_ok(g->hdr))         return GPTW_E_HDRCRC;",
     "    ;"),

    ("the entry-array CRC is not verified before writing",
     "the same, for the half of the table that holds the partitions",
     "    if(gpt_crc32(g->ent, g->n_ent * g->ent_size) != gw_ld32(g->hdr + 88))\n"
     "        return GPTW_E_ENTCRC;",
     "    ;"),

    # Targeting the whole block rather than one line of it. Removing any
    # single guard here changes nothing, because the next one catches the
    # same disk -- the signature check, the CRC check and the entry-LBA
    # sanity check all reject a zeroed backup header. That redundancy is
    # worth keeping in a file like this, but it means the only honest
    # injection is "does not validate the backup at all".
    ("the backup is not validated at all",
     "adding to a half-damaged table is how it becomes wholly damaged",
     "    if(!hdr_ok(g->bhdr))            return GPTW_E_BACKUP;\n"
     "    if(!hdr_crc_ok(g->bhdr))        return GPTW_E_BACKUP;\n"
     "\n"
     "    g->bent_lba = gw_ld64(g->bhdr + 72);\n"
     "    if(g->bent_lba < 2)             return GPTW_E_BACKUP;",
     "    g->bent_lba = gw_ld64(g->bhdr + 72);"),

    ("the dry run writes anyway",
     "the mode whose entire purpose is that it cannot write",
     "    if(!wr) return GPTW_OK;",
     "    if(!wr) wr = 0;"),

    ("the entry goes in the wrong sector of the array",
     "it lands on four other partitions' entries",
     "    uint32_t ent_sec  = slot / per_sec;",
     "    uint32_t ent_sec  = slot;"),

    ("entries-per-sector uses the entry count, not the size",
     "the same mis-aimed write by a different route",
     "    uint32_t per_sec  = SECT / g.ent_size;",
     "    uint32_t per_sec  = SECT / 4;"),

    ("the backup entry sector is computed from the primary's array LBA",
     "the backup entry write lands near the front of the disk",
     "    wlist[nw++] = g.bent_lba + ent_sec;",
     "    wlist[nw++] = g.ent_lba + ent_sec;"),

    ("the reported write count is smaller than the real one",
     "the blast radius is then a claim rather than a measurement",
     "        for(int i = 0; i < nw; i++) written[i] = wlist[i];\n"
     "        *n_written = nw;",
     "        for(int i = 0; i < nw; i++) written[i] = wlist[i];\n"
     "        *n_written = nw - 1;"),

    ("first and last LBA are written swapped",
     "a partition the firmware reads as ending before it starts",
     "        gw_st64(e + 32, first_lba);\n        gw_st64(e + 40, last_lba);",
     "        gw_st64(e + 32, last_lba);\n        gw_st64(e + 40, first_lba);"),

    ("the new entry's type GUID is left zero",
     "the entry reads as unused and the next install overwrites it",
     "        memcpy(e, type_guid, 16);",
     "        ;"),

    ("the backwards-range guard is dropped",
     "last < first then underflows every length computation after it",
     "    if(last_lba < first_lba) return GPTW_E_PARAM;",
     "    ;"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tgw_inj")
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

    print("Injecting %d real partition-table bugs into %s.\n"
          "This is the code that writes to a disk with Windows on it, so the\n"
          "suite must notice every one of them.\n" % (len(BUGS), SRC))

    st, out = build_and_run(120)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2000:])
        return 2
    print("  baseline: the unmodified writer passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-52s INJECTION STALE (%d)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))
            st, _ = build_and_run(60)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"
            print("  %2d. %-52s %s" % (i, label, verdict))
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
        print("\n  Each of these is a hole in tests/test_gptwrite.c, and this\n"
              "  is the one file where a hole is somebody's Windows install.")
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
