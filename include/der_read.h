#pragma once
#include "types.h"
#include "bignum.h"

/* ════════════════════════════════════════════════════════════════════
   READING DER -- enough of it to get a public key out

   net/der.c WRITES DER. This reads it, and it exists for exactly one
   reason: pairing stage 3 asks lockdownd for `DevicePublicKey` and
   gets back an RSA public key that has to be turned into two bignums
   before a certificate can be built around it.

   ── IT IS NOT A GENERAL ASN.1 PARSER, AND MUST NOT BECOME ONE ───────

   It reads definite-length, primitive and constructed tags, one at a
   time, out of a buffer whose end it never passes. It does not know
   what an OID means, does not follow a schema, does not allocate and
   does not recurse. Everything above that -- what the structure is
   supposed to contain -- is the caller's, spelled out in straight
   line code that can be read against the RFC it came from.

   A general parser would be the wrong thing twice over: it would be
   a lot of code in a kernel that needs two integers out of one
   structure, and a parser that handles everything is a parser nobody
   can check. net/tls.c took the other route and SCANS for the RSA
   OID inside a certificate, which works and is bounded but cannot
   tell you where in the structure it found it.

   ── WHAT IT REFUSES ────────────────────────────────────────────────

   Indefinite lengths (BER, not DER). A length encoded in more bytes
   than it needs, or in more than four. A length that runs past the
   end of the buffer. A tag number above 30 (high-tag-number form).
   A negative INTEGER where a modulus belongs. An INTEGER with a
   non-minimal leading zero, except the ONE leading zero that a
   positive integer with its top bit set is required to carry.

   Every one of those is a structure this code does not understand
   well enough to read, and the right answer to that is to stop.
   ════════════════════════════════════════════════════════════════════ */

#define DER_OK            0
#define DER_E_PARAM     (-1)
#define DER_E_TRUNC     (-2)   /* ran off the end of the buffer      */
#define DER_E_TAG       (-3)   /* not the tag the caller expected    */
#define DER_E_LEN       (-4)   /* indefinite, overlong or absurd     */
#define DER_E_VALUE     (-5)   /* a value that cannot be what it is  */
#define DER_E_TRAILING  (-6)   /* bytes left over after the structure */
#define DER_E_NOTFOUND  (-7)   /* no recognised key structure        */

/* DER tags, the handful used here. */
#define DER_T_INTEGER     0x02
#define DER_T_BITSTRING   0x03
#define DER_T_NULL        0x05
#define DER_T_OID         0x06
#define DER_T_SEQUENCE    0x30

/* A read cursor. Never holds a pointer the caller did not give it. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int            err;        /* sticky, like plist_t and der_t     */
} dr_t;

void dr_init(dr_t *d, const uint8_t *buf, uint32_t len);
int  dr_err(const dr_t *d);
uint32_t dr_left(const dr_t *d);

/* Read one TLV. On success *val points into the caller's buffer and
   *vlen is its length; the cursor moves past the whole thing. Pass
   want = 0 to accept any tag and receive it in *tag. */
int dr_tlv(dr_t *d, uint8_t want, uint8_t *tag,
           const uint8_t **val, uint32_t *vlen);

/* Descend into a constructed tag: `inner` is set up over its contents
   and the outer cursor moves past it. */
int dr_enter(dr_t *d, uint8_t want, dr_t *inner);

/* An INTEGER as a bignum. Refuses a negative value and a
   non-minimally-encoded one. */
int dr_integer_bn(dr_t *d, bn_t *out);

/* ── the one structure this is for ───────────────────────────────────

   An RSA public key, from either of the two encodings that turn up:

     PKCS#1  RSAPublicKey ::= SEQUENCE { modulus INTEGER,
                                         publicExponent INTEGER }

     X.509   SubjectPublicKeyInfo ::= SEQUENCE {
                 algorithm AlgorithmIdentifier,
                 subjectPublicKey BIT STRING }   -- holding the above

   WHICH ONE IS DECIDED BY THE STRUCTURE, NOT BY THE PEM LABEL.
   lockdownd has sent `DevicePublicKey` under both
   "BEGIN RSA PUBLIC KEY" and "BEGIN PUBLIC KEY" depending on the iOS
   version, and a reader that trusts the label fails on the version it
   was not written against. The outer SEQUENCE's first element is an
   INTEGER in one and a SEQUENCE in the other, which tells them apart
   with no ambiguity at all.

   Returns DER_OK, or a negative code. */
int rsa_pub_from_der(const uint8_t *der, uint32_t len,
                     bn_t *n, bn_t *e);

/* The same, from PEM text: the armor lines are skipped (whatever they
   say), the body is base64-decoded, and the DER inside is read.
   `scratch` must have room for the decoded DER -- a 2048-bit key is
   about 270 bytes, a 4096-bit one about 530. */
int rsa_pub_from_pem(const char *pem, uint32_t len,
                     uint8_t *scratch, uint32_t scratch_len,
                     bn_t *n, bn_t *e);

const char *der_read_strerror(int code);
