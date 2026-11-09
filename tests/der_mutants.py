#!/usr/bin/env python3
"""Build malformed DER public keys FROM A REAL ONE.

tests/test_der_read.c's first set of refusal fixtures were hand-typed
toy structures like

    30 06 02 01 01 02 01 01      SEQUENCE { INTEGER 1, INTEGER 1 }

with one thing wrong in the encoding. Twelve injected defects survived
the suite because of them: those fixtures are refused whatever the
length rules do, since a 1-bit modulus fails the minimum-key-size
check at the very end. They tested that the reader says no, not that
it says no FOR THE REASON UNDER TEST.

So every mutant here starts as a real openssl key and differs from it
in exactly one way. If the reader accepts one, the rule that mutant
breaks is not being enforced.

Usage: der_mutants.py <spki.der> <pkcs1.der> <outdir>
Writes one file per mutant and prints "name path" lines.
"""
import sys, os

def rd_len(b, i):
    """Returns (length, header_end_index)."""
    n = b[i]
    if n < 0x80:
        return n, i + 1
    k = n & 0x7F
    v = 0
    for j in range(k):
        v = (v << 8) | b[i + 1 + j]
    return v, i + 1 + k

def enc_len(n):
    if n < 0x80:
        return bytes([n])
    out = []
    while n:
        out.insert(0, n & 0xFF)
        n >>= 8
    return bytes([0x80 | len(out)]) + bytes(out)

def enc_len_padded(n, nbytes):
    """The same length in more bytes than it needs -- not valid DER."""
    out = []
    v = n
    while v:
        out.insert(0, v & 0xFF)
        v >>= 8
    while len(out) < nbytes:
        out.insert(0, 0)
    return bytes([0x80 | len(out)]) + bytes(out)

def find_int(b, i):
    """At index i expect an INTEGER; return (vstart, vlen, next_index)."""
    assert b[i] == 0x02, "expected INTEGER at %d, got %02x" % (i, b[i])
    ln, hs = rd_len(b, i + 1)
    return hs, ln, hs + ln

def rebuild_seq(inner):
    return bytes([0x30]) + enc_len(len(inner)) + inner

def main():
    spki = open(sys.argv[1], "rb").read()
    pk1  = open(sys.argv[2], "rb").read()
    out  = sys.argv[3]
    made = []

    def emit(name, data):
        p = os.path.join(out, "mut_" + name + ".der")
        open(p, "wb").write(data)
        made.append((name, p))

    # ── the outer length, three ways ────────────────────────────────
    _, hs = rd_len(spki, 1)
    body = spki[hs:]
    emit("indef", bytes([0x30, 0x80]) + body + b"\x00\x00")
    emit("overlong", bytes([0x30]) + enc_len_padded(len(body), 3) + body)
    # A length one byte short of the body: the structure then ends
    # early and the bytes after it are trailing data.
    emit("short_outer", bytes([0x30]) + enc_len(len(body) - 1) + body)

    # ── the PKCS#1 structure, picked apart and put back ─────────────
    ln, hs = rd_len(pk1, 1)
    ns, nl, after_n = find_int(pk1, hs)
    es, el, after_e = find_int(pk1, after_n)
    n_bytes = pk1[ns:ns + nl]
    e_bytes = pk1[es:es + el]

    def pk1_from(nb, eb, extra=b"", ntag=0x02):
        inner = bytes([ntag]) + enc_len(len(nb)) + nb \
              + b"\x02" + enc_len(len(eb)) + eb + extra
        return rebuild_seq(inner)

    # The modulus WITHOUT its leading zero: its top bit is set, so it
    # now encodes a negative integer.
    assert n_bytes[0] == 0, "modulus has no sign byte to remove"
    emit("neg_modulus", pk1_from(n_bytes[1:], e_bytes))
    # An EXTRA leading zero: a non-minimal encoding of the same value.
    emit("padded_modulus", pk1_from(b"\x00" + n_bytes, e_bytes))
    # A third INTEGER inside RSAPublicKey -- trailing data in the
    # structure, which a reader that stops after two will not see.
    emit("extra_field", pk1_from(n_bytes, e_bytes, b"\x02\x01\x07"))
    # The modulus under the high-tag-number form.
    emit("high_tag", pk1_from(n_bytes, e_bytes, ntag=0x1F))
    # One byte after the complete structure.
    emit("trailing_pk1", pk1 + b"\x00")
    # A BIT STRING whose unused-bit count is not zero: every byte of
    # the key after it is shifted.
    ln2, hs2 = rd_len(spki, 1)
    i = hs2
    _, ahs = rd_len(spki, i + 1)
    alen, _ = rd_len(spki, i + 1)
    i = ahs + alen                      # past AlgorithmIdentifier
    assert spki[i] == 0x03, "expected BIT STRING at %d" % i
    bl, bhs = rd_len(spki, i + 1)
    newbs = bytes([0x03]) + enc_len(bl) + b"\x01" + spki[bhs + 1:bhs + bl]
    newinner = spki[hs2:i] + newbs
    emit("bitstring_unused", rebuild_seq(newinner))

    # A length UNDER 128 encoded in two bytes. 0x122 padded to three
    # bytes is caught by the "no leading zero in a multi-byte length"
    # rule; this one is caught only by "a short length must be one
    # byte", which is a different rule and needs its own mutant.
    i = hs2
    alen2, ahs2 = rd_len(spki, i + 1)
    alg = spki[ahs2:ahs2 + alen2]
    newalg = bytes([0x30]) + enc_len_padded(len(alg), 1) + alg
    emit("alg_len_padded", rebuild_seq(newalg + spki[ahs2 + alen2:]))

    # An AlgorithmIdentifier whose OID claims a length of 1. A memcmp
    # of nine bytes against it reads eight past the OID -- which is
    # inside the key with a static buffer and a fault with an exact
    # one.
    assert spki[ahs2] == 0x06
    oidlen, ohs = rd_len(spki, ahs2 + 1)
    shortoid = bytes([0x06, 0x01, spki[ohs]])
    newalg2 = shortoid + spki[ohs + oidlen:ahs2 + alen2]
    emit("short_oid", rebuild_seq(bytes([0x30]) + enc_len(len(newalg2))
                                  + newalg2 + spki[ahs2 + alen2:]))

    for name, p in made:
        print(name, p)

main()
