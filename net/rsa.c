/* ════════════════════════════════════════════════════════════════════
   net/rsa.c -- Miller-Rabin, and RSA key generation as a state
   machine.  v0.8.8.2.

   include/rsa.h carries the reasoning: why it steps instead of
   sleeping, why the speed estimate in the handoff was right for the
   code that existed and is wrong now, and why an rsa_keygen_t does
   not go on the stack.

   This file's own hard rule is the one net/bignum.c's header states:
   DIVIDING A uint64_t EMITS A CALL TO libgcc, and __udivmoddi4 is not
   linked into this kernel. rsa_trial_divide() therefore divides a
   bignum by a 32-bit value with a 64-bit DIVIDEND and a 32-bit
   divisor -- which gcc emits as a single `divl` on i386 and is not a
   64/64 division. That distinction is load-bearing and `make` is what
   checks it.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/rsa.h"

const char *rsa_why_name(int why)
{
    switch(why){
    case RSA_OK:      return "ok";
    case RSA_EBITS:   return "unsupported modulus size";
    case RSA_ESTATE:  return "generator already finished";
    case RSA_EGIVEUP: return "no prime found in the candidate budget";
    case RSA_EARITH:  return "the bignum refused an operation";
    case RSA_ENOINV:  return "gcd(e, lambda) was never 1";
    case RSA_EABORT:  return "stopped by the caller";
    default:          return "unknown";
    }
}

/* ── the small primes ────────────────────────────────────────────────
 *
 * Every odd prime below 1000. Trial division by these rejects about
 * four in five odd candidates for the price of a few hundred
 * single-limb remainders, where the alternative is a modexp -- and
 * that ratio is the whole difference between a keygen measured in
 * seconds and one measured in minutes.
 *
 * 2 is not in the table. Candidates are generated odd and stay odd,
 * so testing it would be testing the generator, and a table that
 * includes it invites somebody to pass an even number in and get a
 * plausible answer. rsa_is_probable_prime() refuses even inputs by
 * name instead.
 */
static const uint16_t small_primes[] = {
      3,   5,   7,  11,  13,  17,  19,  23,  29,  31,  37,  41,  43,
     47,  53,  59,  61,  67,  71,  73,  79,  83,  89,  97, 101, 103,
    107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173,
    179, 181, 191, 193, 197, 199, 211, 223, 227, 229, 233, 239, 241,
    251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317,
    331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401,
    409, 419, 421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479,
    487, 491, 499, 503, 509, 521, 523, 541, 547, 557, 563, 569, 571,
    577, 587, 593, 599, 601, 607, 613, 617, 619, 631, 641, 643, 647,
    653, 659, 661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739,
    743, 751, 757, 761, 769, 773, 787, 797, 809, 811, 821, 823, 827,
    829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911, 919,
    929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997
};
#define N_SMALL ((uint32_t)(sizeof small_primes / sizeof small_primes[0]))

/* n mod d, for a d below 65536.
 *
 * THERE IS NO 64-BIT ARITHMETIC IN HERE AT ALL, and the first version
 * of this function had some:
 *
 *     uint64_t acc = ((uint64_t)rem << 32) | n->v[i - 1];
 *     rem = (uint32_t)(acc % (uint64_t)d);
 *
 * with a comment above it claiming that was a 64-by-32 division gcc
 * would emit as one `divl`. IT IS NOT. Both operands are uint64_t by
 * the time the `%` happens, gcc cannot prove the quotient fits in 32
 * bits, and it emits __udivmoddi4 -- which lives in libgcc, is not
 * linked into this kernel, and fails at the LINK step. The comment
 * warning about that very thing sat directly above the code doing it.
 *
 * The fix needs no cleverness, because every entry in small_primes[]
 * is below 2^16. Take SIXTEEN bits at a time instead of thirty-two:
 * the running remainder is below d, so (rem << 16) | half is below
 * 2^32 and every value in the loop is a plain uint32_t.
 *
 * d < 65536 is therefore a REQUIREMENT, not an observation. A caller
 * that passes something larger gets a wrong answer silently, so it is
 * refused.
 */
static uint32_t mod_u32(const bn_t *n, uint32_t d)
{
    uint32_t i, rem = 0;
    if(d == 0 || d >= 0x10000u) return 0;
    for(i = n->n; i > 0; i--){
        uint32_t w = n->v[i - 1];
        rem = ((rem << 16) | (w >> 16))    % d;
        rem = ((rem << 16) | (w & 0xFFFFu)) % d;
    }
    return rem;
}

int rsa_trial_divide(const bn_t *n)
{
    uint32_t i;
    if(bn_is_zero(n)) return 0;
    if(!bn_is_odd(n)) return 0;
    for(i = 0; i < N_SMALL; i++){
        uint32_t p = small_primes[i];
        /* A candidate that IS one of the small primes is prime, not
           composite. Only reachable from a suite -- a 1024-bit
           candidate never is -- and getting it backwards would make
           rsa_is_probable_prime(7) say composite. */
        if(n->n == 1 && n->v[0] == p) return 1;
        if(mod_u32(n, p) == 0) return 0;
    }
    return 1;
}

/* ── Miller-Rabin ────────────────────────────────────────────────────
 *
 * Write n-1 = q * 2^s with q odd. For a witness a, compute
 * x = a^q mod n; if x is 1 or n-1 the witness is satisfied, otherwise
 * square up to s-1 times looking for n-1. A witness that never sees
 * n-1 PROVES n composite.
 *
 * THE SQUARING LOOP IS THE WHOLE TEST. Stop after computing a^q and
 * comparing with 1 and you have written a Fermat test wearing
 * Miller-Rabin's name, and a Fermat test accepts every Carmichael
 * number -- 561, 1105, 1729, and infinitely many more -- for every
 * base coprime to it. tests/test_rsa.c runs those numbers.
 */

/* One witness. 1 = satisfied (n may be prime), 0 = n is composite. */
static int mr_witness(const bn_t *n, const bn_t *q, uint32_t s,
                      const bn_t *a, const bn_t *nm1)
{
    static bn_t x;
    uint32_t i;

    if(!bn_modexp(&x, a, q, n)) return 1;   /* refusal is not a proof */
    if((x.n == 1 && x.v[0] == 1) || bn_cmp(&x, nm1) == 0) return 1;

    for(i = 1; i < s; i++){
        if(!bn_mulmod(&x, &x, &x, n)) return 1;
        if(bn_cmp(&x, nm1) == 0) return 1;
        /* x == 1 before reaching n-1 means a non-trivial square root
           of 1, which is itself a proof of compositeness -- and from
           here on squaring stays at 1 for ever, so stopping early is
           not an optimisation, it is the conclusion. */
        if(x.n == 1 && x.v[0] == 1) return 0;
    }
    return 0;
}

/* A random witness in [2, n-2]. */
static void mr_random_witness(bn_t *a, const bn_t *n)
{
    uint32_t i, limbs = n->n;
    bn_zero(a);
    if(limbs == 0){ bn_set_u32(a, 2); return; }
    for(i = 0; i < limbs; i++) a->v[i] = entropy_u32();
    a->n = limbs;
    /* Reduce into range, then force it to at least 2. bn_mod gives
       [0, n-1]; 0 and 1 are satisfied by every n and would be a
       wasted round, and n-1 is satisfied by every n too. */
    if(!bn_mod(a, a, n)){ bn_set_u32(a, 2); return; }
    if(a->n == 0 || (a->n == 1 && a->v[0] <= 1)) bn_set_u32(a, 2);
}

int rsa_is_probable_prime(const bn_t *n, uint32_t rounds)
{
    static bn_t nm1, q, a;
    uint32_t s = 0, r;

    if(bn_is_zero(n)) return 0;
    if(n->n == 1 && n->v[0] < 2) return 0;          /* 0 and 1        */
    if(n->n == 1 && n->v[0] == 2) return 1;         /* 2 is prime     */
    /* An EVEN input is refused by name rather than answered. bn_modexp
       needs an odd modulus for Montgomery, so an even n here would
       make every witness "satisfied" and the answer would be
       "probably prime" for 4. */
    if(!bn_is_odd(n)) return 0;

    /* rsa_trial_divide() already answers 1 for a value that IS one of
       the small primes, so a 0 here means composite and nothing
       else. */
    if(!rsa_trial_divide(n)) return 0;

    /* Below 997^2 a composite must have a prime factor at or under
       997, and every one of those is in the table -- so surviving
       trial division below that bound is a PROOF of primality, not an
       estimate, and Miller-Rabin has nothing to add. 1009 would also
       be sound and would leave the suite's four- and five-digit cases
       going through the witness loop for no reason. */
    if(n->n == 1 && n->v[0] < 994009u) return 1;    /* 997 * 997 */

    /* n - 1 = q * 2^s, q odd. */
    bn_copy(&nm1, n);
    { static bn_t one; bn_set_u32(&one, 1);
      if(!bn_sub(&nm1, &nm1, &one)) return -RSA_EARITH; }
    bn_copy(&q, &nm1);
    while(!bn_is_odd(&q)){
        if(!bn_shr(&q, &q, 1)) return -RSA_EARITH;
        s++;
    }
    if(s == 0) return -RSA_EARITH;   /* n-1 odd means n even: unreachable */

    /* ── r < rounds, AND IT SAID `r < 1` ────────────────────────────
       Miller-Rabin ran exactly ONE round whatever the caller asked
       for, so rsa_is_probable_prime(n, 64) was a one-round test. The
       line above, clamping 0 to 1, says plainly what was meant.

       Measured on 2284453 = 1069 * 2137, a composite built to have
       many strong liars: 37 of 200 runs called it prime at one
       round, and 29 of 200 at sixty-four. Identical, because the
       extra witnesses were never drawn.

       Nothing caught it, for a reason worth keeping: every
       pseudoprime in tests/rsa_vectors.inc was smaller than
       997 * 997, and below that bound this function proves primality
       by trial division and returns before the witness loop. The
       whole of Miller-Rabin was unreachable from the suite. The
       vectors now carry pseudoprimes whose smallest factor is above
       the table, and the check that compares one round against
       sixty-four is what found this.

       Key GENERATION was never affected -- rsa_keygen_step() has its
       own witness loop and honours k->rounds_needed -- so keys
       already produced are sound. What was wrong is the function
       anything else would call to check one. */
    if(rounds == 0) rounds = 1;
    for(r = 0; r < rounds; r++){
        mr_random_witness(&a, n);
        if(!mr_witness(n, &q, s, &a, &nm1)) return 0;
    }
    return 1;
}

/* ── key generation ──────────────────────────────────────────────── */

/* Stages of the state machine. */
#define KG_NEW_CANDIDATE  0
#define KG_TESTING        1
#define KG_FINISH         2
#define KG_DONE           3
#define KG_FAILED         4

/* How many candidates to examine before giving up on a prime.
 *
 * The density of primes near 2^b is about 1/(b * ln 2), so among ODD
 * candidates roughly one in b*ln(2)/2 is prime -- about 355 for
 * b = 1024. Trial division removes four fifths of the rest for
 * almost nothing, so the number of MODEXPS is far smaller than the
 * number of candidates.
 *
 * 20000 is roughly fifty times the expectation. Reaching it means the
 * witness source is not random -- a stuck entropy_u32() that returns
 * the same value for ever would otherwise spin here until somebody
 * power-cycled the machine. A BUDGET THAT CANNOT BE REACHED IS NOT A
 * BUDGET; this one is a named refusal, RSA_EGIVEUP.
 */
#define KG_BUDGET  20000u

int rsa_keygen_begin(rsa_keygen_t *k, uint32_t modulus_bits)
{
    if(!k) return RSA_EBITS;
    if(modulus_bits < 512u || modulus_bits > (uint32_t)(BN_BITS / 2))
        return RSA_EBITS;
    if(modulus_bits & 1u) return RSA_EBITS;

    bn_zero(&k->n); bn_zero(&k->d); bn_zero(&k->p); bn_zero(&k->q);
    bn_zero(&k->lam); bn_zero(&k->cand);
    bn_set_u32(&k->e, 65537u);

    k->prime_bits    = modulus_bits / 2u;
    k->stage         = KG_NEW_CANDIDATE;
    k->tried         = 0;
    k->rounds_done   = 0;
    /* 64 witnesses on the primes that will actually be kept. This is
       not a tunable: the key goes in a certificate a phone will trust
       for as long as the pair record survives. */
    k->rounds_needed = 64;
    k->have_p        = 0;
    k->why           = RSA_OK;
    return RSA_OK;
}

/* A fresh odd candidate of exactly prime_bits bits.
 *
 * THE TOP TWO BITS ARE BOTH SET, which is not decoration. p and q
 * each at least 2^(b-1) + 2^(b-2) makes their product at least
 * 2^(2b-1) * 1.5, so n has exactly 2b bits -- always. Set only the
 * top bit and two candidates just above 2^(b-1) give a modulus of
 * 2b-1 bits, which is a 2047-bit "2048-bit key" that some peers
 * accept and others reject, and the ones that reject it do so without
 * saying why.
 */
static void new_candidate(rsa_keygen_t *k)
{
    uint32_t limbs = (k->prime_bits + 31u) / 32u;
    uint32_t i, top = k->prime_bits % 32u;

    bn_zero(&k->cand);
    for(i = 0; i < limbs; i++) k->cand.v[i] = entropy_u32();
    k->cand.n = limbs;

    /* Mask down to exactly prime_bits, then force the top two. */
    if(top){
        k->cand.v[limbs - 1] &= (uint32_t)((1u << top) - 1u);
        k->cand.v[limbs - 1] |= (uint32_t)(1u << (top - 1u));
        if(top >= 2u) k->cand.v[limbs - 1] |= (uint32_t)(1u << (top - 2u));
    } else {
        k->cand.v[limbs - 1] |= 0xC0000000u;
    }
    k->cand.v[0] |= 1u;                  /* odd */
    /* No norm() call: the top bit is set, so the top limb is non-zero
       and k->cand.n is already the normalised count. */
}

int rsa_keygen_step(rsa_keygen_t *k)
{
    if(!k) return RSA_KG_FAILED;

    switch(k->stage){

    case KG_NEW_CANDIDATE: {
        if(k->tried >= KG_BUDGET){
            k->why = RSA_EGIVEUP;
            k->stage = KG_FAILED;
            return RSA_KG_FAILED;
        }
        k->tried++;
        new_candidate(k);
        k->rounds_done = 0;
        /* Trial division is cheap enough to do inside the same step:
           it is a few hundred single-limb remainders against a modexp
           of thirty-two limbs, and making it its own step would make
           the progress line move four times for every unit of real
           work. */
        if(!rsa_trial_divide(&k->cand)) return RSA_KG_WORKING;
        k->stage = KG_TESTING;
        return RSA_KG_WORKING;
    }

    case KG_TESTING: {
        /* ONE witness per step. That is the bounded unit: one modexp,
           measured at about 5 ms for a 1024-bit candidate, which is
           short enough that the caller's poll keeps the machine
           responsive and long enough that the loop is not all
           overhead. */
        static bn_t nm1, q, a;
        static bn_t one;
        uint32_t s = 0;

        bn_copy(&nm1, &k->cand);
        bn_set_u32(&one, 1);
        if(!bn_sub(&nm1, &nm1, &one)){
            k->why = RSA_EARITH; k->stage = KG_FAILED; return RSA_KG_FAILED;
        }
        bn_copy(&q, &nm1);
        while(!bn_is_odd(&q)){
            if(!bn_shr(&q, &q, 1)){
                k->why = RSA_EARITH; k->stage = KG_FAILED;
                return RSA_KG_FAILED;
            }
            s++;
        }

        mr_random_witness(&a, &k->cand);
        if(!mr_witness(&k->cand, &q, s, &a, &nm1)){
            k->stage = KG_NEW_CANDIDATE;     /* composite, move on */
            return RSA_KG_WORKING;
        }
        k->rounds_done++;
        if(k->rounds_done < k->rounds_needed) return RSA_KG_WORKING;

        /* It survived every witness. */
        if(!k->have_p){
            bn_copy(&k->p, &k->cand);
            k->have_p = 1;
            k->stage = KG_NEW_CANDIDATE;
            return RSA_KG_WORKING;
        }
        /* p == q would make n a perfect square and the key
           trivially factorable. Vanishingly unlikely and checked
           anyway, because "vanishingly unlikely" is what a stuck
           entropy source makes certain. */
        if(bn_cmp(&k->cand, &k->p) == 0){
            k->stage = KG_NEW_CANDIDATE;
            return RSA_KG_WORKING;
        }
        bn_copy(&k->q, &k->cand);
        k->stage = KG_FINISH;
        return RSA_KG_WORKING;
    }

    case KG_FINISH: {
        static bn_t p1, q1, g, one;
        bn_set_u32(&one, 1);

        if(!bn_mul(&k->n, &k->p, &k->q)) goto arith;
        if(!bn_sub(&p1, &k->p, &one)) goto arith;
        if(!bn_sub(&q1, &k->q, &one)) goto arith;

        /* lambda = lcm(p-1, q-1) = (p-1)(q-1) / gcd(p-1, q-1).
           RFC 8017 defines d against THIS, not against (p-1)(q-1).
           Both give a working private exponent; they are different
           numbers, and net/bignum.c's header records the afternoon
           that cost. */
        if(!bn_gcd(&g, &p1, &q1)) goto arith;
        if(!bn_mul(&k->lam, &p1, &q1)) goto arith;
        if(!bn_divmod(&k->lam, &g, &k->lam, 0)) goto arith;

        /* lambda is EVEN -- the lcm of two even numbers always is --
           which is exactly the case the binary extended Euclid could
           not do and the reason net/bignum.c carries the division
           form. */
        if(!bn_modinv(&k->d, &k->e, &k->lam)){
            /* gcd(65537, lambda) != 1. Possible, rare, and the fix is
               a different prime rather than a different exponent:
               65537 is what every peer expects. */
            k->have_p = 0;
            k->stage  = KG_NEW_CANDIDATE;
            return RSA_KG_WORKING;
        }
        k->stage = KG_DONE;
        return RSA_KG_DONE;
    arith:
        k->why = RSA_EARITH; k->stage = KG_FAILED; return RSA_KG_FAILED;
    }

    case KG_DONE:
        return RSA_KG_DONE;

    default:
        if(k->why == RSA_OK) k->why = RSA_ESTATE;
        return RSA_KG_FAILED;
    }
}

uint32_t rsa_keygen_progress(const rsa_keygen_t *k)
{
    /* AN ESTIMATE, AND THE HEADER SAYS SO. A prime search has no
       length: this is how many candidates have been examined against
       how many a modulus of this size usually needs, halved per
       prime. It can sit still for a long time and it can jump. */
    uint32_t expect, done, pct;
    if(!k) return 0;
    if(k->stage == KG_DONE) return 100;

    /* About b*ln(2)/2 odd candidates per prime; ln(2) ~ 0.693. */
    expect = (k->prime_bits * 693u) / 2000u;
    if(expect == 0) expect = 1;

    done = k->tried;
    if(done > expect * 2u) done = expect * 2u;   /* clamp, don't lie  */
    pct = (done * 50u) / (expect * 2u);
    if(k->have_p && pct < 50u) pct = 50u;
    if(pct > 99u) pct = 99u;
    return pct;
}
