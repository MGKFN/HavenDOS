#!/usr/bin/env python3
"""check_esp_suite.py -- prove tests/test_esp.c can fail.

fs/esp.c writes into the EFI System Partition that Windows boots from. A
bug here is the quietest dangerous bug in the whole project: the partition
table stays perfect, every partition stays where it was, `disk` reports
everything fine -- and the machine stops booting Windows, because one
directory sector got a read-modify-write wrong.

The bugs below are the ones an installer actually ships: space checked
after the first write instead of before, a file overwritten without a copy
being taken, a verify that only looks at the first sector, a dry run that
is not dry. Each is applied in turn and the suite must notice every one.

Run:  python3 tools/check_esp_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "fs/esp.c"
TEST = "tests/test_esp.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("the existing fallback loader is overwritten, not copied aside",
     "somebody's other bootloader, gone, with no way back",
     "    if(rep->would_back_up){\n"
     "        extern void *pmm_alloc_contig(uint32_t nblocks);",
     "    if(0 && rep->would_back_up){\n"
     "        extern void *pmm_alloc_contig(uint32_t nblocks);"),

    ("a previous backup is silently overwritten",
     "the second install eats the file the first one saved",
     "        if(fat32_exists(P_FALLBACK_BAK)) return ESP_E_BACKUP;",
     "        ;"),

    ("free space is not checked at all",
     "a FAT32 volume that runs out mid-write leaves a truncated loader",
     "        if(rep->free_kb < rep->need_kb) return ESP_E_SPACE;",
     "        ;"),

    ("the space needed forgets the second copy of the loader",
     "the fallback copy is as big as the first and is written too",
     "        need += ((lsz + cl - 1) / cl) * cl;          /* the fallback copy         */",
     "        ;"),

    ("files are not read back after writing",
     "\"the write returned success\" is a different claim from \"the bytes are there\"",
     "    if(!verify_file(path, data, len))               return ESP_E_VERIFY;",
     "    ;"),

    ("verify compares the size and nothing else",
     "a file of the right length full of the wrong bytes passes",
     "    while(off < len){\n"
     "        uint32_t n = len - off;",
     "    return 1;\n"
     "    while(off < len){\n"
     "        uint32_t n = len - off;"),

    ("verify checks only the first sector",
     "the tail of an 811 KB loader is never looked at",
     "            if((uint8_t)chunk[i] != want[off + i]) return 0;\n"
     "        off += n;\n"
     "    }\n"
     "    return 1;\n"
     "}",
     "            if((uint8_t)chunk[i] != want[off + i]) return 0;\n"
     "        off += n;\n"
     "        break;\n"
     "    }\n"
     "    return 1;\n"
     "}"),

    # Not injected, and this one is worth writing down rather than
    # quietly dropping. Removing the size check in verify_file() is not
    # detectable by this suite, and the reason is that it is genuinely
    # defence in depth: fat32_read_at() clips a read to the file's length,
    # so a file SHORTER than expected already fails the byte comparison.
    # The size check earns its place only when the file on disk is LONGER
    # than what was written -- a stale directory entry surviving a
    # rewrite -- and fat32_write() always rewrites that entry, so the
    # suite cannot manufacture the case without hand-corrupting a
    # directory sector. The guard stays, because the cost is one
    # comparison and the failure it covers is real on a dying disk; it is
    # recorded here as untested rather than pretended to be tested.

    ("the dry run writes anyway",
     "the mode whose entire purpose is that it cannot write",
     "    if(dry_run) return ESP_OK;",
     "    if(dry_run) dry_run = 0;"),

    ("the backup copy is never verified",
     "a corrupt copy is worse than no copy, because it is trusted",
     "        if(ok) ok = (write_verified(P_FALLBACK_BAK, buf, sz) == ESP_OK);",
     "        if(ok){ fat32_write(P_FALLBACK_BAK,(const char*)buf,sz); ok=1; }"),

    ("the grub.cfg offers no way back to Windows",
     "an install that takes the boot path and does not give Windows back",
     "    \"menuentry \\\"Windows Boot Manager\\\" {\\n\"\n"
     "    \"    search --no-floppy --set=root --file /EFI/Microsoft/Boot/bootmgfw.efi\\n\"\n"
     "    \"    chainloader /EFI/Microsoft/Boot/bootmgfw.efi\\n\"\n"
     "    \"}\\n\"",
     "    \"\""),

    # Deliberately NOT injected: "the FAT is never flushed". fat32_write()
    # already ends with fat32_flush(), so the explicit flush in esp.c is
    # belt-and-braces and removing it changes nothing observable. It is
    # worth keeping -- a future change to fat32_write would make it load
    # bearing again -- but a harness entry for it would report a MISS that
    # means "this is not a bug", which is noise. Same reasoning as the
    # redundant guards found in fs/fat32.c and fs/gpt_write.c.

    ("the menu has no kernel command line",
     "FOUND BY BOOTING IT: the installed system comes up as the installer",
     "    \"    multiboot /boot/havendos.elf mode=desktop gfx=text\\n\"",
     "    \"    multiboot /boot/havendos.elf\\n\""),

    ("the menu boots the installed system into the installer",
     "install=1 belongs to the ISO's entry and nowhere else",
     "    \"    multiboot /boot/havendos.elf mode=desktop gfx=text\\n\"",
     "    \"    multiboot /boot/havendos.elf mode=desktop gfx=text install=1\\n\""),

    ("the loader is written at the wrong length",
     "an off-by-one in a size is a loader the firmware refuses",
     "    r = write_verified(P_LOADER, grub_efi_img, grub_efi_img_size);",
     "    r = write_verified(P_LOADER, grub_efi_img, grub_efi_img_size - 1);"),

    ("the config is written with a truncated length",
     "the same mistake on the file that holds the Windows entry",
     "    r = write_verified(P_CFG, (const uint8_t *)CFG_TEXT, cstrlen(CFG_TEXT));",
     "    r = write_verified(P_CFG, (const uint8_t *)CFG_TEXT, 40);"),

    # Also not injected: ignoring a failed mkdir. On a read-only mount the
    # following write_verified() fails anyway, so the mkdir check is a
    # second line rather than the first. Keeping it, not testing it.

    # ── the loader itself ─────────────────────────────────────────────
    # Not hypothetical: the tree was built and an ISO was shipped with
    # HAVE_GRUB_EFI_EMBED undefined, so fs/esp.c compiled against its
    # zero-byte stub. Every suite passed. tools/check_efi_embed.py now
    # fails the build on it; these make sure the runtime refusal that
    # protects the disk cannot be removed quietly either.
    ("the loader is never checked for being a loader",
     "a zero-byte BOOTX64.EFI written over the one the machine boots",
     "    if(!loader_is_pe32plus(grub_efi_img, grub_efi_img_size))\n"
     "        return ESP_E_LOADER;",
     "    ;"),

    ("the loader check always says yes",
     "the same outcome, reached by a check that looks like it is there",
     "    if(len < ESP_LOADER_MIN)            return 0;",
     "    return 1;\n"
     "    if(len < ESP_LOADER_MIN)            return 0;"),

    ("there is no minimum size",
     "a 1-byte stub, and len-6u wrapping to 4G so every offset is in bounds",
     "    if(len < ESP_LOADER_MIN)            return 0;",
     "    ;"),

    ("the MZ magic is not checked",
     "a raw blob installed as a loader; the firmware refuses, we do not",
     "    if(img[0] != 'M' || img[1] != 'Z')  return 0;",
     "    ;"),

    ("the PE signature is not checked",
     "MZ is two bytes, and two bytes is not evidence of an executable",
     "    if(img[e_lfanew + 0] != 'P' || img[e_lfanew + 1] != 'E'\n"
     "       || img[e_lfanew + 2] != 0 || img[e_lfanew + 3] != 0) return 0;",
     "    ;"),

    ("the machine type is not checked",
     "include/grub_embed.h is i386-pc; writing one to BOOTX64.EFI boots nothing",
     "    if(img[e_lfanew + 4] != 0x64 || img[e_lfanew + 5] != 0x86) return 0;",
     "    ;"),

    ("the PE offset is trusted without a bounds check",
     "a header offset read out of a file is an index into our own memory",
     "    if(e_lfanew > len - 6u)             return 0;",
     "    ;"),

    ("the loader is checked only when writing, not in the survey",
     "the dry run whose job is to tell you this reports ok and then it fails",
     "    if(!loader_is_pe32plus(grub_efi_img, grub_efi_img_size))\n"
     "        return ESP_E_LOADER;\n\n"
     "    cl = fat32_cluster_bytes();",
     "    cl = fat32_cluster_bytes();"),

    # The first version of this injection tried to reorder the fallback
    # write ahead of the backup, and was a no-op: the backup block sits
    # earlier in the function regardless, so the order never changed. What
    # a real implementation gets wrong here is subtler and worse -- it
    # takes a backup of the wrong bytes, so the file is saved and the
    # contents are ours, not theirs.
    # ── v0.8.4: the reinstall decision ───────────────────────────────
    ("everything at the fallback path counts as ours",
     "a stranger's bootloader is overwritten with no copy taken at all",
     "static int fallback_is_ours(void)\n{\n"
     "    if(file_eq_mem(P_FALLBACK, grub_efi_img, grub_efi_img_size)) return 1;",
     "static int fallback_is_ours(void)\n{\n"
     "    return 1;\n"
     "    if(file_eq_mem(P_FALLBACK, grub_efi_img, grub_efi_img_size)) return 1;"),

    ("nothing counts as ours, ever",
     "back to v0.8.3: a second install is refused and needs a manual .HDB dance",
     "static int fallback_is_ours(void)\n{\n"
     "    if(file_eq_mem(P_FALLBACK, grub_efi_img, grub_efi_img_size)) return 1;",
     "static int fallback_is_ours(void)\n{\n"
     "    return 0;\n"
     "    if(file_eq_mem(P_FALLBACK, grub_efi_img, grub_efi_img_size)) return 1;"),

    ("'is it ours' compares the length and nothing else",
     "somebody else's loader that happens to be the same size is eaten",
     "        for(i = 0; i < n; i++)\n"
     "            if(g_cmp_a[i] != mem[off + i]) return 0;",
     "        ;"),

    ("'is it the one we wrote last time' compares the length only",
     "the same mistake on the upgrade path, where the loaders differ",
     "        for(i = 0; i < n; i++)\n"
     "            if(g_cmp_a[i] != g_cmp_b[i]) return 0;",
     "        ;"),

    ("the install re-derives the backup decision instead of using it",
     "the preview screen and the write disagree -- the 0.8.3.1 bus-label bug",
     "       than working it out a second time. */\n"
     "    if(rep->would_back_up){",
     "       than working it out a second time. */\n"
     "    if(rep->has_fallback){"),

    ("the backup is taken from the new loader, not the existing one",
     "a backup that restores our file instead of the one it replaced",
     "        int ok = (fat32_read_at(P_FALLBACK, 0, (char *)buf, sz) == (int)sz);",
     "        int ok = 1; memcpy(buf, grub_efi_img, sz);"),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_tesp_inj")
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

    print("Injecting %d real installer bugs into %s.\n"
          "This is the code that writes to the partition Windows boots from.\n"
          % (len(BUGS), SRC))

    st, out = build_and_run(600)
    if st != "pass":
        print("  the UNMODIFIED tree does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified installer passes\n")

    missed, stale = [], []
    try:
        for i, (label, why, old, new) in enumerate(BUGS, 1):
            n = original.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-56s INJECTION STALE (%d)" % (i, label, n))
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
            print("  %2d. %-56s %s" % (i, label, verdict))
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
