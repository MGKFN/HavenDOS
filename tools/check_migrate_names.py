#!/usr/bin/env python3
"""check_migrate_names.py -- v0.9.6: every name `migrate` must carry by name.

`migrate` copies a FAT16 install to FAT32 entry by entry, under the names
FAT16 lists -- which for anything that is not 8.3-legal is a mangled name
("_account" for ".accounts"). The system on FAT32 asks for the real name.
So every file the source keeps under a name FAT16 cannot hold has to be in
fs/migrate_plan.c's MIG_LONG_NAMES, or a moved system loses it: for
.accounts, every user and password, and the next boot is the setup wizard.

This reads the source for file names given to the filesystem as literals
-- vfs_*("..."), sysvol_*("..."), and #define X_FILE "..." -- and fails if
one has a component that is not 8.3 and is not in that list.

It tests itself first, on a line it makes up, so a pattern that has
stopped matching anything cannot pass by finding nothing.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ("shell", "kernel", "fs", "usb", "net", "drivers", "include")

CALL = re.compile(r'\b(?:vfs|sysvol|fat16|fat32)_(?:read|write|exists|delete|rename|mkdir|'
                  r'write_path|read_path|chmod)\s*\(\s*"([^"]+)"')
DEFN = re.compile(r'#\s*define\s+\w*_FILE\w*\s+"([^"]+)"')


def legal83(comp):
    if comp in (".", ".."):
        return True
    if comp.startswith(".") or comp.count(".") > 1:
        return False
    base, _, ext = comp.partition(".")
    ok = re.fullmatch(r"[A-Za-z0-9_\-~!#$%&'(){}^@`]+", base + ext) is not None
    return ok and 1 <= len(base) <= 8 and len(ext) <= 3


def long_names():
    src = open(os.path.join(ROOT, "fs", "migrate_plan.c")).read()
    m = re.search(r"MIG_LONG_NAMES\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("check_migrate_names: MIG_LONG_NAMES not found in fs/migrate_plan.c")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def names_in(text):
    out = []
    for rx in (CALL, DEFN):
        for m in rx.finditer(text):
            out.append(m.group(1))
    return out


def problems(text, listed, where):
    bad = []
    for name in names_in(text):
        if "%" in name or "://" in name:
            continue                      # a format string or a URL, not a file
        for comp in name.strip("/").split("/"):
            if comp and not legal83(comp) and comp not in listed:
                bad.append("%s: \"%s\" -- '%s' is not 8.3 and not in "
                           "MIG_LONG_NAMES" % (where, name, comp))
    return bad


def main():
    listed = long_names()

    # the self-test: a made-up line must be caught, and a listed one not
    probe = '#define NEW_FILE ".brand_new_setting"\nvfs_read(".accounts", b, n);\n'
    if len(problems(probe, listed, "self-test")) != 1:
        sys.exit("check_migrate_names: the self-test did not catch a made-up "
                 "dotfile -- the patterns have stopped matching")

    bad, seen = [], set()
    for d in DIRS:
        for dp, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                p = os.path.join(dp, f)
                text = open(p, errors="replace").read()
                rel = os.path.relpath(p, ROOT)
                bad += problems(text, listed, rel)
                seen.update(names_in(text))

    if bad:
        print("check_migrate_names: %d name(s) `migrate` would lose" % len(bad))
        for b in bad:
            print("  " + b)
        print("\n  Add each to MIG_LONG_NAMES in fs/migrate_plan.c, or give the\n"
              "  file an 8.3 name.")
        return 1
    stale = sorted(n for n in listed if n not in seen)
    print("check_migrate_names: ok, %d literal file names, %d carried by name%s"
          % (len(seen), len(listed),
             (" (not seen as literals: %s)" % ", ".join(stale)) if stale else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
