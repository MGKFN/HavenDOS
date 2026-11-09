#!/usr/bin/env python3
"""check_tls_stages.py -- one list of TLS handshake stages, in one place.

── THE BUG THIS RELEASE NEARLY SHIPPED ────────────────────────────────

net/tls.c keeps g_tls_last_stage so that a failed handshake on somebody
else's machine can say how far it got.  The numbers were #defined in
net/tls.c, and shell/shell_v076.c -- the `tlsinfo` screen, the only
place a human ever reads them -- carried a SECOND copy of the list:

    static const char *stage_text(int s){
        switch(s){
            case 4: return "ServerHelloDone received";
            case 5: return "ClientKeyExchange sent";
            ...
        }
    }

plus `g_tls_last_stage >= 9` for "established" and a literal "/9".

v0.8.9 inserts three stages for the client-certificate path, two of
them in the middle.  Nothing would have failed to compile.  `tlsinfo`
would simply have reported every stage from 4 onwards under the
PREVIOUS stage's name -- a handshake that died asking for a client
certificate would have read "ServerHelloDone received", which is the
exact sentence that sends somebody looking in the wrong place.

This is include/version.h's lesson in a smaller space: a list of names
copied twice drifts, and this copy drifts silently because a number is
still a valid number after its meaning changes.

── WHAT IS REFUSED ────────────────────────────────────────────────────

In code, never in comments (tools/csource.py does the telling apart):

  1. A TLS_STAGE_* #define anywhere but include/tls.h.
  2. Any of the stage NAME strings anywhere but net/tls.c, which holds
     tls_stage_name() -- the one function that turns a number into
     words.
  3. g_tls_last_stage compared against a bare integer literal.  The
     comparison must name a stage, so that inserting one moves it.

(3) is the check that matters: the other two describe how the list is
kept in one place, and (3) is what stops the next reader quietly
hardcoding a 9 again.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join("include", "tls.h")
OWNER = os.path.join("net", "tls.c")

# The words tls_stage_name() produces.  Kept here deliberately rather
# than parsed out of net/tls.c: this list is what the check is ABOUT,
# and reading it from the file it is checking would make the check
# agree with whatever that file happens to say.
STAGE_WORDS = [
    "nothing attempted",
    "ClientHello sent",
    "ServerHello received",
    "Certificate parsed",
    "CertificateRequest received",
    "ServerHelloDone received",
    "client Certificate sent",
    "ClientKeyExchange sent",
    "CertificateVerify sent",
    "ChangeCipherSpec sent",
    "Finished sent",
    "server ChangeCipherSpec received",
    "session established",
]

DEF_RE = re.compile(r"#\s*define\s+TLS_STAGE_[A-Z_]+")
CMP_RE = re.compile(r"g_tls_last_stage\s*(?:==|!=|>=|<=|>|<)\s*(\d+)")
CMP_RE_REV = re.compile(r"(\d+)\s*(?:==|!=|>=|<=|>|<)\s*g_tls_last_stage")

problems = []
scanned = 0


def sources():
    for sub in ("boot", "drivers", "fs", "include", "kernel", "net",
                "shell", "usb", "tools"):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".c") or name.endswith(".h"):
                yield os.path.join(sub, name)


for rel in sources():
    path = os.path.join(ROOT, rel)
    try:
        with open(path, "r", errors="replace") as fh:
            raw = fh.read()
    except OSError:
        continue
    scanned += 1
    code = csource.blank(raw)

    for m in DEF_RE.finditer(code):
        if rel != HEADER:
            line = code[:m.start()].count("\n") + 1
            problems.append(
                "%s:%d  a TLS_STAGE_* #define outside %s"
                % (rel, line, HEADER))

    if rel != OWNER:
        for word in STAGE_WORDS:
            idx = code.find('"' + word)
            if idx >= 0:
                line = code[:idx].count("\n") + 1
                problems.append(
                    '%s:%d  the stage name "%s" -- only %s may name a '
                    "stage, through tls_stage_name()" % (rel, line, word, OWNER))

    for rx in (CMP_RE, CMP_RE_REV):
        for m in rx.finditer(code):
            line = code[:m.start()].count("\n") + 1
            problems.append(
                "%s:%d  g_tls_last_stage compared against the literal %s "
                "-- name the stage instead, so inserting one moves it"
                % (rel, line, m.group(1)))

if problems:
    sys.stderr.write("check_tls_stages: %d problem(s):\n\n"
                     % len(problems))
    for p in problems:
        sys.stderr.write("  " + p + "\n")
    sys.stderr.write("""
  The stage numbers live in include/tls.h and the names live in
  net/tls.c's tls_stage_name(). A second copy of either drifts the
  moment a stage is inserted, and it drifts WITHOUT a compile error --
  which is how `tlsinfo` came to be one stage out of step in the first
  place. See this file's header.
""")
    sys.exit(1)

print("check_tls_stages: ok, %d stage(s) defined once in %s, named once "
      "in %s, %d file(s) scanned"
      % (len(STAGE_WORDS), HEADER, OWNER, scanned))
