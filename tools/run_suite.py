#!/usr/bin/env python3
"""run_suite.py -- run one native suite and actually report the result.

This exists because `make test` could not fail.

Every one of the 31 suite invocations in the Makefile ended in

    /tmp/hd_tdn | tail -1

and a shell pipeline exits with the status of its LAST command, which is
`tail`, which always succeeds. So a suite could fail every check it has
and `make test` still exited 0. Planted proof: one character changed in
tests/test_diskname.c printed "19/20 passed" and `make test` exited 0.

A compile failure did fail the build -- the recipes are
`$(TESTCC) ... && /tmp/x | tail -1`, so a non-zero compiler status
short-circuits the `&&`. That is why a missing stub in
tests/test_accounts.c was caught immediately while a wrong answer never
would be. Compile errors: fatal. Wrong answers: invisible.

Which means every "the suites were green" claim in this tree rested on a
person reading the scrollback. The 20 October handoff says "the suites
were green and the harness said 23 of 23 at the start of the day" --
that was a human eyeballing output, not a check.

Two jobs, in one place rather than 31:

  1. Exit with the SUITE's status, while still printing one tidy line.
  2. Refuse a suite that did not run enough checks.

Job 2 is the other half of the same disease. `tests/*_fixture.inc` ships
in the source archive but `tests/*.img` does not, and the generator
rules list both as targets -- so make saw the .inc, skipped the rule,
and five suites reported `0/0 passed` or `0/1 passed` with no fixture to
run against. 621 of 1995 checks, 31% of the total, silently absent, and
`0/0 passed` is not a failure to anything that only reads the text.

So each suite declares the number of checks it had when it was last
known good, and a run that comes in under it fails. A suite that grows
is fine (the floor is a floor); a suite that shrinks is a suite whose
fixture went missing, whose body got #if'd out, or whose loop stopped
early.

    python3 tools/run_suite.py --name test_xhci --min 150 -- /tmp/hd_txhci
    python3 tools/run_suite.py --name test_fat16path --allow-skip -- /tmp/hd_tfp

Run with --bless to print the floors a passing tree would declare, for
updating the Makefile after suites legitimately grow.
"""
import argparse
import re
import subprocess
import sys

SUMMARY = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s+passed\s*$")
SKIPPED = re.compile(r"\bSKIP\b")

# A suite that hangs is a suite that found something (the unbounded-wait
# class of bug), so it must not hang the build instead.
TIMEOUT = 600


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--min", type=int, default=1,
                    help="checks this suite must run; a run under it fails")
    ap.add_argument("--allow-skip", action="store_true",
                    help="this suite may legitimately skip (it says why)")
    ap.add_argument("--bless", action="store_true",
                    help="print the floor this run would declare")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        sys.stderr.write("run_suite: no command given\n")
        return 2

    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=TIMEOUT)
    except FileNotFoundError:
        print("  %s: FAILED -- %s was never built" % (a.name, cmd[0]))
        return 1
    except subprocess.TimeoutExpired:
        print("  %s: FAILED -- hung, killed after %ds" % (a.name, TIMEOUT))
        print("    a suite that hangs has usually found an unbounded wait.")
        return 1

    out = (r.stdout or "") + (r.stderr or "")
    lines = out.splitlines()

    passed = total = None
    for line in reversed(lines):
        m = SUMMARY.match(line)
        if m:
            passed, total = int(m.group(1)), int(m.group(2))
            break

    def dump(why):
        print("  %s: FAILED -- %s" % (a.name, why))
        tail = [l for l in lines if l.strip()][-25:]
        for l in tail:
            print("    | %s" % l)

    # A declared skip, printed loudly rather than swallowed. The handoff
    # records test_fat16path sitting in `make test` output "with no
    # result for three releases" because a `tail -1` ate the notice.
    if passed is None:
        if a.allow_skip and SKIPPED.search(out):
            note = next((l.strip() for l in lines if SKIPPED.search(l)), "SKIP")
            print("  %s: SKIPPED -- %s" % (a.name, note))
            return 0
        dump("no 'N/M passed' line -- the suite printed no result")
        return 1

    if a.bless:
        print("%s %d" % (a.name, total))
        return 0

    if r.returncode != 0 or passed != total:
        dump("%d of %d checks failed (exit %d)"
             % (total - passed, total, r.returncode))
        return 1

    if total < a.min:
        print("  %s: FAILED -- ran %d checks, expected at least %d"
              % (a.name, total, a.min))
        print("    A suite that shrinks has lost something rather than")
        print("    fixed something. The usual cause is a missing fixture:")
        print("    tests/*.img is a build product and is NOT in the source")
        print("    archive. Run the three generators:")
        print("      python3 tools/make_fat32_fixture.py tests")
        print("      python3 tools/make_esp_fixture.py tests")
        print("      python3 tools/make_update_fixture.py tests")
        print("    They need dosfstools and mtools.")
        return 1

    grew = "  (+%d since the floor)" % (total - a.min) if total > a.min else ""
    print("  %s: %d/%d passed%s" % (a.name, passed, total, grew))
    return 0


if __name__ == "__main__":
    sys.exit(main())
