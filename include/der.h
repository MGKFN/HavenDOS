#pragma once
#include "types.h"
#include "bignum.h"

/* ════════════════════════════════════════════════════════════════════
   include/der.h -- writing DER.  v0.8.8.5.  Pairing stage 3, piece one.

   Everything stage 3 needs is built on this: an X.509 v3 certificate
   is DER, a PKCS#1 signature is over DER, and the device's pair
   record carries three certificates. So a quiet error here is a
   certificate the phone rejects with no explanation, and the error is
   three layers below where it shows up.

   ── WHAT MAKES IT DER AND NOT JUST BER ──────────────────────────────

   DER is BER with the choices removed, and the two that matter here
   are both places this code could be plausibly wrong and still look
   right in a hex dump:

     THE LENGTH MUST BE MINIMAL. Under 128 it is one byte. From 128 up
     it is 0x80|n followed by n big-endian bytes with NO LEADING ZERO.
     Encoding 200 as `81 C8` is right; as `82 00 C8` is BER and not
     DER, and a strict parser refuses it.

     AN INTEGER IS SIGNED, big-endian, and minimal. THIS IS THE ONE
     THAT BREAKS RSA. A 2048-bit modulus has its top bit set, so as a
     signed integer it is negative -- it MUST be prefixed with a 0x00
     byte, making the content 257 bytes and not 256. Get it wrong and
     every verifier in the world rejects the certificate, and the hex
     dump looks entirely reasonable. The matching rule at the other
     end: leading zero bytes are stripped, so INTEGER 0 is `02 01 00`
     and never `02 00`.

   ── HOW A SEQUENCE GETS ITS LENGTH ──────────────────────────────────

   A constructed type's length is not known until its contents are
   written. Three ways out, and the choice matters:

     RESERVE A FIXED-SIZE LENGTH and patch it. Simple, and it emits
     NON-MINIMAL lengths -- so it is not DER. Rejected for that reason
     alone.

     BUILD BACKWARDS, from the end of the buffer towards the front.
     Correct, and every function has to be read backwards too.

     WRITE THE CONTENTS, THEN MOVE THEM. der_seq_end() computes the
     real length, encodes the header, and memmoves the contents up to
     make room. That is what this does. The move is a few hundred
     bytes a handful of times per certificate, and the API reads in
     the order the document does.

   ── ERRORS ──────────────────────────────────────────────────────────

   Every call checks and the context remembers. Once a write has
   failed, every later call is a no-op and der_ok() stays 0 -- so a
   caller may write a whole document and check once at the end, and
   CANNOT get a short document that looks complete. A truncated
   certificate that still parses is the failure this arrangement
   exists to make impossible.
   ════════════════════════════════════════════════════════════════════ */

/* Universal class tags, as they appear on the wire. */
#define DER_BOOLEAN       0x01
#define DER_INTEGER       0x02
#define DER_BIT_STRING    0x03
#define DER_OCTET_STRING  0x04
#define DER_NULL          0x05
#define DER_OID           0x06
#define DER_UTF8STRING    0x0C
#define DER_PRINTABLE     0x13
#define DER_UTCTIME       0x17
#define DER_SEQUENCE      0x30     /* constructed */
#define DER_SET           0x31     /* constructed */

/* Context-specific constructed tag [n], which X.509 v3 uses for the
   version wrapper and the extensions block. */
#define DER_CTX(n)        (0xA0 | ((n) & 0x1F))
/* Context-specific primitive [n], for the SubjectAltName members. */
#define DER_CTX_PRIM(n)   (0x80 | ((n) & 0x1F))

typedef struct {
    uint8_t  *buf;
    uint32_t  cap;
    uint32_t  len;
    int       ok;        /* 0 once anything has failed; never recovers */
} der_t;

/* Point a writer at a buffer. */
void der_init(der_t *d, uint8_t *buf, uint32_t cap);

/* Has everything so far succeeded? The only thing a caller must
   check, and it must be checked before the buffer is used. */
int  der_ok(const der_t *d);

/* Bytes written so far. Meaningless unless der_ok(). */
uint32_t der_len(const der_t *d);

/* ── primitives ──────────────────────────────────────────────────── */

/* A complete TLV with `len` bytes of value copied from `val`. */
void der_tlv(der_t *d, uint8_t tag, const uint8_t *val, uint32_t len);

/* INTEGER from an unsigned big-endian byte string. Leading zeroes are
   stripped and a 0x00 is prepended when the top bit is set, which is
   what makes it a positive signed integer. Zero encodes as 02 01 00. */
void der_integer(der_t *d, const uint8_t *be, uint32_t len);

/* INTEGER from a bignum, which is how a modulus and an exponent get
   into a certificate. */
void der_integer_bn(der_t *d, const bn_t *a);

/* INTEGER from a small value, for version numbers and exponents. */
void der_integer_u32(der_t *d, uint32_t v);

/* BIT STRING with no unused trailing bits, which is every use in
   X.509 except the KeyUsage extension. The leading octet counting
   unused bits is written for you. */
void der_bitstring(der_t *d, const uint8_t *val, uint32_t len);

/* OBJECT IDENTIFIER from its arcs. The first two are combined as
   40*a+b, the rest are base-128 with the continuation bit set on all
   but the last byte. */
void der_oid(der_t *d, const uint32_t *arcs, uint32_t n);

void der_null(der_t *d);
void der_bool(der_t *d, int v);
void der_string(der_t *d, uint8_t tag, const char *s);

/* Raw bytes, already encoded elsewhere -- for splicing one document
   into another. */
void der_raw(der_t *d, const uint8_t *p, uint32_t len);

/* ── constructed ─────────────────────────────────────────────────── */

/* Open a SEQUENCE, SET or context-specific constructed type. Returns
   a marker to hand back to der_seq_end(). Nesting is fine.

   The marker carries BOTH the offset and the tag, because end() needs
   the tag and this is where it is given. The alternative -- naming
   the tag at both ends -- lets a SEQUENCE be opened and a SET closed,
   with the length still correct, producing a document that parses as
   something nobody wrote. */
uint32_t der_seq_begin(der_t *d, uint8_t tag);

/* Close it: the header is encoded and the contents moved to fit. */
void der_seq_end(der_t *d, uint32_t marker);
