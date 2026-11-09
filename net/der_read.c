/* net/der_read.c -- see include/der_read.h. */
#include "der_read.h"
#include "b64.h"
#include "string.h"

void dr_init(dr_t *d, const uint8_t *buf, uint32_t len)
{
    d->p = buf;
    d->end = buf ? buf + len : 0;
    d->err = buf ? DER_OK : DER_E_PARAM;
}
int dr_err(const dr_t *d){ return d ? d->err : DER_E_PARAM; }
uint32_t dr_left(const dr_t *d)
{
    if (!d || d->err || !d->p || d->p > d->end) return 0;
    return (uint32_t)(d->end - d->p);
}

static int fail(dr_t *d, int code)
{
    if (!d->err) d->err = code;
    return d->err;
}

int dr_tlv(dr_t *d, uint8_t want, uint8_t *tag,
           const uint8_t **val, uint32_t *vlen)
{
    uint8_t t, b;
    uint32_t len = 0;

    if (!d) return DER_E_PARAM;
    if (d->err) return d->err;
    if (dr_left(d) < 2) return fail(d, DER_E_TRUNC);

    t = *d->p++;
    /* High-tag-number form: tag number 31 means "more bytes follow".
       Nothing here uses one, and accepting it would mean accepting a
       structure this code cannot name. */
    if ((t & 0x1F) == 0x1F) return fail(d, DER_E_TAG);
    if (want && t != want)  return fail(d, DER_E_TAG);

    b = *d->p++;
    if (b == 0x80) {
        /* Indefinite length. Legal BER, illegal DER, and the thing a
           reader accepts just before it starts scanning for an
           end-of-contents marker it cannot bound. */
        return fail(d, DER_E_LEN);
    }
    if (b & 0x80) {
        int nb = b & 0x7F, i;
        if (nb > 4) return fail(d, DER_E_LEN);
        if (dr_left(d) < (uint32_t)nb) return fail(d, DER_E_TRUNC);
        for (i = 0; i < nb; i++) len = (len << 8) | *d->p++;
        /* DER requires the shortest encoding: a length under 128 must
           be one byte, and a multi-byte length must not have a
           leading zero. Both are how two different encodings of the
           same structure get different signatures. */
        if (len < 0x80) return fail(d, DER_E_LEN);
        if (nb > 1 && len < (1u << (8 * (nb - 1)))) return fail(d, DER_E_LEN);
    } else {
        len = b;
    }
    if (len > dr_left(d)) return fail(d, DER_E_TRUNC);

    if (tag)  *tag  = t;
    if (val)  *val  = d->p;
    if (vlen) *vlen = len;
    d->p += len;
    return DER_OK;
}

int dr_enter(dr_t *d, uint8_t want, dr_t *inner)
{
    const uint8_t *v = 0;
    uint32_t n = 0;
    int r;
    if (!d || !inner) return DER_E_PARAM;
    /* A constructed tag has bit 6 set. Entering a primitive one would
       be reading its payload as structure. */
    if (want && !(want & 0x20)) return fail(d, DER_E_TAG);
    r = dr_tlv(d, want, 0, &v, &n);
    if (r != DER_OK) { dr_init(inner, 0, 0); inner->err = r; return r; }
    dr_init(inner, v, n);
    return DER_OK;
}

int dr_integer_bn(dr_t *d, bn_t *out)
{
    const uint8_t *v = 0;
    uint32_t n = 0;
    int r;

    if (!d || !out) return DER_E_PARAM;
    r = dr_tlv(d, DER_T_INTEGER, 0, &v, &n);
    if (r != DER_OK) return r;
    if (n == 0) return fail(d, DER_E_VALUE);      /* no such INTEGER */
    /* A set top bit means a negative two's-complement integer. A
       modulus is not negative, and reading one as unsigned would
       produce a number that is wrong by 2^k with no sign of trouble. */
    if (v[0] & 0x80) return fail(d, DER_E_VALUE);
    if (n > 1 && v[0] == 0 && !(v[1] & 0x80)) {
        /* A leading zero is required ONLY to keep a positive value
           with its top bit set from looking negative. Any other
           leading zero is a non-minimal encoding. */
        return fail(d, DER_E_VALUE);
    }
    /* Skip the sign byte if it is there. */
    if (n > 1 && v[0] == 0) { v++; n--; }
    /* bn_from_bytes() returns NONZERO on success -- the opposite of
       the convention in this file and in net/der.c, and the same as
       bn_to_bytes(), whose header says "0 if it does not fit". The
       first version of this line read `!= 0` and refused every real
       key with DER_E_VALUE. */
    if (!bn_from_bytes(out, v, n)) return fail(d, DER_E_VALUE);
    return DER_OK;
}

/* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
static int rsa_pub_pkcs1(const uint8_t *der, uint32_t len,
                         bn_t *n, bn_t *e)
{
    dr_t top, seq;
    int r;
    dr_init(&top, der, len);
    r = dr_enter(&top, DER_T_SEQUENCE, &seq);
    if (r != DER_OK) return r;
    if (dr_left(&top) != 0) return DER_E_TRAILING;
    r = dr_integer_bn(&seq, n);
    if (r != DER_OK) return r;
    r = dr_integer_bn(&seq, e);
    if (r != DER_OK) return r;
    if (dr_left(&seq) != 0) return DER_E_TRAILING;
    return DER_OK;
}

/* SubjectPublicKeyInfo ::= SEQUENCE { AlgorithmIdentifier,
                                       BIT STRING } */
static int rsa_pub_spki(const uint8_t *der, uint32_t len,
                        bn_t *n, bn_t *e)
{
    dr_t top, seq, alg;
    const uint8_t *bs = 0;
    uint32_t bslen = 0;
    int r;

    dr_init(&top, der, len);
    r = dr_enter(&top, DER_T_SEQUENCE, &seq);
    if (r != DER_OK) return r;
    if (dr_left(&top) != 0) return DER_E_TRAILING;

    /* ── THE ALGORITHM IS CHECKED, AND THE FIRST VERSION DID NOT ────
       This used to skip the AlgorithmIdentifier, with a comment
       claiming that checking the OID would need an OID table and
       that a non-RSA key would fail to parse further down anyway.

       tests/test_der_read.c's bit-flip sweep proved that wrong in one
       line: corrupting byte 6 of a real key -- the OID's own tag --
       produced the SAME modulus, because nothing ever looked at it.
       A reader that ignores a field accepts a key somebody edited.

       It is one memcmp against nine bytes. There is no table.
       rsaEncryption is 1.2.840.113549.1.1.1, and RFC 3279 makes the
       NULL parameters mandatory for it -- an absent or non-NULL
       parameter is a different algorithm wearing the same OID. */
    {
        static const uint8_t RSA_OID[9] = {
            0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01
        };
        const uint8_t *oid = 0, *par = 0;
        uint32_t oidlen = 0, parlen = 0;

        r = dr_enter(&seq, DER_T_SEQUENCE, &alg);
        if (r != DER_OK) return r;
        r = dr_tlv(&alg, DER_T_OID, 0, &oid, &oidlen);
        if (r != DER_OK) return r;
        if (oidlen != sizeof RSA_OID) return DER_E_VALUE;
        if (memcmp(oid, RSA_OID, sizeof RSA_OID) != 0) return DER_E_VALUE;
        r = dr_tlv(&alg, DER_T_NULL, 0, &par, &parlen);
        if (r != DER_OK) return r;
        if (parlen != 0) return DER_E_VALUE;
        if (dr_left(&alg) != 0) return DER_E_TRAILING;
    }

    r = dr_tlv(&seq, DER_T_BITSTRING, 0, &bs, &bslen);
    if (r != DER_OK) return r;
    if (dr_left(&seq) != 0) return DER_E_TRAILING;
    /* The first octet of a BIT STRING counts the unused bits in the
       last one. For a key it is always zero, and a nonzero value here
       would shift every byte that follows. */
    if (bslen < 2 || bs[0] != 0) return DER_E_VALUE;
    return rsa_pub_pkcs1(bs + 1, bslen - 1, n, e);
}

int rsa_pub_from_der(const uint8_t *der, uint32_t len, bn_t *n, bn_t *e)
{
    dr_t top, seq;
    uint8_t t = 0;
    int r;

    if (!der || !n || !e || len < 8) return DER_E_PARAM;

    /* Which of the two it is, decided by the first element inside the
       outer SEQUENCE: an INTEGER for PKCS#1, a SEQUENCE for SPKI. The
       PEM label is not consulted, because lockdownd has used both
       labels and the structure never lies. */
    dr_init(&top, der, len);
    if (dr_enter(&top, DER_T_SEQUENCE, &seq) != DER_OK) return DER_E_NOTFOUND;
    if (dr_tlv(&seq, 0, &t, 0, 0) != DER_OK) return DER_E_NOTFOUND;

    if (t == DER_T_INTEGER)  { r = rsa_pub_pkcs1(der, len, n, e); }
    else if (t == DER_T_SEQUENCE) { r = rsa_pub_spki(der, len, n, e); }
    else return DER_E_NOTFOUND;

    if (r != DER_OK) return r;
    /* A modulus of a few bits and an exponent of 1 are both
       structurally valid and cryptographically meaningless. Refusing
       them here means the pairing code never has to. */
    if (bn_bits(n) < 512) return DER_E_VALUE;
    if (bn_bits(e) < 2)   return DER_E_VALUE;
    return DER_OK;
}

int rsa_pub_from_pem(const char *pem, uint32_t len,
                     uint8_t *scratch, uint32_t scratch_len,
                     bn_t *n, bn_t *e)
{
    uint32_t i = 0, body_start, body_end;
    int dec;

    if (!pem || !scratch || !n || !e) return DER_E_PARAM;
    if (len == 0 || scratch_len == 0) return DER_E_PARAM;

    /* Find the end of the BEGIN line. What that line SAYS is not
       read: "RSA PUBLIC KEY" and "PUBLIC KEY" are both correct for
       different iOS versions and the structure decides anyway. */
    while (i + 5 <= len && memcmp(pem + i, "-----", 5) != 0) i++;
    if (i + 5 > len) {
        /* No armor at all. Treat the whole thing as base64, because a
           plist <data> value that has already been decoded once looks
           exactly like this. */
        body_start = 0;
        body_end = len;
    } else {
        while (i < len && pem[i] != '\n') i++;
        if (i >= len) return DER_E_PARAM;
        body_start = i + 1;
        body_end = body_start;
        while (body_end + 5 <= len &&
               memcmp(pem + body_end, "-----", 5) != 0) body_end++;
        if (body_end <= body_start) return DER_E_PARAM;
    }

    dec = b64_decode((const uint8_t *)pem + body_start,
                     (int)(body_end - body_start),
                     scratch, (int)scratch_len);
    if (dec < 0) return DER_E_PARAM;
    return rsa_pub_from_der(scratch, (uint32_t)dec, n, e);
}

const char *der_read_strerror(int code)
{
    switch (code) {
    case DER_OK:           return "ok";
    case DER_E_PARAM:      return "bad argument";
    case DER_E_TRUNC:      return "the structure runs past the end of the data";
    case DER_E_TAG:        return "not the tag that belongs there";
    case DER_E_LEN:        return "a length this reader will not accept";
    case DER_E_VALUE:      return "a value that cannot be what it claims";
    case DER_E_TRAILING:   return "bytes left over after the structure";
    case DER_E_NOTFOUND:   return "not a public key structure we know";
    default:               return "unknown";
    }
}

/* ════════════════════════════════════════════════════════════════════
   WHAT THIS WAS PROVED AGAINST, AND WHAT THE SUITE HAD TO LEARN

   22 defects injected one at a time. The FIRST run caught ten of
   them. That is the number worth recording, because the suite was
   passing 64 of 64 checks at the time and looked thorough.

   Three rounds of fixing the suite, not the code, got it to 19:

     1. EVERY REFUSAL FIXTURE WAS A TOY. They were hand-typed
        structures like SEQUENCE { INTEGER 1, INTEGER 1 } with one
        encoding rule broken -- and a 1-bit modulus is refused by the
        minimum-key-size check at the very end no matter what the
        length rules do. Twelve injections survived behind that. Now
        tests/der_mutants.py takes a real openssl key apart and puts
        it back with exactly one thing wrong.

     2. THE FIXTURES HAD 4096 BYTES OF LEGAL MEMORY BEHIND THEM. A
        reader that walks two bytes past the end of a 294-byte key
        read inside a static array, so neither the suite nor ASAN saw
        anything. Every fixture now goes through a malloc of exactly
        its own length, and the 294-prefix truncation sweep allocates
        each prefix separately.

     3. THE PUBLIC API COULD NOT REACH TWO GUARDS.
        rsa_pub_from_der() refuses anything under eight bytes, so the
        "a TLV needs two bytes" guard was unreachable through it; and
        no caller passes dr_enter() a primitive tag or dr_tlv() a
        want of zero that it then acts on. Those are now called
        directly. An unreachable guard is either tested or deleted.

   The code changed once in all of this, and the bit-flip sweep is
   what found it: corrupting byte 6 of a real key -- the algorithm
   OID's own tag -- produced the SAME modulus, because this file used
   to skip the AlgorithmIdentifier with a comment claiming that
   checking it would need an OID table. It needs one memcmp against
   nine bytes.

   ── THE THREE SURVIVORS, WHICH ARE REDUNDANCY AND NOT GAPS ──────────

   1. Removing the indefinite-length branch survives. 0x80 is both
      "indefinite" and "a length in zero bytes", and the minimal-
      encoding rule below it refuses the second reading. The branch
      stays because DER_E_LEN with a comment naming BER is a better
      thing to debug than a length that came out as zero.

   2. Not skipping the modulus's sign byte survives, because
      bn_from_bytes() ignores leading zeroes. The skip stays so that
      the byte length a caller can derive matches the modulus.

   3. Removing `oidlen != sizeof RSA_OID` survives, because the
      memcmp that follows differs anyway for every real non-RSA key.
      It stays because that argument depends on what happens to
      follow the OID in the buffer, which is not something this
      function controls, and a nine-byte compare against a one-byte
      field is the kind of thing that is only safe by accident.
   ════════════════════════════════════════════════════════════════════ */
