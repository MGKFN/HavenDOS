#!/usr/bin/env python3
"""check_update_suite.py -- prove tests/test_update.c can fail.

fs/update_src.c decides where a new kernel comes from and whether it is
the one it claims to be, and then fs/update.c writes it over the kernel
the machine boots.  The failure mode is not subtle -- the machine does not
come back -- but the bugs that cause it are, because they are all of the
form "a check that looks like it is there".

Every harness written for this tree so far has found holes: FAT32 missed
10 of 26, ESP 8 of 16, the FAT16 formatter 6 of 21, the additive installer
9 of 21, AHCI 12 of 32.  A suite that passes on its first run is the least
trustworthy result available.

Run:  python3 tools/check_update_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/update_src.c"
TEST = "tests/test_update.c"
EXTRA = ["fs/gpt.c", "fs/exfat.c"]
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    # ── the manifest parser ──────────────────────────────────────────
    ("the magic line is not checked",
     "any file at that path becomes an update, including Windows' own",
     "            if(i >= len || text[i] != MAGIC[i]) return UPDM_E_MAGIC;",
     "            if(i >= len) return UPDM_E_MAGIC;"),

    ("the format number is not checked",
     "a manifest from a later HavenDOS is misread field by field",
     "            if(fv != 1u) return UPDM_E_FORMAT;",
     "            ;"),

    ("rubbish after the format number is tolerated",
     "'HAVENDOS-UPDATE 1 or so' is not a manifest and must not look like one",
     "        if(i < len && text[i] != '\\n') return UPDM_E_MAGIC;",
     "        ;"),

    ("the required fields are not required",
     "a manifest with no size parses, and the size is then zero",
     "    if(!out->have_version || !out->have_size || !out->have_kernel)\n"
     "        return UPDM_E_FIELD;",
     "    ;"),

    ("a size of zero is accepted",
     "a zero-length kernel is written over the one that boots",
     "    if(out->size == 0) return UPDM_E_VALUE;",
     "    ;"),

    ("the size is allowed to overflow 32 bits",
     "a wrapped size compares as small enough for the buffer",
     "                    if(v > 0x19999999u) return UPDM_E_VALUE;",
     "                    ;"),

    ("a size with letters in it is accepted",
     "'12x4' becomes 12, and the image is read short",
     "                    if(text[z] < '0' || text[z] > '9') return UPDM_E_VALUE;",
     "                    if(text[z] < '0' || text[z] > '9') break;"),

    ("kernel= may contain a path",
     "a manifest that can name ..\\Microsoft\\Boot\\bootmgfw.efi",
     "                    if(text[z] == '/' || text[z] == '\\\\' || text[z] == ':')\n"
     "                        return UPDM_E_VALUE;",
     "                    ;"),

    ("a crc wider than 32 bits is accepted",
     "the top digits are shifted off and the rest compares equal to nothing",
     "                    if(n >= 8) return UPDM_E_VALUE;   /* wider than 32 bits */",
     "                    ;"),

    ("a crc with a non-hex digit is accepted",
     "half a checksum is worse than none, because it is believed",
     "                    else return UPDM_E_VALUE;",
     "                    else break;"),

    ("out is not initialised before the parse",
     "a refused manifest leaves whatever the caller's stack held",
     "        out->have_version = 0; out->have_kernel = 0;\n"
     "        out->have_size = 0;    out->have_crc = 0;",
     "        ;"),

    ("trailing CR is left on a value",
     "every manifest written on Windows ends its lines with one",
     "            while(ve > vs && (text[ve-1] == ' '  || text[ve-1] == '\\t' ||\n"
     "                              text[ve-1] == '\\r')) ve--;",
     "            while(ve > vs && (text[ve-1] == ' '  || text[ve-1] == '\\t')) ve--;"),

    # ── the ESP source ───────────────────────────────────────────────
    ("an ESP with no update on it is reported as a source",
     "upd_source_select() commits to a source it cannot read. The "
     "anchor carries the is_esp line above it because v0.8.6.3 gave "
     "the USB source a probe with the same shape, and an old anchor "
     "that matched both silently stopped matching anything",
     "        if(!v[i].is_esp) continue;              /* found by type GUID    */\n"
     "        if(!fat32_vol_mount_ro(&v[i])) continue;\n"
     "        if(fat32_exists(UPD_ESP_MANIFEST)){",
     "        if(!v[i].is_esp) continue;              /* found by type GUID    */\n"
     "        if(!fat32_vol_mount_ro(&v[i])) continue;\n"
     "        if(1){"),

    # v0.8.6.3: and the same defect in the USB probe, which is a
    # separate function and would otherwise be guarded by nothing.
    ("a stick with no update on it is reported as a source",
     "same defect, other probe. The two share a reader but not a "
     "probe, so each needs its own injection",
     "        g_usb_why.mounted++;\n"
     "        if(fat32_exists(UPD_ESP_MANIFEST)){",
     "        g_usb_why.mounted++;\n"
     "        if(1){"),

    ("a FIXED disk is accepted as a stick (FAT32 arm)",
     "the removable check is the ONLY thing separating the USB source "
     "from the ESP source. Without it, `update` reads a manifest off "
     "the machine's own disk and calls it a USB stick on screen",
     "        if(!blk_removable(v[i].disk)) continue;\n"
     "        g_usb_why.volumes++;",
     "        g_usb_why.volumes++;"),

    ("a FIXED disk is accepted as a stick (exFAT arm)",
     "the same check, on the arm v0.8.8.2 added. It went in as a copy "
     "of the FAT32 one and a copy is a thing that can be forgotten -- "
     "which is why this is its own injection rather than one that "
     "happens to match twice",
     "        if(!blk_removable(v[i].disk)) continue;\n"
     "        g_usb_why.xvolumes++;",
     "        g_usb_why.xvolumes++;"),

    ("the stick is probed AFTER the EFI System Partition",
     "a staged ESP update is never deleted after it applies, so the "
     "leftover would be found first for ever and a stick plugged in "
     "two minutes ago would never be looked at",
     """    if(SRC_USB.probe()) return &SRC_USB;
    if(SRC_ESP.probe()) return &SRC_ESP;""",
     """    if(SRC_ESP.probe()) return &SRC_ESP;
    if(SRC_USB.probe()) return &SRC_USB;"""),

    ("a volume that is not the ESP is used anyway",
     "the update is looked for on somebody else's data partition",
     "        if(!v[i].is_esp) continue;              /* found by type GUID    */",
     "        ;"),

    ("the staged file's length is not checked against the manifest",
     "a truncated copy is read as though it were whole",
     "    if(fat32_file_size(path) != m->size) return 0;  /* before a byte is read */",
     "    ;"),

    ("the image is read into a buffer without checking it fits",
     "m->size comes out of a text file somebody typed",
     "    if(m->size > bufsz) return 0;\n"
     "\n"
     "    /* UPD_ESP_DIR \"/\" kernel, built here rather than in the manifest, so\n"
     "       the directory is not something a manifest can choose. */",
     "    /* UPD_ESP_DIR \"/\" kernel, built here rather than in the manifest, so\n"
     "       the directory is not something a manifest can choose. */"),

    ("the update directory is not prepended to the kernel name",
     "the manifest's name is used as a whole path after all",
     "    const char *d = UPD_ESP_DIR;\n"
     "    int i = 0, j;\n"
     "    for(i = 0; d[i] && i < cap - 2; i++) path[i] = d[i];\n"
     "    path[i++] = '/';",
     "    const char *d = UPD_ESP_DIR;\n"
     "    int i = 0, j;\n"
     "    (void)d;"),

    ("release does not unmount the volume it borrowed",
     "fs/fat32.c holds one volume at a time, and that one is the system's",
     "    fat32_vol_release();\n"
     "    g_f32_open = 0;",
     "    g_f32_open = 0;"),

    ("a probe that fails leaves the volume mounted",
     "an ESP with no update on it costs the machine its own FAT32 "
     "mount. Anchored on the comment above the release, because "
     "v0.8.6.3's USB probe ends in the same four lines",
     """        /* An ESP with no update staged on it is not an error and not a
           source.  Put it back and keep looking: a machine can have more
           than one, and the one with the update on it is the one we
           want. */
        fat32_vol_release();""",
     """        /* An ESP with no update staged on it is not an error and not a
           source.  Put it back and keep looking: a machine can have more
           than one, and the one with the update on it is the one we
           want. */
        ;"""),

    ("a USB probe that fails leaves the volume mounted",
     "same defect, other probe: a stick with somebody's photos on it "
     "costs the machine its own FAT32 mount",
     """        /* A stick with no update staged on it is not an error and not a
           source -- somebody's photos are not a failed update.  Put it
           back and keep looking. */
        fat32_vol_release();""",
     """        /* A stick with no update staged on it is not an error and not a
           source -- somebody's photos are not a failed update.  Put it
           back and keep looking. */
        ;"""),

    # ── the CD-ROM source ────────────────────────────────────────────
    ("a synthesised manifest claims to carry a checksum",
     "an ISO made before 0.8.4 has none, and the screen would say it does",
     "        m->have_size = 0;    m->have_crc = 0;",
     "        m->have_size = 0;    m->have_crc = 1;"),

    ("a manifest on a disc is believed over the disc's own directory entry",
     "the length that matters is the one the filesystem knows",
     "        if(code == UPDM_OK && m->size != g_cd_size) return UPDM_E_VALUE;",
     "        ;"),

    ("the disc is read from the wrong sector",
     "an off-by-one in an LBA is 2048 bytes of the previous file",
     "    lba  = g_cd_lba;",
     "    lba  = g_cd_lba + 1;"),

    ("a missing /version.txt becomes a version rather than 'unknown'",
     "an empty string compares as older than everything and downgrades silently",
     "        const char *u = \"unknown\";",
     "        const char *u = \"\";"),

    ("a disc whose kernel is zero bytes is offered as a source",
     "FOUND BY THIS SUITE: probe said yes and the manifest step then refused",
     "    if(g_cd_size == 0) return 0;",
     "    ;"),

    # ── choosing, and naming ─────────────────────────────────────────
    ("the disc is preferred over the EFI System Partition",
     "the stale disc in the tray beats the file somebody just copied over",
     "    if(SRC_ESP.probe()) return &SRC_ESP;\n"
     "    if(SRC_CD.probe())  return &SRC_CD;",
     "    if(SRC_CD.probe())  return &SRC_CD;\n"
     "    if(SRC_ESP.probe()) return &SRC_ESP;"),

    ("both sources are given the same name on screen",
     "0.8.3.1 shipped exactly this: the picker said NVMe, the confirm said VirtIO",
     "    case UPD_SRC_CDROM: return \"CD-ROM\";",
     "    case UPD_SRC_CDROM: return \"EFI System Partition\";"),

    ("the checksum function is not the one the GPT uses",
     "a CRC only our own code can compute is our code agreeing with itself",
     "    return gpt_crc32(buf, len);",
     "    return gpt_crc32(buf, len) ^ 1u;"),
    # ── the exFAT arm, v0.8.8.2 ──────────────────────────────────────
    ("the USB source never looks at exFAT",
     "which is how it was until v0.8.8.2, and a USB stick of 32 GB or "
     "more is formatted exFAT by default. The refusal was by design "
     "and printed in small print, and it was reported as 'USB "
     "discovery still broken for 32 GB drives' four releases running",
     """    n = exfat_scan_volumes(v, FAT32_MAX_VOLS);""",
     """    n = 0;"""),

    ("the exFAT arm does not check the staged file's length",
     "the file's real size is compared with the manifest's BEFORE a "
     "byte is read. Without it a short file is read as m->size bytes "
     "and the tail of the buffer is whatever was there -- which the "
     "checksum then runs over. The FAT32 arm is injection 18; this is "
     "the same rule on the arm added in v0.8.8.2",
     """    if(st.size != m->size) return 0;            /* before a byte is read */""",
     """    if(0) return 0;"""),

    # ── NOT A DEFECT ON ANY FIXTURE THIS CAN BUILD, and recorded ────
    #
    # "the exFAT arm reads a DIRECTORY as a kernel" was written as an
    # injection -- remove `if(st.is_dir) return 0;` -- and reported
    # MISSED. tests/update_stick.img DOES carry a directory where a
    # kernel would be (/EFI/HAVENDOS/update/notakernel, added for
    # exactly this), and the suite does point a manifest at it.
    #
    # It is missed because the check on the line after it refuses
    # first: a directory's reported size is its allocation, which is
    # never the staged kernel's 393216 bytes, so `st.size != m->size`
    # returns 0 before is_dir would have. Making the two
    # distinguishable needs a directory whose size happens to equal
    # the kernel's, which is not something a fixture generator can
    # arrange through mkfs.exfat and the FUSE driver.
    #
    # The check STAYS. It is ordered first on purpose and it is the
    # one that still holds if somebody later relaxes the size
    # comparison -- which injection 34 shows is a real hazard. But it
    # is defence, not a tested behaviour, and saying so is the
    # difference between coverage and the appearance of it.
    #
    # Left as a note because v0.8.6.3's harness proved two of its ten
    # injections not to be defects, and an injection deleted without a
    # word is one somebody writes again.


    ("a stick is released on the wrong driver",
     "usb_src_release() has to unmount whichever of the two is open. "
     "Releasing fat32 while exfat holds the volume leaves the stick "
     "mounted for the rest of the session, and the next probe short-"
     "circuits on it",
     """    if(g_xf_open){ exfat_unmount(); g_xf_open = 0; return; }""",
     """    if(0){ exfat_unmount(); g_xf_open = 0; return; }"""),

    ("the exFAT volumes are counted into the FAT32 tally",
     "'no FAT32 volume on it' and 'an exFAT volume that would not "
     "mount' send somebody to two different places, and v0.8.6.5's "
     "whole lesson was that one counter for four failures is a "
     "diagnostic that lies",
     """        g_usb_why.xvolumes++;""",
     """        g_usb_why.volumes++;"""),

]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tupd_inj")
    b = subprocess.run(CC + ["-o", exe, TEST] + EXTRA,
                       capture_output=True, text=True)
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

    print("Injecting %d real update-path bugs into %s.\n"
          "This is the code that decides which bytes replace the running\n"
          "kernel, and whether they are the bytes they claim to be.\n"
          % (len(BUGS), SRC))

    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified source passes\n")

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
