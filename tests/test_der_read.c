/* ================================================================
   tests/test_der_read.c -- reading DER, judged by openssl
   ================================================================
   net/der_read.c turns lockdownd's `DevicePublicKey` into two
   bignums. Everything downstream -- the device certificate, the pair
   request, the TLS session -- is built on those two numbers, and a
   modulus that is wrong in one byte produces a certificate the phone
   rejects with no explanation.

   openssl is the judge for every value here: the keys come from
   `openssl genrsa`, the encodings from `openssl rsa`, and the modulus
   this reader produces is compared against the one openssl prints.
   Our reader agreeing with our writer would prove nothing, and there
   is no writer for these two structures anyway.

   ── THE TWO CHECKS THAT ARE WORTH MORE THAN THE REST ────────────────

   1. THE TRUNCATION SWEEP. Every prefix of a valid 2048-bit key --
      all 294 of them -- must be refused, and none may read past the
      end of the buffer. A reader that handles the complete structure
      and walks off a short one is the normal shape of this bug, and
      a hand-picked "truncated" case finds it only by luck.

   2. THE BIT FLIP SWEEP. Every single byte of a valid key is
      corrupted in turn, and the result must be either a refusal or a
      DIFFERENT modulus -- never the same modulus. A reader that
      ignores a field it does not use will silently accept a key
      somebody edited.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "types.h"
#include "bignum.h"
#include "der_read.h"
#include "../net/bignum.c"
#include "../net/b64.c"
#include "../net/der_read.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

static uint8_t g_der[4096], g_scratch[4096];
static char    g_pem[8192];
static uint32_t g_derlen, g_pemlen;

/* ── WHY THESE READS GO THROUGH THE HEAP ─────────────────────────────
   A fixture read into a 4096-byte static array has 4096 bytes of
   legal memory behind it, so a reader that walks two bytes past the
   END OF THE DATA is still inside the array and neither ASAN nor this
   suite notices. Removing the "at least a tag and a length byte"
   guard from dr_tlv() survived the whole suite for exactly that
   reason.

   exact() copies to a malloc of precisely the right size. Under the
   ASAN build the Makefile uses, one byte past the data is then a
   fault with a stack trace, and the guard is tested rather than
   assumed. */
static uint8_t *exact(const void *src, uint32_t len)
{
    uint8_t *p = (uint8_t *)malloc(len ? len : 1);
    if(p && len) memcpy(p, src, len);
    return p;
}

static int slurp(const char *path, void *buf, uint32_t cap, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if(!f) return 0;
    n = fread(buf, 1, cap, f);
    fclose(f);
    *len = (uint32_t)n;
    return n > 0;
}

/* openssl's modulus, as a bignum, for comparison. */
static int openssl_modulus(const char *keypem, bn_t *out)
{
    char cmd[1024], line[2048];
    uint8_t raw[1024];
    uint32_t n = 0;
    FILE *f;
    int i;
    snprintf(cmd, sizeof cmd,
             "openssl rsa -in %s -noout -modulus 2>/dev/null "
             "| sed 's/^Modulus=//' > /tmp/hd_dr_mod.hex", keypem);
    if(system(cmd)) return 0;
    f = fopen("/tmp/hd_dr_mod.hex", "rb");
    if(!f) return 0;
    if(!fgets(line, sizeof line, f)){ fclose(f); return 0; }
    fclose(f);
    for(i = 0; line[i] && line[i+1]; ){
        int hi, lo;
        char a = line[i], b = line[i+1];
        if(a == '\n' || a == '\r') break;
        hi = (a >= '0' && a <= '9') ? a - '0'
           : (a >= 'A' && a <= 'F') ? a - 'A' + 10
           : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0'
           : (b >= 'A' && b <= 'F') ? b - 'A' + 10
           : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
        if(hi < 0 || lo < 0) break;
        if(n >= sizeof raw) return 0;
        raw[n++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }
    if(!n) return 0;
    /* bn_from_bytes returns NONZERO on success. */
    return bn_from_bytes(out, raw, n) != 0;
}

static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return system(cmd) == 0;
}

/* Generate a key and all four encodings of its public half. */
static int make_key(const char *tag, int bits, const char *pubexp)
{
    char pem[128];
    snprintf(pem, sizeof pem, "/tmp/hd_dr_%s.pem", tag);
    if(pubexp)
        { if(!sh("openssl genpkey -algorithm RSA "
                 "-pkeyopt rsa_keygen_bits:%d -pkeyopt rsa_keygen_pubexp:%s "
                 "-out %s >/dev/null 2>&1", bits, pubexp, pem)) return 0; }
    else
        { if(!sh("openssl genrsa -out %s %d >/dev/null 2>&1", pem, bits))
              return 0; }
    return sh("openssl rsa -in %s -pubout -outform DER -out "
              "/tmp/hd_dr_%s_spki.der >/dev/null 2>&1", pem, tag)
        && sh("openssl rsa -in %s -pubout -outform PEM -out "
              "/tmp/hd_dr_%s_spki.pem >/dev/null 2>&1", pem, tag)
        && sh("openssl rsa -in %s -RSAPublicKey_out -outform DER -out "
              "/tmp/hd_dr_%s_pk1.der >/dev/null 2>&1", pem, tag)
        && sh("openssl rsa -in %s -RSAPublicKey_out -outform PEM -out "
              "/tmp/hd_dr_%s_pk1.pem >/dev/null 2>&1", pem, tag);
}

static bn_t g_n, g_e, g_ref;

static void check_form(const char *tag, const char *form, int bits,
                       uint32_t want_e, int is_pem)
{
    char path[160], keypem[128], what[256];
    int r;
    snprintf(keypem, sizeof keypem, "/tmp/hd_dr_%s.pem", tag);
    snprintf(path, sizeof path, "/tmp/hd_dr_%s_%s.%s", tag, form,
             is_pem ? "pem" : "der");

    if(is_pem){
        if(!slurp(path, g_pem, sizeof g_pem, &g_pemlen)){
            snprintf(what, sizeof what, "%s %s: the fixture exists", tag, form);
            ck(what, 0, 0); return;
        }
        r = rsa_pub_from_pem(g_pem, g_pemlen, g_scratch,
                             sizeof g_scratch, &g_n, &g_e);
    } else {
        if(!slurp(path, g_der, sizeof g_der, &g_derlen)){
            snprintf(what, sizeof what, "%s %s: the fixture exists", tag, form);
            ck(what, 0, 0); return;
        }
        r = rsa_pub_from_der(g_der, g_derlen, &g_n, &g_e);
    }

    snprintf(what, sizeof what, "%d-bit %s %s reads", bits, form,
             is_pem ? "PEM" : "DER");
    ck(what, r == DER_OK, r);
    if(r != DER_OK) return;

    snprintf(what, sizeof what, "  ...the modulus is openssl's");
    ck(what, openssl_modulus(keypem, &g_ref) && bn_cmp(&g_n, &g_ref) == 0, 0);
    snprintf(what, sizeof what, "  ...and it is %d bits", bits);
    ck(what, (int)bn_bits(&g_n) == bits, (long)bn_bits(&g_n));
    {
        bn_t we;
        bn_set_u32(&we, want_e);
        snprintf(what, sizeof what, "  ...the exponent is %u", want_e);
        ck(what, bn_cmp(&g_e, &we) == 0, 0);
    }
}

int main(void)
{
    printf("\n=== test_der_read: DER public keys, judged by openssl ===\n");

    if(system("which openssl >/dev/null 2>&1") != 0){
        printf("\n  openssl is not installed, and it is the judge for every\n"
               "  value here. SKIP, not a pass.\n\nSKIP\n");
        return 77;
    }

    head("generating keys with openssl");
    ck("2048-bit, e=65537", make_key("k2048", 2048, 0), 0);
    ck("1024-bit, e=65537", make_key("k1024", 1024, 0), 0);
    ck("4096-bit, e=65537", make_key("k4096", 4096, 0), 0);
    /* e=3 is a legal public exponent and a different number of bytes
       in the encoding, which is where an off-by-one in the INTEGER
       reader shows up. */
    ck("2048-bit, e=3",     make_key("ke3",   2048, "3"), 0);

    head("all four encodings of the same key");
    check_form("k2048", "spki", 2048, 65537, 0);
    check_form("k2048", "pk1",  2048, 65537, 0);
    check_form("k2048", "spki", 2048, 65537, 1);
    check_form("k2048", "pk1",  2048, 65537, 1);

    head("other sizes, and a different exponent");
    check_form("k1024", "spki", 1024, 65537, 0);
    check_form("k4096", "spki", 4096, 65537, 0);
    check_form("k4096", "pk1",  4096, 65537, 1);
    check_form("ke3",   "spki", 2048, 3, 0);
    check_form("ke3",   "pk1",  2048, 3, 1);

    /* ── THE TRUNCATION SWEEP ────────────────────────────────────── */
    head("every prefix of a valid key is refused, and none of them crash");
    {
        int bad = -1, ok_count = 0;
        uint32_t i;
        slurp("/tmp/hd_dr_k2048_spki.der", g_der, sizeof g_der, &g_derlen);
        ck("the full key is this long", g_derlen > 200, (long)g_derlen);
        for(i = 0; i < g_derlen; i++){
            /* Each prefix in a buffer of exactly its own length, so a
               read one byte past it is a fault and not a value. */
            uint8_t *q = exact(g_der, i);
            int r = rsa_pub_from_der(q, i, &g_n, &g_e);
            free(q);
            if(r == DER_OK){ bad = (int)i; break; }
            ok_count++;
        }
        /* Not one prefix may parse. A reader that accepts a short
           structure has stopped checking something. */
        ck("not one of them parses", bad < 0, bad);
        ck("...and all of them were tried",
           ok_count == (int)g_derlen, ok_count);
        ck("the full length still parses",
           rsa_pub_from_der(g_der, g_derlen, &g_n, &g_e) == DER_OK, 0);
    }

    /* ── THE BIT FLIP SWEEP ──────────────────────────────────────── */
    head("every byte corrupted in turn: refused, or a different key");
    {
        bn_t good_n, good_e;
        uint32_t i;
        int same = -1, refused = 0, changed = 0;
        slurp("/tmp/hd_dr_k2048_spki.der", g_der, sizeof g_der, &g_derlen);
        rsa_pub_from_der(g_der, g_derlen, &good_n, &good_e);
        for(i = 0; i < g_derlen; i++){
            uint8_t save = g_der[i];
            g_der[i] ^= 0xFF;
            if(rsa_pub_from_der(g_der, g_derlen, &g_n, &g_e) != DER_OK)
                refused++;
            /* BOTH halves. The first version of this sweep compared
               only the modulus and reported byte 292 as a survivor --
               which is the low byte of the public exponent 0x010001.
               Corrupting it changes e and leaves n alone, so "the
               same modulus" was the wrong invariant: a key is the
               PAIR, and a reader that lets somebody edit e has let
               them edit the key. */
            else if(bn_cmp(&g_n, &good_n) != 0 || bn_cmp(&g_e, &good_e) != 0)
                changed++;
            else { same = (int)i; g_der[i] = save; break; }
            g_der[i] = save;
        }
        /* The forbidden outcome is the same key from different bytes:
           that means a byte was read and ignored, and a key somebody
           edited would be accepted as the original. */
        ck("no corruption yields the SAME key", same < 0, same);
        ck("...and every byte was tried",
           refused + changed == (int)g_derlen, refused + changed);
        printf("          %d refused, %d gave a different key\n",
               refused, changed);
        ck("the original still parses after the sweep",
           rsa_pub_from_der(g_der, g_derlen, &g_n, &g_e) == DER_OK
           && bn_cmp(&g_n, &good_n) == 0 && bn_cmp(&g_e, &good_e) == 0, 0);
    }

    /* ── structures this reader will not accept ──────────────────────
       EVERY MUTANT HERE STARTS AS A REAL KEY. The first version of
       this block used hand-typed toy structures --
       SEQUENCE { INTEGER 1, INTEGER 1 } with one encoding rule
       broken -- and twelve injected defects survived the suite
       because of it: a 1-bit modulus is refused by the minimum
       key-size check no matter what the length rules do, so those
       fixtures proved the reader says no, not that it says no for the
       reason under test.

       tests/der_mutants.py takes an openssl key apart and puts it
       back with exactly one thing wrong. If one of these is accepted,
       the rule it breaks is not being enforced. */
    head("malformed encodings, each built from a real key");
    {
        static const struct { const char *file; const char *what; } MUT[] = {
          { "mut_indef.der",
            "an indefinite outer length (legal BER, illegal DER)" },
          { "mut_overlong.der",
            "an outer length padded to three bytes" },
          { "mut_short_outer.der",
            "an outer length one byte short of its body" },
          { "mut_neg_modulus.der",
            "the modulus with its sign byte removed -- now negative" },
          { "mut_padded_modulus.der",
            "the modulus with a second leading zero (non-minimal)" },
          { "mut_extra_field.der",
            "a third INTEGER inside RSAPublicKey" },
          { "mut_high_tag.der",
            "the modulus under the high-tag-number form" },
          { "mut_trailing_pk1.der",
            "one byte after a complete PKCS#1 key" },
          { "mut_bitstring_unused.der",
            "a BIT STRING claiming one unused bit" },
          { "mut_alg_len_padded.der",
            "a length under 128 encoded in two bytes" },
          { "mut_short_oid.der",
            "an algorithm OID claiming a length of one" },
        };
        unsigned m;
        ck("the mutants were generated",
           sh("mkdir -p /tmp/hd_dr_mut && python3 -I tests/der_mutants.py "
              "/tmp/hd_dr_k2048_spki.der /tmp/hd_dr_k2048_pk1.der "
              "/tmp/hd_dr_mut >/dev/null 2>&1"), 0);
        for(m = 0; m < sizeof MUT / sizeof MUT[0]; m++){
            char path[256];
            uint32_t len = 0;
            snprintf(path, sizeof path, "/tmp/hd_dr_mut/%s", MUT[m].file);
            if(!slurp(path, g_der, sizeof g_der, &len)){
                ck(MUT[m].what, 0, -1);
                continue;
            }
            {
                uint8_t *q = exact(g_der, len);
                int r = rsa_pub_from_der(q, len, &g_n, &g_e);
                free(q);
                ck(MUT[m].what, r != DER_OK, r);
            }
        }
        ck("an empty buffer",
           rsa_pub_from_der(g_der, 0, &g_n, &g_e) != DER_OK, 0);
        ck("a NULL buffer",
           rsa_pub_from_der(0, 100, &g_n, &g_e) != DER_OK, 0);
        /* One and two bytes, each in an allocation of exactly that
           size: the path where the reader needs a tag AND a length
           byte before it may dereference either. */
        {
            uint8_t *q1 = exact("\x30", 1), *q2 = exact("\x30\x81", 2);
            ck("a one-byte buffer",
               rsa_pub_from_der(q1, 1, &g_n, &g_e) != DER_OK, 0);
            ck("a two-byte buffer",
               rsa_pub_from_der(q2, 2, &g_n, &g_e) != DER_OK, 0);
            free(q1); free(q2);
        }
    }

    /* ── a key for a DIFFERENT ALGORITHM ─────────────────────────────
       An EC public key is a perfectly well-formed
       SubjectPublicKeyInfo whose AlgorithmIdentifier is not
       rsaEncryption and whose BIT STRING holds a curve point rather
       than a SEQUENCE. It is the fixture that tests the OID check for
       real, because the OID differs in LENGTH as well as in value. */
    head("a key that is not RSA");
    {
        uint32_t len = 0;
        if(sh("openssl ecparam -name prime256v1 -genkey -noout "
              "-out /tmp/hd_dr_ec.pem >/dev/null 2>&1")
           && sh("openssl ec -in /tmp/hd_dr_ec.pem -pubout -outform DER "
                 "-out /tmp/hd_dr_ec_spki.der >/dev/null 2>&1")
           && slurp("/tmp/hd_dr_ec_spki.der", g_der, sizeof g_der, &len)){
            int r = rsa_pub_from_der(g_der, len, &g_n, &g_e);
            ck("an EC P-256 public key is refused", r != DER_OK, r);
            printf("          %s\n", der_read_strerror(r));
        } else {
            printf("    (openssl has no prime256v1 -- skipping)\n");
        }
        /* And a DSA key, whose AlgorithmIdentifier parameters are a
           SEQUENCE rather than NULL. */
        if(sh("openssl dsaparam -genkey 1024 -out /tmp/hd_dr_dsa.pem "
              ">/dev/null 2>&1")
           && sh("openssl dsa -in /tmp/hd_dr_dsa.pem -pubout -outform DER "
                 "-out /tmp/hd_dr_dsa_spki.der >/dev/null 2>&1")
           && slurp("/tmp/hd_dr_dsa_spki.der", g_der, sizeof g_der, &len)){
            ck("a DSA public key is refused",
               rsa_pub_from_der(g_der, len, &g_n, &g_e) != DER_OK, 0);
        } else {
            printf("    (no DSA in this openssl -- skipping)\n");
        }
    }

    /* ── dr_enter, directly ──────────────────────────────────────────
       No caller passes it a primitive tag, so the guard inside it is
       unreachable through rsa_pub_from_der() and an injection that
       removes it survives the whole suite. Checked here instead of
       left as code nothing exercises. */
    head("dr_tlv needs a tag AND a length byte before it reads either");
    {
        /* rsa_pub_from_der() refuses anything under eight bytes before
           it reaches dr_tlv at all, so the tiny-buffer checks above
           never exercise this -- which is why an injection removing
           the guard survived the suite twice. Called directly, on
           allocations of exactly one and two bytes, so that reading
           the second byte of a one-byte buffer is a fault under ASAN
           rather than whatever happens to follow in a static array. */
        uint8_t *q0 = exact("", 0);
        uint8_t *q1 = exact("\x30", 1);
        uint8_t *q2 = exact("\x30\x00", 2);
        dr_t d;
        dr_init(&d, q0, 0);
        ck("zero bytes is not a TLV", dr_tlv(&d, 0, 0, 0, 0) != DER_OK, 0);
        dr_init(&d, q1, 1);
        ck("one byte is not a TLV", dr_tlv(&d, 0, 0, 0, 0) != DER_OK, 0);
        dr_init(&d, q2, 2);
        ck("two bytes is a TLV with an empty value",
           dr_tlv(&d, 0, 0, 0, 0) == DER_OK, 0);
        free(q0); free(q1); free(q2);
    }

    head("dr_tlv with no expected tag still refuses the high-tag form");
    {
        /* The only caller that passes want = 0 is the format sniff in
           rsa_pub_from_der(), which refuses an unknown tag anyway --
           so the high-tag guard is unreachable through the public API
           and an injection removing it survived the suite. Checked
           directly, because an unreachable guard is either tested or
           deleted. */
        uint8_t *q = exact("\x1f\x01\x02\x01\x01", 5);
        dr_t d;
        uint8_t tag = 0;
        dr_init(&d, q, 5);
        ck("tag number 31 is refused even with want = 0",
           dr_tlv(&d, 0, &tag, 0, 0) != DER_OK, tag);
        free(q);
    }

    head("dr_enter refuses to descend into a primitive tag");
    {
        static const uint8_t intseq[] = { 0x02, 0x02, 0x30, 0x00 };
        dr_t d, in;
        dr_init(&d, intseq, sizeof intseq);
        ck("an INTEGER is not something to enter",
           dr_enter(&d, DER_T_INTEGER, &in) != DER_OK, 0);
        dr_init(&d, intseq, sizeof intseq);
        ck("...and the cursor did not move", dr_left(&d) == 4,
           (long)dr_left(&d));
    }

    head("a valid key with a byte appended is refused, not ignored");
    {
        slurp("/tmp/hd_dr_k2048_spki.der", g_der, sizeof g_der, &g_derlen);
        g_der[g_derlen] = 0x00;
        /* Trailing data means the caller was handed two things and
           told it had one. Ignoring it is how a parser reads the
           first of two keys and reports success. */
        ck("one trailing zero byte",
           rsa_pub_from_der(g_der, g_derlen + 1, &g_n, &g_e) != DER_OK, 0);
        g_der[g_derlen] = 0x30;
        ck("a trailing SEQUENCE tag",
           rsa_pub_from_der(g_der, g_derlen + 1, &g_n, &g_e) != DER_OK, 0);
        /* And on the PKCS#1 form, which reaches a DIFFERENT trailing
           check -- the SPKI one above does not exercise it, which is
           how an injection that removed it survived. */
        slurp("/tmp/hd_dr_k2048_pk1.der", g_der, sizeof g_der, &g_derlen);
        g_der[g_derlen] = 0x00;
        ck("one trailing byte on a PKCS#1 key",
           rsa_pub_from_der(g_der, g_derlen + 1, &g_n, &g_e) != DER_OK, 0);
    }

    /* ── a PRIVATE key must not read as a public one ─────────────── */
    head("a private key is refused, not silently misread");
    {
        /* RSAPrivateKey is SEQUENCE { version INTEGER 0, n, e, d, ... }.
           A reader that just takes the first two INTEGERs gets
           version as the modulus and the real modulus as the
           exponent -- a "key" with a 0-bit modulus. This is the one
           misreading that would not look like an error. */
        ck("the fixture exists",
           sh("openssl rsa -in /tmp/hd_dr_k2048.pem -outform DER -out "
              "/tmp/hd_dr_priv.der >/dev/null 2>&1"), 0);
        if(slurp("/tmp/hd_dr_priv.der", g_der, sizeof g_der, &g_derlen)){
            int r = rsa_pub_from_der(g_der, g_derlen, &g_n, &g_e);
            ck("it is refused", r != DER_OK, r);
            printf("          %s\n", der_read_strerror(r));
        } else ck("the private DER could be read", 0, 0);
    }

    /* ── PEM handling ────────────────────────────────────────────── */
    head("PEM: the armor is skipped and the label is not consulted");
    {
        slurp("/tmp/hd_dr_k2048_spki.pem", g_pem, sizeof g_pem, &g_pemlen);
        ck("the real thing reads",
           rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, sizeof g_scratch,
                            &g_n, &g_e) == DER_OK, 0);
        /* lockdownd has sent DevicePublicKey under both
           "BEGIN RSA PUBLIC KEY" and "BEGIN PUBLIC KEY" depending on
           the iOS version. The structure decides, so a mislabelled
           SPKI must still read. */
        ck("...and so does the same body under the WRONG label",
           sh("sed 's/PUBLIC KEY/RSA PUBLIC KEY/' "
              "/tmp/hd_dr_k2048_spki.pem > /tmp/hd_dr_mislabel.pem")
           && slurp("/tmp/hd_dr_mislabel.pem", g_pem, sizeof g_pem, &g_pemlen)
           && rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, sizeof g_scratch,
                               &g_n, &g_e) == DER_OK, 0);
        ck("...with the same modulus",
           openssl_modulus("/tmp/hd_dr_k2048.pem", &g_ref)
           && bn_cmp(&g_n, &g_ref) == 0, 0);
        /* A plist <data> value that has already been base64-decoded
           once arrives as bare base64 with no armor at all. */
        ck("bare base64 with no armor reads",
           sh("grep -v -- ----- /tmp/hd_dr_k2048_spki.pem "
              "> /tmp/hd_dr_bare.txt")
           && slurp("/tmp/hd_dr_bare.txt", g_pem, sizeof g_pem, &g_pemlen)
           && rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, sizeof g_scratch,
                               &g_n, &g_e) == DER_OK, 0);
        ck("CRLF line endings read",
           sh("sed 's/$/\\r/' /tmp/hd_dr_k2048_spki.pem "
              "> /tmp/hd_dr_crlf.pem")
           && slurp("/tmp/hd_dr_crlf.pem", g_pem, sizeof g_pem, &g_pemlen)
           && rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, sizeof g_scratch,
                               &g_n, &g_e) == DER_OK, 0);
        ck("a PEM body with a character outside base64 is refused",
           sh("sed 's/^MII/M!I/' /tmp/hd_dr_k2048_spki.pem "
              "> /tmp/hd_dr_bad.pem")
           && slurp("/tmp/hd_dr_bad.pem", g_pem, sizeof g_pem, &g_pemlen)
           && rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, sizeof g_scratch,
                               &g_n, &g_e) != DER_OK, 0);
        ck("a scratch buffer too small is refused, not truncated",
           slurp("/tmp/hd_dr_k2048_spki.pem", g_pem, sizeof g_pem, &g_pemlen)
           && rsa_pub_from_pem(g_pem, g_pemlen, g_scratch, 16,
                               &g_n, &g_e) != DER_OK, 0);
    }

    head("a key too small to be a key is refused");
    {
        /* SEQUENCE { INTEGER 5, INTEGER 3 } -- structurally a perfect
           RSAPublicKey and cryptographically nothing. Refused here so
           the pairing code never has to ask. */
        static const uint8_t tiny[] = { 0x30, 0x06, 0x02, 0x01, 0x05,
                                        0x02, 0x01, 0x03 };
        ck("a 3-bit modulus",
           rsa_pub_from_der(tiny, sizeof tiny, &g_n, &g_e) != DER_OK, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
