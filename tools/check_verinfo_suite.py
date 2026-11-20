#!/usr/bin/env python3
"""check_verinfo_suite.py -- prove tests/test_verinfo.c can fail.

kernel/verinfo.c (v0.9.7) decides what HavenDOS 1 calls itself and which
updates a system takes. A defect here is an update from a channel nobody
chose, or a release that names itself wrongly on every screen.

Run:  python3 tools/check_verinfo_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "kernel/verinfo.c"
TEST = "tests/test_verinfo.c"
CC = ["gcc", "-std=gnu99", "-Wall"]

BUGS = [
    ("the subscription takes a LESS stable channel",
     "a Stable system takes a Beta: the one thing a channel exists to stop",
     "    return upd >= sub;",
     "    return upd <= sub;"),
    ("a manifest that names no channel is refused",
     "every 0.9.6.x update file, and every one a person writes by hand, "
     "is refused once 0.9.7 is installed",
     "    if(upd < 0) return 1;           /* a manifest from before 0.9.7 names none */",
     "    if(upd < 0) return 0;"),
    ("Stable shows its channel too",
     "\"HavenDOS 1, version 27H1 (OS Build 27100.1000, Stable)\" -- not the "
     "string THS-RM-1004 promised",
     "    if(channel != CH_STABLE){",
     "    if(1){"),
    ("the revision is not padded to four digits",
     "a local build 27100.0007 shows as 27100.7, which reads as newer "
     "than 27100.1000 to a person",
     "    at = putn(out, at, max, revision, 4);    /* 27100.0999 is a local build */",
     "    at = putn(out, at, max, revision, 1);"),
    ("a number that is no channel is accepted",
     "channel=CH25 in a manifest becomes a channel of 25, between Beta and "
     "Public Beta",
     "            for(int i = 0; i < NCH; i++) if(CH[i].ch == v) return v;\n            return -1;",
     "            return v;"),
    ("names are compared with case",
     "`update channel Stable` and channel=Beta in a manifest are refused",
     "        if(lc((unsigned char)*a) != lc((unsigned char)*b)) return 0;",
     "        if(*a != *b) return 0;"),
    ("a prefix of a name is taken for it",
     "'betaa' and 'stab' become channels -- a typo picks one",
     "        if(!*a || !*b) return !*a && !*b;",
     "        if(!*a || !*b) return 1;"),
    ("product 0 gets HavenDOS 1's string",
     "every 0.9.x screen says \"HavenDOS 0, version ...\"",
     "    if(product < 1){                         /* the 0.9.x line, as ever */",
     "    if(product < 0){"),
    # Recorded, not kept: "the buffer bound is off by one" (at < max
    # instead of max - 1 in put()) was MISSED on the first run, rightly --
    # the terminator is then written at max - 1 over the last character,
    # so the output is the same string and nothing is written past the
    # end. Not a defect; the short-buffer check covers the real one.
]

HARMLESS = ("a comment is reworded (must not be caught)", "nothing changed",
            "static int lc(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }",
            "/* lower case */\nstatic int lc(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }")

if __name__ == "__main__":
    sys.exit(inject.run("verinfo", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
                        preamble="Proving tests/test_verinfo.c can fail."))
