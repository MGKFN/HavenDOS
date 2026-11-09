#pragma once
#include "types.h"
#include "bignum.h"

/* ════════════════════════════════════════════════════════════════════
   include/rsa.h -- primality testing and RSA key generation.  v0.8.8.2.

   PAIRING STAGE 3, PIECE TWO. The pair request carries a host
   certificate and a device certificate the host signed, so the host
   needs a key of its own, and nothing in this tree could make one:
   net/tls_crypto.c had a public-key operation and nothing else.

   ── THE SPEED QUESTION IS ANSWERED, AND THE OLD NUMBER WAS RIGHT ────

   v0.8.7.9's notes asked "whether RSA keygen is practical on this
   kernel at all" and the handoff's estimate was 15 to 25 minutes.
   That estimate was correct FOR THE CODE THAT EXISTED: byte-wise
   double-and-add at 0.073 s per exponent byte put one Miller-Rabin
   round on a 1024-bit candidate at about 2.3 s.

   Measured on net/bignum.c, same machine, same method:

       one 1024-bit modexp (= one Miller-Rabin round)      5.2 ms
       one 2048-bit private-key operation                 28.3 ms
       one 2048-bit public operation, e = 65537            0.81 ms

   Three orders of magnitude, which is what Montgomery reduction is
   for. A keygen is a few hundred of those rounds, so it is seconds
   here and a minute or two on the HP -- NOT measured on the HP, and
   the figure above is a fast 64-bit container CPU running 64-bit
   code. Measure it there before quoting it.

   ── WHY IT IS A STATE MACHINE AND NOT A FUNCTION ────────────────────

   include/kpoll.h: "kpoll_sleep_ms() may not appear in drivers/, usb/,
   net/ or fs/ at all", and tools/check_kpoll_sites.py enforces it.
   This file lives in net/, so it CANNOT yield, and a keygen that
   cannot yield is a frozen machine with no way to tell a slow prime
   search from a hang.

   Adding a crypto/ directory to sidestep a rule about four directory
   names would be obeying its letter and breaking it. A progress
   CALLBACK into the caller would be worse: it puts kernel_poll() at
   the bottom of a net/ call stack, which is precisely the reentrancy
   the bottom half's guards exist to refuse.

   So the loop belongs to the caller. rsa_keygen_step() does a bounded
   piece of work -- at most one Miller-Rabin round, which is one
   modexp -- and returns. shell/ owns the loop, the polling, the
   progress line and the keypress that aborts it. Interruptibility is
   then structural rather than promised.

   ── SIZE: THIS DOES NOT GO ON THE STACK ─────────────────────────────

   An rsa_keygen_t holds seven bn_t and is over 3.5 KB. The kernel's
   stack is not large. Put it in a static or on the heap; a local will
   not fail here, it will corrupt whatever is below it.
   ════════════════════════════════════════════════════════════════════ */

/* ── refusals, each its own code ─────────────────────────────────────
   Never a bare zero. v0.8.7.7 spent a release on what a nameless
   refusal costs. */
#define RSA_OK           0
#define RSA_EBITS        1   /* modulus size not supported            */
#define RSA_ESTATE       2   /* step() called on a finished generator */
#define RSA_EGIVEUP      3   /* candidates exhausted -- see the note  */
#define RSA_EARITH       4   /* the bignum refused something          */
#define RSA_ENOINV       5   /* gcd(e, lambda) != 1 every time        */
#define RSA_EABORT       6   /* the caller asked to stop              */

const char *rsa_why_name(int why);

/* ── primality ───────────────────────────────────────────────────────

   MILLER-RABIN, NOT FERMAT, and the difference is a whole class of
   number. A Fermat test accepts every Carmichael number -- 561, 1105,
   1729 and infinitely many more -- for every base coprime to it. An
   implementation that computes a^(n-1) mod n and compares with 1 is
   not a weaker primality test, it is a wrong one, and it is wrong on
   inputs a random candidate search will eventually produce.

   rounds: witnesses to try. Each is one modexp. 64 is the usual
   number for a key that will be kept; a candidate search uses 1 and
   then re-tests the survivor properly, because almost every composite
   dies on the first witness.

   Returns 1 probably prime, 0 composite, -RSA_* on a refusal. */
int rsa_is_probable_prime(const bn_t *n, uint32_t rounds);

/* Trial division by the small primes, as a cheap pre-filter. Returns
   0 composite, 1 "not divisible by any of them", and it is the
   difference between a keygen of seconds and one of minutes: about
   four in five odd candidates die here, for the price of a few
   thousand single-limb remainders instead of a modexp. */
int rsa_trial_divide(const bn_t *n);

/* ── key generation ──────────────────────────────────────────────── */

#define RSA_KG_WORKING   1   /* call step() again                     */
#define RSA_KG_DONE      2   /* k->n, k->e, k->d are the key          */
#define RSA_KG_FAILED    3   /* k->why says which refusal             */

typedef struct {
    /* the key, once RSA_KG_DONE */
    bn_t     n, e, d;
    bn_t     p, q;
    bn_t     lam;            /* lcm(p-1, q-1): d is the inverse mod
                                THIS, not mod (p-1)(q-1) -- RFC 8017,
                                and net/bignum.c's header says what
                                getting that wrong looks like       */
    bn_t     cand;           /* the candidate being examined          */

    uint32_t prime_bits;     /* half the modulus                      */
    uint32_t stage;          /* internal                              */
    uint32_t tried;          /* candidates examined, both primes      */
    uint32_t rounds_done;    /* witnesses passed by `cand` so far     */
    uint32_t rounds_needed;
    int      have_p;
    int      why;
} rsa_keygen_t;

/* modulus_bits must be even and between 512 and BN_BITS/2. 2048 is
   what pairing needs; 512 exists so a suite can run the whole thing
   in reasonable time, and it is NOT a key size to use. */
int rsa_keygen_begin(rsa_keygen_t *k, uint32_t modulus_bits);

/* One bounded piece of work. RSA_KG_WORKING, RSA_KG_DONE or
   RSA_KG_FAILED. The caller polls and prints between calls. */
int rsa_keygen_step(rsa_keygen_t *k);

/* Progress, for the line the caller prints: 0 to 100. It is an
   HONEST ESTIMATE AND NOT A FRACTION OF ANYTHING KNOWN -- a prime
   search has no length. It is derived from how many candidates have
   been examined against how many a modulus of this size usually
   needs, so it can sit still and it can jump. A progress bar that
   pretends to know is worse than a spinner. */
uint32_t rsa_keygen_progress(const rsa_keygen_t *k);

/* ── the witness source ──────────────────────────────────────────────
   kernel/entropy.c supplies this in the kernel. A native suite
   supplies its own, seeded, so runs are reproducible -- and that is
   legitimate here in a way a stubbed driver is not: a primality
   DECISION must not depend on which witnesses were drawn, and
   tests/test_rsa.c proves it by also running fixed adversarial bases
   against the numbers chosen to defeat them. */
extern uint32_t entropy_u32(void);
