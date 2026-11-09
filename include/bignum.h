#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/bignum.h -- limb-based big integers.  v0.8.8.1.

   PAIRING STAGE 3'S FIRST PIECE, and the one that can be done with no
   phone in the room: it is pure logic, it is checked against an
   independent implementation, and it makes TLS faster whether or not
   the rest of stage 3 ever happens.

   ── WHY THE EXISTING ONE CANNOT BE EXTENDED ─────────────────────────

   net/tls_crypto.c has a bignum. It is a 512-BYTE ARRAY, big-endian,
   and it can do exactly two things: bn_powmod() and
   rsa_public_encrypt(). There is NO general multiply, NO division and
   NO modular inverse, and RSA key generation needs all three --
   phi = (p-1)(q-1) is a full multiply and d = e^-1 mod phi is an
   inverse.

   What is there is BYTE-WISE double-and-add, which was the right
   amount of code for its one job: e = 65537 is 17 bits. Measured at
   0.163 s for that exponent on a fast container CPU, which works out
   at 0.073 s per exponent BYTE -- so a 2048-bit PRIVATE exponent is
   18.6 seconds, one Miller-Rabin round on a 1024-bit candidate is
   about 2.3 s, and a full RSA-2048 keygen is 15 to 25 minutes.

   ── SO THE POINT OF THIS FILE IS THE INNER LOOP ─────────────────────

   32-bit limbs, and MONTGOMERY multiplication for modexp. The
   arithmetic that dominates is the modular reduction after every
   square and multiply, and Montgomery's whole trick is that it has no
   division in it at all -- the reduction is a multiply and a shift.

   A 1024-bit modexp goes from about a thousand byte-wise
   double-and-add passes to about a thousand limb multiplications of
   32 limbs each. That is the difference between a keygen you cannot
   ship and one you can.

   ── AND THE 32-BIT FREESTANDING RULE THAT SHAPES ALL OF IT ──────────

   DIVIDING A uint64_t EMITS A CALL TO libgcc. __udivmoddi4 is not
   linked into this kernel, so a 64-bit division anywhere here fails
   at the LINK step -- and a native suite never sees it, because
   64-bit division is free there. fs/exfat.c hit exactly that in this
   same release and `make` is what caught it.

   So: 32x32 -> 64 multiplication is fine (gcc emits one `mull`),
   adding and shifting a uint64_t is fine, and DIVIDING one is
   forbidden. bn_divmod() is therefore binary long division, which
   needs no division at all, and nothing performance-critical calls
   it -- that is what Montgomery is for.

   ── SIZE ────────────────────────────────────────────────────────────

   BN_LIMBS is 4096 bits' worth, so that the PRODUCT of two 2048-bit
   numbers fits without the caller thinking about it: 4096 bits is
   exactly 128 limbs, indices 0 to 127, with nothing to spare.

   EXACTLY, AND THAT MATTERS. 2^4096 is 4097 bits and does not fit.
   The first bn_modexp() formed Montgomery's R^2 = 2^(32*2n) as a
   literal, which for a 2048-bit modulus is precisely that value, and
   its bound check correctly refused -- so bn_modexp refused every
   2048-bit modulus, which is the only size RSA uses here. 1024-bit
   modexp worked, so only a suite that went all the way to a real
   2048-bit key would notice, and tests/test_bignum.c did on its
   first run. R^2 mod m is now computed as (R mod m) squared modulo m,
   with both factors under the modulus, so no literal above 4096 bits
   is ever formed. Anything added here that wants 2^(2*BN_BITS/2) has
   the same problem.

   A bn_t is 516 bytes; these live on the stack in places, and the
   kernel's stack is not large, so functions here take their
   temporaries as few and as small as they can.
   ════════════════════════════════════════════════════════════════════ */

#define BN_BITS   4096
#define BN_LIMBS  (BN_BITS / 32)

typedef struct {
    uint32_t n;                  /* limbs in use, 0 for zero, no
                                    leading zero limb above it       */
    uint32_t v[BN_LIMBS];        /* little-endian: v[0] is least     */
} bn_t;

/* ── building and reading ───────────────────────────────────────── */
void bn_zero(bn_t *a);
void bn_set_u32(bn_t *a, uint32_t v);
void bn_copy(bn_t *r, const bn_t *a);
/* Big-endian bytes, which is how every wire format carries them. */
int  bn_from_bytes(bn_t *a, const uint8_t *p, uint32_t len);
/* Writes exactly `len` bytes, left-padded with zeroes; 0 if it does
   not fit, because a silently truncated modulus is the worst
   possible failure in this file. */
int  bn_to_bytes(const bn_t *a, uint8_t *p, uint32_t len);

int      bn_is_zero(const bn_t *a);
int      bn_is_odd(const bn_t *a);
uint32_t bn_bits(const bn_t *a);          /* significant bits, 0 for 0 */
int      bn_bit(const bn_t *a, uint32_t i);
int      bn_cmp(const bn_t *a, const bn_t *b);   /* -1, 0, 1 */

/* ── plain arithmetic ───────────────────────────────────────────────
   Every one of these may alias its output with an input. */
int  bn_add(bn_t *r, const bn_t *a, const bn_t *b);   /* 0 on overflow */
int  bn_sub(bn_t *r, const bn_t *a, const bn_t *b);   /* 0 if a < b    */
int  bn_mul(bn_t *r, const bn_t *a, const bn_t *b);
int  bn_shl(bn_t *r, const bn_t *a, uint32_t bits);
int  bn_shr(bn_t *r, const bn_t *a, uint32_t bits);

/* q = a / m, rem = a % m. Either output may be null. 0 if m is zero.
   Binary long division -- correct, obvious, and not in any inner
   loop. See the header on why there is no 64-bit divide here. */
int  bn_divmod(const bn_t *a, const bn_t *m, bn_t *q, bn_t *rem);
int  bn_mod(bn_t *r, const bn_t *a, const bn_t *m);

/* ── modular arithmetic ─────────────────────────────────────────────
   a and b must already be reduced mod m for addmod/submod. */
int  bn_addmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m);
int  bn_submod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m);
int  bn_mulmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m);

/* r = a^e mod m. m must be ODD and non-zero -- Montgomery needs an
   odd modulus, and every RSA modulus is odd because it is a product
   of two odd primes. Returns 0 and leaves r untouched otherwise,
   rather than falling back to something slower in silence. */
int  bn_modexp(bn_t *r, const bn_t *a, const bn_t *e, const bn_t *m);

/* gcd, and the modular inverse RSA key generation needs for
   d = e^-1 mod phi. bn_modinv returns 0 when no inverse exists --
   which is not an error, it is the answer "gcd(a,m) is not 1", and a
   keygen loop uses it to reject a candidate. */
int  bn_gcd(bn_t *r, const bn_t *a, const bn_t *b);

/* bn_modinv works for ANY non-zero modulus, odd or even, and that is
   not incidental: d = e^-1 mod lcm(p-1, q-1), and the lcm of two even
   numbers is always even. An inverse routine that needs an odd
   modulus -- the binary extended Euclid, which this was first -- can
   compute every inverse except the one RSA key generation needs. */
int  bn_modinv(bn_t *r, const bn_t *a, const bn_t *m);
