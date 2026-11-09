#!/usr/bin/env python3
"""check_der_suite.py -- prove tests/test_der.c can fail.

It passed 57 of 57 first run, which in this tree is the least
trustworthy result there is.

net/der.c is the foundation of pairing stage 3: an X.509 certificate
IS DER, a PKCS#1 signature is computed over DER, and the pair record
carries three certificates. A quiet error here is a certificate the
phone rejects with no reason given, three layers below where the
symptom appears -- and every defect below produces a document that
looks entirely reasonable in a hex dump.

── THE TWO THAT ACTUALLY BREAK RSA ───────────────────────────────────

  THE SIGN BYTE. A DER INTEGER is SIGNED. A 2048-bit modulus has its
  top bit set by definition, so it must carry a leading 0x00 and
  encode as 257 content bytes. Drop that and every verifier in the
  world rejects the certificate. Nothing about the bytes looks wrong.

  THE MINIMAL LENGTH. 200 bytes of content is `81 C8`. `82 00 C8`
  is BER, is not DER, and a strict parser refuses it -- while a
  lenient one accepts it, so whether it works depends on which kind of
  parser is at the other end and nothing ever says which.

── AND THE ONE THAT IS NOT ABOUT ENCODING AT ALL ─────────────────────

An overflow that does not latch. The writer is used by writing a whole
document and checking once at the end; if a failed write let later
ones succeed, the caller would be handed a SHORT DOCUMENT THAT LOOKS
COMPLETE. That is worse than any wrong byte, because the length is
right and the content parses.

Run:  python3 tools/check_der_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "net/der.c"
TEST = "tests/test_der.c"
# -fno-builtin so the memcpy and memmove calls inside net/der.c reach
# kernel/string.c's versions rather than being replaced by the
# compiler's own. The suite links the kernel's string.c on purpose --
# see its header: glibc's memcpy survives an overlapping copy and the
# kernel's byte-by-byte forward one does not, so testing against the
# host's would have tested a memcpy that does not ship.
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("an INTEGER never gets its sign byte",
     "THE ONE THAT BREAKS RSA. A 2048-bit modulus reads as a negative "
     "number and every verifier rejects the certificate. 256 content "
     "bytes where there should be 257, and a hex dump that looks "
     "perfectly reasonable",
     """    pad = (be[i] & 0x80u) ? 1 : 0;         /* keep it positive */""",
     """    pad = 0;"""),

    ("an INTEGER always gets a sign byte",
     "the other way round, and just as wrong: a leading 0x00 on a "
     "value that does not need one is NOT minimal, so it is BER "
     "rather than DER. 65537 becomes 02 04 00 01 00 01",
     """    pad = (be[i] & 0x80u) ? 1 : 0;         /* keep it positive */""",
     """    pad = 1;"""),

    ("leading zeroes are not stripped from an INTEGER",
     "a bignum exports at its full fixed width, so every modulus "
     "arrives with hundreds of leading zero bytes. Not stripping them "
     "makes a 2048-bit key encode as a 4096-bit integer with a "
     "leading run of zeroes -- non-minimal, and the wrong length",
     """    while(i < len && be[i] == 0) i++;      /* strip leading zeroes */""",
     """    /* no stripping */"""),

    ("INTEGER zero encodes with no content byte",
     "an INTEGER has at least one content octet. `02 00` is not a "
     "legal encoding of zero and a strict parser refuses the whole "
     "structure, which in a certificate is the serial number",
     """    if(i == len){                          /* the value is zero */
        put1(d, DER_INTEGER);
        put1(d, 1);
        put1(d, 0);
        return;
    }""",
     """    if(i == len){
        put1(d, DER_INTEGER);
        put1(d, 0);
        return;
    }"""),

    ("the short length form runs to 128 instead of 127",
     "0x80 is the marker that says 'long form follows', so a content "
     "length of exactly 128 written as a single 0x80 byte is read as "
     "an indefinite length and the parser walks off looking for an "
     "end-of-contents marker that is not there",
     """    if(n < 0x80u){ put1(d, (uint8_t)n); return; }""",
     """    if(n <= 0x80u){ put1(d, (uint8_t)n); return; }"""),

    ("the long length form is not minimal",
     "200 bytes encoded as `82 00 C8` instead of `81 C8`. It is BER, "
     "it is not DER, and a lenient parser accepts it -- so whether "
     "the certificate works depends on which parser is at the other "
     "end, which is the worst possible failure mode",
     """    if(n <= 0xFFu){        put1(d, 0x81); put1(d, (uint8_t)n); return; }""",
     """    if(n <= 0xFFu){        put1(d, 0x82); put1(d, 0x00);
                           put1(d, (uint8_t)n); return; }"""),

    ("len_bytes disagrees with put_len about the long form",
     "der_seq_end() sizes the gap with len_bytes() and fills it with "
     "put_len(). If they disagree the contents are moved by the wrong "
     "amount and EVERY BYTE after the header is off by one -- which "
     "is a certificate that parses as far as the first length and "
     "then becomes nonsense",
     """    if(n <= 0xFFu) return 2;""",
     """    if(n <= 0xFFu) return 3;"""),

    ("a BIT STRING has no unused-bits octet",
     "the public key in a certificate is a BIT STRING. Without the "
     "leading count the whole key is shifted one byte earlier and the "
     "first byte of the key is read as the bit count",
     """    put_len(d, len + 1);
    put1(d, 0x00);                         /* unused bits */""",
     """    put_len(d, len);"""),

    ("an OID's first two arcs are not folded",
     "1.2.840... must begin 2A, which is 1*40+2. Emitting them "
     "separately makes every OID in the document one byte longer and "
     "unrecognisable -- the signature algorithm becomes an OID nobody "
     "has ever registered",
     """    body[bl++] = (uint8_t)(arcs[0] * 40u + arcs[1]);""",
     """    body[bl++] = (uint8_t)arcs[0];
    body[bl++] = (uint8_t)arcs[1];"""),

    ("an OID's continuation bit is set on the last group too",
     "base-128 marks every byte but the last. Setting it on the last "
     "one as well means the parser keeps reading into whatever "
     "follows the OID, which is the next field of the certificate",
     """            body[bl++] = (uint8_t)(g[gn] | (gn ? 0x80u : 0u));""",
     """            body[bl++] = (uint8_t)(g[gn] | 0x80u);"""),

    ("an OID with a first arc above 2 is accepted",
     "40*3+b overflows into the second arc's range, so 3.x encodes as "
     "a completely different OID that is perfectly well-formed. A "
     "refusal is the only correct answer",
     """    if(arcs[0] > 2){ d->ok = 0; return; }""",
     """    /* any first arc */"""),

    ("DER TRUE is encoded as 01 rather than FF",
     "BER allows any non-zero byte and DER requires all-ones. X.509 "
     "marks a critical extension with a BOOLEAN, so this is the "
     "difference between an extension the phone must honour and one a "
     "strict parser rejects the certificate over",
     """    uint8_t b = v ? 0xFF : 0x00;""",
     """    uint8_t b = v ? 0x01 : 0x00;"""),

    ("a sequence is closed with the wrong tag",
     "the marker carries the tag so that an opened SEQUENCE cannot be "
     "closed as a SET. Hard-coding it here is exactly that bug: the "
     "lengths stay correct and the document parses as a structure "
     "nobody wrote",
     """    uint8_t  tag     = DER_MARK_TAG(marker);""",
     """    uint8_t  tag     = DER_SEQUENCE;"""),

    ("the sequence contents are copied, not moved",
     "memcpy with overlapping regions, which they are whenever the "
     "header is shorter than the contents -- almost always. What "
     "lands in the buffer depends on the direction the libc happens "
     "to copy in, so it works on one machine and not another",
     """    memmove(d->buf + off + hdr, d->buf + off, content);""",
     """    memcpy(d->buf + off + hdr, d->buf + off, content);"""),

    ("a failed write does not latch",
     "THE WORST ONE HERE, and not about encoding at all. A caller "
     "writes a whole certificate and checks der_ok() once at the end. "
     "If a failed write lets later ones succeed, the caller is handed "
     "a SHORT DOCUMENT THAT LOOKS COMPLETE -- correct length, parses "
     "cleanly, missing a field",
     """    if(n > d->cap - d->len){ d->ok = 0; return; }""",
     """    if(n > d->cap - d->len){ return; }"""),

    ("an oversized write is truncated instead of refused",
     "the bytes that fit are written and the rest are dropped, so the "
     "length field says one thing and the content is another. A "
     "parser reads the length, walks past the end of the buffer, and "
     "the kernel reads whatever is there",
     """    if(n > d->cap - d->len){ d->ok = 0; return; }
    if(p) memcpy(d->buf + d->len, p, n);""",
     """    if(n > d->cap - d->len){ n = d->cap - d->len; }
    if(p) memcpy(d->buf + d->len, p, n);"""),
]

# Must NOT be caught: a comment reworded.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the encoding changed",
    """void der_null(der_t *d){ der_tlv(d, DER_NULL, 0, 0); }""",
    """/* NULL: a tag and a zero length, no content */
void der_null(der_t *d){ der_tlv(d, DER_NULL, 0, 0); }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "der", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_der.c can fail.\n"
                 "It passed 57 of 57 first time. Every defect below\n"
                 "produces a document that looks entirely reasonable\n"
                 "in a hex dump and is rejected by the phone."))
