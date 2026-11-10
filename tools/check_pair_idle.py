#!/usr/bin/env python3
"""check_pair_idle.py -- no lockdownd connection may be held across a keygen.

── THE BUG THIS RELEASE SHIPPED ───────────────────────────────────────

`pair` opened a connection to lockdownd, asked for DevicePublicKey, and
then generated two 2048-bit RSA keys before sending anything else. On
Kyan's HP those keys took 54180 ms and 17090 ms. For seventy-one
seconds the connection was open and completely silent.

lockdownd closes an idle connection. It is entitled to, and it did. The
FIN was on the wire before the 3728-byte pair request went out, the
request went into a connection the device had already finished with,
and the first read afterwards picked the FIN up.

Nothing in the keygen needs the device. The only thing taken from it is
the public key, and that is parsed and in hand before the arithmetic
starts. The connection was held open for no reason other than the order
the function happened to be written in -- which is the kind of defect
that survives every unit test, because each piece works.

── WHAT IS REQUIRED ───────────────────────────────────────────────────

In any function that calls keygen():

  1. lockdown_close() appears before the first keygen() call.
     Hang up. A connection nobody is going to use for a minute is a
     connection the other end is right to reclaim.

  2. lockdown_connect() appears after the last keygen() call and
     before any lockdown_request() / lockdown_get_value() /
     lockdown_start_session(). Call back. lockdown_connect() is
     idempotent, so this costs nothing when the connection is still
     up and is the whole fix when it is not.

Comments are stripped first (tools/csource.py), so a rule described in
prose does not satisfy itself.

Run:  python3 tools/check_pair_idle.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csource

FILES = ["shell/paircmd.c"]

# A lockdown call that needs a live connection underneath it.
TALKS = re.compile(r"\blockdown_(request|get_value|query_type|"
                   r"start_session|enable_ssl|stop_session)\s*\(")


def functions(text):
    """(name, body, offset) for every brace-balanced top-level function.

    Deliberately crude -- it only has to find the function a keygen()
    call sits in, and this tree's style puts the opening brace of a
    definition in column one."""
    out = []
    for m in re.finditer(r"^([A-Za-z_][\w \t\*]*?)\b(\w+)\s*\([^;{]*\)\s*\{",
                         text, re.M):
        start = text.index("{", m.start())
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append((m.group(2), text[start:i + 1], start))
    return out


def main():
    bad = []
    seen_keygen = 0

    for path in FILES:
        if not os.path.exists(path):
            bad.append("%s: missing" % path)
            continue
        text = csource.blank(open(path, encoding="utf-8").read())

        for name, body, _ in functions(text):
            calls = [m.start() for m in re.finditer(r"\bkeygen\s*\(", body)]
            # The definition of keygen() itself is not a caller of it.
            if not calls or name == "keygen":
                continue
            seen_keygen += 1

            first, last = calls[0], calls[-1]

            closes = [m.start() for m in
                      re.finditer(r"\blockdown_close\s*\(", body)]
            if not any(c < first for c in closes):
                bad.append(
                    "%s: %s() generates a key with a lockdownd "
                    "connection still open. lockdown_close() must come "
                    "first -- lockdownd closes an idle connection and a "
                    "2048-bit keygen is over a minute on real hardware."
                    % (path, name))

            conns = [m.start() for m in
                     re.finditer(r"\blockdown_connect\s*\(", body)]
            if not any(c > last for c in conns):
                bad.append(
                    "%s: %s() never calls lockdown_connect() after the "
                    "keygen. It is idempotent; without it everything "
                    "below talks into a closed stream."
                    % (path, name))
            else:
                after = min(c for c in conns if c > last)
                talks = [m.start() for m in TALKS.finditer(body)
                         if m.start() > last]
                early = [t for t in talks if t < after]
                if early:
                    bad.append(
                        "%s: %s() talks to lockdownd at offset %d, after "
                        "the keygen and before the reconnect."
                        % (path, name, early[0]))

    if seen_keygen == 0:
        bad.append("no caller of keygen() found at all -- this check has "
                   "stopped checking anything. If pairing moved, move "
                   "FILES with it.")

    if bad:
        print("check_pair_idle: FAIL")
        for b in bad:
            print("  " + b)
        return 1

    print("check_pair_idle: ok -- %d keygen caller(s), each hangs up "
          "first and calls back after" % seen_keygen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
