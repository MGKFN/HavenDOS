#!/usr/bin/env python3
"""check_exfat_suite.py -- prove tests/test_exfat.c can fail.

It passed 83 of 83 on its first run, and the fixture was wrong: it
contained three contiguous files and no fragmented one, so the FAT
chain walk -- half the driver -- was never executed, and the check
that was supposed to notice said "at least one file is a contiguous
run" and passed. The generator fragments a file on purpose now and the
suite demands both forms.

That is the shape of defect this harness exists to keep finding, and
exFAT is unusually rich in them because a reader has four ways to be
half-right:

  - the two cluster walks. NoFatChain says the allocation is one
    contiguous run and the FAT holds nothing for it, and writers set
    it for anything laid down in one go. A reader that handles one
    form works on about half of all real volumes.
  - the directory entry SET. 0x85 file, 0xC0 stream, then as many
    0xC1 name entries as the name needs. SecondaryCount says how many
    follow; scanning for the next 0xC0 instead walks into the
    following file's set whenever a deleted entry sits between them --
    and this fixture HAS deleted entries, because six filler files are
    removed to make the fragmentation.
  - the name. Fifteen UTF-16 characters per entry. A reader that takes
    the first and stops matches nothing, which reads as "file not
    found" rather than as a bug.
  - the geometry. Every field in the boot sector is a multiplier on
    somebody's removable media. Arithmetic on an unchecked length is
    how a reader walks off the end of a disk rather than refusing it.

Run:  python3 tools/check_exfat_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "fs/exfat.c"
TEST = "tests/test_exfat.c"
# Source files only, no -l: tools/inject.py builds with
# cc + ["-o", exe, test], so a library named here would land BEFORE
# the sources that need it and never resolve. The suite carries its
# own CRC-32 rather than linking zlib -- see the note in it.
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "fs/exfat.c"]

BUGS = [
    ("the cluster walk always follows the FAT",
     "works on a fragmented volume and walks garbage on a fresh one -- "
     "which is every stick straight out of the packet, because a "
     "writer sets NoFatChain for anything laid down in one go. The FAT "
     "holds nothing for those clusters, so this reads whatever was "
     "left there",
     """    if(no_chain){
        uint32_t n = cur + 1;
        if(!clust_valid(n)) return 0;
        *out = n;
        return 1;
    }""",
     """    if(0){
        uint32_t n = cur + 1;
        if(!clust_valid(n)) return 0;
        *out = n;
        return 1;
    }"""),

    ("the cluster walk always assumes contiguity",
     "the same defect pointing the other way, and the one a suite "
     "tested only against a freshly formatted volume cannot see at "
     "all. A fragmented file reads the right number of bytes from the "
     "wrong clusters",
     """    if(no_chain){
        uint32_t n = cur + 1;""",
     """    if(1){
        uint32_t n = cur + 1;"""),

    ("the name stops after the first 0xC1 entry",
     "fifteen UTF-16 characters, so every long name is truncated to "
     "its first fifteen -- and a truncated name matches nothing, so "
     "this presents as 'file not found' rather than as a bug",
     """            } else if(sub[0] == E_NAME && got_stream){""",
     """            } else if(sub[0] == E_NAME && got_stream && nlen == 0){"""),

    # ── NOT A DEFECT ON A WELL-FORMED VOLUME, and recorded ───────────
    #
    # "the name length is not bounded by NameLength" was written as an
    # injection and reported MISSED, twice, and then read properly.
    #
    # exFAT pads the unused character slots of the last 0xC1 entry with
    # 0x0000, and utf16_append() stops at a NUL. So on any volume a
    # formatter wrote, the bound and the padding agree and removing one
    # of them changes nothing -- and this fixture is fsck.exfat-clean
    # by construction, so it CANNOT contain the malformed set that
    # would tell them apart.
    #
    # The bound stays. It is defence against a directory that has been
    # truncated or corrupted, which is the ordinary state of a stick
    # somebody pulled out mid-write, and a reader that trusts the
    # padding there appends fifteen characters of the next entry to a
    # filename. Testing it needs a hand-built malformed directory,
    # which is a fixture of a different kind from this one.
    #
    # Left as a note because v0.8.6.3's harness proved two of its ten
    # injections not to be defects, and an injection deleted without a
    # word is one somebody writes again.

    ("the entry set is scanned for a 0xC0 instead of counted",
     "SecondaryCount says how many entries belong to THIS file. "
     "Scanning instead walks into the next file's set whenever a "
     "deleted entry sits between them -- and this fixture has deleted "
     "entries, because the generator removes six filler files to "
     "fragment one of the others",
     """        secondary = ent[1];""",
     """        secondary = 18;"""),

    # ── NOT A DEFECT, and recorded ───────────────────────────────────
    #
    # "a deleted entry set is listed" was written as an injection and
    # reported MISSED. The fixture DOES contain one -- ZOMBIE.TMP is
    # written last and removed, and the image carries its three
    # entries as 0x05, 0x40 and 0x41, which is 0x85/0xC0/0xC1 with the
    # in-use bit cleared.
    #
    # It is missed because the in-use test is redundant with the one on
    # the line after it: a deleted file entry is 0x05, and `if(type !=
    # E_FILE) continue` skips it anyway. There is no byte that passes
    # the type test and fails the in-use test, because deleting an
    # entry is defined as clearing that bit.
    #
    # The check stays: it says what the loop means, and a later
    # refactor that matched on `type & 0x7F` -- which is a reasonable
    # thing to write -- would make it load-bearing immediately.
    #
    # Recorded rather than deleted. v0.8.6.3's harness proved two of
    # ten injections not to be defects, and one deleted without a word
    # is one somebody writes again.

    ("the end-of-directory marker is ignored",
     "a zero type byte means there are no more entries ANYWHERE in "
     "this directory, and the space beyond it is not initialised. "
     "Treating it as 'skip' walks the whole cluster reading whatever "
     "is there",
     """        if(type == 0x00){ d->done = 1; return 0; }   /* end of directory */""",
     """        if(type == 0x00) continue;"""),

    ("a directory's own cluster chain is not followed",
     "a directory bigger than one cluster -- which on this volume is "
     "4096 bytes, i.e. 128 entries, i.e. about forty files with "
     "ordinary names -- stops halfway through with no error",
     """        uint32_t n;
        if(!fat_next(d->cluster, &n) || n == CL_EOF || n == CL_BAD ||
           !clust_valid(n)){ d->done = 1; return 0; }
        d->cluster = n;""",
     """        d->done = 1; return 0;"""),

    ("the boot sector's heap offset is not sanity checked",
     "every address below is computed from it. A volume whose heap "
     "claims to start inside its own FAT is read at addresses the "
     "arithmetic invented, on somebody's removable media",
     """    if(g_heap_off < g_fat_off + g_fat_len){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }""",
     """    /* not checked */"""),

    ("the root cluster is not checked against the cluster count",
     "clust_lba() subtracts 2 and multiplies, so a root cluster of 0 "
     "or of four billion is a read at a wild offset rather than a "
     "refusal",
     """    if(g_root < CL_FIRST || g_root >= CL_FIRST + g_nclust){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }""",
     """    /* not checked */"""),

    ("a 4K-sector volume is read as if it were 512",
     "every sector address would be out by a factor of eight. It is a "
     "real format and this reader does not do it; reading it anyway "
     "is a wrong answer where a refusal belongs",
     """    if(bps_shift != 9){ g_why = EXFAT_ESECTORSIZE; exfat_unmount(); return 0; }""",
     """    (void)bps_shift;"""),

    ("TexFAT is read as ordinary exFAT",
     "revision 2.x is a different filesystem wearing the same "
     "signature. Reading it as 1.x is the plausible wrong answer",
     """    if(bs[105] != 1){ g_why = EXFAT_EREVISION; exfat_unmount(); return 0; }""",
     """    /* revision not checked */"""),

    # ── NOT A DEFECT, and recorded ───────────────────────────────────
    #
    # "a refusal does not leave the volume unmounted" was written as an
    # injection, reported MISSED, chased with a test that mounts a good
    # volume and then a bad one, and reported MISSED again. Then read
    # properly.
    #
    # exfat_mount_dev() calls exfat_unmount() on its FIRST LINE, before
    # it touches the device, so g_mounted is already 0 by the time any
    # refusal path is reached. That is the right semantic -- attempting
    # a mount invalidates the previous one, whether or not it succeeds
    # -- and it makes every later unmount in the function redundant for
    # the flag.
    #
    # They are not quite redundant for everything: the read function,
    # its context pointer and the partition base are set after that
    # first line, so without the trailing unmount a refused mount
    # leaves this file holding a pointer the caller gave it. Nothing
    # can reach it, because every entry point tests g_mounted, but a
    # dangling pointer to somebody's device context is not a thing to
    # keep on purpose. The calls stay for that.
    #
    # Three injections into this file have now proved not to be
    # defects, and all three are guards that are redundant TODAY
    # against a specific second guard. Each is recorded with the
    # change that would make it load-bearing again.

    ("reading past the end of a file is an error rather than zero",
     "a caller reading to EOF in a loop cannot tell 'nothing left' "
     "from 'something broke', so it either stops early on a good file "
     "or loops for ever on a bad one",
     """    if(off >= e.size) return 0;                 /* past the end is not
                                                   a failure */""",
     """    if(off >= e.size) return -EXFAT_ENOTFOUND;"""),

    ("a read is not clamped to the file's length",
     "the length comes from the caller and the file ends where the "
     "directory entry says. Without the clamp a read runs on into "
     "whatever follows the file on the volume and reports it as file "
     "contents",
     """    if((uint64_t)len > e.size - off) len = (uint32_t)(e.size - off);""",
     """    /* not clamped */"""),

    ("the partition offset is ignored",
     "a stick is a partition, not a whole disk. Every address would "
     "be relative to the start of the device, so the MBR gets read as "
     "a boot sector -- which is a different failure from not "
     "supporting exFAT and sends the reader somewhere else entirely",
     """    if(!g_rd(g_ctx, g_part + lba, buf)) {""",
     """    if(!g_rd(g_ctx, lba, buf)) {"""),

    ("a directory is readable as a file",
     "a directory's clusters are entry records, so this hands the "
     "caller 32-byte structures and calls them file contents",
     """    if(e.is_dir) return -EXFAT_EISDIR;""",
     """    /* directories readable */"""),

    ("walking through a file as if it were a directory is allowed",
     "README.TXT/x would be looked up by treating the file's first "
     "cluster as a directory and reading its bytes as entries",
     """            if(!e.is_dir) return -EXFAT_ENOTDIR;""",
     """            /* not checked */"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """uint32_t exfat_cluster_count(void){ return g_nclust; }""",
    """/* how many clusters the heap holds, as the boot sector says */
uint32_t exfat_cluster_count(void){ return g_nclust; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "exfat", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_exfat.c can fail.\n"
                 "It passed 83 of 83 first time against a fixture with "
                 "no fragmented file in it, so half the driver had "
                 "never run."))
