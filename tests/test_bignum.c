/* ════════════════════════════════════════════════════════════════════
   test_bignum.c -- net/bignum.c against CPython and OpenSSL.

   THIS IS THE SUITE THAT MATTERS MOST IN THE RELEASE, and the one
   where a green first run would mean the least.

   A bignum suite written from the same understanding as the bignum it
   tests is worth almost nothing: both sides are the same arithmetic,
   and a carry handled wrongly in one is handled wrongly in the other.
   tests/test_usbmux.c passed 79 of 79 against a sixteen-byte header
   the phone has never used, for exactly that reason. RSA is that
   hazard in its purest form, because wrong arithmetic produces
   numbers that look exactly as random as right arithmetic.

   So every expected value here comes from tests/bignum_vectors.inc,
   which tools/make_bignum_vectors.py generates from

     CPython's integers -- an independent arbitrary-precision
     implementation nobody on this project wrote, and

     OpenSSL -- `openssl genrsa` produces the 2048-bit key at the
     bottom, so n, e, d, p and q are a real key from the library the
     rest of the world uses, not three primes a script multiplied
     together.

   A pass means the kernel's arithmetic agrees with CPython and with
   OpenSSL. All three could still be wrong in the same way, and that
   is not a risk worth more effort than this.

   ── WHAT THE GENERATOR ALREADY CAUGHT ───────────────────────────────

   It asserted d * e == 1 mod phi(n) and OpenSSL failed it. RFC 8017
   defines d against lambda(n) = lcm(p-1, q-1), and phi is a multiple
   of lambda -- so e^-1 mod phi is ALSO a working private exponent,
   decryption succeeds either way, and it is a different number from
   the one in the key file. Checking bn_modinv against phi would have
   failed against a perfectly correct extended Euclid, and the hunt
   would have gone into the kernel. BN_RSA_lam is the one to use.

   ── THE CHECKS THAT EXIST BECAUSE OF HOW THIS CODE FAILS ────────────

   the limb boundary     0xFFFFFFFF + 1. An addition that forgets the
                         carry and one that applies it twice both
                         produce numbers.
   squaring in place     bn_mul(&x, &x, &x). Accumulating into the
                         destination while still reading it is wrong
                         in a way that gives ALMOST the right answer,
                         and every aliasing case here is checked
                         because the header promises aliasing works.
   the normalised form   n is the limb count with no leading zero limb
                         above it. Two bn_t holding the same value
                         with different n compare unequal in any
                         implementation that trusts n, so every result
                         is checked for a zero top limb.
   Montgomery's domain   an even modulus must be REFUSED, not silently
                         wrong: mont_n0inv does not exist for one.
   the 2048-bit exponent the operation the old byte-wise bn_powmod
                         took 18.6 seconds over. If this is wrong,
                         TLS still completes a handshake and the
                         server rejects it, which looks like a network
                         fault.

   ── AND THE ONE THING A NATIVE SUITE CANNOT SEE ─────────────────────

   64-bit DIVISION. It is free here and it is a link error in the
   kernel, because __udivmoddi4 lives in libgcc and libgcc is not
   linked. fs/exfat.c hit that in this same release and `make` caught
   it, not a suite. tools/check_bignum_suite.py greps for it; this
   file cannot.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../include/bignum.h"
#include "bignum_vectors.inc"

static int total = 0, fails = 0;

static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){
        fails++;
        printf("  FAIL  %s   (got %ld)\n", what, got);
    }
}

/* ── the two helpers every check below goes through ──────────────────
   A bn_t compared against a byte string, and a bn_t checked for the
   normalised form. Keeping both in one place is what makes it cheap
   to assert the invariant on EVERY result rather than on the ones
   that came to mind. */

static int same(const bn_t *a, const unsigned char *p, unsigned len)
{
    uint8_t buf[BN_BITS / 8];
    unsigned i, skip = 0;

    if(!bn_to_bytes(a, buf, sizeof buf)) return 0;
    /* The vectors are minimal-length big-endian; buf is left-padded. */
    while(skip < sizeof buf - len && buf[skip] == 0) skip++;
    if(sizeof buf - skip != len){
        /* A shorter vector than the value is a mismatch, but a vector
           with a leading zero byte (hx() emits "{ 0 }" for zero) is
           not. */
        if(!(len == 1 && p[0] == 0)) return 0;
        for(i = 0; i < sizeof buf; i++) if(buf[i]) return 0;
        return 1;
    }
    for(i = 0; i < len; i++) if(buf[skip + i] != p[i]) return 0;
    return 1;
}

/* The normalised form, asserted on every single result.
   n == 0 for zero, and v[n-1] != 0 otherwise. An implementation that
   leaves a zero top limb behind still prints the right number and
   still compares wrong against a value built another way. */
static int normal(const bn_t *a)
{
    uint32_t i;
    if(a->n > BN_LIMBS) return 0;
    if(a->n == 0) return 1;
    if(a->v[a->n - 1] == 0) return 0;
    for(i = a->n; i < BN_LIMBS; i++) if(a->v[i]) return 0;
    return 1;
}

static void load(bn_t *a, const unsigned char *p, unsigned len)
{
    if(!bn_from_bytes(a, p, len)){
        printf("  FAIL  bn_from_bytes refused a %u-byte vector\n", len);
        fails++; total++;
        bn_zero(a);
    }
}

int main(void)
{
    bn_t a, b, r, m, e, q, rem, t;
    int i;

    printf("test_bignum: net/bignum.c against CPython and OpenSSL\n\n");

    /* ── the building blocks, before any arithmetic ──────────────── */
    printf("bytes in and out\n");
    {
        static const unsigned char z[] = { 0 };
        static const unsigned char one[] = { 0x01 };
        static const unsigned char lead[] = { 0x00, 0x00, 0x01, 0x00 };
        uint8_t out[4];

        bn_zero(&a);
        ck("zero has n == 0", a.n == 0, (long)a.n);
        ck("  ... and is normalised", normal(&a), 0);
        ck("  ... and bn_is_zero agrees", bn_is_zero(&a) == 1, 0);
        ck("  ... and has 0 bits", bn_bits(&a) == 0, (long)bn_bits(&a));
        ck("  ... and is not odd", bn_is_odd(&a) == 0, 0);
        ck("  ... and round-trips", same(&a, z, 1), 0);

        bn_set_u32(&a, 1);
        ck("one is odd", bn_is_odd(&a) == 1, 0);
        ck("  ... has 1 bit", bn_bits(&a) == 1, (long)bn_bits(&a));
        ck("  ... and round-trips", same(&a, one, 1), 0);

        /* LEADING ZERO BYTES must not survive into the limb count.
           A modulus read off a wire format is left-padded to its
           length, and a bn_t that remembers the padding compares
           unequal to the same number built from a minimal encoding --
           which is how a certificate verifies on one path and not on
           another. */
        load(&a, lead, sizeof lead);
        ck("leading zero bytes do not inflate the limb count",
           a.n == 1, (long)a.n);
        ck("  ... the value is 0x100", a.v[0] == 0x100, (long)a.v[0]);
        ck("  ... and it is normalised", normal(&a), 0);

        /* bn_to_bytes into too small a buffer must REFUSE. A silently
           truncated modulus is the worst failure this file can
           produce: it is still a number and it still encrypts. */
        bn_set_u32(&a, 0x11223344);
        ck("bn_to_bytes fills exactly the width asked for",
           bn_to_bytes(&a, out, 4) == 1, 0);
        ck("  ... big-endian", out[0] == 0x11 && out[3] == 0x44,
           (long)out[0]);
        ck("bn_to_bytes refuses a buffer that cannot hold the value",
           bn_to_bytes(&a, out, 3) == 0, 0);

        bn_set_u32(&a, 0x00000044);
        ck("  ... and left-pads a short value into a wide buffer",
           bn_to_bytes(&a, out, 4) == 1 && out[0] == 0 && out[3] == 0x44,
           (long)out[0]);
    }

    /* ── bit access and comparison ───────────────────────────────── */
    printf("bits and comparison\n");
    {
        bn_set_u32(&a, 0x80000000u);
        ck("bit 31 of 0x80000000", bn_bit(&a, 31) == 1, 0);
        ck("bit 30 of 0x80000000", bn_bit(&a, 30) == 0, 0);
        ck("  ... 32 significant bits", bn_bits(&a) == 32,
           (long)bn_bits(&a));
        ck("a bit above the value reads 0, it does not read memory",
           bn_bit(&a, BN_BITS - 1) == 0, 0);

        bn_set_u32(&a, 5);
        bn_set_u32(&b, 7);
        ck("5 < 7", bn_cmp(&a, &b) == -1, bn_cmp(&a, &b));
        ck("7 > 5", bn_cmp(&b, &a) == 1, bn_cmp(&b, &a));
        ck("5 == 5", bn_cmp(&a, &a) == 0, bn_cmp(&a, &a));

        /* COMPARISON ACROSS LIMB COUNTS. The one that goes wrong when
           cmp walks limbs from v[0] up instead of from the top down. */
        load(&a, BN_RSA_n, sizeof BN_RSA_n);
        bn_set_u32(&b, 1);
        ck("a 2048-bit number is greater than 1",
           bn_cmp(&a, &b) == 1, bn_cmp(&a, &b));
        ck("  ... and 1 is less than it",
           bn_cmp(&b, &a) == -1, bn_cmp(&b, &a));
    }

    /* ── addition ────────────────────────────────────────────────── */
    printf("addition  (%d vectors from CPython)\n", BN_ADD_N);
    for(i = 0; i < BN_ADD_N; i++){
        const bnvec_t *v = &BN_ADD[i];
        load(&a, v->a, v->a_len);
        load(&b, v->b, v->b_len);
        ck(v->what, bn_add(&r, &a, &b) == 1, 0);
        ck("  ... matches CPython", same(&r, v->r, v->r_len), i);
        ck("  ... normalised", normal(&r), i);

        /* commutativity, free, and it catches an asymmetric carry */
        ck("  ... b + a is the same", bn_add(&t, &b, &a) == 1
           && bn_cmp(&t, &r) == 0, i);

        /* ALIASING: the header promises r may be a or b. */
        bn_copy(&t, &a);
        ck("  ... in place (r == a)", bn_add(&t, &t, &b) == 1
           && bn_cmp(&t, &r) == 0, i);
        bn_copy(&t, &b);
        ck("  ... in place (r == b)", bn_add(&t, &a, &t) == 1
           && bn_cmp(&t, &r) == 0, i);
    }

    /* ── subtraction ─────────────────────────────────────────────── */
    printf("subtraction  (%d vectors)\n", BN_SUB_N);
    for(i = 0; i < BN_SUB_N; i++){
        const bnvec_t *v = &BN_SUB[i];
        load(&a, v->a, v->a_len);
        load(&b, v->b, v->b_len);
        ck(v->what, bn_sub(&r, &a, &b) == 1, 0);
        ck("  ... matches CPython", same(&r, v->r, v->r_len), i);
        ck("  ... normalised", normal(&r), i);
        bn_copy(&t, &a);
        ck("  ... in place", bn_sub(&t, &t, &b) == 1
           && bn_cmp(&t, &r) == 0, i);
    }
    {
        /* A NEGATIVE RESULT IS A NAMED REFUSAL, not arithmetic that
           went below zero. bn_t is unsigned; a borrow off the top
           limb is 2^4096 - 1, which is a plausible number and the
           root of the kind of defect that shows up as a handshake
           failure three layers away. */
        bn_set_u32(&a, 5);
        bn_set_u32(&b, 7);
        bn_set_u32(&r, 0xDEADBEEF);
        ck("5 - 7 refuses", bn_sub(&r, &a, &b) == 0, 0);
        ck("  ... and leaves r alone rather than wrapping",
           r.n == 1 && r.v[0] == 0xDEADBEEF, (long)r.v[0]);

        bn_set_u32(&a, 7);
        ck("7 - 7 is zero", bn_sub(&r, &a, &a) == 1 && bn_is_zero(&r), 0);
        ck("  ... with n == 0, not n == 1 holding a zero limb",
           r.n == 0, (long)r.n);
    }

    /* ── multiplication, and squaring on its own ─────────────────── */
    printf("multiplication  (%d vectors)\n", BN_MUL_N);
    for(i = 0; i < BN_MUL_N; i++){
        const bnvec_t *v = &BN_MUL[i];
        load(&a, v->a, v->a_len);
        load(&b, v->b, v->b_len);
        ck(v->what, bn_mul(&r, &a, &b) == 1, 0);
        ck("  ... matches CPython", same(&r, v->r, v->r_len), i);
        ck("  ... normalised", normal(&r), i);
        ck("  ... b * a is the same", bn_mul(&t, &b, &a) == 1
           && bn_cmp(&t, &r) == 0, i);
        bn_copy(&t, &a);
        ck("  ... in place (r == a)", bn_mul(&t, &t, &b) == 1
           && bn_cmp(&t, &r) == 0, i);
    }

    printf("squaring, which is the aliasing case  (%d vectors)\n",
           BN_SQ_N);
    for(i = 0; i < BN_SQ_N; i++){
        const bnvec_t *v = &BN_SQ[i];
        load(&a, v->a, v->a_len);
        /* THE ONE THAT MATTERS: all three arguments the same object.
           An implementation that accumulates into r while reading a
           gives a result that is close enough to look like a number. */
        bn_copy(&t, &a);
        ck(v->what, bn_mul(&t, &t, &t) == 1, 0);
        ck("  ... matches CPython", same(&t, v->r, v->r_len), i);
        ck("  ... normalised", normal(&t), i);
        /* and the same value computed without aliasing */
        bn_copy(&b, &a);
        ck("  ... same as x * x with separate operands",
           bn_mul(&r, &a, &b) == 1 && bn_cmp(&r, &t) == 0, i);
    }

    /* ── shifts ──────────────────────────────────────────────────── */
    printf("shifts\n");
    {
        bn_set_u32(&a, 1);
        ck("1 << 0 is 1", bn_shl(&r, &a, 0) == 1 && r.n == 1
           && r.v[0] == 1, (long)r.v[0]);
        ck("1 << 31", bn_shl(&r, &a, 31) == 1 && r.n == 1
           && r.v[0] == 0x80000000u, (long)r.v[0]);
        /* A WHOLE-LIMB SHIFT. 32 is the boundary where a naive
           implementation does v >> (32 - 0), which is undefined
           behaviour and on x86 is a shift by 32 that returns v. */
        ck("1 << 32 crosses a whole limb", bn_shl(&r, &a, 32) == 1
           && r.n == 2 && r.v[0] == 0 && r.v[1] == 1, (long)r.n);
        ck("1 << 33", bn_shl(&r, &a, 33) == 1 && r.n == 2
           && r.v[1] == 2, (long)r.v[1]);
        ck("  ... has 34 significant bits", bn_bits(&r) == 34,
           (long)bn_bits(&r));
        ck("1 << 64", bn_shl(&r, &a, 64) == 1 && r.n == 3
           && r.v[2] == 1, (long)r.n);

        ck("a shift off the top refuses rather than truncating",
           bn_shl(&r, &a, BN_BITS) == 0, 0);

        load(&a, BN_RSA_n, sizeof BN_RSA_n);
        ck("shifting a 2048-bit value left 64 and back is identity",
           bn_shl(&r, &a, 64) == 1 && bn_shr(&r, &r, 64) == 1
           && bn_cmp(&r, &a) == 0, 0);
        ck("  ... and the round trip is normalised", normal(&r), 0);

        bn_set_u32(&a, 1);
        ck("1 >> 1 is zero", bn_shr(&r, &a, 1) == 1 && bn_is_zero(&r),
           (long)r.n);
        ck("  ... with n == 0", r.n == 0, (long)r.n);
        ck("a shift right past the top is zero, not a refusal",
           bn_shr(&r, &a, BN_BITS) == 1 && bn_is_zero(&r), 0);
    }

    /* ── division ────────────────────────────────────────────────── */
    printf("division  (%d vectors, quotient and remainder)\n", BN_DIV_N);
    for(i = 0; i < BN_DIV_N; i++){
        const bnvec_t *dv = &BN_DIV[i];
        const bnvec_t *rv = &BN_REM[i];
        load(&a, dv->a, dv->a_len);
        load(&m, dv->b, dv->b_len);
        ck(dv->what, bn_divmod(&a, &m, &q, &rem) == 1, 0);
        ck("  ... quotient matches CPython", same(&q, dv->r, dv->r_len), i);
        ck("  ... remainder matches CPython", same(&rem, rv->r, rv->r_len), i);
        ck("  ... quotient normalised", normal(&q), i);
        ck("  ... remainder normalised", normal(&rem), i);
        /* The remainder is less than the divisor. Holds for every
           pair, independent of the vectors, so it catches a bug the
           vectors happened to miss. */
        ck("  ... remainder < divisor", bn_cmp(&rem, &m) == -1, i);
        /* q*m + rem == a, which is the whole of division in one line */
        ck("  ... q * m + rem == a", bn_mul(&t, &q, &m) == 1
           && bn_add(&t, &t, &rem) == 1 && bn_cmp(&t, &a) == 0, i);
        /* bn_mod alone must agree with bn_divmod's remainder */
        ck("  ... bn_mod agrees", bn_mod(&r, &a, &m) == 1
           && bn_cmp(&r, &rem) == 0, i);
        /* either output may be null */
        ck("  ... quotient only", bn_divmod(&a, &m, &t, 0) == 1
           && bn_cmp(&t, &q) == 0, i);
        ck("  ... remainder only", bn_divmod(&a, &m, 0, &t) == 1
           && bn_cmp(&t, &rem) == 0, i);
    }
    {
        bn_set_u32(&a, 100);
        bn_zero(&m);
        ck("division by zero refuses", bn_divmod(&a, &m, &q, &rem) == 0, 0);
        ck("  ... and bn_mod by zero refuses too",
           bn_mod(&r, &a, &m) == 0, 0);
    }

    /* ── modular add and subtract ────────────────────────────────── */
    printf("modular addition and subtraction\n");
    {
        bn_set_u32(&m, 97);
        bn_set_u32(&a, 90);
        bn_set_u32(&b, 20);
        ck("(90 + 20) mod 97 == 13", bn_addmod(&r, &a, &b, &m) == 1
           && r.n == 1 && r.v[0] == 13, (long)(r.n ? r.v[0] : 0));
        ck("(20 - 90) mod 97 == 27", bn_submod(&r, &b, &a, &m) == 1
           && r.n == 1 && r.v[0] == 27, (long)(r.n ? r.v[0] : 0));
        ck("  ... which is a WRAP, not a borrow off the top limb",
           r.n == 1, (long)r.n);
        ck("(90 - 20) mod 97 == 70", bn_submod(&r, &a, &b, &m) == 1
           && r.n == 1 && r.v[0] == 70, (long)(r.n ? r.v[0] : 0));
        ck("(x - x) mod m is zero with n == 0",
           bn_submod(&r, &a, &a, &m) == 1 && r.n == 0, (long)r.n);

        /* The 2048-bit case, where addmod's intermediate sum exceeds
           the modulus by one limb and the reduction is a subtract. */
        load(&m, BN_RSA_n, sizeof BN_RSA_n);
        load(&a, BN_RSA_ct, sizeof BN_RSA_ct);
        bn_mod(&a, &a, &m);
        ck("addmod on a 2048-bit modulus stays reduced",
           bn_addmod(&r, &a, &a, &m) == 1 && bn_cmp(&r, &m) == -1, 0);
        ck("  ... and equals 2a mod n the long way",
           bn_add(&t, &a, &a) == 1 && bn_mod(&t, &t, &m) == 1
           && bn_cmp(&t, &r) == 0, 0);
        ck("  ... and is normalised", normal(&r), 0);
    }

    /* ── mulmod ──────────────────────────────────────────────────── */
    printf("mulmod\n");
    {
        load(&m, BN_RSA_n, sizeof BN_RSA_n);
        load(&a, BN_RSA_ct, sizeof BN_RSA_ct);
        load(&b, BN_RSA_msg, sizeof BN_RSA_msg);
        bn_mod(&a, &a, &m);
        bn_mod(&b, &b, &m);
        ck("mulmod agrees with multiply-then-divide",
           bn_mulmod(&r, &a, &b, &m) == 1
           && bn_mul(&t, &a, &b) == 1 && bn_mod(&t, &t, &m) == 1
           && bn_cmp(&t, &r) == 0, 0);
        ck("  ... and is normalised", normal(&r), 0);
        ck("  ... and reduced", bn_cmp(&r, &m) == -1, 0);
        bn_copy(&t, &a);
        ck("mulmod in place", bn_mulmod(&t, &t, &b, &m) == 1
           && bn_cmp(&t, &r) == 0, 0);
    }

    /* ── modexp: the operation the whole file exists for ─────────── */
    printf("modexp  (%d vectors from CPython's pow)\n", BN_EXP_N);
    for(i = 0; i < BN_EXP_N; i++){
        const bnexp_t *v = &BN_EXP[i];
        load(&a, v->b, v->b_len);
        load(&e, v->e, v->e_len);
        load(&m, v->m, v->m_len);
        ck(v->what, bn_modexp(&r, &a, &e, &m) == 1, 0);
        ck("  ... matches CPython's pow()", same(&r, v->r, v->r_len), i);
        ck("  ... normalised", normal(&r), i);
        ck("  ... reduced mod m", bn_is_zero(&m) || bn_cmp(&r, &m) == -1, i);
    }
    {
        /* MONTGOMERY'S DOMAIN. mont_n0inv() is the inverse of -m mod
           2^32 and does not exist for an even m. The header promises
           a refusal; a silent wrong answer here is an RSA that
           produces ciphertext nobody can decrypt. */
        bn_set_u32(&a, 3);
        bn_set_u32(&e, 5);
        bn_set_u32(&m, 8);
        bn_set_u32(&r, 0xC0FFEE);
        ck("modexp refuses an even modulus",
           bn_modexp(&r, &a, &e, &m) == 0, 0);
        ck("  ... and leaves r untouched rather than guessing",
           r.n == 1 && r.v[0] == 0xC0FFEE, (long)r.v[0]);
        bn_zero(&m);
        ck("modexp refuses a zero modulus",
           bn_modexp(&r, &a, &e, &m) == 0, 0);
    }

    /* ── the modular inverse ─────────────────────────────────────── */
    printf("modular inverse  (%d vectors)\n", BN_INV_N);
    for(i = 0; i < BN_INV_N; i++){
        const bninv_t *v = &BN_INV[i];
        load(&a, v->a, v->a_len);
        load(&m, v->m, v->m_len);
        if(v->exists){
            ck(v->what, bn_modinv(&r, &a, &m) == 1, 0);
            ck("  ... matches CPython's pow(a,-1,m)",
               same(&r, v->r, v->r_len), i);
            ck("  ... normalised", normal(&r), i);
            /* THE PROPERTY, not the vector: a * r mod m == 1. This is
               what an inverse IS, it holds whatever CPython said, and
               it is what caught the sign: the division-form modinv
               returned m minus the right answer on every input, and
               the negation fails this check just as loudly as garbage
               would.

               MODULUS 1 IS THE EXCEPTION, and it is a real one rather
               than an excuse: every residue mod 1 is 0, so the
               inverse is 0 and a * 0 mod 1 is 0, not 1. This check
               asserted 1 unconditionally and the m == 1 vector failed
               it -- the suite being wrong, not the kernel. Asserted
               explicitly here so that nobody later "fixes" bn_modinv
               to return 1 for it and disagrees with bn_modexp, which
               special-cases m == 1 the same way. */
            if(m.n == 1 && m.v[0] == 1){
                ck("  ... modulus 1: the inverse is 0, and a * 0 mod 1 is 0",
                   bn_is_zero(&r) && bn_mulmod(&t, &a, &r, &m) == 1
                   && bn_is_zero(&t), i);
            } else {
                ck("  ... a * r mod m == 1", bn_mulmod(&t, &a, &r, &m) == 1
                   && t.n == 1 && t.v[0] == 1, i);
            }
        } else {
            /* NOT AN ERROR -- the answer is "gcd(a,m) is not 1", and a
               keygen loop uses it to reject a candidate. */
            ck(v->what, bn_modinv(&r, &a, &m) == 0, 0);
        }
    }

    printf("gcd\n");
    {
        bn_set_u32(&a, 12);
        bn_set_u32(&b, 18);
        ck("gcd(12, 18) == 6", bn_gcd(&r, &a, &b) == 1 && r.n == 1
           && r.v[0] == 6, (long)(r.n ? r.v[0] : 0));
        ck("gcd(18, 12) == 6", bn_gcd(&r, &b, &a) == 1 && r.n == 1
           && r.v[0] == 6, (long)(r.n ? r.v[0] : 0));
        bn_zero(&b);
        ck("gcd(12, 0) == 12", bn_gcd(&r, &a, &b) == 1 && r.n == 1
           && r.v[0] == 12, (long)(r.n ? r.v[0] : 0));
        ck("gcd(0, 12) == 12", bn_gcd(&r, &b, &a) == 1 && r.n == 1
           && r.v[0] == 12, (long)(r.n ? r.v[0] : 0));
        /* An EVEN/EVEN pair, which is the branch Stein's algorithm
           has that Euclid's does not. */
        bn_set_u32(&a, 1 << 20);
        bn_set_u32(&b, 1 << 14);
        ck("gcd of two powers of two is the smaller",
           bn_gcd(&r, &a, &b) == 1 && r.n == 1 && r.v[0] == (1u << 14),
           (long)(r.n ? r.v[0] : 0));
        /* Coprime 2048-bit material: gcd(e, lambda) must be 1, which
           is exactly what makes the key's d exist. */
        load(&a, BN_RSA_e, sizeof BN_RSA_e);
        load(&b, BN_RSA_lam, sizeof BN_RSA_lam);
        ck("gcd(e, lambda) == 1 for a real key",
           bn_gcd(&r, &a, &b) == 1 && r.n == 1 && r.v[0] == 1,
           (long)(r.n ? r.v[0] : 0));
    }

    /* ── a real RSA-2048 key, from OpenSSL ───────────────────────── */
    printf("RSA-2048, on a key from `openssl genrsa`\n");
    {
        bn_t n_, p_, q_, lam_, d_, e_, msg_, ct_;

        load(&n_,   BN_RSA_n,   sizeof BN_RSA_n);
        load(&e_,   BN_RSA_e,   sizeof BN_RSA_e);
        load(&d_,   BN_RSA_d,   sizeof BN_RSA_d);
        load(&p_,   BN_RSA_p,   sizeof BN_RSA_p);
        load(&q_,   BN_RSA_q,   sizeof BN_RSA_q);
        load(&lam_, BN_RSA_lam, sizeof BN_RSA_lam);
        load(&msg_, BN_RSA_msg, sizeof BN_RSA_msg);
        load(&ct_,  BN_RSA_ct,  sizeof BN_RSA_ct);

        ck("the modulus is 2048 bits",
           bn_bits(&n_) == BN_RSA_BITS, (long)bn_bits(&n_));
        ck("p * q == n", bn_mul(&t, &p_, &q_) == 1
           && bn_cmp(&t, &n_) == 0, 0);
        ck("  ... and n is odd, which is what Montgomery needs",
           bn_is_odd(&n_) == 1, 0);

        /* d = e^-1 mod lambda. lambda, NOT phi -- see the header. */
        ck("d * e mod lambda == 1", bn_mulmod(&t, &d_, &e_, &lam_) == 1
           && t.n == 1 && t.v[0] == 1, (long)(t.n ? t.v[0] : 0));
        ck("and bn_modinv recomputes OpenSSL's own d",
           bn_modinv(&r, &e_, &lam_) == 1 && bn_cmp(&r, &d_) == 0, 0);

        /* THE ROUND TRIP. The public operation is 17 bits of
           exponent; the private one is 2048, and is the operation the
           byte-wise bn_powmod took 18.6 seconds over. */
        ck("m^e mod n is OpenSSL's ciphertext",
           bn_modexp(&r, &msg_, &e_, &n_) == 1
           && bn_cmp(&r, &ct_) == 0, 0);
        ck("c^d mod n is the message again",
           bn_modexp(&t, &ct_, &d_, &n_) == 1
           && bn_cmp(&t, &msg_) == 0, 0);
        ck("  ... and the recovered message is normalised",
           normal(&t), 0);

        /* The signing direction: m^d then ^e. RSA is symmetric here
           and PKCS#1 signing is this way round, so it is the
           operation stage 3 will actually call. */
        ck("m^d mod n then ^e mod n is the message (the signing way)",
           bn_modexp(&r, &msg_, &d_, &n_) == 1
           && bn_modexp(&t, &r, &e_, &n_) == 1
           && bn_cmp(&t, &msg_) == 0, 0);
    }

    /* ── THE BYTE LAYOUT net/tls_crypto.c ACTUALLY USES ──────────────
       rsa_public_encrypt() moved onto this file in v0.8.8.1. The
       arithmetic above is checked against OpenSSL, but the WIRING is
       not: that function keeps its PKCS#1 block right-aligned in a
       512-byte buffer and hands bn_from_bytes a pointer part-way into
       it, then writes the result back left-padded to the modulus
       width.

       Every one of those is an offset, and an offset is the kind of
       thing that is correct by reasoning and wrong in the tree. So
       this repeats the exact layout -- right-aligned load from a
       512-byte buffer, exponentiate, fixed-width store -- and
       compares against OpenSSL's ciphertext. */
    printf("the byte layout net/tls_crypto.c hands this file\n");
    {
        enum { RSA_MAX_BYTES = 512 };
        static uint8_t em[RSA_MAX_BYTES];
        static uint8_t outb[RSA_MAX_BYTES];
        unsigned mod_bytes = (unsigned)sizeof BN_RSA_n;
        bn_t M, E, EM, R;

        ck("the key's modulus is a whole number of bytes",
           mod_bytes == BN_RSA_BITS / 8, (long)mod_bytes);

        /* right-aligned, exactly as rsa_public_encrypt() builds it */
        memset(em, 0, sizeof em);
        memcpy(em + RSA_MAX_BYTES - sizeof BN_RSA_msg,
               BN_RSA_msg, sizeof BN_RSA_msg);

        ck("the modulus loads from a bare pointer",
           bn_from_bytes(&M, BN_RSA_n, mod_bytes) == 1, 0);
        ck("the exponent loads from its own length",
           bn_from_bytes(&E, BN_RSA_e, (unsigned)sizeof BN_RSA_e) == 1, 0);
        ck("the block loads from part-way into the buffer",
           bn_from_bytes(&EM, em + RSA_MAX_BYTES - mod_bytes,
                         mod_bytes) == 1, 0);
        /* THE OFFSET ITSELF. Loading from the wrong place in the
           buffer gives a number either way, so this asserts that
           what came out of the right-aligned buffer is the same
           value as the message loaded straight from its own bytes.
           Written first as `bn_cmp(&EM, &a) != 0 || 1`, which passes
           unconditionally -- a check that cannot fail is worse than
           no check, because it reads as coverage. */
        ck("  ... and the value loaded is the message itself",
           bn_from_bytes(&t, BN_RSA_msg, (unsigned)sizeof BN_RSA_msg) == 1
           && bn_cmp(&EM, &t) == 0, 0);

        ck("the exponentiation runs", bn_modexp(&R, &EM, &E, &M) == 1, 0);
        ck("the result stores at the modulus width",
           bn_to_bytes(&R, outb, mod_bytes) == 1, 0);
        ck("  ... and is OpenSSL's ciphertext, byte for byte",
           memcmp(outb, BN_RSA_ct,
                  mod_bytes < sizeof BN_RSA_ct ? mod_bytes
                                               : sizeof BN_RSA_ct) == 0
           && mod_bytes >= sizeof BN_RSA_ct, 0);

        /* And the leading zero the ciphertext may have: bn_to_bytes
           left-pads, and a caller that copies mod_bytes out of a
           shorter buffer would drop a byte off the front. */
        ck("  ... left-padded, so a short ciphertext still fills the width",
           mod_bytes == BN_RSA_BITS / 8, (long)mod_bytes);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
