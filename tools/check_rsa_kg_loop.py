#!/usr/bin/env python3
"""check_rsa_kg_loop.py -- rsa_keygen_step() returns POSITIVE codes.

── THE BUG THIS RELEASE SHIPPED ───────────────────────────────────────

include/rsa.h:

    #define RSA_KG_WORKING   1
    #define RSA_KG_DONE      2
    #define RSA_KG_FAILED    3

All positive, and all meaningful. Nearly everything else in this tree
uses the other convention -- zero or positive for success, negative for
an error -- so a loop written from habit reads:

    r = rsa_keygen_step(&g_kg);
    if (r < 0)  { ...failed... }
    if (r == 1) break;              /* "done" */

Both lines are wrong and neither fails to compile. `r == 1` is
RSA_KG_WORKING, so the loop stops after the FIRST bounded piece of
work and the caller copies a key that does not exist yet. `r < 0`
never fires, so a generator that genuinely gave up would be stepped
for ever.

shell/paircmd.c had exactly that. On Kyan's HP it printed

    root key: done in 70 ms

for a 2048-bit key -- which is 577 steps and about half a second of
this arithmetic -- and `pair` then refused its own key three functions
later with "a key too small to pair with". The certificate builder
catching at the end what the generator was never asked for.

shell/rsacmd.c, which does the same job, had it right from the start:
`if (r != RSA_KG_WORKING) break;`. One file right and one file wrong,
with no compiler able to tell them apart, is what this check is for.

── WHAT IS REFUSED ────────────────────────────────────────────────────

In code, never in comments (tools/csource.py does the telling apart):

  1. The result of rsa_keygen_step() compared against a bare integer
     literal. Name the constant: RSA_KG_WORKING, RSA_KG_DONE or
     RSA_KG_FAILED.
  2. The result tested for being negative. None of the three codes
     is, so such a test is dead and the failure it was meant to catch
     is not caught.

Tests are checked too. tests/test_rsa.c drove the generator at 512
bits through loops of its own for releases while the only caller that
mattered was broken -- a suite that uses the API correctly proves the
API works and says nothing about whether anybody else used it that
way.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# `r = rsa_keygen_step(...)` then, within a few lines, a comparison of
# that variable. Rather than track the variable, the check looks for
# the two shapes that are wrong wherever they appear in a function that
# calls the generator -- which is narrow enough to have no false
# positives in this tree and broad enough to catch the defect.
CALL_RE = re.compile(r"(\w+)\s*=\s*rsa_keygen_step\s*\(")
BAD_LIT = re.compile(r"\b%s\s*(?:==|!=|<|>|<=|>=)\s*(\d+)")
BAD_NEG = re.compile(r"\b%s\s*<\s*0\b")

problems = []
scanned = 0


def sources():
    for sub in ("drivers", "fs", "kernel", "net", "shell", "usb", "tests"):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".c"):
                yield os.path.join(sub, name)


for rel in sources():
    path = os.path.join(ROOT, rel)
    try:
        with open(path, "r", errors="replace") as fh:
            raw = fh.read()
    except OSError:
        continue
    code = csource.blank(raw)
    if "rsa_keygen_step" not in code:
        continue
    scanned += 1

    for m in CALL_RE.finditer(code):
        var = re.escape(m.group(1))
        # TO THE END OF THE ENCLOSING FUNCTION, not a fixed number of
        # characters. The first version of this looked at the next
        # 1200 characters and missed the very defect it was written
        # for: the comment explaining the convention sits between the
        # call and the comparison, and csource.blank() preserves
        # length, so the line fell outside the window. A check that
        # cannot see past a comment is a check that stops working the
        # moment somebody documents the thing being checked.
        end = code.find("\n}", m.end())
        window = code[m.end():end if end > 0 else len(code)]
        for mm in re.finditer(BAD_LIT.pattern % var, window):
            line = code[:m.end() + mm.start()].count("\n") + 1
            problems.append(
                "%s:%d  the result of rsa_keygen_step() compared against "
                "the literal %s -- name the constant (RSA_KG_WORKING is 1, "
                "RSA_KG_DONE is 2, RSA_KG_FAILED is 3)"
                % (rel, line, mm.group(1)))
        for mm in re.finditer(BAD_NEG.pattern % var, window):
            line = code[:m.end() + mm.start()].count("\n") + 1
            problems.append(
                "%s:%d  the result of rsa_keygen_step() tested for being "
                "negative -- none of the three codes is, so this never "
                "fires and the failure it guards is not caught"
                % (rel, line))

if problems:
    seen = []
    for p in problems:
        if p not in seen:
            seen.append(p)
    sys.stderr.write("check_rsa_kg_loop: %d problem(s):\n\n" % len(seen))
    for p in seen:
        sys.stderr.write("  " + p + "\n")
    sys.stderr.write("""
  rsa_keygen_step() returns RSA_KG_WORKING (1), RSA_KG_DONE (2) or
  RSA_KG_FAILED (3). All positive. A loop that breaks on `== 1` stops
  after one step and copies a key that does not exist; one that tests
  `< 0` for failure never notices a generator that gave up. Both
  compile. See this file's header for what that cost.
""")
    sys.exit(1)

print("check_rsa_kg_loop: ok, %d file(s) drive the generator and all of "
      "them name the codes" % scanned)
