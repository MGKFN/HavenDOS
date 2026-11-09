/* ================================================================
   include/x509.h -- X.509 v3 certificates, and the signature on them
   ================================================================
   Pairing stage 3, v0.8.8.9.  net/der.c has been in the tree since
   v0.8.8.6 -- written, tested, and with NOTHING CALLING IT. This is
   its caller.

   libimobiledevice's common/userpref.c,
   pair_record_generate_keys_and_certs(), is what the phone expects:
   two RSA-2048 key pairs and THREE X.509 v3 certificates --

     root    self-signed, basicConstraints CA:TRUE, critical
     host    signed by root, CA:FALSE, keyUsage digitalSignature +
             keyEncipherment, critical
     device  the phone's own public key, signed by root

   -- all SHA-256 signed. That is the shape this file builds.

   ── WHY THE SIGNATURE AND THE CERTIFICATE ARE ONE FILE ──────────────

   Because the thing being signed is the exact bytes of the
   TBSCertificate, and the only way to be sure of that is to sign the
   bytes the encoder just produced, in the same function, from the
   same buffer. An interface that hands a TBS out to be signed
   elsewhere invites the caller to re-encode it -- and a certificate
   whose signature covers a DIFFERENT encoding of the same values
   verifies nowhere, while looking completely correct in a hex dump.

   ── WHAT JUDGES IT ──────────────────────────────────────────────────

   openssl. tests/test_x509.c builds certificates here and hands them
   to `openssl x509` to parse and to `openssl verify` to check the
   chain. Our own parser agreeing with our own encoder would prove
   nothing at all, and a broken signature that LOOKS right is the
   worst thing this sub-project could produce.
   ================================================================ */
#ifndef X509_H
#define X509_H
#include "types.h"
#include "bignum.h"

#define X509_OK              0
#define X509_E_SPACE       (-1)   /* the buffer is too small         */
#define X509_E_PARAM       (-2)
#define X509_E_KEY         (-3)   /* the modulus is unusable         */
#define X509_E_SIGN        (-4)   /* the RSA operation failed        */
#define X509_E_DER         (-5)   /* the encoder gave up             */

/* KeyUsage bits, in the order RFC 5280 numbers them. */
#define X509_KU_DIGITAL_SIG   0x80
#define X509_KU_NON_REPUD     0x40
#define X509_KU_KEY_ENCIPHER  0x20
#define X509_KU_DATA_ENCIPHER 0x10
#define X509_KU_KEY_AGREE     0x08
#define X509_KU_KEY_CERT_SIGN 0x04
#define X509_KU_CRL_SIGN      0x02

/* ── PKCS#1 v1.5 ─────────────────────────────────────────────────
 * EMSA-PKCS1-v1_5 over a SHA-256 digest, then the RSA private-key
 * operation.  `out` must have room for the modulus, and the
 * signature is always exactly that long -- left-padded with zeroes
 * if the result is numerically smaller, which RFC 8017 requires and
 * which a naive bn_to_bytes() would silently shorten.
 *
 * Returns X509_OK, or a negative code. */
int pkcs1_sign_sha256(const bn_t *n, const bn_t *d,
                      const uint8_t digest[32],
                      uint8_t *out, uint32_t outcap, uint32_t *outlen);

/* The padded block on its own, for a suite that wants to compare it
 * against `openssl rsautl` rather than trust the signature wholesale. */
int pkcs1_pad_sha256(uint32_t modulus_bytes, const uint8_t digest[32],
                     uint8_t *out, uint32_t outcap);

/* ── the certificate ─────────────────────────────────────────────── */
typedef struct {
    const char *subject_cn;
    const char *issuer_cn;       /* same as subject for self-signed   */
    uint32_t    serial;

    const bn_t *subject_n;       /* whose key this certifies          */
    const bn_t *subject_e;

    const bn_t *issuer_n;        /* whose key signs it                */
    const bn_t *issuer_d;

    int         is_ca;           /* basicConstraints CA:TRUE          */
    uint8_t     key_usage;       /* 0 to omit the extension           */

    /* UTCTime, exactly 13 characters: "YYMMDDHHMMSSZ". Not derived
       from a clock here, because a certificate whose bytes depend on
       when the test ran cannot be compared against a fixture. */
    const char *not_before;
    const char *not_after;
} x509_req_t;

/* Build and sign. *len is set to the DER length on success. */
int x509_build(const x509_req_t *req, uint8_t *out, uint32_t cap,
               uint32_t *len);

const char *x509_strerror(int code);

#endif /* X509_H */
