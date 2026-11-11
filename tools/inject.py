#!/usr/bin/env python3
"""inject.py -- the injection loop, once.

── WHY THIS IS SHARED AND THE HARNESSES ARE NOT ───────────────────────

An injection harness damages the code on purpose and checks that the
suite notices. The harness is therefore the thing you trust instead of
thinking, and this project has already had one lie to it:

  tools/check_xhci_suite.py's first version restored only the file it was
  about to damage. Once a BAR bug was injected, that file stayed broken
  for every injection after it -- so eleven injections "caught" the
  leftover, and ONE OF THEM WAS A REAL MISS hiding in the pile.

That bug is in the loop, not in the list of defects. Five harnesses with
five copies of the loop is five chances to write it again, and v0.8.6 is
the release whose whole subject is that a thing copied three times had
drifted in all three. So the loop lives here and each harness is a list
of defects, a source file and a command line.

── THE SELF-CHECK IS PART OF THE LOOP ─────────────────────────────────

tools/check_machine_suite.py reported all 20 caught on its first run,
which by this tree's rule is the least trustworthy result available -- so
it was checked by hand: a harmless injection was appended LAST and
correctly reported MISSED. That proved two things at once, that MISSED is
reachable at all, and that the damaging injections before it had been
fully restored.

Done by hand it is a thing somebody remembered to do once. Here it is
required: every harness declares a HARMLESS change, it is run last, and
the harness FAILS if it is reported as caught. A harness whose harmless
injection is "caught" is either restoring badly or has a suite asserting
something that is not behaviour, and both of those make every other
result in it meaningless.

── WHAT COUNTS AS CAUGHT ──────────────────────────────────────────────

  the suite exits non-zero   caught
  the suite does not compile caught -- a type the compiler can see is
                             still a defect the tree refused
  the suite hangs            caught, and it has to be: the unbounded-wait
                             class of bug HANGS rather than failing, and
                             kernel/machine.c exists because of one
  a `checks` command fails   caught, and this one needs saying
  the suite still passes      MISSED

── WHY A BUILD CHECK CAN BE THE THING THAT CATCHES ────────────────────

Some defects are not reachable from a suite, by design.

tests/test_diskname.c states each bus's word a second time, in its own
simulated ops tables, so its assertions are written against the
SPECIFICATION rather than against whatever fs/blkdev.c says today. That
is deliberate -- a suite that reads the kernel's table and compares it
with itself is the copy problem in a new shape -- but it means the suite
cannot possibly notice a typo in the kernel's table.

tools/check_diskname_sync.py is what notices that, by comparing both
files against the table of words IT holds. So the pair is: the suite
covers the logic, the build check covers the text, and an injection into
the text has to be caught by the check.

A harness therefore declares the build checks that are part of its
coverage, and a defect either of them refuses is caught. What is NOT
allowed is a harness with no suite coverage at all hiding behind a grep:
the baseline run still has to pass the suite, and `checks` run only
against injected code.
"""
import os
import subprocess
import sys
import tempfile


def _build_and_run(cc, test, exe, timeout):
    b = subprocess.run(cc + ["-o", exe, test], capture_output=True,
                       text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def _checks_pass(checks):
    """True if every declared build check still accepts the tree."""
    for cmd in checks:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return False, cmd
    return True, None


def run(name, src, test, cc, bugs, harmless, timeout=60, files=None,
        preamble="", checks=()):
    """Damage `src` once per entry in `bugs` and require `test` to notice.

    src       the file defects are planted in
    test      the suite that has to catch them
    cc        the compiler command, as a list, without -o
    bugs      [(label, why, old, new), ...]; old must occur exactly once
    harmless  (label, why, old, new) that must NOT be caught
    files     every file restored before every injection; defaults to
              (src, test). Pass more when an injection touches two.
    checks    build-check commands that are part of this harness's
              coverage; a defect one of them refuses counts as caught.
              See the header for when that is legitimate.
    """
    if not os.path.exists(src):
        sys.stderr.write("run me from the source root\n")
        return 2
    files = tuple(files) if files else (src, test)
    # v0.8.9.8: an injection may name its own file as a fifth element,
    # for a suite that covers a chain of files -- test_dhcp.c compiles
    # ipv4.c, udp.c and dhcp.c, and DHCP's three defects were one in
    # each. Every such file is restored before every injection too.
    extra = tuple(b[4] for b in list(bugs) + [harmless] if len(b) > 4)
    files = tuple(dict.fromkeys(files + extra))
    pristine = {f: open(f, encoding="utf-8").read() for f in files}
    exe = os.path.join(tempfile.gettempdir(), "hd_inj_" + name)

    if preamble:
        print(preamble.rstrip() + "\n")
    print("Injecting %d defects into %s, plus one that must NOT be caught.\n"
          % (len(bugs), src))

    st, out = _build_and_run(cc, test, exe, timeout)
    if st != "pass":
        print("  the UNMODIFIED suite does not pass (%s) -- fix that first"
              % st)
        print(out[-3000:])
        return 2
    ok, which = _checks_pass(checks)
    if not ok:
        print("  a declared build check already refuses the UNMODIFIED "
              "tree:\n    %s" % " ".join(which))
        return 2
    print("  baseline: the unmodified suite passes%s\n"
          % (" and %d build check(s) accept the tree" % len(checks)
             if checks else ""))

    missed, stale = [], []
    harmless_verdict = None
    try:
        for i, bug in enumerate(list(bugs) + [harmless], 1):
            label, why, old, new = bug[:4]
            target = bug[4] if len(bug) > 4 else src
            is_harmless = (i == len(bugs) + 1)

            # Every file back to pristine before every injection, not just
            # the one about to be damaged. See the header.
            for f, text in pristine.items():
                open(f, "w", encoding="utf-8").write(text)

            n = pristine[target].count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-56s INJECTION STALE (%d matches)"
                      % (i, label, n))
                continue
            open(target, "w", encoding="utf-8").write(
                pristine[target].replace(old, new, 1))

            st, _ = _build_and_run(cc, test, exe, timeout)
            if st == "pass":
                ok, which = _checks_pass(checks)
                if not ok:
                    st = "check"
            if st == "pass":
                verdict = "MISSED"
            elif st == "check":
                verdict = "caught (%s)" % os.path.basename(which[-1])
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"

            if is_harmless:
                harmless_verdict = st
                print("  %2d. %-56s %s  <- must be MISSED"
                      % (i, label, verdict))
            else:
                if st == "pass":
                    missed.append((label, why))
                print("  %2d. %-56s %s" % (i, label, verdict))
    finally:
        for f, text in pristine.items():
            open(f, "w", encoding="utf-8").write(text)

    print()
    bad = 0
    if stale:
        print("  %d injection(s) no longer match the source:" % len(stale))
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
        bad = 1
    if missed:
        print("  %d defect(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s\n        %s" % (label, why))
        bad = 1
    if harmless_verdict != "pass" and not stale:
        print("  THE HARMLESS INJECTION WAS CAUGHT (%s)." % harmless_verdict)
        print("    %s" % harmless[1])
        print("  That means one of two things, and both make every result")
        print("  above meaningless: either a damaging injection was not")
        print("  fully restored before this one ran -- the exact bug")
        print("  check_xhci_suite.py's first loop had, which faked eleven")
        print("  catches and hid a real miss among them -- or the suite is")
        print("  asserting something that is not behaviour.")
        bad = 1

    if bad:
        return 1
    print("  all %d caught, and the harmless one correctly missed"
          % len(bugs))
    return 0
