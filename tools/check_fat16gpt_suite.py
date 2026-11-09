#!/usr/bin/env python3
"""check_fat16gpt_suite.py -- prove tests/test_fat16gpt.c can fail.

The bug this suite is about was not caught by any native test. It was
caught by booting the result: harness/uefi_dualboot.py installed HavenDOS
beside Windows under OVMF, the firmware ran the loader, GRUB ran, the
kernel started -- and the installer came up, on a machine that had just
been installed, because fs/fat16.c looked for the volume at sector 2048
and the additive installer had put it at 11053056.

So the suite that now covers it had better be able to fail.

Run:  python3 tools/check_fat16gpt_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/fat16.c"
TEST = "tests/test_fat16gpt.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("the GPT is never consulted",
     "THE BUG THAT SHIPPED: the volume is only ever looked for at 2048",
     "                if(!find_gpt_volume()) return 0;",
     "                return 0;"),

    ("the partition's own LBA is found and then not used",
     "found, reported, and read from the wrong place anyway",
     "        g_part_lba = parts[i].first_lba;\n"
     "        return 1;",
     "        return 1;"),

    ("the base is not reset between mounts",
     "a failed search leaves the previous attempt's LBA in place",
     "    g_part_lba = FAT16_DEFAULT_PART_LBA;\n"
     "\n"
     "    /* ── Check MBR at LBA 0 for a valid FAT16 partition entry ─",
     "    /* ── Check MBR at LBA 0 for a valid FAT16 partition entry ─"),

    ("any partition type is accepted",
     "somebody's FAT16 data partition mounted and written into",
     "        for(k = 0; k < 16; k++)\n"
     "            if(parts[i].type_guid[k] != OURS[k]){ ours = 0; break; }\n"
     "        if(!ours) continue;",
     "        (void)OURS; (void)k;"),

    # Deliberately NOT injected: removing the `is_esp` skip. It is
    # redundant for the OUTCOME, because the type-GUID comparison on the
    # next lines rejects an ESP anyway -- an ESP's type GUID is not
    # HAVENDOS-PART-01. An injection for it would report a MISS that means
    # "this is not a bug".
    #
    # It stays, and is worth the one comparison, because the two checks
    # fail differently: the GUID check is about identifying OUR partition,
    # and the ESP skip is about never touching THAT one, whatever we think
    # we have identified. If the GUID check is ever loosened -- to accept
    # an older install's type, say -- the skip is what stops the loosening
    # reaching the ESP. tests/test_fat16gpt.c asserts the behaviour (an
    # ESP-typed partition with a HavenDOS volume planted in it is not
    # mounted), which is the part that matters.
    #
    # Same reasoning, and the same honesty, as the redundant guards
    # recorded in tools/check_esp_suite.py and check_installadd_suite.py.

    # Not injected either: the two BPB checks inside find_gpt_volume().
    # fat16_init() re-reads sector 0 of whatever base the search settled on
    # and checks the same signature and the same bytes-per-sector before it
    # mounts anything, so removing the early pair changes no outcome -- the
    # mount fails a few lines later instead of the search failing now.
    #
    # They earn their place by stopping the search at the FIRST partition
    # that looks wrong rather than at the first one that looks right-ish:
    # without them the search picks a candidate on the strength of its type
    # GUID and its OEM name alone, and then hands a bad one to the mount
    # instead of carrying on to the next partition. On a disk with two
    # HavenDOS-typed partitions, one of them damaged, that is the
    # difference between booting and not.
    #
    # tests/test_fat16gpt.c asserts the behaviour from the outside: a
    # partition with our type GUID and our OEM name but no valid BPB is
    # not mounted.

    ("the OEM name is not checked",
     "the type GUID alone, on a partition somebody else reused",
     "        if(!(sec[3]=='H'&&sec[4]=='A'&&sec[5]=='V'&&sec[6]=='E'&&sec[7]=='N'))\n"
     "            continue;",
     "        ;"),

    ("the read callback's return convention is inverted",
     "0 means success to fs/gpt.c and 1 to fat16_raw_read",
     "    return fat16_raw_read(lba, (uint8_t *)buf) ? 0 : -1;",
     "    return fat16_raw_read(lba, (uint8_t *)buf);"),

    # Also not injected: removing the gpt_present() guard. gpt_scan() runs
    # next, reads LBA 1 itself, finds no "EFI PART" signature and returns a
    # non-positive count, and the `if(n <= 0) return 0` below produces the
    # same refusal. One sector read either way; the guard says what it is
    # doing, which the fallthrough does not.

    ("partition-relative reads ignore the base entirely",
     "every read lands at the start of the disk",
     "    return fat16_raw_read(g_part_lba + part_lba, buf);",
     "    return fat16_raw_read(part_lba, buf);"),

    ("writes go to the default base while reads use the found one",
     "the subtlest version: it mounts, it reads, and it writes elsewhere",
     "    return fat16_raw_write(g_part_lba + part_lba, buf);",
     "    return fat16_raw_write(FAT16_DEFAULT_PART_LBA + part_lba, buf);"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tf16g_inj")
    b = subprocess.run(CC + ["-o", exe, TEST], capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    if r.returncode == 2:
        return "caught-exit2", r.stdout
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    original = open(SRC, encoding="utf-8").read()

    print("Injecting %d bugs into the volume search in %s.\n"
          "The first one is what shipped.\n" % (len(BUGS), SRC))

    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified driver passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-58s INJECTION STALE (%d)" % (i, label, n))
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
            elif st == "caught-exit2":
                verdict = "caught (ran the model out of sectors)"
            else:
                verdict = "caught"
            print("  %2d. %-58s %s" % (i, label, verdict))
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
