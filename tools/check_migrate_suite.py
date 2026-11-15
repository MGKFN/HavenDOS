#!/usr/bin/env python3
"""check_migrate_suite.py -- prove tests/test_migrate.c can fail.

fs/migrate_plan.c is where `migrate` decides what a partition holds, what
FAT16 called a file, and what the boot menu says. Each injection below is
a way one of those goes wrong that the QEMU harness would only find by
losing somebody's data or their boot:

  - a signature is not recognised: an NTFS or BitLocker volume is offered
    as "empty", and ERASE formats it;
  - an unknown boot sector is taken as empty;
  - Microsoft reserved is takeable (it never has a filesystem, so the
    signature test cannot save it);
  - the dotfile name map drops the underscore, or forgets the legacy form:
    the copy carries .accounts under the wrong name and the moved system
    has no users;
  - the menu searches by label (two volumes share it), drops vol=, puts
    the old install first, or makes HavenDOS the default;
  - the serial is printed in the wrong order, and GRUB finds nothing.

Run:  python3 tools/check_migrate_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "fs/migrate_plan.c"
TEST = "tests/test_migrate.c"
CC = ["gcc", "-std=gnu99", "-O1", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("NTFS is not recognised",
     "the Windows C: drive is offered as an empty partition",
     """    if(at(s0, 3, "NTFS    "))       return "NTFS";\n""",
     ""),

    ("BitLocker is not recognised",
     "an encrypted volume has no other signature at all",
     """    if(at(s0, 3, "-FVE-FS-"))       return "BitLocker";\n""",
     ""),

    ("ext4's magic is read at the wrong offset",
     "a Linux root partition is offered as empty",
     """    if(s2[56] == 0x53 && s2[57] == 0xEF) return "ext2/3/4";""",
     """    if(s2[57] == 0x53 && s2[58] == 0xEF) return "ext2/3/4";"""),

    ("an unknown boot sector counts as empty",
     "the rule is to stay off what we cannot name, not to guess",
     """    if(s0[510] == 0x55 && s0[511] == 0xAA) return "an unknown filesystem";""",
     """    (void)0;"""),

    ("an unreadable sector counts as empty",
     "a read error is not evidence the partition is blank",
     """    if(!s0 || !s2 || !s7 || !s128) return "unreadable";""",
     """    if(!s0 || !s2 || !s7 || !s128) return 0;"""),

    ("Microsoft reserved is takeable",
     "the MSR never has a filesystem, so no signature test protects it",
     """    return geq(t, T_MSDATA) || geq(t, T_LINUX);""",
     """    return geq(t, T_MSDATA) || geq(t, T_LINUX) || geq(t, T_MSR);"""),

    ("the dotfile underscore is dropped",
     ".accounts would be looked for on FAT16 as 'account', and copied "
     "under its FAT16 name instead of its real one",
     """    if(p[0] == '.' && p[1]){ n8[i++] = '_'; p++; }""",
     """    if(p[0] == '.' && p[1]){ p++; }"""),

    ("the legacy form is not skipped",
     "a pre-0.9.3 .acc entry is copied as itself AND as .accounts",
     """        mig_fat16_legacy_name(MIG_LONG_NAMES[i], f);
        if(streq_ci(f, listed)) return 1;""",
     """        mig_fat16_legacy_name(MIG_LONG_NAMES[i], f);"""),

    (".accounts is not carried by name",
     "the moved system has no users and runs setup",
     """    ".accounts",          /* shell/login.c         -- the users, hashed   */\n""",
     ""),

    ("the menu searches by label",
     "both volumes are labelled HAVENDOS; GRUB takes the first it finds",
     """    put(b, "    search --no-floppy --set=root --fs-uuid "); put(b, ser); put(b, "\\n");""",
     """    put(b, "    search --no-floppy --set=root --label HAVENDOS\\n"); (void)ser;"""),

    ("vol= is left off the kernel line",
     "GRUB loads the right kernel and the kernel mounts the wrong volume",
     """    put(b, "    multiboot /boot/havendos.elf " MIG_ARGS " vol="); put(b, ser); put(b, "\\n");""",
     """    put(b, "    multiboot /boot/havendos.elf " MIG_ARGS "\\n");"""),

    ("HavenDOS becomes the default",
     "Windows was the default; a migration is not a reason to change it",
     """    put(&b, "set timeout=8\\nset default=1\\n\\n");""",
     """    put(&b, "set timeout=8\\nset default=0\\n\\n");"""),

    ("the old install keeps its entry after finish",
     "`migrate finish` removes a partition the menu still points at",
     """    if(old_serial)
        entry(&b,""",
     """    if(1)
        entry(&b,"""),

    ("the serial's halves are swapped",
     "GRUB prints the high word first; swapped, --fs-uuid finds nothing",
     """    for(int i = 0; i < 4; i++) out[i]     = hx[(v >> (28 - 4 * i)) & 15];""",
     """    for(int i = 0; i < 4; i++) out[i]     = hx[(v >> (12 - 4 * i)) & 15];"""),

    ("a full buffer returns a truncated menu",
     "a grub.cfg cut off mid-entry is a machine that boots nothing",
     """    return b.bad ? -1 : b.n;
}

int mig_part_menu""",
     """    return b.n;
}

int mig_part_menu"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """/* ── menus ─────────""",
    """/* ── the menus ─────""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "migrate", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        preamble="Proving tests/test_migrate.c can fail.\n"
                 "Each injection is a way `migrate` takes the wrong partition, "
                 "loses a file's name, or writes a menu that boots the wrong thing."))
