#!/usr/bin/env python3
"""check_b64_suite.py -- prove tests/test_b64.c can fail.

── WHY THIS FILE DID NOT EXIST UNTIL v0.8.9, AND SHOULD HAVE ──────────

net/b64.c's own header records nineteen injected defects, all nineteen
caught, and names the two that survived until the suite learned to feed
it a byte with the top bit set. That work was real and it was done in
v0.8.8.11 -- and it was never written down as a harness, so none of
those nineteen could be run again.

tools/check_harnesses.py's message says the rest of it: "a harness
nobody runs cannot tell you it is broken." A harness that was run once
by hand and left in a comment is the same thing one step earlier.

── AND SIX INJECTIONS HAD BEEN ORPHANED ───────────────────────────────

v0.8.8.11 replaced four base64 implementations with this one. Six of
tools/check_lockdown_suite.py's injections were aimed at the decoder
that used to live in usb/lockdown.c, and when that decoder was deleted
those six stopped matching anything. inject.py reports a stale
injection rather than counting it as caught -- which is the only
reason this was noticed, one release later. They are re-homed here,
against the code that now does the work.

The judge for the suite itself is Python's base64 module: 2010 generated
cases plus the edges. This harness decides whether that suite would
notice if the decoder were wrong.

Run:  python3 tools/check_b64_suite.py

NEVER in a background process. This patches the tree and restores it
in a `finally`.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/b64.c"
TEST = "tests/test_b64.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ── THE SIX RE-HOMED FROM check_lockdown_suite.py ─────────────────
    ("the decoder stops at whitespace instead of skipping it",
     "THE BUG shell/shell_v073.c's decoder would have had. A plist "
     "<data> element is wrapped across lines with a tab on each, so "
     "stopping at the first character outside the alphabet returns the "
     "first line of a key and nothing else -- about a sixth of it",
     """        if (is_ws(c)) continue;""",
     """        if (is_ws(c)) break;"""),

    ("a character outside the alphabet is skipped rather than refused",
     "a wrong guess about where a value is becomes a SHORTER KEY THAT "
     "STILL LOOKS LIKE A KEY, and pairing would sign something with it",
     """        v = DEC[c];
        if (v < 0) return -1;""",
     """        v = DEC[c];
        if (v < 0) continue;"""),

    ("the decoder writes past the end of its buffer",
     "the bound is the caller's capacity and the only thing between a "
     "long value and whatever is next in memory",
     """                if (n >= outcap) return -1;""",
     """                if (0) return -1;"""),

    ("a group cut short is accepted",
     "ONE character is six bits with nothing to put them in -- an "
     "encoding that stopped mid-character, which is a reply that was "
     "truncated rather than a value",
     """    if (quad == 1) return -1;""",
     """    ;"""),

    ("base64 after the padding is accepted",
     "the padding is the end of the value. Anything after it belongs "
     "to something else, and guessing which is how a decoder "
     "concatenates two keys into one",
     """        if (pad) return -1;""",
     """        ;"""),

    ("the decoder assembles the bits in the wrong order",
     "six bits at a time, most significant first. Shifting the "
     "accumulator the other way produces plausible-looking bytes",
     """            bits -= 8;
            if (!measure) {
                if (n >= outcap) return -1;
                out[n] = (uint8_t)((acc >> bits) & 0xFF);
            }""",
     """            bits -= 8;
            if (!measure) {
                if (n >= outcap) return -1;
                out[n] = (uint8_t)(acc & 0xFF);
            }"""),

    # ── THE TWO net/b64.c's HEADER NAMES AS NEARLY-MISSED ─────────────
    # Both survived their first run, and the header says why. They are
    # here so that the suite's answer to them is a fact rather than a
    # sentence in a comment.
    ("more than two padding characters are accepted",
     """"A===" is not base64. net/b64.c's header records this one as """
     "having SURVIVED its first run, because no test fed it three pads",
     """            if (pad > 2) return -1;""",
     """            ;"""),

    ("a byte with the top bit set indexes the table",
     "DEC has 128 entries. 0xFF reaches twelve past the end -- and the "
     "header records this as the other survivor, caught only once the "
     "suite learned to feed it a high byte",
     """        if (c >= 128) return -1;""",
     """        ;"""),

    # ── THE MEASURING MODE, which sizes every caller's buffer ─────────
    ("measuring mode writes to the buffer it was not given",
     "a NULL out with outcap 0 means MEASURE: validate and return the "
     "length. Writing in that mode is a null dereference in every "
     "caller that sizes a buffer before filling it",
     """    int measure = (out == 0 && outcap == 0);""",
     """    int measure = 0;"""),

    ("measuring and decoding disagree about the length",
     "a caller sizes its buffer from the first and fills it with the "
     "second",
     """            n++;
        }
    }""",
     """            n++;
        }
        if (measure) n++;
    }"""),

    ("a NULL output with a non-zero capacity is accepted",
     "neither a decode nor a measure. Taken as a decode it writes to "
     "null; taken as a measure it silently ignores a capacity the "
     "caller meant",
     """    if (!out && !measure)          return -1;""",
     """    ;"""),

    ("a negative length is taken as a huge one",
     "inlen comes from a caller's arithmetic",
     """    if (!in || inlen < 0)          return -1;""",
     """    if (!in)                       return -1;"""),

    # ── THE ENCODER ───────────────────────────────────────────────────
    ("the encoder's own length arithmetic is one out",
     "b64_encoded_len() is what every caller sizes its buffer with. "
     "One too few and the encode refuses; one too many and a caller "
     "believes it has room it does not",
     """    quads = (inlen + 2) / 3;""",
     """    quads = (inlen + 1) / 3;"""),

    ("the wrap column is not rounded down to a multiple of four",
     "a line break inside a quad is legal for a DECODER -- they skip "
     "whitespace wherever it falls -- but the column is what makes a "
     "file comparable against Apple's writer line for line",
     """    wrap -= wrap % 4;""",
     """    ;"""),

    ("the encoder writes past the capacity it was given",
     "the one bound the caller has",
     """    if (need < 0 || need > outcap) return -1;""",
     """    if (need < 0) return -1;"""),

    ("a NULL input with a non-zero length is accepted",
     "a caller that lost track of its own buffer",
     """    if (!in && inlen) return -1;""",
     """    ;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """static int is_ws(uint8_t c)""",
    """/* space, tab, CR or LF */
static int is_ws(uint8_t c)""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "b64", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_b64.c can fail.\n"
                 "net/b64.c's header records nineteen injections and "
                 "this file is where they\nbecome re-runnable. Six of "
                 "them were orphaned in check_lockdown_suite.py when\n"
                 "the decoder they aimed at was deleted."))
