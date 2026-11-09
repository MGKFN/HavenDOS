#!/usr/bin/env python3
"""check_pair_suite.py -- prove tests/test_pair.c can fail.

── WHY THIS FILE DID NOT EXIST UNTIL v0.8.9 ───────────────────────────

usb/pair.c's trailer records twenty-seven injected defects, twenty-four
caught on the first run, and names the three that survived and what was
wrong with the fixtures. That work was done in v0.8.8.11. It was never
written down as a harness, so none of those twenty-seven could be run
again -- and tools/check_b64_suite.py has just been created for exactly
the same reason about the same release.

tools/check_harnesses.py's own message is the rule being broken: "a
harness nobody runs cannot tell you it is broken." A harness run once
by hand and left as prose in a comment is that, one step earlier.

This file holds the ones that comment names explicitly, plus the
v0.8.9 store. It does not claim to be all twenty-seven: the others
were described in a sentence rather than in code, and inventing them
back would be guessing at what was run.

── WHAT THE STORE ADDS, AND WHY IT IS THE HIGHER STAKE ────────────────

The record now holds TWO PRIVATE KEYS. A round trip that is almost
right is a file that reloads cleanly, pairs, and then fails at
CertificateVerify -- with nothing on screen connecting the two. So the
store's injections are mostly about refusing rather than about
producing: a damaged record must be refused, not loaded as a key that
is nearly correct.

Run:  python3 tools/check_pair_suite.py

NEVER in a background process. This patches the tree and restores it
in a `finally`. Under ASAN, which is how the Makefile runs the suite.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/pair.c"
TEST = "tests/test_pair.c"
CC = ["gcc", "-std=gnu99", "-g", "-fsanitize=address,undefined",
      "-Iinclude", "-Itests", "net/tls_crypto.c"]

BUGS = [
    # ══════════════════════════════════════════════════════════════════
    # THE CERTIFICATES -- the ones usb/pair.c's trailer names
    # ══════════════════════════════════════════════════════════════════
    ("the root is not a CA",
     "basicConstraints CA:TRUE is what makes the other two "
     "certificates' chain legal. A device that checks refuses a chain "
     "whose root is not one",
     """    q.is_ca      = 1;
    q.not_before = NOT_BEFORE;
    q.not_after  = NOT_AFTER;
    r = x509_build(&q, rec->root_cert, sizeof rec->root_cert,""",
     """    q.is_ca      = 0;
    q.not_before = NOT_BEFORE;
    q.not_after  = NOT_AFTER;
    r = x509_build(&q, rec->root_cert, sizeof rec->root_cert,"""),

    ("the host certificate is a CA too",
     "a leaf that can issue certificates",
     """    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "HavenDOS Root";
    q.serial     = 2;
    q.subject_n  = &host->n;  q.subject_e = &host->e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 0;""",
     """    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "HavenDOS Root";
    q.serial     = 2;
    q.subject_n  = &host->n;  q.subject_e = &host->e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 1;"""),

    ("the host certificate signs itself",
     "self-signed instead of root-signed: the chain has nothing "
     "holding it together",
     """    q.subject_n  = &host->n;  q.subject_e = &host->e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 0;
    /* ── v0.8.9: keyUsage""",
     """    q.subject_n  = &host->n;  q.subject_e = &host->e;
    q.issuer_n   = &host->n;  q.issuer_d  = &host->d;
    q.is_ca      = 0;
    /* ── v0.8.9: keyUsage"""),

    ("the host certificate declares no keyUsage",
     "include/x509.h has described the host certificate as carrying "
     "digitalSignature + keyEncipherment since v0.8.8.9 and this "
     "function did not set it until v0.8.9. digitalSignature is the "
     "usage a TLS CertificateVerify exercises",
     """    q.key_usage  = X509_KU_DIGITAL_SIG | X509_KU_KEY_ENCIPHER;""",
     """    ;"""),

    ("the issuer name does not chain",
     "the signature would still verify and the NAME would not match, "
     "which is how a chain that looks right in a dump fails on a "
     "device",
     """    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "HavenDOS Root";""",
     """    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "Somebody Else";"""),

    ("THE DEVICE CERTIFICATE CARRIES OUR OWN MODULUS",
     "the one that would look perfect in every structural check. The "
     "host certifies the DEVICE's public key so that both ends of the "
     "later TLS session are vouched for by the same authority; "
     "certifying our own key three times passes every parse, every "
     "extension check and `openssl verify`, and fails on the phone",
     """    q.subject_n  = dev_n;     q.subject_e = dev_e;""",
     """    q.subject_n  = &host->n;  q.subject_e = &host->e;"""),

    ("all three serial numbers are 1",
     "`openssl verify` does not care for a two-certificate chain and "
     "lockdownd does not either, so this survived its first run. A "
     "certificate authority that issues duplicate serials is wrong "
     "anyway, and the day something checks is not the day to find out",
     """    q.serial     = 2;
    q.subject_n  = &host->n;  q.subject_e = &host->e;""",
     """    q.serial     = 1;
    q.subject_n  = &host->n;  q.subject_e = &host->e;"""),

    ("a key too small to pair with is accepted",
     "a 2048-bit pairing with a 17-bit key",
     """    if (bn_bits(&root->n) < 1024 || bn_bits(&host->n) < 1024)
        return PAIR_E_KEY;""",
     """    ;"""),

    ("the device's key is not checked at all",
     "DevicePublicKey comes off the wire",
     """    if (bn_bits(dev_n) < 1024) return PAIR_E_DEVKEY;""",
     """    ;"""),

    # ── the UUID ──────────────────────────────────────────────────────
    ("the UUID is not terminated",
     "this survived its first run because the test's buffer happened "
     "to hold a zero at offset 36. It is filled with 'X' now and the "
     "byte after the UUID is asserted",
     """    out[o] = 0;""",
     """    ;"""),

    ("the UUID is not a version-4 UUID",
     "lockdownd does not check, which is exactly the problem: a HostID "
     "that is not a UUID works until the day something does check it",
     """    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);""",
     """    ;"""),

    ("the UUID's dashes go in the wrong places",
     "8-4-4-4-12 is the canonical form and a pair record on disk is "
     "compared against one written by libimobiledevice",
     """        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';""",
     """        if (i == 4 || i == 6 || i == 8) out[o++] = '-';"""),

    ("a buffer too small for a UUID is written to anyway",
     "37 bytes, and the caller's buffer is the only bound",
     """    if (!out || cap < 37) return PAIR_E_PARAM;""",
     """    if (!out) return PAIR_E_PARAM;"""),

    # ── the request ───────────────────────────────────────────────────
    ("the capacity is inferred afterwards instead of checked first",
     "the first version of this did exactly that, and it was wrong in "
     "the case it most needed to be right: a buffer so small that the "
     "FIRST write is refused leaves the length at zero, so a hopeless "
     "capacity reported itself as a bug in the plist writer",
     """        uint32_t need = b64 + b64 / 68 + 1024;
        if (cap < need) return PAIR_E_SPACE;""",
     """        (void)b64;"""),

    ("the request omits the device certificate",
     "the device would have nothing of its own in the record",
     """        plist_kv_data(&p, "DeviceCertificate", rec->dev_cert, rec->dev_len);""",
     """        ;"""),

    ("ExtendedPairingErrors is not asked for",
     "without it a device that refuses says 'PairingFailed' and "
     "nothing else, and the whole value of this request is in its "
     "reply",
     """        plist_kv_bool(&p, "ExtendedPairingErrors", 1);""",
     """        ;"""),

    # ── the reply scanner ─────────────────────────────────────────────
    ("the reply scanner does not require the value to be a <string>",
     "this survived its first run because the fixture had no later "
     "</string> for a loose scanner to run to. The fixture is a <data> "
     "value followed by a real <string> now -- which is what a scanner "
     "that skipped the opening tag would return",
     """        if (memcmp(xml + j, "<string>", 8) != 0) return 0;""",
     """        while (j < len && xml[j] != '>') j++;
        j++;
        j -= 8;
        if (0) return 0;"""),

    ("a pending dialog is read as a success",
     "the difference between 'tap Trust' and 'paired'",
     """        if (eq(v, vlen, "PairingDialogResponsePending")) return PAIR_R_PENDING;""",
     """        ;"""),

    ("an Error is not looked for before a Result",
     "a reply can carry both",
     """    if (find_string(xml, len, "Error", &v, &vlen)) {""",
     """    if (0) {"""),

    # ══════════════════════════════════════════════════════════════════
    # v0.8.9 -- THE STORE. Two private keys on a FAT volume.
    # ══════════════════════════════════════════════════════════════════
    ("the CRC covers the line that carries it",
     "the CRC is computed over everything above its own line. "
     "Including that line means checking a value against itself",
     """    crc = gpt_crc32(out, w.len);""",
     """    crc = gpt_crc32(out, w.len + 8);"""),

    ("the CRC is not checked on the way in",
     "one flipped base64 character changes a decoded byte and nothing "
     "about the structure. Without the check that is a private key "
     "that is almost right",
     """    crc_is = gpt_crc32(in, body_end);
    if (crc_is != crc_said) return PAIR_E_CRC;""",
     """    (void)crc_said; (void)body_end; crc_is = 0; (void)crc_is;"""),

    ("the CRC is computed over the whole buffer rather than the body",
     "off by the length of the CRC line, so no record ever verifies",
     """    crc_is = gpt_crc32(in, body_end);""",
     """    crc_is = gpt_crc32(in, len);"""),

    ("the version is not checked",
     "this file will grow a field one day. A reader that ignored the "
     "version would load the first eleven fields of a twelve-field "
     "record and call it a pairing",
     """    if (ver != PS_VERSION) return PAIR_E_VERSION;""",
     """    ;"""),

    ("fields are accepted in any order",
     "the order IS the format. A reader that accepts any order cannot "
     "tell a record from one with two fields swapped -- and RootN "
     "where HostN belongs is a record that signs with the wrong key",
     """        v = ps_match(in, s, e, PS_NAME[field]);
        if (!v) return PAIR_E_FORMAT;
        pos = next;""",
     """        v = ps_match(in, s, e, PS_NAME[field]);
        if (!v) { v = s; while (v < e && in[v] != ' ') v++; v++; }
        pos = next;"""),

    ("a field name matches on a prefix",
     "'Root' would match the line 'RootN ...', so one field would "
     "be read as another",
     """    if (in[s + n] != ' ') return 0;""",
     """    ;"""),

    ("a UUID is accepted on its length alone",
     "36 characters of anything would become a HostID",
     """    if (n != 36) return 0;
    for (i = 0; i < 36; i++) {""",
     """    if (n != 36) return 0;
    for (i = 0; i < 0; i++) {"""),

    ("a decoded certificate is copied without its length",
     "the record's own lengths are what every later use is bounded by",
     """            case PS_F_ROOTCERT:
                memcpy(t.rec.root_cert, scratch, (uint32_t)n);
                t.rec.root_len = (uint32_t)n;
                break;""",
     """            case PS_F_ROOTCERT:
                memcpy(t.rec.root_cert, scratch, (uint32_t)n);
                t.rec.root_len = PAIR_CERT_MAX;
                break;"""),

    ("a record with no private key is accepted on the way in",
     "it would reload cleanly, pair, and fail at CertificateVerify -- "
     "which is the failure include/pair.h says this struct exists to "
     "make impossible",
     """    if (bn_bits(&t.root.n) < 1024 || bn_bits(&t.host.n) < 1024 ||
        bn_is_zero(&t.root.d)     || bn_is_zero(&t.host.d) ||
        bn_is_zero(&t.root.e)     || bn_is_zero(&t.host.e))
        return PAIR_E_KEY;""",
     """    ;"""),

    ("a record with no private key is written out",
     "the same refusal on the way out, where there is something to say "
     "about it",
     """    if (bn_bits(&st->root.n) < 1024 || bn_bits(&st->host.n) < 1024 ||
        bn_is_zero(&st->root.d)     || bn_is_zero(&st->host.d) ||
        bn_is_zero(&st->root.e)     || bn_is_zero(&st->host.e))
        return PAIR_E_KEY;""",
     """    ;"""),

    ("the writer keeps no byte back for the terminator",
     "the buffer is handed to vfs_write() as a length AND is NUL "
     "terminated for anything that reads it as a string",
     """    if (n + 1 > w->cap - w->len) { w->ok = 0; return; }""",
     """    if (n > w->cap - w->len) { w->ok = 0; return; }"""),

    ("the writer truncates instead of refusing",
     "a record encoded into a buffer too small for it is a file that "
     "decodes to a shorter key, and net/b64.h's header makes the same "
     "argument about a decoder that shortens one",
     """    if (!w->ok) return;
    /* One byte held back for the terminator, always. */
    if (n + 1 > w->cap - w->len) { w->ok = 0; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;""",
     """    if (!w->ok) return;
    if (n + 1 > w->cap - w->len) n = w->cap - w->len - 1;
    memcpy(w->buf + w->len, s, n);
    w->len += n;"""),

    ("a public exponent of zero is accepted",
     "the one field the size and private-key checks say nothing about. "
     "A zero exponent encodes as a ZERO-LENGTH field -- a record that "
     "writes cleanly and will not read back. Added because the "
     "harness proposed the bignum-writer defect below and the only "
     "honest way to make that unreachable was to refuse this here",
     """        bn_is_zero(&st->root.e)     || bn_is_zero(&st->host.e))
        return PAIR_E_KEY;""",
     """        0)
        return PAIR_E_KEY;"""),

    # ── NOT REACHABLE, recorded rather than counted ───────────────────
    #
    # v0.8.6.3's rule: an injection that cannot express a REACHABLE
    # defect must not be added, because it reports a catch the suite
    # did not earn. Two were dropped here and both are worth naming so
    # nobody writes them again.
    #
    # "a bignum is written with its leading zeroes trimmed to nothing"
    #     -- removing `if (nb == 0) nb = 1;` in psw_bn_field(). Only a
    #     ZERO bignum reaches it, and pair_store_encode() now refuses a
    #     record with a zero modulus, a zero private exponent or a zero
    #     public exponent before any field is written. The guard is
    #     defensive and stays: bn_to_bytes() with a length of zero is
    #     the kind of call that becomes reachable when somebody adds a
    #     twelfth field.
    #
    # "the bignum buffer bound is not checked"
    #     -- removing `if (nb > sizeof tmp)`. tmp is BN_LIMBS*4 + 4
    #     bytes and bn_bits() cannot exceed BN_LIMBS*32, so nb is at
    #     most BN_LIMBS*4 and the bound cannot be crossed by any value
    #     a bn_t can hold. Unreachable by construction rather than by
    #     a check elsewhere -- which is exactly why the check stays.

    # ── the signer ────────────────────────────────────────────────────
    ("pair_tls_sign signs with the ROOT key",
     "the host certificate certifies the HOST key. One struct field, "
     "and the result is a perfectly good PKCS#1 v1.5 signature the "
     "device refuses while every certificate in the chain checks out",
     """    if (pkcs1_sign_sha256(&k->n, &k->d, digest, sig, sigcap, siglen)""",
     """    if (pkcs1_sign_sha256(&k->n, &k->e, digest, sig, sigcap, siglen)"""),

    ("pair_tls_sign reports a failed signature as a success",
     "net/tls.c would then put an empty signature on the wire",
     """    if (pkcs1_sign_sha256(&k->n, &k->d, digest, sig, sigcap, siglen)
        != X509_OK)
        return PAIR_E_KEY;""",
     """    pkcs1_sign_sha256(&k->n, &k->d, digest, sig, sigcap, siglen);"""),

    ("pair_tls_sign takes a NULL key",
     "it is reached through a function pointer from net/tls.c",
     """    if (!k || !digest || !sig) return PAIR_E_PARAM;""",
     """    if (!digest || !sig) return PAIR_E_PARAM;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """static const char HEX[] = "0123456789abcdef";""",
    """/* lower case, upper-cased below */
static const char HEX[] = "0123456789abcdef";""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "pair", SRC, TEST, CC, BUGS, HARMLESS, timeout=600,
        preamble="Proving tests/test_pair.c can fail.\n"
                 "openssl verifies the chain and the signature; the "
                 "store holds two private keys,\nso its injections are "
                 "mostly about refusing a record that is almost right."))
