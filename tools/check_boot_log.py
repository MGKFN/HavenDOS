#!/usr/bin/env python3
"""check_boot_log.py — fail the build if invented boot output comes back.

HavenDOS shipped three boot screens for a long time and two of them were
fiction.  The "verbose boot log" was a table of 24 string literals of which
four were real; it told every machine that ATA was absent, that FAT16 was on
a VirtIO partition, that the heap was 4 MB at 0x00200000 (a 256 KB static
array) and that the GDT had 256 descriptors (it has 3).  `dmesg` printed its
own copy with timestamps typed one millisecond apart that nothing measured.

That is all gone: the boot log is written by kernel/klog.c from values the
drivers have just read.  This script exists so it cannot come back quietly,
the same way make_version.py --check stops version literals drifting.

Two things are refused, both precise enough to have no false positives:

  1. A string literal shaped like a log timestamp -- "[   0.008] ..." -- in
     any kernel source.  Nothing should ever TYPE a timestamp; klog renders
     them from boot_ms().

  2. A table of log lines keyed by a status tag -- {"OK", ... or {"WARN", ...
     -- which is the exact shape of the array that was removed.

  3. A BOOT LOG LINE THAT CLAIMS AN IRQ THE KERNEL HAS NOT WIRED UP,
     added in v0.8.8.  The line that prompted it was

         ok  PS/2: keyboard on IRQ1, mouse on IRQ12

     and it was wrong -- but the note that found it was wrong too, which
     is why this is mechanical now rather than a correction.  The note
     said the slave PIC stays masked so IRQ12 cannot fire.  It does not:
     irq_enable_mouse() unmasks IRQ12 and the cascade, and kernel/main.c
     calls it on the line immediately before printing that entry.

     THREE layers have to agree before an IRQ reaches a handler:

       1. the device must generate it.  drivers/mouse.c clears bit 1 of
          the 8042 config byte -- "IRQ12 stays off, we poll instead".
       2. the PIC must not mask it.  This one was fine.
       3. something must be REGISTERED for it.  drivers/keyboard.c is
          the only caller of irq_register() in the tree, and it
          registers IRQ1.

     A checker cannot read layer 1 -- it is a bit in a config byte
     written by a driver.  It can read layers 2 and 3, and layer 3 alone
     refuses the line, because a boot log entry saying work happens on
     an IRQ with no handler is false whatever the PIC is doing.  So the
     rule is: a klog() line naming IRQ<n> must have an irq_register(<n>,
     ...) somewhere in the tree.

     The claim this replaces is still a legitimate thing to SAY -- "mouse
     polled (8042 IRQ12 off)" names the same interrupt and claims
     nothing about it -- so the pattern requires the "on IRQ<n>" form,
     which is the one that asserts a wiring.

A line that genuinely needs any of these spells  boot-log-ok  in a
comment on it.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

SCAN_DIRS = ("kernel", "drivers", "shell", "fs", "net")
OPT_OUT = "boot-log-ok"

# "[   0.008]" or "[0.008]" inside a string literal.
FAKE_STAMP = re.compile(r'"[^"\n]*\[\s*\d+\.\d+\s*\][^"\n]*"')
# The removed table's shape: a brace followed by a status tag string.
LOG_TABLE = re.compile(r'\{\s*"(OK|WARN|ERR|FAIL)"\s*,')

# "... on IRQ12 ..." inside a string literal: the form that asserts a
# handler. "IRQ12 off" and "mouse polled" claim nothing and are allowed.
CLAIMS_IRQ = re.compile(r'"[^"\n]*\bon\s+IRQ\s*(\d{1,2})\b[^"\n]*"')
IRQ_REGISTERED = re.compile(r'\birq_register\s*\(\s*(\d{1,2})\s*,')


def registered_irqs(root="."):
    """Every IRQ number something in the tree registers a handler for."""
    found = set()
    for d in SCAN_DIRS:
        path = os.path.join(root, d)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not name.endswith((".c", ".h")):
                continue
            text = open(os.path.join(path, name), encoding="utf-8",
                        errors="replace").read()
            for m in IRQ_REGISTERED.finditer(text):
                found.add(int(m.group(1)))
    return found


def scan(root="."):
    bad = []
    wired = registered_irqs(root)
    for d in SCAN_DIRS:
        path = os.path.join(root, d)
        if not os.path.isdir(path):
            continue
        for name in sorted(os.listdir(path)):
            if not name.endswith((".c", ".h")):
                continue
            f = os.path.join(path, name)
            text = open(f, encoding="utf-8", errors="replace").read()
            # ── v0.8.8: COMMENTS BLANKED, through tools/csource.py.
            #
            # This check read raw lines for three releases, and the IRQ
            # rule added here found out why that was wrong on its first
            # run: the comment explaining the corrected boot log line
            # QUOTES the old one, so the check refused the fix for the
            # bug it was written to catch.
            #
            # csource.py's own header says a false positive is not a
            # harmless checker being careful -- it teaches whoever hits
            # it to add an exemption, and an exemption added to silence
            # a checker bug is one that will hide a real finding later.
            # The other two rules here had the same exposure: a
            # paragraph describing the removed log table, or quoting a
            # timestamp, would have been refused too.
            raw = text.splitlines()
            for n, line in enumerate(csource.blank(text).splitlines(), 1):
                if OPT_OUT in (raw[n - 1] if n - 1 < len(raw) else ""):
                    continue
                if FAKE_STAMP.search(line):
                    bad.append((f, n, "a typed log timestamp", raw[n - 1].strip()))
                if LOG_TABLE.search(line):
                    bad.append((f, n, "a table of log lines", raw[n - 1].strip()))
                m = CLAIMS_IRQ.search(line)
                if m and int(m.group(1)) not in wired:
                    bad.append((f, n, "claims work on IRQ%s, and nothing "
                                "in the tree registers a handler for it"
                                % m.group(1), raw[n - 1].strip()))
    return bad


def main():
    bad = scan()
    if not bad:
        print("check_boot_log: clean, no invented boot output")
        return 0
    print("check_boot_log: FAILED\n")
    for f, n, why, text in bad:
        print("  %s:%d  %s" % (f, n, why))
        print("      %s" % text[:100])
    print("""
The boot log is written by kernel/klog.c from values the drivers have just
read.  Call klog() with the real number instead of describing it, or mark the
line  boot-log-ok  if it genuinely is not boot output.

For an IRQ claim: either register a handler for it -- irq_register(n, fn) in
kernel/idt.c's table -- or say what actually happens.  "mouse polled (8042
IRQ12 off)" names the interrupt and asserts nothing about it, which is the
form that is true.""")
    return 1


if __name__ == "__main__":
    sys.exit(main())
