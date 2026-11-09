/* ════════════════════════════════════════════════════════════════════
   test_rsa.c -- Miller-Rabin and RSA key generation.

   THE NUMBERS THAT MATTER HERE ARE NOT RANDOM ONES. A primality test
   is almost-right on random input in three distinct ways, and all
   three produce a key that factors instantly:

     a FERMAT TEST accepts every Carmichael number -- 561, 1105,
     1729, 2465 -- for every base coprime to it;
     MILLER-RABIN WITHOUT THE SQUARING LOOP is a Fermat test with
     extra steps and fails on the same numbers;
     MILLER-RABIN WITH ONE FIXED BASE accepts the strong
     pseudoprimes to that base -- 2047, 3277, 4033 for base 2.

   tools/make_rsa_vectors.py carries the published lists for all
   three, classified by `openssl prime` or SymPy and not by anything
   in this tree. The suite runs them at 1 round and at 64; **a
   Carmichael number must be refused at ONE round**, because a
   candidate search spends almost all its time at one round and that
   is where a wrong answer becomes a key.

   ── THE WITNESS SOURCE IS SEEDED, DELIBERATELY ──────────────────────

   entropy_u32() here is a seeded PRNG so runs are reproducible. That
   is legitimate in a way a stubbed driver is not: a primality
   DECISION must not depend on which witnesses were drawn, and this
   suite proves that rather than assuming it -- it re-runs the whole
   hard-case set under a source that returns nothing but 2, and then
   nothing but 3, which are precisely the fixed bases the
   pseudoprime lists defeat.

   ── AND THE KEY IT GENERATES IS CHECKED AS A KEY ────────────────────

   Not "the function returned DONE". n == p*q, p and q both prime by
   an independent test, n of exactly the requested bit length,
   d*e == 1 mod lambda, and a full encrypt/decrypt round trip on a
   message the suite picked. A keygen that produced two copies of the
   same prime, or a 2047-bit modulus, would satisfy a return code.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/bignum.h"
#include "../include/rsa.h"
#include "rsa_vectors.inc"

static int total = 0, fails = 0;

static void ck(const char *what, int ok, long got)
{
    total++;
    if(!ok){
        fails++;
        printf("  FAIL  %s   (got %ld)\n", what, got);
    }
}

/* ── the witness source, under the suite's control ──────────────────
   Three modes, because which one is in force is the point of half
   the checks below. */
#define ENT_PRNG   0
#define ENT_ALL_2  1
#define ENT_ALL_3  2
/* ENT_REPLAY: hand back a bn_t's limbs, cycling. new_candidate()
   fills each limb from entropy_u32(), then forces the top two bits
   and the low one -- so replaying the limbs of a value that already
   has those bits set reproduces that value EXACTLY. Pointed at the p
   a keygen just found, it makes the next candidate equal p, which is
   the only way to reach the p == q check without waiting for a
   coincidence that will not happen. */
#define ENT_REPLAY 3
static int      ent_mode = ENT_PRNG;
static bn_t     ent_replay;
static uint32_t ent_replay_i = 0;
static uint32_t ent_state = 0x13572468u;

uint32_t entropy_u32(void)
{
    switch(ent_mode){
    case ENT_ALL_2: return 2u;
    case ENT_ALL_3: return 3u;
    case ENT_REPLAY: {
        uint32_t v;
        if(ent_replay.n == 0) return 2u;
        v = ent_replay.v[ent_replay_i % ent_replay.n];
        ent_replay_i++;
        return v;
    }
    default: break;
    }
    /* xorshift32 -- reproducible, and not trying to be good */
    ent_state ^= ent_state << 13;
    ent_state ^= ent_state >> 17;
    ent_state ^= ent_state << 5;
    return ent_state;
}

static void load(bn_t *a, const unsigned char *p, unsigned len)
{
    if(len == 1 && p[0] == 0){ bn_zero(a); return; }
    if(!bn_from_bytes(a, p, len)){
        printf("  FAIL  bn_from_bytes refused a %u-byte vector\n", len);
        fails++; total++;
        bn_zero(a);
    }
}

/* Run a whole vector group and require every answer. */
static void run_group(const char *label, const rsavec_t *g, int n,
                      uint32_t rounds)
{
    bn_t v;
    int i;
    for(i = 0; i < n; i++){
        int want = g[i].prime, got;
        load(&v, g[i].v, g[i].v_len);
        got = rsa_is_probable_prime(&v, rounds);
        if(got != want){
            char buf[160];
            /* The label names the hazard, because "FAIL 561" on its
               own does not say that 561 is a Carmichael number and
               that accepting it means this is a Fermat test. */
            snprintf(buf, sizeof buf, "%s: %s should be %s",
                     label, g[i].what, want ? "prime" : "COMPOSITE");
            ck(buf, 0, got);
        } else {
            total++;
        }
    }
}

int main(void)
{
    bn_t a, b, t;
    int i;

    printf("test_rsa: Miller-Rabin and keygen, against openssl's "
           "classification\n\n");

    /* ── trial division on its own ───────────────────────────────── */
    printf("trial division\n");
    {
        bn_set_u32(&a, 0);
        ck("zero is not prime", rsa_trial_divide(&a) == 0, 0);
        bn_set_u32(&a, 4);
        ck("an even number is refused", rsa_trial_divide(&a) == 0, 0);
        bn_set_u32(&a, 3);
        ck("3 IS one of the small primes, so it survives",
           rsa_trial_divide(&a) == 1, 0);
        bn_set_u32(&a, 997);
        ck("997, the largest in the table, survives",
           rsa_trial_divide(&a) == 1, 0);
        bn_set_u32(&a, 9);
        ck("9 is divisible by 3", rsa_trial_divide(&a) == 0, 0);
        bn_set_u32(&a, 994009);        /* 997 * 997 */
        ck("997 squared is caught", rsa_trial_divide(&a) == 0, 0);
        bn_set_u32(&a, 1009);
        ck("1009 is past the table and survives it",
           rsa_trial_divide(&a) == 1, 0);

        /* THE MULTI-LIMB PATH. mod_u32 walks limbs from the top down
           sixteen bits at a time; a one-limb value never exercises
           the carry of the remainder between limbs. */
        load(&a, RSA_KEY_p, sizeof RSA_KEY_p);
        ck("a 1024-bit prime survives trial division",
           rsa_trial_divide(&a) == 1, 0);
        /* p * 3 is divisible by 3 and is 1026 bits, so the remainder
           has to be carried across 32 limbs correctly to see it. */
        bn_set_u32(&b, 3);
        ck("a 1026-bit multiple of 3 is caught",
           bn_mul(&t, &a, &b) == 1 && rsa_trial_divide(&t) == 0, 0);
        bn_set_u32(&b, 991);
        ck("a 1034-bit multiple of 991 is caught",
           bn_mul(&t, &a, &b) == 1 && rsa_trial_divide(&t) == 0, 0);
    }

    /* ── the easy cases ──────────────────────────────────────────── */
    printf("small primes and composites  (%d + %d vectors)\n",
           RSA_SMALL_P_N, RSA_SMALL_C_N);
    run_group("small", RSA_SMALL_P, RSA_SMALL_P_N, 64);
    run_group("small", RSA_SMALL_C, RSA_SMALL_C_N, 64);

    /* ── the three hazards, at ONE round and at 64 ───────────────── */
    printf("CARMICHAEL numbers  (%d vectors) -- a Fermat test accepts "
           "all of them\n", RSA_CARMICHAEL_N);
    run_group("carmichael, 1 round", RSA_CARMICHAEL, RSA_CARMICHAEL_N, 1);
    run_group("carmichael, 64 rounds", RSA_CARMICHAEL, RSA_CARMICHAEL_N, 64);

    printf("strong pseudoprimes to base 2  (%d vectors)\n", RSA_SPSP2_N);
    run_group("spsp2, 1 round", RSA_SPSP2, RSA_SPSP2_N, 1);
    run_group("spsp2, 64 rounds", RSA_SPSP2, RSA_SPSP2_N, 64);

    printf("strong pseudoprimes to base 3  (%d vectors)\n", RSA_SPSP3_N);
    run_group("spsp3, 1 round", RSA_SPSP3, RSA_SPSP3_N, 1);
    run_group("spsp3, 64 rounds", RSA_SPSP3, RSA_SPSP3_N, 64);

    /* ── the same sets under a source that only ever says 2, then 3 ─
       This is the check that a fixed-base implementation cannot pass.
       Note it must still get the EASY cases right, so a mode that
       broke everything would be caught too. */
    printf("the hard cases again, with an entropy source stuck at 2\n");
    ent_mode = ENT_ALL_2;
    run_group("stuck at 2: carmichael", RSA_CARMICHAEL,
              RSA_CARMICHAEL_N, 64);
    run_group("stuck at 2: spsp2", RSA_SPSP2, RSA_SPSP2_N, 64);
    run_group("stuck at 2: small primes", RSA_SMALL_P, RSA_SMALL_P_N, 64);

    printf("and again with it stuck at 3\n");
    ent_mode = ENT_ALL_3;
    run_group("stuck at 3: carmichael", RSA_CARMICHAEL,
              RSA_CARMICHAEL_N, 64);
    run_group("stuck at 3: spsp3", RSA_SPSP3, RSA_SPSP3_N, 64);
    run_group("stuck at 3: small primes", RSA_SMALL_P, RSA_SMALL_P_N, 64);
    ent_mode = ENT_PRNG;

    /* ── THE HAZARDS THAT ACTUALLY REACH MILLER-RABIN ────────────────
       Everything above is smaller than 997 * 997, and net/rsa.c
       proves primality below that by trial division alone -- so not
       one of those vectors chooses a witness, squares anything, or
       executes a line of mr_witness().

       tools/check_rsa_suite.py is what said so: TEN of eighteen
       injections were MISSED while this suite passed 295 of 295,
       because all ten live in code it never ran. "The witness is
       always 2" cannot be caught by a strong pseudoprime to base 2
       that a division refuses first.

       These groups have no prime factor at or below 997 --
       tools/make_rsa_vectors.py constructs them and refuses to emit
       any that trial division could catch. They are the only vectors
       in this file that test the primality test. */
    printf("the hazards that trial division CANNOT refuse\n");
    /* NO ONE-ROUND RUN OVER THESE. A Carmichael number survives a
       single random witness about a third of the time -- that is the
       property the block further down relies on deliberately -- so a
       group that demanded COMPOSITE at one round would be asserting
       something false and would pass or fail with the PRNG. It did
       pass, once, and then stopped. 64 rounds is where the answer is
       certain. */
    run_group("hard carmichael, 64 rounds",
              RSA_HARD_CARMICHAEL, RSA_HARD_CARMICHAEL_N, 64);
    run_group("hard spsp2, 64 rounds",
              RSA_HARD_SPSP2, RSA_HARD_SPSP2_N, 64);
    run_group("hard spsp3, 64 rounds",
              RSA_HARD_SPSP3, RSA_HARD_SPSP3_N, 64);

    /* ── WHY THERE IS NO PINNED RUN OVER THE HARD GROUPS ─────────────
       There was one, and it failed, and the check was wrong rather
       than the code.

       A STRONG PSEUDOPRIME TO BASE 2 PASSES A MILLER-RABIN ROUND
       WITH WITNESS 2. That is what the words mean. Pinning the
       entropy source at 2 makes every witness 2 -- mr_random_witness
       fills each limb from the source and reduces mod n, and for a
       single-limb n that is 2 mod n, which is 2 -- so a CORRECT
       implementation accepts all ten of them, and must.

       The pinned runs over the SMALL groups above pass, and that is
       the trap: those vectors are below 997*997, trial division
       refuses them before a witness is ever chosen, and the check
       therefore passed without the property it names being true.
       A green check standing on an unrelated mechanism is worse than
       no check, because nobody looks at it again.

       What actually catches a hard-coded base is the ENT_PRNG run
       just above: with varying witnesses a base-2 pseudoprime is
       refused by some other base, and an implementation pinned to 2
       accepts it. That is the whole of the test, and it needs the
       hard groups to reach Miller-Rabin at all. */

    /* ── THE ROUND COUNT HAS TO MEAN SOMETHING ───────────────────────
       "The witness loop runs once whatever was asked" was MISSED
       because nothing distinguished 1 round from 64. A hard
       Carmichael number is composite for most bases but not all, so
       one round sometimes passes and 64 does not -- which is exactly
       the property `rounds` exists to buy, and the only way to
       observe it is to count how many witnesses get used. */
    printf("the round count is honoured\n");
    {
        int i, one_round_passes = 0;
        ent_mode = ENT_PRNG;
        for(i = 0; i < RSA_HARD_CARMICHAEL_N; i++){
            bn_t v;
            int k;
            load(&v, RSA_HARD_CARMICHAEL[i].v, RSA_HARD_CARMICHAEL[i].v_len);
            /* Run 1 round many times; a Carmichael number survives a
               single random witness with probability at least 1/4,
               so over this many tries at least one must get through
               if the round count is really 1. */
            for(k = 0; k < 40; k++)
                if(rsa_is_probable_prime(&v, 1)){ one_round_passes++; break; }
        }
        ck("one round lets at least one hard Carmichael through",
           one_round_passes > 0, one_round_passes);
        /* ...and 64 rounds never does. This pair is what makes the
           round count observable: either check alone passes for an
           implementation that ignores it. */
        {
            int leaked = 0, i2, k2;
            for(i2 = 0; i2 < RSA_HARD_CARMICHAEL_N; i2++){
                bn_t v;
                load(&v, RSA_HARD_CARMICHAEL[i2].v,
                     RSA_HARD_CARMICHAEL[i2].v_len);
                for(k2 = 0; k2 < 8; k2++)
                    if(rsa_is_probable_prime(&v, 64)) leaked++;
            }
            ck("...and 64 rounds never does", leaked == 0, leaked);
        }
    }

    /* ── AN EVEN MODULUS, ABOVE THE TABLE ────────────────────────────
       bn_modexp refuses an even modulus, so every witness comes back
       "satisfied", so an even number reaches "probably prime" unless
       something refuses it first. Below 997^2 trial division does.
       Above it, only an explicit check does -- and "an even modulus
       is accepted into Miller-Rabin" was MISSED for exactly that
       reason. */
    printf("an even number above the table is still composite\n");
    {
        bn_t v;
        bn_set_u32(&v, 994010u);          /* 997*997 + 1, even        */
        ck("994010 is composite", rsa_is_probable_prime(&v, 64) == 0, 0);
        bn_set_u32(&v, 0xFFFFFFFEu);
        ck("4294967294 is composite", rsa_is_probable_prime(&v, 64) == 0, 0);
        bn_set_u32(&v, 2u);
        ck("...but 2 itself is prime", rsa_is_probable_prime(&v, 64) == 1, 0);
    }

    /* ── THE PROOF BOUND IS A SQUARE, NOT THE TABLE'S TOP ────────────
       Surviving trial division proves primality only below 997*997.
       "The trial-division proof bound is wrong by a square" was
       MISSED because nothing in the vectors sat between 997 and
       997^2 of a composite whose smallest factor is above the table.
       1009 * 1009 is the first one. */
    printf("the proof bound is 997 squared, not 997\n");
    {
        bn_t v;
        bn_set_u32(&v, 1009u * 1009u);
        ck("1009*1009 is composite", rsa_is_probable_prime(&v, 64) == 0, 0);
        bn_set_u32(&v, 1009u * 1013u);
        ck("1009*1013 is composite", rsa_is_probable_prime(&v, 64) == 0, 0);
        bn_set_u32(&v, 994009u);
        ck("997*997 itself is composite",
           rsa_is_probable_prime(&v, 64) == 0, 0);
        bn_set_u32(&v, 994013u);
        ck("...and 994013 just above it is prime",
           rsa_is_probable_prime(&v, 64) == 1, 0);
    }

    /* ── the real thing: 1024-bit primes and their product ───────── */
    printf("a real 2048-bit key's components\n");
    {
        bn_t p_, q_, n_;
        load(&p_, RSA_KEY_p, sizeof RSA_KEY_p);
        load(&q_, RSA_KEY_q, sizeof RSA_KEY_q);
        load(&n_, RSA_KEY_n, sizeof RSA_KEY_n);

        ck("p is 1024 bits", bn_bits(&p_) == RSA_KEY_BITS / 2,
           (long)bn_bits(&p_));
        ck("p is prime", rsa_is_probable_prime(&p_, 8) == 1, 0);
        ck("q is prime", rsa_is_probable_prime(&q_, 8) == 1, 0);
        ck("n = p*q is 2048 bits", bn_bits(&n_) == RSA_KEY_BITS,
           (long)bn_bits(&n_));
        /* n is a 2048-bit composite with no small factors, which is
           the one input that separates a working Miller-Rabin from
           trial division alone. */
        ck("n survives trial division", rsa_trial_divide(&n_) == 1, 0);
        ck("  ... and Miller-Rabin still calls it COMPOSITE",
           rsa_is_probable_prime(&n_, 1) == 0, 0);
    }

    /* ── keygen ──────────────────────────────────────────────────── */
    printf("keygen refusals\n");
    {
        static rsa_keygen_t k;
        ck("an odd modulus size is refused",
           rsa_keygen_begin(&k, 1025) == RSA_EBITS, 0);
        ck("too small is refused", rsa_keygen_begin(&k, 256) == RSA_EBITS, 0);
        ck("too large for the bn_t is refused",
           rsa_keygen_begin(&k, BN_BITS) == RSA_EBITS, 0);
        ck("512 is accepted (for suites, not for keys)",
           rsa_keygen_begin(&k, 512) == RSA_OK, 0);
        ck("  ... and names its refusals",
           rsa_why_name(RSA_EGIVEUP)[0] != 0, 0);
    }

    printf("generating a 512-bit key, and checking it AS A KEY\n");
    {
        static rsa_keygen_t k;
        int r, steps = 0;
        bn_t chk, msg, ct, pt, one;

        ck("begin", rsa_keygen_begin(&k, 512) == RSA_OK, 0);
        ck("  ... progress starts at 0", rsa_keygen_progress(&k) == 0,
           (long)rsa_keygen_progress(&k));

        /* THE ROUND COUNT IS THE KEY'S CONFIDENCE, so it is asserted
           rather than assumed. "The primes are kept after one
           witness" was MISSED because every other check here is
           satisfied by a prime found in one round -- the primes ARE
           prime, they were just accepted on far less evidence than
           64 witnesses, and nothing downstream can tell. */
        ck("  ... and 64 witnesses are required before a prime is kept",
           k.rounds_needed == 64, (long)k.rounds_needed);

        {
            int even_cand = 0;
            do {
                r = rsa_keygen_step(&k);
                steps++;
                /* EVERY CANDIDATE IS ODD. new_candidate() forces the
                   low bit, and without that half of them are thrown
                   away by rsa_trial_divide() for nothing. The waste
                   is invisible from the outside -- the key still
                   comes out right -- so the candidate itself is
                   checked while it exists. */
                if(k.cand.n != 0 && !bn_is_odd(&k.cand)) even_cand++;
                /* A BOUND ON THE LOOP. Without one, a keygen that
                   never terminates makes the suite hang rather than
                   fail, and a hang in CI reads as an infrastructure
                   problem. */
                if(steps > 2000000){ r = RSA_KG_FAILED; break; }
            } while(r == RSA_KG_WORKING);
            ck("no candidate was ever even", even_cand == 0, even_cand);
        }

        /* A LOWER BOUND ON THE WORK. 64 witnesses for each of two
           primes is 128 modular exponentiations at the very least,
           and each is its own step. A generator that keeps a prime
           after one witness finishes in a small fraction of that,
           and this is the only check that notices the difference
           without looking inside. */
        ck("  ... and it did at least 128 witness steps", steps >= 128,
           steps);

        ck("it finished", r == RSA_KG_DONE, (long)k.why);
        if(r == RSA_KG_DONE){
            printf("    (%d steps)\n", steps);
            ck("progress ends at 100", rsa_keygen_progress(&k) == 100,
               (long)rsa_keygen_progress(&k));

            /* every property of a key, not just the return code */
            ck("the modulus is EXACTLY 512 bits -- not 511",
               bn_bits(&k.n) == 512, (long)bn_bits(&k.n));
            ck("p * q == n", bn_mul(&chk, &k.p, &k.q) == 1
               && bn_cmp(&chk, &k.n) == 0, 0);
            ck("p != q", bn_cmp(&k.p, &k.q) != 0, 0);
            ck("p is prime by an independent round count",
               rsa_is_probable_prime(&k.p, 32) == 1, 0);
            ck("q is prime", rsa_is_probable_prime(&k.q, 32) == 1, 0);
            ck("p is 256 bits", bn_bits(&k.p) == 256, (long)bn_bits(&k.p));
            ck("q is 256 bits", bn_bits(&k.q) == 256, (long)bn_bits(&k.q));
            ck("e is 65537", k.e.n == 1 && k.e.v[0] == 65537,
               (long)k.e.v[0]);
            ck("n is odd", bn_is_odd(&k.n) == 1, 0);
            ck("lambda is EVEN, which is why bn_modinv had to change",
               bn_is_odd(&k.lam) == 0, 0);

            bn_set_u32(&one, 1);
            ck("d * e == 1 mod lambda",
               bn_mulmod(&chk, &k.d, &k.e, &k.lam) == 1
               && bn_cmp(&chk, &one) == 0, 0);
            ck("d < lambda", bn_cmp(&k.d, &k.lam) == -1, 0);

            /* ── LAMBDA, RECOMPUTED HERE FROM p AND q ────────────────
               The two checks above are satisfied by d = e^-1 mod phi
               as well, because lambda DIVIDES phi -- so d*e is 1 mod
               lambda either way, and if k.lam itself holds phi then
               "d < lambda" passes too. "d is computed against phi
               instead of lambda" was MISSED for exactly that reason:
               every property was checked against the value under
               test rather than against one derived independently.

               lambda = (p-1)(q-1) / gcd(p-1, q-1), worked out here
               from the primes. If k.lam is phi this fails, and
               nothing else in the file does. */
            {
                bn_t pm1, qm1, phi, g, lam2;
                bn_copy(&pm1, &k.p); bn_sub(&pm1, &pm1, &one);
                bn_copy(&qm1, &k.q); bn_sub(&qm1, &qm1, &one);
                ck("phi = (p-1)(q-1)", bn_mul(&phi, &pm1, &qm1) == 1, 0);
                ck("gcd(p-1, q-1)", bn_gcd(&g, &pm1, &qm1) == 1, 0);
                ck("  ... and it is EVEN, since both are",
                   bn_is_odd(&g) == 0, 0);
                ck("lambda = phi / that gcd",
                   bn_divmod(&phi, &g, &lam2, 0) == 1, 0);
                ck("the key's lambda IS lcm(p-1,q-1), not phi",
                   bn_cmp(&k.lam, &lam2) == 0, 0);
                /* And they really are different numbers here, so the
                   check above is not passing by coincidence. */
                ck("  ... and phi differs from it on this key",
                   bn_mul(&phi, &pm1, &qm1) == 1
                   && bn_cmp(&phi, &lam2) != 0, 0);
            }

            /* THE ROUND TRIP, which is what the key is for. */
            bn_set_u32(&msg, 0);
            for(i = 0; i < 7; i++){
                bn_set_u32(&chk, 0x9E3779B9u);
                bn_mul(&msg, &msg, &chk);
                bn_set_u32(&chk, (uint32_t)(i + 12345));
                bn_add(&msg, &msg, &chk);
            }
            ck("  ... the test message is smaller than the modulus",
               bn_mod(&msg, &msg, &k.n) == 1, 0);
            ck("m^e mod n then ^d mod n is m again",
               bn_modexp(&ct, &msg, &k.e, &k.n) == 1
               && bn_modexp(&pt, &ct, &k.d, &k.n) == 1
               && bn_cmp(&pt, &msg) == 0, 0);
            ck("  ... and the ciphertext is not the plaintext",
               bn_cmp(&ct, &msg) != 0, 0);
            /* signing direction: m^d then ^e. PKCS#1 signing is this
               way round, so it is the one stage 3 calls. */
            ck("m^d mod n then ^e mod n is m again (the signing way)",
               bn_modexp(&ct, &msg, &k.d, &k.n) == 1
               && bn_modexp(&pt, &ct, &k.e, &k.n) == 1
               && bn_cmp(&pt, &msg) == 0, 0);
        }

        ck("stepping a finished generator still says DONE",
           rsa_keygen_step(&k) == RSA_KG_DONE, 0);
    }

    /* ── q MUST NOT BE ALLOWED TO EQUAL p ────────────────────────────
       n = p^2 is factored by taking a square root, so the check is
       the difference between a key and a decoration. It is also
       unreachable by waiting: two independent 256-bit primes collide
       with probability around 2^-250.

       So the collision is ARRANGED. Run until p is found, then point
       the entropy source at p's own limbs: new_candidate() fills
       each limb from the source and then forces the top two bits and
       the low one, all of which p already has, so the next candidate
       IS p. A correct generator rejects it, asks for another, gets p
       again, and eventually runs out of budget. One without the
       check returns a key whose two primes are the same number.

       "p and q are allowed to be equal" was MISSED before this,
       because nothing in the suite could ever produce the collision
       the check exists for. */
    printf("q is not allowed to equal p\n");
    {
        static rsa_keygen_t k;
        int r, steps = 0;

        ent_mode = ENT_PRNG;
        ck("begin", rsa_keygen_begin(&k, 512) == RSA_OK, 0);

        /* step until p exists */
        do {
            r = rsa_keygen_step(&k);
            if(++steps > 2000000) break;
        } while(r == RSA_KG_WORKING && !k.have_p);
        ck("p was found", k.have_p == 1, steps);
        ck("  ... and it is prime", rsa_is_probable_prime(&k.p, 16) == 1, 0);

        /* now every candidate will be p */
        bn_copy(&ent_replay, &k.p);
        ent_replay_i = 0;
        ent_mode = ENT_REPLAY;

        /* ONE WITNESS PER CANDIDATE FROM HERE, and the reason is
           runtime rather than rigour. Every candidate from here IS p,
           which is prime, so each survives trial division and then
           all 64 witnesses before the p == q check rejects it -- and
           the budget is 20000 candidates. That is 1.28 million
           256-bit modular exponentiations and it took this suite from
           thirty seconds to NINE MINUTES, which in turn put
           tools/check_rsa_suite.py past three hours and it was killed
           before finishing. A harness nobody can run to the end has
           proved nothing.

           The round count is asserted directly in the 512-bit block
           above, where it belongs. This block is about one thing: a
           generator that is handed the same prime twice must not
           return it as both p and q. */
        k.rounds_needed = 1;

        do {
            r = rsa_keygen_step(&k);
            if(++steps > 2000000){ r = RSA_KG_FAILED; break; }
        } while(r == RSA_KG_WORKING);
        ent_mode = ENT_PRNG;

        /* Either it gives up, or -- if it somehow finished -- the two
           primes are still different. Both are correct; returning a
           key with p == q is not. */
        if(r == RSA_KG_DONE){
            ck("it finished, and p != q anyway", bn_cmp(&k.p, &k.q) != 0, 0);
            ck("  ... and n is not a perfect square",
               bn_mul(&ent_replay, &k.p, &k.p) == 1
               && bn_cmp(&ent_replay, &k.n) != 0, 0);
        } else {
            ck("a source that only ever offers p gives up by name",
               r == RSA_KG_FAILED && k.why == RSA_EGIVEUP, (long)k.why);
            ck("  ... rather than returning a key with p == q",
               r != RSA_KG_DONE, (long)r);
        }
    }

    /* ── A MODULUS SIZE THAT IS NOT A MULTIPLE OF 64 ─────────────────
       new_candidate() has two branches for forcing the top two bits:
       one for a prime width that is a whole number of limbs, and one
       for a width that is not. 512 and 1024 and 2048 all take the
       first. THE SECOND HAD NEVER RUN, and "the candidate's top two
       bits are not forced" was MISSED because its injection lives
       there -- dead code in the one function that decides how many
       bits a key really has.

       rsa_keygen_begin() accepts any even size from 512 up, so 530
       reaches it: 265 bits per prime, 9 bits in the top limb. */
    printf("a 530-bit modulus, where the top limb is partly used\n");
    {
        static rsa_keygen_t k;
        int r, steps = 0;

        ent_mode = ENT_PRNG;
        ck("begin at 530 bits", rsa_keygen_begin(&k, 530) == RSA_OK, 0);
        {
            /* EVERY CANDIDATE, not just the two that became primes.
               Checking only the finished key leaves this to chance:
               with the second bit unforced a candidate still has it
               set half the time, so one key passes 25% of the time
               and the check reports MISSED about as often as not.
               A keygen examines dozens of candidates and all of them
               must have both top bits, which is a property with no
               coin flip in it. */
            bn_t prev; int sampled = 0, bad_hi = 0, bad_2nd = 0;
            bn_zero(&prev);
            do {
                r = rsa_keygen_step(&k);
                if(k.cand.n != 0 && bn_cmp(&k.cand, &prev) != 0){
                    bn_copy(&prev, &k.cand);
                    sampled++;
                    if(!bn_bit(&k.cand, 264)) bad_hi++;
                    if(!bn_bit(&k.cand, 263)) bad_2nd++;
                }
                if(++steps > 2000000){ r = RSA_KG_FAILED; break; }
            } while(r == RSA_KG_WORKING);
            ck("  many candidates were examined", sampled > 20, sampled);
            ck("  EVERY candidate had its highest bit set",
               bad_hi == 0, bad_hi);
            ck("  EVERY candidate had its second-highest bit set",
               bad_2nd == 0, bad_2nd);
        }
        ck("it finished", r == RSA_KG_DONE, (long)k.why);
        if(r == RSA_KG_DONE){
            ck("each prime is EXACTLY 265 bits",
               bn_bits(&k.p) == 265 && bn_bits(&k.q) == 265,
               (long)bn_bits(&k.p));
            /* THE TOP TWO BITS, which is what makes p*q 530 bits and
               not 529. Checking the modulus width alone is not
               enough: with only the high bit forced, p*q is 529 or
               530 depending on where in the range the primes landed,
               so that check passes about half the time and a flaky
               check is one nobody trusts. */
            ck("  ... with the HIGHEST bit set",
               bn_bit(&k.p, 264) == 1 && bn_bit(&k.q, 264) == 1, 0);
            ck("  ... and the SECOND highest set too",
               bn_bit(&k.p, 263) == 1 && bn_bit(&k.q, 263) == 1, 0);
            ck("so the modulus is EXACTLY 530 bits, never 529",
               bn_bits(&k.n) == 530, (long)bn_bits(&k.n));
            ck("p * q == n", bn_mul(&ent_replay, &k.p, &k.q) == 1
               && bn_cmp(&ent_replay, &k.n) == 0, 0);
        }
    }

    /* ── and the same two bits on the whole-limb branch ──────────────
       512 bits per modulus is 256 per prime, which is eight whole
       limbs, so the `else` arm forces 0xC0000000 in one go. It is
       the arm every supported key size actually takes and it had no
       check of its own either. */
    printf("the whole-limb branch forces the same two bits\n");
    {
        static rsa_keygen_t k;
        int r, steps = 0;
        ent_mode = ENT_PRNG;
        ck("begin at 512 bits", rsa_keygen_begin(&k, 512) == RSA_OK, 0);
        do {
            r = rsa_keygen_step(&k);
            if(++steps > 2000000){ r = RSA_KG_FAILED; break; }
        } while(r == RSA_KG_WORKING);
        ck("it finished", r == RSA_KG_DONE, (long)k.why);
        if(r == RSA_KG_DONE){
            ck("the highest bit of each prime is set",
               bn_bit(&k.p, 255) == 1 && bn_bit(&k.q, 255) == 1, 0);
            ck("  ... and the second highest",
               bn_bit(&k.p, 254) == 1 && bn_bit(&k.q, 254) == 1, 0);
            ck("so the modulus is EXACTLY 512 bits",
               bn_bits(&k.n) == 512, (long)bn_bits(&k.n));
        }
    }

    /* ── a stuck entropy source must FAIL, not spin ──────────────── */
    printf("a stuck entropy source is a named refusal, not a hang\n");
    {
        static rsa_keygen_t k;
        int r, steps = 0;
        ent_mode = ENT_ALL_2;       /* every candidate identical */
        ck("begin", rsa_keygen_begin(&k, 512) == RSA_OK, 0);
        do {
            r = rsa_keygen_step(&k);
            if(++steps > 4000000){ r = RSA_KG_WORKING; break; }
        } while(r == RSA_KG_WORKING && steps <= 4000000);
        ent_mode = ENT_PRNG;
        /* With entropy_u32() returning 2 for ever, every candidate is
           the same even-limbed value; it is forced odd and has the
           top two bits set, so it is one fixed number. Either it is
           prime -- in which case p == q is caught and the budget runs
           out -- or it is composite and the budget runs out. Both end
           in RSA_EGIVEUP. What must NOT happen is an unbounded loop. */
        ck("it gave up rather than spinning for ever",
           r == RSA_KG_FAILED, (long)r);
        ck("  ... and said why, by name",
           k.why == RSA_EGIVEUP, (long)k.why);
        ck("  ... which has a name",
           rsa_why_name(k.why)[0] != 0, 0);
    }
    /* ════════════════════════════════════════════════════════════════
       v0.8.9.4 -- AT THE SIZE THAT IS ACTUALLY USED

       Everything above generates 512-bit keys, because 512 is quick
       and the state machine is the same at any size. That reasoning
       is sound and it left a gap: the ONE caller that matters asks
       for 2048, and nothing had ever watched a 2048-bit key come out
       of this generator end to end.

       It costs about half a second. `pair` asks for two of them.
       ════════════════════════════════════════════════════════════════ */
    printf("\n  a 2048-bit key, the size pairing actually asks for\n");
    {
        rsa_keygen_t *k = (rsa_keygen_t *)malloc(sizeof *k);
        long steps = 0;
        int r = -1;

        ck("the allocation", k != 0, 0);
        if (k) {
            ck("begin at 2048 bits",
               rsa_keygen_begin(k, 2048) == RSA_OK, 0);
            for (;;) {
                r = rsa_keygen_step(k);
                if (r != RSA_KG_WORKING) break;
                if (++steps > 4000000L) break;
            }
            ck("it finishes", r == RSA_KG_DONE, r);
            /* THE CHECK THE SHELL DID NOT MAKE. A generator that says
               it is done and a key that is the size asked for are two
               different claims, and shell/paircmd.c trusted the first
               for three releases while copying a modulus of zero
               bits. */
            ck("the modulus is 2048 bits, not merely nonzero",
               bn_bits(&k->n) == 2048, (long)bn_bits(&k->n));
            ck("the private exponent is of the same order",
               bn_bits(&k->d) > 2000 && bn_bits(&k->d) <= 2048,
               (long)bn_bits(&k->d));
            ck("e is 65537", bn_bits(&k->e) == 17, (long)bn_bits(&k->e));
            /* It took real work. One step is what the broken caller
               accepted as a finished key. */
            ck("and it took hundreds of steps, not one", steps > 100,
               steps);

            /* And it is a KEY: a round trip through it. */
            {
                bn_t m, c, back;
                bn_set_u32(&m, 0);
                m.v[0] = 0xDEADBEEFu; m.v[1] = 0x12345678u; m.n = 2;
                ck("encrypt with e",
                   bn_modexp(&c, &m, &k->e, &k->n) == 1, 0);
                ck("decrypt with d",
                   bn_modexp(&back, &c, &k->d, &k->n) == 1, 0);
                ck("and the message comes back",
                   bn_cmp(&back, &m) == 0, 0);
            }
            free(k);
        }
    }



    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
