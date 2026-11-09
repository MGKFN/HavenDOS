#!/usr/bin/env python3
"""check_statics.py -- two modules' private state must not merge.

── WHY THIS EXISTS ────────────────────────────────────────────────────

v0.8.7 added net/netdev.c, whose table needed a counter, and it was
called `g_ndev` because fs/blkdev.c's is called `g_ndev`. usb/core.c
also has one, and it is also called `g_ndev`.

In the kernel that is three translation units and three private
variables, which is correct and is what `static` is for.

tests/test_usbnet.c INCLUDES both net/netdev.c and usb/core.c, because
the Apple USB Ethernet driver's whole job is three requests through the
REAL core against a simulated phone, and a stubbed core would let the
driver pass while the core's half of alternate settings was wrong.

The arrays clashed loudly -- `usb_dev_t g_dev[16]` against
`netdev_t g_dev[4]`, two incompatible types, a hard error, fixed in a
minute.

THE COUNTERS DID NOT CLASH AT ALL.

    static int g_ndev;

with no initialiser is a TENTATIVE definition, and C permits any number
of tentative definitions of the same object with the same type in one
translation unit. The two MERGED INTO ONE VARIABLE. No error, no
warning, at any -W level. usb_enumerate() set it to the number of USB
devices on the bus and netdev_count() read it as the number of
registered NICs, so the NIC layer dispatched through a table slot whose
ops pointer was null.

It was found by a segmentation fault and it took a sanitiser to place.

A build that silently merges two modules' private state is not a thing
to leave resting on two files happening to pick different names. This
tree's own rule: four releases of being careful did not work, and one
grep does.

── WHAT IT REFUSES ───────────────────────────────────────────────────

For every suite in tests/ that #includes kernel .c files, a static
OBJECT name defined by two of the files that suite pulls in.

Scoped to suites on purpose. Two unrelated drivers both calling a
counter `g_count` is fine and always will be -- nothing ever compiles
them into one translation unit. It is only a defect when something
does, and the suites are the only things that do.

Static FUNCTIONS are not checked: two of those in one translation unit
is a redefinition the compiler refuses out loud, which is a bad
afternoon rather than a silent wrong answer.

Run:  python3 tools/check_statics.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import blank                                     # noqa: E402

TESTS = "tests"

# `#include "../usb/core.c"` -- a suite pulling a kernel module in.
INC = re.compile(r'^\s*#\s*include\s+"((?:\.\./)+[^"]+\.c)"', re.MULTILINE)

# A static OBJECT definition: `static <type> name;`, `= ...` or `[...]`.
# The `(` alternative is what tells a function apart from an object, and
# it is excluded rather than matched -- a function's name is followed by
# its parameter list.
OBJ = re.compile(
    r'^\s*static\s+'
    r'(?:(?:const|volatile|unsigned|signed|struct|union|enum)\s+)*'
    r'[A-Za-z_][A-Za-z_0-9]*\s*'
    r'(?:\*\s*)*'
    r'([A-Za-z_][A-Za-z_0-9]*)\s*'
    r'(?:\[[^;=]*\]\s*)*'
    r'(?:=|;)',
    re.MULTILINE)


def statics_of(path):
    """Every static object name defined in `path`, with its line."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    code = blank(raw)
    out = {}
    for m in OBJ.finditer(code):
        name = m.group(1)
        if name in out:
            continue
        out[name] = code[:m.start()].count("\n") + 1
    return out


def main():
    if not os.path.isdir(TESTS):
        sys.stderr.write("run me from the source root\n")
        return 2

    cache = {}
    bad = []
    suites = 0
    pairs = 0

    for fn in sorted(os.listdir(TESTS)):
        if not fn.endswith(".c"):
            continue
        suite = os.path.join(TESTS, fn)
        raw = open(suite, encoding="utf-8", errors="replace").read()

        mods = []
        for inc in INC.findall(blank(raw)):
            rel = os.path.normpath(os.path.join(TESTS, inc))
            if os.path.exists(rel) and rel not in mods:
                mods.append(rel)
        if len(mods) < 2:
            continue
        suites += 1

        for m in mods:
            if m not in cache:
                cache[m] = statics_of(m)

        for i in range(len(mods)):
            for j in range(i + 1, len(mods)):
                a, b = mods[i], mods[j]
                pairs += 1
                shared = set(cache[a]) & set(cache[b])
                for name in sorted(shared):
                    bad.append((fn, a, cache[a][name], b, cache[b][name],
                                name))

    if bad:
        print("check_statics: two modules one suite includes share a "
              "static object\n")
        for suite, a, la, b, lb, name in bad:
            print("  %s includes both:" % suite)
            print("    %s:%d  static ... %s" % (a, la, name))
            print("    %s:%d  static ... %s" % (b, lb, name))
            print()
        print("  An uninitialised `static <type> %s;` in each is a "
              "TENTATIVE" % bad[0][5])
        print("  definition, and C merges tentative definitions of the "
              "same object")
        print("  in one translation unit. The two become ONE VARIABLE, "
              "with no")
        print("  error and no warning at any -W level -- so one module "
              "writes")
        print("  the other's private state and the symptom is a wrong "
              "answer")
        print("  somewhere else entirely. v0.8.7 lost an afternoon to "
              "exactly")
        print("  this: usb/core.c's device count and net/netdev.c's NIC "
              "count")
        print("  were the same `int`.")
        print("\n  Rename one of them for what it holds. Do not rely on "
              "the two")
        print("  files never being compiled together: a suite that does "
              "it is")
        print("  what this check found.")
        return 1

    print("check_statics: ok, %d suite(s) include 2+ kernel modules; "
          "%d module pair(s) share no static object" % (suites, pairs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
