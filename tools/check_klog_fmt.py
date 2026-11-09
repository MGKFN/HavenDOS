#!/usr/bin/env python3
"""check_klog_fmt.py -- klog() is not printf, and the difference is silent.

── WHY THIS EXISTS ────────────────────────────────────────────────────

kernel/klog.c implements its own formatter. It understands

    %d  %u  %x  %s  %c  %%

and ONE optional pad digit, 1 to 8, so `%4x` prints 00c1 and the device
columns in the PCI lines line up.

It understands nothing else. An unrecognised specifier falls to the
default case, which prints the specifier LITERALLY -- and, crucially,
DOES NOT CONSUME ITS ARGUMENT. Every later specifier on that line then
takes the wrong one.

So `%02X`, the obvious way to write a MAC address byte, does not merely
look wrong. It silently shifts the whole line.

v0.8.7's first `usb eth` on the HP printed:

    eth0  %04X:%04X  if 1452 alt 4776  ep %02X/%02X
          mac %02X:%02X:%02X:%02X:%02X:%02X

1452 is 0x05AC and 4776 is 0x12A8 -- the vendor and product ids, having
slid into the interface and alternate-setting columns because the two
`%04X` ahead of them consumed nothing. The MAC line was entirely
literal.

That line was the ONE diagnostic the whole release depended on: it is
what `usb eth` exists to print, and what the hardware test plan tells
the tester to read. A formatting bug took out the instrument.

── WHAT IT REFUSES ───────────────────────────────────────────────────

Any klog() format string using a specifier klog does not implement.

It does not check argument COUNTS -- that needs a parser this does not
have, and the compiler cannot help because klog is variadic. The
specifier check is the one that matters, because an unsupported
specifier is the thing that shifts the arguments.

Run:  python3 tools/check_klog_fmt.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csource import blank                                     # noqa: E402

# Exactly what kernel/klog.c's switch handles, with its one pad digit.
OK = re.compile(r'%(?:[1-8])?[duxsc%]')

CALL = re.compile(r'\bklog\s*\(\s*KLOG_\w+\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
LIT  = re.compile(r'"((?:[^"\\]|\\.)*)"')

SKIP_DIRS = {".git", "__pycache__", "tests", "tools", "harness",
             "isodir", "dist", "manual", "docs", "rescue"}


def main():
    if not os.path.isdir("kernel"):
        sys.stderr.write("run me from the source root\n")
        return 2

    bad = []
    checked = calls = 0

    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if not fn.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(root, fn), ".").replace("\\", "/")
            checked += 1
            raw = open(os.path.join(root, fn), encoding="utf-8",
                       errors="replace").read()
            code = blank(raw)          # comments out: see tools/csource.py
            for m in CALL.finditer(code):
                calls += 1
                s = "".join(LIT.findall(m.group(1)))
                line = code[:m.start()].count("\n") + 1
                i = 0
                while i < len(s):
                    if s[i] != "%":
                        i += 1
                        continue
                    ok = OK.match(s, i)
                    if ok:
                        i = ok.end()
                        continue
                    bad.append((rel, line, s[i:i + 4], s[:56]))
                    i += 1

    if bad:
        print("check_klog_fmt: klog() cannot format these\n")
        for rel, line, spec, ctx in bad:
            print("  %s:%d  %s" % (rel, line, spec))
            print("      in \"%s...\"" % ctx)
        print("\n  klog understands %d %u %x %s %c %% and one pad digit")
        print("  1-8, so %4x prints 00c1 and %2x prints 0a. Anything else")
        print("  is printed LITERALLY and its argument is NOT consumed, so")
        print("  every later specifier on the line takes the wrong one.")
        print("\n  v0.8.7 shipped `%04X:%04X ... if %u alt %u` and the HP")
        print("  printed the vendor and product ids in the interface and")
        print("  alternate-setting columns. Use %4x / %2x, or build the")
        print("  string by hand and print it with %s.")
        return 1

    print("check_klog_fmt: ok, %d klog call(s) across %d files use only "
          "specifiers klog implements" % (calls, checked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
