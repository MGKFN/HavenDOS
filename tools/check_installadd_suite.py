#!/usr/bin/env python3
"""check_installadd_suite.py -- prove tests/test_installadd.c can fail.

The suite this checks makes the strongest claim in the project: that
installing HavenDOS beside Windows leaves every sector on the disk
byte-identical except four. A claim that size is worth nothing from a
suite that has not been shown capable of failing.

The record so far, in this tree: the FAT32 suite reported 168/168 while
missing ten of twenty-six injected bugs; the ESP suite reported 74/74
while missing eight of sixteen; the FAT16 formatter suite reported 81/81
while missing six of twenty-one. Every one of those gaps was real.

Two of the injections below are not inventions:

  - "the read callback's return convention is inverted" is the bug this
    suite caught the first time it ran. gpt_rd_fn returns 0 for success
    and inst_disk_read returns 1, and passing one through the other made
    a 512 GB Windows disk look like it had no partition table.

  - "the ESP loader check passes anything" is the bug that shipped: the
    Makefile never defined HAVE_GRUB_EFI_EMBED, so fs/esp.c compiled
    against a zero-byte stub.

Run:  python3 tools/check_installadd_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/install.c"
TEST = "tests/test_installadd.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    # ── the preview ─────────────────────────────────────────────────
    ("the dry run goes on and writes",
     "the mode whose entire purpose is that it cannot write",
     "    rep->plan_ok = 1;\n"
     "    if(dry_run){",
     "    rep->plan_ok = 1;\n"
     "    if(0){"),

    ("the dry run hands gpt_add_partition a write function",
     "the preview rewrites the partition table it is previewing",
     "    r = gpt_add_partition(add_disk_rd, 0, (void *)(long)disk,\n"
     "                          first, last, ADD_TYPE_GUID, \"HavenDOS\",\n"
     "                          &entry, written, &nw);\n"
     "    if(r == GPTW_E_NOSLOT)                      return INSTADD_E_ENTRY;",
     "    r = gpt_add_partition(add_disk_rd, add_disk_wr, (void *)(long)disk,\n"
     "                          first, last, ADD_TYPE_GUID, \"HavenDOS\",\n"
     "                          &entry, written, &nw);\n"
     "    if(r == GPTW_E_NOSLOT)                      return INSTADD_E_ENTRY;"),

    ("the ESP is previewed with a write function",
     "files written onto Windows' boot partition during a preview",
     "    rep->esp_rc = esp_install(disk, &rep->esp, entry, 1 /* dry run */);",
     "    rep->esp_rc = esp_install(disk, &rep->esp, entry, 0);"),

    # ── the refusals ────────────────────────────────────────────────
    ("a live session is allowed to write",
     "the ISO's ordinary boot entry repartitioning somebody's disk",
     "        extern int boot_is_live(void);\n"
     "        if(boot_is_live())                      return INSTADD_E_LIVE;",
     "        ;"),

    ("a disk with no write path is attempted anyway",
     "every write fails and the install stops somewhere in the middle",
     "        extern int inst_disk_is_readonly(int);\n"
     "        if(inst_disk_is_readonly(disk))         return INSTADD_E_READONLY;",
     "        ;"),

    # Deliberately NOT injected: removing the inst_disk_has_gpt() check.
    # It is redundant for the OUTCOME. gpt_find_free() runs next and reads
    # the header itself; on a disk with no signature it returns
    # GPTW_E_NOTGPT, and on one with a corrupt CRC GPTW_E_HDRCRC, and both
    # fall into the same `return INSTADD_E_NOGPT`. So the suite sees the
    # same refusal with the guard removed, and an injection for it would
    # report a MISS that means "this is not a bug".
    #
    # The guard stays: it is one sector read, it refuses before the gap
    # search allocates its 16-gap array, and it is the same check
    # fs/esp.c's mount path makes. tests/test_installadd.c asserts the
    # refusal itself ("a disk with no GPT is refused, not guessed at"),
    # which is the behaviour that matters. Same reasoning as the redundant
    # guards recorded in tools/check_esp_suite.py and check_fat32_suite.py.

    ("the volume is not checked for room before it is formatted",
     "a kernel that stops copying two thirds of the way through",
     "        if(need_bytes > rep->vol_free_bytes)    return INSTADD_E_VOLSPACE;",
     "        ;"),

    # ── the geometry of the new partition ───────────────────────────
    ("the read callback's return convention is inverted",
     "THE BUG THIS SUITE CAUGHT: 0 means success to gpt.c, 1 to the installer",
     "    return inst_disk_read((int)(long)ctx, lba, (uint8_t *)buf) ? 0 : -1;",
     "    return inst_disk_read((int)(long)ctx, lba, (uint8_t *)buf);"),

    ("the write callback's convention is inverted too",
     "every successful write reported as a failure, halfway through a table",
     "    return inst_disk_write((int)(long)ctx, lba, (const uint8_t *)buf) ? 0 : -1;",
     "    return inst_disk_write((int)(long)ctx, lba, (const uint8_t *)buf);"),

    ("the partition is not aligned to a megabyte",
     "slower on every SSD, and wrong-looking in Windows' own disk manager",
     "    first = (gap.first_lba + (INSTADD_ALIGN - 1)) / INSTADD_ALIGN\n"
     "            * INSTADD_ALIGN;",
     "    first = gap.first_lba;"),

    ("the partition is one sector longer than the space it was given",
     "an overlap with whatever begins at the end of the gap",
     "    last = first + want - 1;",
     "    last = first + want;"),

    ("the size is not capped, so a huge gap produces no valid geometry",
     "a refusal on a disk with plenty of room, for no reason the user can see",
     "    if(want > INSTADD_MAX_SECTORS) want = INSTADD_MAX_SECTORS;",
     "    ;"),

    ("a gap below the floor is accepted",
     "a partition too small to hold the kernel, formatted and then filled",
     "    if(want < INSTADD_MIN_SECTORS)              return INSTADD_E_TOOSMALL;",
     "    ;"),

    ("the type GUID is Microsoft Basic Data",
     "Windows enumerates it, gives it a letter, and offers to repair it",
     "static const uint8_t ADD_TYPE_GUID[16] = {\n"
     "    'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'\n"
     "};",
     "static const uint8_t ADD_TYPE_GUID[16] = {\n"
     "    0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,\n"
     "    0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7\n"
     "};"),

    ("an existing HavenDOS install is not noticed",
     "REPORTED FROM A REAL RUN: \"no free space\" on a disk that just worked",
     "    if(havendos_find_existing(disk) >= 0)       return INSTADD_E_ALREADY;",
     "    ;"),

    ("the existing-install check runs after the gap search",
     "a disk with room gets a second HavenDOS and two HAVENDOS labels",
     "    if(havendos_find_existing(disk) >= 0)       return INSTADD_E_ALREADY;\n"
     "\n"
     "    r = gpt_find_free(add_disk_rd, (void *)(long)disk,",
     "    r = gpt_find_free(add_disk_rd, (void *)(long)disk,"),

    ("the existing install is matched on anything but the type GUID",
     "every partition on the disk looks like ours, so nothing installs ever",
     "        for(k = 0; k < 16; k++)\n"
     "            if(p[i].type_guid[k] != ADD_TYPE_GUID[k]){ same = 0; break; }",
     "        (void)k;"),

    # ── the order of operations ─────────────────────────────────────
    ("the boot files go on the ESP before the partition is filled",
     "a reboot in the middle leaves the firmware pointing at an empty volume",
     "    if(cb) cb(step, total, \"Formatting the new partition...\");\n"
     "    if(!fat16_format(add_wr, 0, &s_geom, (uint32_t)first, \"HAVENDOS\"))\n"
     "        return INSTADD_E_FORMAT;",
     "    rep->esp_rc = esp_install(disk, &rep->esp, entry, 0);\n"
     "    if(rep->esp_rc != ESP_OK)                   return INSTADD_E_ESP;\n"
     "    if(!fat16_format(add_wr, 0, &s_geom, (uint32_t)first, \"HAVENDOS\"))\n"
     "        return INSTADD_E_FORMAT;"),

    ("the volume is addressed from sector 2048 instead of its own start",
     "the format lands 500 GB away from the partition, inside Windows",
     "    s_part_lba     = first;",
     "    s_part_lba     = PART_START_SECT;"),

    ("the geometry computed for the gap is not the one used to format",
     "a BPB describing one layout and a FAT written in another",
     "    s_geom         = rep->geom;",
     "    ;"),

    ("the cluster counter is not reset between installs",
     "the second install allocates clusters the first one used",
     "    s_next_cluster = 2;\n"
     "    s_geom         = rep->geom;",
     "    s_geom         = rep->geom;"),

    # ── the ESP ─────────────────────────────────────────────────────
    ("the boot files are never put on the ESP",
     "a complete installation that nothing on the machine points at",
     "    rep->esp_rc = esp_install(disk, &rep->esp, entry, 0);\n"
     "    if(rep->esp_rc != ESP_OK)                   return INSTADD_E_ESP;\n"
     "\n"
     "    /* ── read it back ─",
     "    ;\n"
     "\n"
     "    /* ── read it back ─"),

    ("the ESP is installed onto disk 0 whatever disk was asked for",
     "a survey of one disk and a write to another, with nothing to object",
     "    rep->esp_rc = esp_install(disk, &rep->esp, entry, 0);",
     "    rep->esp_rc = esp_install(0, &rep->esp, entry, 0);"),

    # ── the read-back ───────────────────────────────────────────────
    ("nothing is read back at the end",
     "\"the writes returned success\" is a different claim from \"it is there\"",
     "        if(!inst_rd(0, v))                      return INSTADD_E_VERIFY;\n"
     "        if(v[510] != 0x55 || v[511] != 0xAA)    return INSTADD_E_VERIFY;",
     "        ;"),

    ("the kernel's write result is ignored",
     "an install that reports success with a truncated kernel on the disk",
     "        if(!r)                                  return INSTADD_E_FILES;",
     "        ;"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tia_inj")
    b = subprocess.run(CC + ["-o", exe, TEST], capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    if r.returncode == 2:
        return "caught-exit2", r.stdout       # the sparse model refused
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    original = open(SRC, encoding="utf-8").read()

    print("Injecting %d real bugs into the additive installer in %s.\n"
          "Two of them are not inventions: see the docstring.\n" % (len(BUGS), SRC))

    st, out = build_and_run(1800)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-3000:])
        return 2
    print("  baseline: the unmodified installer passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-62s INJECTION STALE (%d)" % (i, label, n))
                continue
            open(SRC, "w", encoding="utf-8").write(original.replace(old, new, 1))
            st, _ = build_and_run(900)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            elif st == "caught-exit2":
                verdict = "caught (ran the model out of sectors)"
            else:
                verdict = "caught"
            print("  %2d. %-62s %s" % (i, label, verdict))
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
