#!/usr/bin/env python3
"""External judge for tests/test_b64.c.

Python's `base64` module is the reference. The C side writes one line
per case:

    E <inlen> <wrap> <encoded-with-newlines-as-\\n>
    D <inlen> <hex-of-decoded>

and this regenerates the same input -- (i*37+11) & 255, which is
cheap to agree on -- and checks the encoding or the decoding against
base64.b64encode / b64decode.

Exit 0 if every line checks out, 1 otherwise, with the first few
mismatches on stderr. The count of cases is printed on stdout so the C
side can assert that the judge actually looked at something: a judge
that silently examined zero lines would pass forever.
"""
import base64, sys

def pat(n):
    return bytes(((i * 37 + 11) & 255) for i in range(n))

def wrapped(data, wrap):
    s = base64.b64encode(data).decode()
    if wrap <= 0:
        return s
    wrap -= wrap % 4
    if wrap < 4:
        return s
    return "\n".join(s[i:i+wrap] for i in range(0, len(s), wrap))

def main():
    bad, seen = [], 0
    for line in open(sys.argv[1]):
        line = line.rstrip("\n")
        if not line:
            continue
        kind, rest = line.split(" ", 1)
        seen += 1
        if kind == "E":
            n, wrap, enc = rest.split(" ", 2)
            enc = enc.replace("\\n", "\n")
            want = wrapped(pat(int(n)), int(wrap))
            if enc != want:
                bad.append("E len=%s wrap=%s\n   got  %r\n   want %r"
                           % (n, wrap, enc[:90], want[:90]))
        elif kind == "D":
            n, hexs = rest.split(" ", 1)
            got = bytes.fromhex(hexs) if hexs != "-" else b""
            want = pat(int(n))
            if got != want:
                bad.append("D len=%s\n   got  %r\n   want %r"
                           % (n, got[:40], want[:40]))
        else:
            bad.append("unknown line kind %r" % kind)
    print(seen)
    for b in bad[:5]:
        sys.stderr.write(b + "\n")
    if bad:
        sys.stderr.write("%d of %d cases wrong\n" % (len(bad), seen))
        return 1
    return 0

sys.exit(main())
