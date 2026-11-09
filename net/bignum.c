/* ════════════════════════════════════════════════════════════════════
   net/bignum.c -- limb-based big integers.  v0.8.8.1.

   include/bignum.h says why this exists and what shapes it. The two
   rules that govern every line here:

     1. NO 64-BIT DIVISION. It emits a call to libgcc's __udivmoddi4,
        which is not linked into this kernel, so it fails at the LINK
        step -- and a native suite never sees it, because 64-bit
        division is free there. 32x32 -> 64 multiplication is fine;
        gcc emits one `mull`.

     2. EVERY FUNCTION MAY BE CALLED WITH ITS OUTPUT ALIASING AN
        INPUT. bn_mul(&x, &x, &x) is a thing a caller will write, and
        the one that squares is the one it will write first. Each
        function that cannot work in place says so by using a
        temporary, rather than by documenting a restriction somebody
        will not read.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/bignum.h"
#include "../include/string.h"

/* ── the limb basics ─────────────────────────────────────────────── */

static void norm(bn_t *a)
{
    while(a->n && a->v[a->n - 1] == 0) a->n--;
}

void bn_zero(bn_t *a){ a->n = 0; memset(a->v, 0, sizeof a->v); }

void bn_set_u32(bn_t *a, uint32_t v)
{
    bn_zero(a);
    if(v){ a->v[0] = v; a->n = 1; }
}

void bn_copy(bn_t *r, const bn_t *a)
{
    if(r == a) return;
    r->n = a->n;
    memcpy(r->v, a->v, (uint32_t)a->n * 4u);
    if(a->n < BN_LIMBS)
        memset(r->v + a->n, 0, (BN_LIMBS - a->n) * 4u);
}

int bn_is_zero(const bn_t *a){ return a->n == 0; }
int bn_is_odd(const bn_t *a){ return a->n && (a->v[0] & 1u); }

uint32_t bn_bits(const bn_t *a)
{
    uint32_t top;
    uint32_t b;
    if(!a->n) return 0;
    top = a->v[a->n - 1];
    b = (a->n - 1) * 32u;
    while(top){ b++; top >>= 1; }
    return b;
}

int bn_bit(const bn_t *a, uint32_t i)
{
    uint32_t w = i / 32u;
    if(w >= a->n) return 0;
    return (int)((a->v[w] >> (i % 32u)) & 1u);
}

int bn_cmp(const bn_t *a, const bn_t *b)
{
    uint32_t i;
    if(a->n != b->n) return a->n > b->n ? 1 : -1;
    for(i = a->n; i > 0; i--){
        if(a->v[i - 1] != b->v[i - 1])
            return a->v[i - 1] > b->v[i - 1] ? 1 : -1;
    }
    return 0;
}

int bn_from_bytes(bn_t *a, const uint8_t *p, uint32_t len)
{
    uint32_t i;
    bn_zero(a);
    if(!p) return 0;
    /* Leading zeroes are skipped rather than counted, so a 256-byte
       buffer holding a small number does not claim 64 limbs. */
    while(len && *p == 0){ p++; len--; }
    if(len > BN_LIMBS * 4u) return 0;
    for(i = 0; i < len; i++){
        uint32_t b = p[len - 1 - i];
        a->v[i / 4u] |= b << ((i % 4u) * 8u);
    }
    a->n = (len + 3u) / 4u;
    norm(a);
    return 1;
}

int bn_to_bytes(const bn_t *a, uint8_t *p, uint32_t len)
{
    uint32_t i;
    if(!p) return 0;
    /* A modulus that does not fit is refused rather than truncated.
       A silently shortened key is the one failure in this file that
       would look like it worked. */
    if(bn_bits(a) > len * 8u) return 0;
    for(i = 0; i < len; i++){
        uint32_t idx = len - 1 - i;
        uint32_t w = i / 4u;
        p[idx] = (w < a->n) ? (uint8_t)(a->v[w] >> ((i % 4u) * 8u)) : 0;
    }
    return 1;
}

/* ── add and subtract ────────────────────────────────────────────── */

int bn_add(bn_t *r, const bn_t *a, const bn_t *b)
{
    uint32_t i, n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    if(n > BN_LIMBS) return 0;
    for(i = 0; i < n; i++){
        c += (uint64_t)(i < a->n ? a->v[i] : 0);
        c += (uint64_t)(i < b->n ? b->v[i] : 0);
        r->v[i] = (uint32_t)c;
        c >>= 32;
    }
    if(c){
        if(n >= BN_LIMBS){ r->n = n; norm(r); return 0; }
        r->v[n++] = (uint32_t)c;
    }
    for(i = n; i < BN_LIMBS; i++) r->v[i] = 0;
    r->n = n;
    norm(r);
    return 1;
}

int bn_sub(bn_t *r, const bn_t *a, const bn_t *b)
{
    uint32_t i;
    uint64_t borrow = 0;
    if(bn_cmp(a, b) < 0) return 0;
    for(i = 0; i < a->n; i++){
        uint64_t x = (uint64_t)a->v[i];
        uint64_t y = (uint64_t)(i < b->n ? b->v[i] : 0) + borrow;
        if(x >= y){ r->v[i] = (uint32_t)(x - y); borrow = 0; }
        else      { r->v[i] = (uint32_t)(x + 0x100000000ull - y);
                    borrow = 1; }
    }
    for(i = a->n; i < BN_LIMBS; i++) r->v[i] = 0;
    r->n = a->n;
    norm(r);
    return 1;
}

/* ── multiply ────────────────────────────────────────────────────── */

int bn_mul(bn_t *r, const bn_t *a, const bn_t *b)
{
    /* A temporary, unconditionally. bn_mul(&x,&x,&x) is the first
       thing anybody writes -- squaring -- and accumulating into the
       destination while reading it is wrong in a way that produces
       almost-right answers. */
    static bn_t t;
    uint32_t i, j;

    if(a->n + b->n > BN_LIMBS) return 0;
    bn_zero(&t);
    for(i = 0; i < a->n; i++){
        uint64_t carry = 0;
        uint32_t ai = a->v[i];
        if(!ai) continue;
        for(j = 0; j < b->n; j++){
            /* 32x32 -> 64, plus the running limb, plus the carry.
               The largest value here is (2^32-1)^2 + 2(2^32-1), which
               is under 2^64, so this cannot overflow. */
            uint64_t p = (uint64_t)ai * (uint64_t)b->v[j]
                       + (uint64_t)t.v[i + j] + carry;
            t.v[i + j] = (uint32_t)p;
            carry = p >> 32;
        }
        {
            uint32_t k = i + b->n;
            while(carry){
                uint64_t p = (uint64_t)t.v[k] + carry;
                t.v[k] = (uint32_t)p;
                carry = p >> 32;
                k++;
            }
        }
    }
    t.n = a->n + b->n;
    if(t.n > BN_LIMBS) t.n = BN_LIMBS;
    norm(&t);
    bn_copy(r, &t);
    return 1;
}

/* ── shifts ──────────────────────────────────────────────────────── */

int bn_shl(bn_t *r, const bn_t *a, uint32_t bits)
{
    uint32_t words = bits / 32u, sb = bits % 32u;
    uint32_t i, n;
    static bn_t t;

    if(bn_is_zero(a)){ bn_zero(r); return 1; }
    n = a->n + words + 1u;
    if(n > BN_LIMBS) return 0;
    bn_zero(&t);
    for(i = 0; i < a->n; i++){
        /* Two halves rather than a 64-bit shift by a variable amount:
           a shift of 32 is undefined in C, and `sb` is 0 whenever the
           shift is a whole number of words. */
        t.v[i + words] |= (sb ? (a->v[i] << sb) : a->v[i]);
        if(sb) t.v[i + words + 1] |= (uint32_t)(a->v[i] >> (32u - sb));
    }
    t.n = n;
    norm(&t);
    bn_copy(r, &t);
    return 1;
}

int bn_shr(bn_t *r, const bn_t *a, uint32_t bits)
{
    uint32_t words = bits / 32u, sb = bits % 32u;
    uint32_t i;
    static bn_t t;

    bn_zero(&t);
    if(words >= a->n){ bn_zero(r); return 1; }
    for(i = 0; i + words < a->n; i++){
        uint32_t lo = a->v[i + words];
        uint32_t hi = (i + words + 1 < a->n) ? a->v[i + words + 1] : 0;
        t.v[i] = sb ? ((lo >> sb) | (uint32_t)(hi << (32u - sb))) : lo;
    }
    t.n = a->n - words;
    norm(&t);
    bn_copy(r, &t);
    return 1;
}

/* ── division ────────────────────────────────────────────────────────
 *
 * Binary long division: shift the remainder left one bit, pull in the
 * next bit of the dividend, subtract the divisor if it fits.
 *
 * O(bits) iterations of an O(limbs) subtract, which for 2048 bits over
 * 64 limbs is about 130 000 limb operations. That is slow, and it is
 * deliberately not on any path that matters: bn_modexp() uses
 * Montgomery reduction, which has no division in it. This is for
 * setup, for bn_mod(), and for the places where clarity is worth more
 * than speed.
 *
 * Knuth's algorithm D would be ten times faster and needs a 64/32
 * division for its quotient estimate, which this kernel cannot link.
 * A half-limb version of D is possible and is not worth the risk in
 * the release that introduces the file. */
int bn_divmod(const bn_t *a, const bn_t *m, bn_t *q, bn_t *rem)
{
    static bn_t qq, rr;
    uint32_t i, bits;

    if(bn_is_zero(m)) return 0;
    bn_zero(&qq);
    bn_zero(&rr);
    bits = bn_bits(a);

    for(i = bits; i > 0; i--){
        /* rr = rr*2 + bit(i-1) */
        uint32_t k;
        uint32_t carry = 0;
        for(k = 0; k < rr.n; k++){
            uint32_t nx = (rr.v[k] >> 31) & 1u;
            rr.v[k] = (rr.v[k] << 1) | carry;
            carry = nx;
        }
        if(carry){
            if(rr.n >= BN_LIMBS) return 0;
            rr.v[rr.n++] = 1;
        }
        if(bn_bit(a, i - 1)){
            if(rr.n == 0){ rr.v[0] = 1; rr.n = 1; }
            else rr.v[0] |= 1u;
        }
        norm(&rr);

        if(bn_cmp(&rr, m) >= 0){
            bn_sub(&rr, &rr, m);
            {
                uint32_t w = (i - 1) / 32u;
                if(w >= BN_LIMBS) return 0;
                qq.v[w] |= 1u << ((i - 1) % 32u);
                if(w + 1 > qq.n) qq.n = w + 1;
            }
        }
    }
    norm(&qq);
    norm(&rr);
    if(q)   bn_copy(q, &qq);
    if(rem) bn_copy(rem, &rr);
    return 1;
}

int bn_mod(bn_t *r, const bn_t *a, const bn_t *m)
{
    return bn_divmod(a, m, 0, r);
}

/* ── modular add and subtract ────────────────────────────────────── */

int bn_addmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m)
{
    static bn_t t;
    if(!bn_add(&t, a, b)) return 0;
    if(bn_cmp(&t, m) >= 0) bn_sub(&t, &t, m);
    bn_copy(r, &t);
    return 1;
}

int bn_submod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m)
{
    static bn_t t;
    if(bn_cmp(a, b) >= 0){
        bn_sub(&t, a, b);
    } else {
        /* a - b + m, and a and b are both already reduced, so this
           cannot go negative a second time. */
        bn_sub(&t, b, a);
        bn_sub(&t, m, &t);
    }
    bn_copy(r, &t);
    return 1;
}

int bn_mulmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *m)
{
    static bn_t t;
    if(!bn_mul(&t, a, b)) return 0;
    return bn_mod(r, &t, m);
}

/* ════════════════════════════════════════════════════════════════════
   MONTGOMERY

   The reduction after every square and multiply is what a modexp
   spends its life doing, and doing it with division is what made the
   old byte-wise code 18 seconds per 2048-bit private-key operation.

   Montgomery's trick: work with a*R mod m instead of a, where
   R = 2^(32*n) and n is the modulus's limb count. Then the reduction
   is a multiply by a precomputed constant and a shift -- no division
   anywhere, and a shift by a whole number of limbs is free because it
   is an array index.

   It needs m to be ODD, because the constant is an inverse modulo
   2^32 and an even number has none. Every RSA modulus is odd.
   ════════════════════════════════════════════════════════════════════ */

/* -m^-1 mod 2^32, by Newton's iteration on the 2-adic inverse. Five
   doublings take 2 correct bits to 32. No division. */
static uint32_t mont_n0inv(uint32_t m0)
{
    uint32_t x = 1;
    int i;
    for(i = 0; i < 5; i++) x *= 2u - m0 * x;   /* x = m0^-1 mod 2^32 */
    return (uint32_t)(0u - x);
}

/* r = a*b*R^-1 mod m, the CIOS form. a and b must be < m. */
static int mont_mul(bn_t *r, const bn_t *a, const bn_t *b,
                    const bn_t *m, uint32_t n0inv)
{
    /* n+2 limbs of accumulator, zeroed. Static because it is 516
       bytes and this is called a few thousand times per modexp; the
       kernel's stack is not the place for it. */
    static uint32_t t[BN_LIMBS + 2];
    uint32_t n = m->n;
    uint32_t i, j;

    if(n + 2 > BN_LIMBS + 2) return 0;
    for(i = 0; i < n + 2; i++) t[i] = 0;

    for(i = 0; i < n; i++){
        uint64_t carry = 0;
        uint32_t ai = (i < a->n) ? a->v[i] : 0;
        uint32_t ui;

        /* t += a_i * b */
        for(j = 0; j < n; j++){
            uint64_t p = (uint64_t)ai * (uint64_t)((j < b->n) ? b->v[j] : 0)
                       + (uint64_t)t[j] + carry;
            t[j] = (uint32_t)p;
            carry = p >> 32;
        }
        {
            uint64_t s = (uint64_t)t[n] + carry;
            t[n] = (uint32_t)s;
            t[n + 1] += (uint32_t)(s >> 32);
        }

        /* t += (t_0 * n0inv mod 2^32) * m, which clears t_0 */
        ui = t[0] * n0inv;
        carry = 0;
        for(j = 0; j < n; j++){
            uint64_t p = (uint64_t)ui * (uint64_t)m->v[j]
                       + (uint64_t)t[j] + carry;
            t[j] = (uint32_t)p;
            carry = p >> 32;
        }
        {
            uint64_t s = (uint64_t)t[n] + carry;
            t[n] = (uint32_t)s;
            t[n + 1] += (uint32_t)(s >> 32);
        }

        /* shift down one limb: t_0 is now zero by construction */
        for(j = 0; j <= n; j++) t[j] = t[j + 1];
        t[n + 1] = 0;
    }

    /* The result is in [0, 2m); one conditional subtract finishes it. */
    bn_zero(r);
    for(i = 0; i < n + 1 && i < BN_LIMBS; i++) r->v[i] = t[i];
    r->n = (n + 1 < BN_LIMBS) ? n + 1 : BN_LIMBS;
    norm(r);
    if(bn_cmp(r, m) >= 0) bn_sub(r, r, m);
    return 1;
}

int bn_modexp(bn_t *r, const bn_t *a, const bn_t *e, const bn_t *m)
{
    static bn_t rr, base, acc, tmp;
    uint32_t n0inv;
    uint32_t i, bits;

    /* An even or zero modulus has no Montgomery constant. Refused
       rather than quietly falling back to something slower: a caller
       that passes an even modulus has a bug, and hiding it costs more
       than the refusal. */
    if(bn_is_zero(m) || !bn_is_odd(m)) return 0;

    /* m == 1: everything is 0 mod 1, and the Montgomery path would
       divide the world by R mod 1. */
    if(m->n == 1 && m->v[0] == 1){ bn_zero(r); return 1; }

    n0inv = mont_n0inv(m->v[0]);

    /* ── R mod m and R^2 mod m ───────────────────────────────────────
       R^2 IS NEVER FORMED AS A NUMBER, and the first version of this
       function did form it:

           rr.v[m->n * 2u] = 1;          // R^2 = 2^(32 * 2n)

       For a 2048-bit modulus m->n is 64, so that is limb index 128 --
       one past the end of a bn_t, because BN_LIMBS is 128. The bound
       check above it turned the overflow into a refusal rather than a
       write into the next static, so the failure was honest; but what
       it refused was EVERY 2048-BIT MODULUS, which is the only size
       this file exists to serve. tests/test_bignum.c found it on its
       first run: 1024-bit modexp passed and RSA-2048 did not.

       The header's sizing note was right about the wrong thing. The
       PRODUCT of two 2048-bit numbers is 4096 bits and fits exactly;
       2^4096 is 4097 bits and does not.

       So: R mod m is 2^(32n) reduced, which needs n+1 limbs and
       always fits. R^2 mod m is then that value SQUARED MODULO m --
       two factors each smaller than m, so the product is under 4096
       bits and fits exactly. No literal 2^4096 anywhere. */
    bn_zero(&rr);
    if(m->n + 1u > BN_LIMBS) return 0;
    rr.v[m->n] = 1;                      /* R = 2^(32 * n)            */
    rr.n = m->n + 1u;
    if(!bn_mod(&acc, &rr, m)) return 0;  /* acc = R mod m, which IS
                                            1 in Montgomery form     */
    if(!bn_mulmod(&rr, &acc, &acc, m)) return 0;   /* rr = R^2 mod m  */

    /* base = a*R mod m, with a reduced first: a may exceed m. */
    if(!bn_mod(&tmp, a, m)) return 0;
    if(!mont_mul(&base, &tmp, &rr, m, n0inv)) return 0;

    /* Square and multiply, most significant bit first. Not a sliding
       window: this is the first version of the file and a window is
       an optimisation with a table in it. */
    bits = bn_bits(e);
    for(i = bits; i > 0; i--){
        if(!mont_mul(&acc, &acc, &acc, m, n0inv)) return 0;
        if(bn_bit(e, i - 1))
            if(!mont_mul(&acc, &acc, &base, m, n0inv)) return 0;
    }

    /* Out of Montgomery form: multiply by 1. */
    bn_set_u32(&tmp, 1);
    if(!mont_mul(r, &acc, &tmp, m, n0inv)) return 0;
    return 1;
}

/* ── gcd and the modular inverse ─────────────────────────────────────
 *
 * RSA key generation needs d = e^-1 mod phi, and a keygen loop needs
 * gcd(e, phi) == 1 before it bothers.
 *
 * Binary (Stein's) gcd and the binary extended Euclidean algorithm:
 * shifts and subtracts, no division, which suits this file's one hard
 * rule and is faster than the division form anyway.
 */
int bn_gcd(bn_t *r, const bn_t *a, const bn_t *b)
{
    static bn_t x, y;
    uint32_t shift = 0;

    bn_copy(&x, a);
    bn_copy(&y, b);
    if(bn_is_zero(&x)){ bn_copy(r, &y); return 1; }
    if(bn_is_zero(&y)){ bn_copy(r, &x); return 1; }

    /* Factor out the common powers of two. */
    while(!bn_is_odd(&x) && !bn_is_odd(&y)){
        bn_shr(&x, &x, 1);
        bn_shr(&y, &y, 1);
        shift++;
    }
    while(!bn_is_odd(&x)) bn_shr(&x, &x, 1);
    do {
        while(!bn_is_odd(&y)) bn_shr(&y, &y, 1);
        if(bn_cmp(&x, &y) > 0){ static bn_t t; bn_copy(&t, &x);
                                bn_copy(&x, &y); bn_copy(&y, &t); }
        bn_sub(&y, &y, &x);
    } while(!bn_is_zero(&y));

    bn_shl(r, &x, shift);
    return 1;
}

int bn_modinv(bn_t *r, const bn_t *a, const bn_t *m)
{
    /* ── WHY THIS IS THE DIVISION FORM AND NOT THE BINARY ONE ────────
     *
     * The first version of this function was the binary extended
     * Euclidean algorithm -- subtract and halve, no division, which
     * suits this file's one hard rule and is faster. It refused an
     * EVEN modulus, honestly and by design, because its halving step
     * needs to add m into an odd coefficient to make it even and that
     * only works when m is odd.
     *
     * THE ONE INVERSE RSA KEY GENERATION NEEDS HAS AN EVEN MODULUS.
     * d = e^-1 mod lambda(n), lambda(n) = lcm(p-1, q-1), and the lcm
     * of two even numbers is always even. phi(n) = (p-1)(q-1) is even
     * for the same reason, so switching to phi does not help. The
     * binary form could compute every inverse except the only one
     * this file was written for.
     *
     * tests/test_bignum.c found that: the 512-bit vectors passed
     * (tools/make_bignum_vectors.py builds them with `| 1`, so every
     * one had an odd modulus) and `bn_modinv recomputes OpenSSL's own
     * d` failed. A suite with only odd moduli in it would have gone
     * out green.
     *
     * So this is the ordinary division form, which works for any
     * modulus. bn_divmod() is binary long division and nothing here
     * is in an inner loop -- a keygen calls this once per candidate.
     *
     * ── AND IT STILL HAS NO SIGNED ARITHMETIC IN IT ─────────────────
     *
     * The Bezout coefficients are negative on alternate steps, and
     * bn_t is unsigned. Rather than carry a sign bit, this uses the
     * continued-fraction property: with
     *
     *     x[-1] = 1,  x[0] = 0,  x[k+1] = x[k-1] + q[k] * x[k]
     *
     * every x is NON-NEGATIVE and the coefficient of a is x[n] with
     * sign (-1)^n -- so the sign is the loop's parity and the
     * magnitudes never go below zero. The parity decides between x
     * and m - x at the end, which is the only place it is used.
     *
     * The magnitudes stay under m, which is why no size check beyond
     * bn_add's own is needed here.
     */
    static bn_t u, v, q, rem, xp, xc, t;
    uint32_t parity = 0;

    if(bn_is_zero(m)) return 0;

    /* m == 1: everything is 0 mod 1, and 0 is its inverse there.
       bn_modexp() special-cases m == 1 the same way, and the two
       disagreeing would be a defect that only shows up in a keygen
       loop that happened to try it. */
    if(m->n == 1 && m->v[0] == 1){ bn_zero(r); return 1; }

    if(!bn_mod(&u, a, m)) return 0;
    if(bn_is_zero(&u)) return 0;           /* 0 has no inverse        */
    bn_copy(&v, m);

    bn_set_u32(&xp, 1);
    bn_zero(&xc);

    while(!bn_is_zero(&v)){
        if(!bn_divmod(&u, &v, &q, &rem)) return 0;
        bn_copy(&u, &v);
        bn_copy(&v, &rem);
        /* x[k+1] = x[k-1] + q * x[k], then shift the window along. */
        if(!bn_mul(&t, &q, &xc)) return 0;
        if(!bn_add(&t, &t, &xp)) return 0;
        bn_copy(&xp, &xc);
        bn_copy(&xc, &t);
        parity ^= 1u;
    }

    /* u now holds gcd(a, m). An inverse exists only when it is 1, and
       "no inverse" is an ANSWER rather than a failure -- a keygen loop
       uses it to reject a candidate exponent. */
    bn_set_u32(&t, 1);
    if(bn_cmp(&u, &t) != 0) return 0;

    /* xp is the MAGNITUDE of the coefficient of a. Its sign is
       (-1)^(k-1) where k is the number of loop iterations, so an EVEN
       iteration count leaves it positive and an odd one means the
       answer is m - xp.

       The direction was wrong the first time and every single result
       came back as m minus the right answer -- modinv(3,11) gave 7
       rather than 4. What caught it was not the known-answer vectors
       but the check next to them, `a * r mod m == 1`: that is what an
       inverse IS, it holds whatever CPython said, and it fails for
       the negation just as loudly as for garbage. */
    if(!bn_mod(&xp, &xp, m)) return 0;
    if(parity == 0u || bn_is_zero(&xp)){
        bn_copy(r, &xp);
    } else {
        if(!bn_sub(r, m, &xp)) return 0;
    }
    return 1;
}
