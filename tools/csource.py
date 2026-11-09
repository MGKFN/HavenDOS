#!/usr/bin/env python3
"""csource.py -- reading C source the way a checker has to, once.

── WHY THIS IS SHARED ─────────────────────────────────────────────────

A build check that greps for a string in a .c file has to tell code from
comments, and the obvious way to do it is wrong:

    code = line.split("/*")[0]

That handles a comment opened and closed on one line, and nothing else.
A /* */ block spanning several lines leaves its middle looking exactly
like code, so a checker reports a sentence in a paragraph of prose.

Both of this tree's grep-shaped checks hit it:

  - tools/check_blk_dispatch.py, on its first run, reported a sentence
    in shell/shell.c that happened to mention nvme_write().
  - tools/check_bus_label.py, in v0.8.6.1, reported the word "USB" in
    shell/fat16cmd.c's header comment -- a file-opening paragraph
    explaining why the command exists.

A false positive is not a harmless checker being careful. It teaches
whoever hits it to add an exemption, and an exemption added to silence a
bug in the checker is an exemption that will one day hide a real one.

So the line-classifying lives here and both checks use it. v0.8.6 spent
its whole release deleting three copies of a thing that had drifted in
all three; two copies of this would be the same bet again.

── WHAT IT DOES ───────────────────────────────────────────────────────

blank(text) returns the same text with every comment -- block and
line -- replaced by spaces, and every newline kept. So line numbers
still point at the line, and a checker can walk the blanked copy while
printing from the original.

Optionally it blanks `extern ...;` declarations too: a declaration names
a function without calling it, and the old code wrote one inside the
very block that then called it, so skipping whole lines was not an
option either.

String literals are NOT blanked, because the checks that use this are
looking FOR string literals. A checker that needs them gone should say
so; nothing here does.
"""
import re

DECL = re.compile(r"\bextern\b[^;{}]*;")


def _blank_run(text):
    """Replace with spaces, keeping newlines so line numbers survive."""
    return "".join(c if c == "\n" else " " for c in text)


def blank(text, strip_externs=False):
    """Comments (and optionally extern declarations) replaced by spaces.

    Line count and every line's length are preserved, so
    blank(text).splitlines()[n] lines up with text.splitlines()[n].
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(_blank_run(text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    text = "".join(out)
    if strip_externs:
        text = DECL.sub(lambda m: _blank_run(m.group(0)), text)
    return text


def code_lines(text, strip_externs=False):
    """blank()'s result, split -- for walking beside the original."""
    return blank(text, strip_externs).splitlines()


# ── the opt-out marker, and what counts as a reason ──────────────────
#
# Three build checks let a single line opt out with a marker and a
# REASON -- `blk-dispatch-ok: <why>`, `bus-label-ok: <why>`,
# `nic-dispatch-ok: <why>` -- and all three documented the same rule in
# the same words:
#
#     A bare marker is how an exemption becomes a habit.
#
# None of them enforced it. All three tested the reason with
#
#     re.search(r"<marker>:\s*\S", window)
#
# and in C the thing immediately after a bare marker is the `*/` that
# closes the comment. `*` is a non-space character, so
#
#     /* blk-dispatch-ok: */
#
# satisfied `\S` and silenced the check with no reason at all. Found in
# v0.8.7 while proving the NEW checker could fail -- by the third of the
# three cases a checker has to pass, which is the one that is easy not
# to write: the marker must not work WITHOUT a reason. The first two
# cases (a real violation is caught, a violation inside a block comment
# is not) both passed, and would have shipped a fourth checker with the
# hole copied into it.
#
# So this lives here, once, beside the comment-stripping it belongs
# with, and all three checks call it. v0.8.6 spent a release deleting
# three copies of a dispatch that had drifted in all three; three copies
# of this regex had already drifted nowhere, because none of them ever
# worked.
#
# A reason must begin on the marker's own line. A marker whose reason is
# on the following line reads, at the point it is written, exactly like
# a bare one -- and the person adding it is the person this rule is for.

def has_reason(window, marker):
    """True if `marker` appears in `window` WITH a reason after it.

    `window` is the marker line and the few above it, as the checks
    already build it. `marker` is the full marker name, colon excluded:
    "blk-dispatch-ok", "bus-label-ok", "nic-dispatch-ok".

    Comment punctuation is not a reason. Neither is a single character:
    `/* bus-label-ok: x */` is a bare marker with a fig leaf, and the
    whole purpose of requiring a reason is that writing one is enough
    work to make somebody ask whether they should.
    """
    pat = re.compile(re.escape(marker) + r":(.*)$", re.MULTILINE)
    for m in pat.finditer(window):
        tail = m.group(1)
        # The reason ends where the comment does. Without this, the CODE
        # on a line like `/* blk-dispatch-ok: */ nvme_read(0,0,1,b);`
        # reads as the reason -- which is the same bug one layer along,
        # and is why this function has a test beside it.
        end = tail.find("*/")
        if end >= 0:
            tail = tail[:end]
        tail = tail.replace("*", " ").strip()
        if len(tail) >= 3 and re.search(r"[A-Za-z]{3}", tail):
            return True
    return False
