/* ================================================================
   tests/test_x509.c -- certificates, judged by openssl
   ================================================================
   Pairing stage 3.  The thing being built here is a certificate a
   PHONE has to accept, and the failure this suite exists to prevent
   is the one the handoff names: "a broken RSA that looks like it
   works is the worst outcome this project could produce."

   So nothing in this file parses a certificate. openssl does:

     openssl x509      -- does it parse at all, and are the
                          extensions there, critical, and right
     openssl asn1parse -- is it DER, not merely BER
     openssl verify    -- does the SIGNATURE check out against the
                          issuer, which is the only question that
                          matters and the only one our own code
                          cannot be allowed to answer

   The keys come from `openssl genrsa` rather than from
   rsa_keygen_step(), for two reasons: a 2048-bit keygen is 15-25
   minutes and would make this suite unrunnable, and a key from
   outside means a signature failure is OUR arithmetic rather than
   possibly our key.

   And because a judge that has never convicted anybody is not a
   judge, the suite flips one byte inside a signed certificate and
   requires openssl to reject it.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "types.h"
#include "bignum.h"
#include "der.h"
#include "x509.h"

#include "../net/bignum.c"
#include "../net/der.c"
#include "../net/x509.c"

/* sha256 lives in net/tls_crypto.c, which also carries a lot that is
   not wanted here; the one function is declared and that file is
   compiled in by the Makefile rule for this suite. */

/* net/tls_crypto.c is compiled in for its sha256(), and its
   rsa_public_encrypt() wants an entropy source from the kernel. The
   stub is never reached: nothing in this suite calls that function,
   and the certificate signature uses the PRIVATE key operation in
   net/x509.c, which takes no randomness at all. PKCS#1 v1.5 SIGNING
   is deterministic -- that is a property of the padding, not an
   omission here. */
uint32_t entropy_u32(void){ return 0x9E3779B9u; }

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

static int run(const char *cmd)
{
    char b[1200];
    snprintf(b, sizeof b, "%s > /tmp/hd_x509.out 2>&1", cmd);
    return system(b);
}

static int grep_out(const char *needle)
{
    char b[600];
    snprintf(b, sizeof b, "grep -q -- \"%s\" /tmp/hd_x509.out", needle);
    return system(b) == 0;
}

/* ── a key, made outside ─────────────────────────────────────────── */
typedef struct { bn_t n, e, d; } rsakey_t;

static int hex_to_bn(const char *path, bn_t *out)
{
    static uint8_t raw[1024];
    FILE *f = fopen(path, "rb");
    char  hex[4096];
    size_t got, i, n = 0;
    if(!f) return 0;
    got = fread(hex, 1, sizeof hex - 1, f);
    fclose(f);
    hex[got] = 0;
    for(i = 0; i + 1 < got; ){
        int hi, lo;
        char a = hex[i], b2 = hex[i+1];
        if(a == '\n' || a == ' ' || a == ':'){ i++; continue; }
        hi = (a >= '0' && a <= '9') ? a - '0'
           : (a >= 'a' && a <= 'f') ? a - 'a' + 10
           : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
        lo = (b2 >= '0' && b2 <= '9') ? b2 - '0'
           : (b2 >= 'a' && b2 <= 'f') ? b2 - 'a' + 10
           : (b2 >= 'A' && b2 <= 'F') ? b2 - 'A' + 10 : -1;
        if(hi < 0 || lo < 0) break;
        if(n >= sizeof raw) return 0;
        raw[n++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }
    if(!n) return 0;
    return bn_from_bytes(out, raw, (uint32_t)n);
}

static int make_key(const char *tag, int bits, rsakey_t *k)
{
    char cmd[1200];
    char pem[128], nh[128], dh[128];

    snprintf(pem, sizeof pem, "/tmp/hd_x509_%s.pem", tag);
    snprintf(nh,  sizeof nh,  "/tmp/hd_x509_%s_n.hex", tag);
    snprintf(dh,  sizeof dh,  "/tmp/hd_x509_%s_d.hex", tag);

    snprintf(cmd, sizeof cmd,
             "openssl genrsa -out %s %d >/dev/null 2>&1", pem, bits);
    if(system(cmd)) return 0;

    /* The modulus comes out of openssl already clean. */
    snprintf(cmd, sizeof cmd,
             "openssl rsa -in %s -noout -modulus 2>/dev/null "
             "| sed 's/^Modulus=//' > %s", pem, nh);
    if(system(cmd)) return 0;

    /* The private exponent is a colon-separated block between two
       labels in the -text output. */
    snprintf(cmd, sizeof cmd,
             "openssl rsa -in %s -text -noout 2>/dev/null "
             "| awk '/^privateExponent:/{f=1;next} /^prime1:/{f=0} f' "
             "| tr -d ' :\\n' > %s", pem, dh);
    if(system(cmd)) return 0;

    if(!hex_to_bn(nh, &k->n)) return 0;
    if(!hex_to_bn(dh, &k->d)) return 0;
    bn_set_u32(&k->e, 65537);
    return 1;
}

static int write_der(const char *path, const uint8_t *p, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    size_t w;
    if(!f) return 0;
    w = fwrite(p, 1, n, f);
    fclose(f);
    return w == n;
}

static uint8_t g_root[8192], g_host[8192];
static uint32_t g_rootlen, g_hostlen;

int main(void)
{
    rsakey_t rk, hk;
    x509_req_t q;
    int r;

    printf("X.509 v3 and PKCS#1 v1.5, as judged by openssl\n");
    printf("==============================================\n");

    if(system("which openssl > /dev/null 2>&1") != 0){
        printf("\n  openssl is not installed. Every judge in this suite is\n"
               "  openssl, so this is a SKIP and not a pass.\n");
        printf("\nSKIP\n");
        return 77;
    }

    /* ── 1: the padding, on its own ──────────────────────────────── */
    head("EMSA-PKCS1-v1_5, against RFC 8017 by construction");
    {
        uint8_t em[256];
        uint8_t dg[32];
        int i;
        for(i = 0; i < 32; i++) dg[i] = (uint8_t)i;

        ck("a 256-byte modulus pads", pkcs1_pad_sha256(256, dg, em, sizeof em)
           == X509_OK, 0);
        ck("it starts 00 01", em[0] == 0x00 && em[1] == 0x01, em[1]);
        ck("the padding is 0xFF", em[2] == 0xFF && em[200] == 0xFF, em[2]);
        ck("a single 0x00 separates padding from the DigestInfo",
           em[256 - 51 - 1] == 0x00, em[256 - 51 - 1]);
        ck("the DigestInfo follows immediately",
           em[256 - 51] == 0x30 && em[256 - 50] == 0x31, em[256 - 51]);
        ck("the digest is at the end",
           em[256 - 32] == 0 && em[255] == 31, em[255]);
        ck("a modulus too small for the DigestInfo is REFUSED, not "
           "truncated",
           pkcs1_pad_sha256(60, dg, em, sizeof em) == X509_E_KEY, 0);
    }

    /* ── 2: two keys from outside ────────────────────────────────── */
    head("two 2048-bit keys, generated by openssl");
    ck("the root key loads",  make_key("root", 2048, &rk), 0);
    ck("the host key loads",  make_key("host", 2048, &hk), 0);
    ck("the root modulus is 2048 bits", bn_bits(&rk.n) == 2048,
       (long)bn_bits(&rk.n));
    ck("the host modulus is 2048 bits", bn_bits(&hk.n) == 2048,
       (long)bn_bits(&hk.n));
    if(fails){ printf("\n%d/%d passed\n", total - fails, total); return 1; }

    /* ── 2b: I2OSP -- THE SIGNATURE IS ALWAYS k BYTES ────────────
       Found by injection, not by thinking about it. The suite as
       first written passed 42/42 against a net/x509.c whose
       bn_to_bytes() call asked for the signature's OWN byte length
       instead of the modulus length -- the exact defect the comment
       beside that line warns about. It passed because with this key
       and this digest the signature happened to have a non-zero top
       byte, which it does 255 times in 256.

       So the case is SEARCHED FOR rather than hoped for: sign a
       counter until one turns up whose signature is numerically
       small enough to lose its leading byte, then require the
       encoding to have kept it. Roughly 256 signatures expected,
       which is a second of arithmetic and buys a check that cannot
       pass by luck. */
    head("a signature whose top byte is zero is still k bytes long");
    {
        uint32_t k = (bn_bits(&rk.n) + 7u) / 8u;
        uint8_t  sig[512], dg[32];
        uint32_t slen = 0, tries;
        int found = 0;

        for(tries = 0; tries < 4000 && !found; tries++){
            uint8_t ctr[4];
            ctr[0] = (uint8_t)tries; ctr[1] = (uint8_t)(tries >> 8);
            ctr[2] = 0; ctr[3] = 0;
            sha256(ctr, 4, dg);
            if(pkcs1_sign_sha256(&rk.n, &rk.d, dg, sig, sizeof sig, &slen)
               != X509_OK) break;
            /* Either the interesting case (a leading zero that a
               natural-length encoding would drop) or an encoding
               that is ALREADY short -- both end the search, so a
               broken implementation fails on the length check below
               and says so, rather than running out of tries and
               reporting the search itself as the problem. */
            if(slen != k || sig[0] == 0x00) found = 1;
        }
        ck("a short signature was found to test with (expected ~256 tries)",
           found, (long)tries);
        if(found){
            ck("...and it is still exactly the modulus length",
               slen == k, (long)slen);
            ck("...with the leading zero kept, as I2OSP requires",
               sig[0] == 0x00, sig[0]);
        }
    }

    /* ── 3: the root CA, self-signed ─────────────────────────────── */
    head("a self-signed root CA");
    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Root CA";
    q.issuer_cn  = "HavenDOS Root CA";
    q.serial     = 1;
    q.subject_n  = &rk.n; q.subject_e = &rk.e;
    q.issuer_n   = &rk.n; q.issuer_d  = &rk.d;
    q.is_ca      = 1;
    q.key_usage  = X509_KU_KEY_CERT_SIGN | X509_KU_CRL_SIGN;
    q.not_before = "260101000000Z";
    q.not_after  = "360101000000Z";
    r = x509_build(&q, g_root, sizeof g_root, &g_rootlen);
    ck("it builds", r == X509_OK, r);
    if(r != X509_OK) printf("      %s\n", x509_strerror(r));
    ck("it is a plausible size", g_rootlen > 500 && g_rootlen < 2000,
       (long)g_rootlen);
    ck("it writes out", write_der("/tmp/hd_root.der", g_root, g_rootlen), 0);

    ck("openssl x509 PARSES it",
       run("openssl x509 -inform DER -in /tmp/hd_root.der -noout -text") == 0, 0);
    ck("...and says it is v3", grep_out("Version: 3"), 0);
    ck("...signed with sha256WithRSAEncryption",
       grep_out("sha256WithRSAEncryption"), 0);
    ck("...with our subject", grep_out("HavenDOS Root CA"), 0);
    ck("...CA:TRUE", grep_out("CA:TRUE"), 0);
    ck("...and basicConstraints is CRITICAL",
       grep_out("X509v3 Basic Constraints: critical"), 0);
    ck("...keyUsage is critical too",
       grep_out("X509v3 Key Usage: critical"), 0);
    ck("...and says Certificate Sign",
       grep_out("Certificate Sign"), 0);

    ck("openssl asn1parse accepts it as DER",
       run("openssl asn1parse -inform DER -in /tmp/hd_root.der") == 0, 0);

    /* THE ONE THAT MATTERS: does the signature check out? */
    ck("the certificate is converted to PEM",
       run("openssl x509 -inform DER -in /tmp/hd_root.der "
           "-out /tmp/hd_root.pem") == 0, 0);
    ck("openssl VERIFIES the self-signature",
       run("openssl verify -CAfile /tmp/hd_root.pem /tmp/hd_root.pem") == 0, 0);
    if(!grep_out("OK")) run("cat /tmp/hd_x509.out");

    /* ── 4: a host certificate signed by the root ────────────────── */
    head("a host certificate, signed by that root");
    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "HavenDOS Root CA";
    q.serial     = 2;
    q.subject_n  = &hk.n; q.subject_e = &hk.e;   /* the HOST's key ... */
    q.issuer_n   = &rk.n; q.issuer_d  = &rk.d;   /* ... the ROOT signs */
    q.is_ca      = 0;
    q.key_usage  = X509_KU_DIGITAL_SIG | X509_KU_KEY_ENCIPHER;
    q.not_before = "260101000000Z";
    q.not_after  = "360101000000Z";
    r = x509_build(&q, g_host, sizeof g_host, &g_hostlen);
    ck("it builds", r == X509_OK, r);
    ck("it writes out", write_der("/tmp/hd_host.der", g_host, g_hostlen), 0);

    ck("openssl parses it",
       run("openssl x509 -inform DER -in /tmp/hd_host.der -noout -text") == 0, 0);
    ck("...issuer is the root", grep_out("Issuer: CN *= *HavenDOS Root CA"), 0);
    ck("...subject is the host", grep_out("Subject: CN *= *HavenDOS Host"), 0);
    ck("...CA:FALSE", grep_out("CA:FALSE"), 0);
    ck("...keyUsage is Digital Signature + Key Encipherment",
       grep_out("Digital Signature") && grep_out("Key Encipherment"), 0);

    ck("it converts to PEM",
       run("openssl x509 -inform DER -in /tmp/hd_host.der "
           "-out /tmp/hd_host.pem") == 0, 0);

    /* THE CHAIN. This checks the root's private key actually signed
       the host's TBS bytes, with our padding, our digest and our
       modular exponentiation. Nothing of ours is consulted. */
    ck("openssl VERIFIES the chain root -> host",
       run("openssl verify -CAfile /tmp/hd_root.pem /tmp/hd_host.pem") == 0, 0);
    if(!grep_out("OK")) run("cat /tmp/hd_x509.out");

    /* ── 5: does the judge have teeth ────────────────────────────── */
    head("corrupting a signed certificate, so openssl proves it is checking");
    {
        /* Flip a byte well inside the TBS -- in the subject key, not
           in the signature -- so the structure still parses and only
           the signature is wrong. A corruption openssl REFUSES TO
           PARSE would prove nothing about whether it checks
           signatures. */
        static uint8_t bad[8192];
        uint32_t at = g_hostlen / 3;
        memcpy(bad, g_host, g_hostlen);
        bad[at] ^= 0x01;
        ck("the corrupted certificate writes out",
           write_der("/tmp/hd_bad.der", bad, g_hostlen), 0);
        ck("it still PARSES (so this tests the signature, not the syntax)",
           run("openssl x509 -inform DER -in /tmp/hd_bad.der -noout -text")
               == 0, 0);
        ck("it converts to PEM",
           run("openssl x509 -inform DER -in /tmp/hd_bad.der "
               "-out /tmp/hd_bad.pem") == 0, 0);
        ck("openssl REFUSES to verify it against the root",
           run("openssl verify -CAfile /tmp/hd_root.pem /tmp/hd_bad.pem")
               != 0, 0);

        /* And the other half: a good certificate against the WRONG
           root must also fail, or "verify" is not checking an issuer
           either. */
        {
            x509_req_t q2;
            static uint8_t other[8192];
            uint32_t olen = 0;
            memset(&q2, 0, sizeof q2);
            q2.subject_cn = "Someone Else CA";
            q2.issuer_cn  = "Someone Else CA";
            q2.serial     = 9;
            q2.subject_n  = &hk.n; q2.subject_e = &hk.e;
            q2.issuer_n   = &hk.n; q2.issuer_d  = &hk.d;
            q2.is_ca      = 1;
            q2.key_usage  = X509_KU_KEY_CERT_SIGN;
            q2.not_before = "260101000000Z";
            q2.not_after  = "360101000000Z";
            ck("a second, unrelated CA builds",
               x509_build(&q2, other, sizeof other, &olen) == X509_OK, 0);
            write_der("/tmp/hd_other.der", other, olen);
            run("openssl x509 -inform DER -in /tmp/hd_other.der "
                "-out /tmp/hd_other.pem");
            ck("the host certificate does NOT verify against it",
               run("openssl verify -CAfile /tmp/hd_other.pem "
                   "/tmp/hd_host.pem") != 0, 0);
        }
    }

    /* ── 6: refusals ─────────────────────────────────────────────── */
    head("it refuses rather than producing something half-made");
    {
        uint8_t small[64];
        uint32_t n2 = 0;
        memset(&q, 0, sizeof q);
        q.subject_cn = "x"; q.issuer_cn = "x"; q.serial = 1;
        q.subject_n = &rk.n; q.subject_e = &rk.e;
        q.issuer_n = &rk.n; q.issuer_d = &rk.d;
        q.not_before = "260101000000Z"; q.not_after = "360101000000Z";
        ck("a buffer too small is refused",
           x509_build(&q, small, sizeof small, &n2) != X509_OK, 0);

        memset(&q, 0, sizeof q);
        ck("a request with no keys is refused",
           x509_build(&q, g_host, sizeof g_host, &n2) == X509_E_PARAM, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
