/* ================================================================
   tests/test_pair.c -- the pair request, judged by openssl and
                        plistlib
   ================================================================
   usb/pair.c builds the message that makes an iPhone put "Trust this
   computer?" on screen. It is three certificates and two UUIDs, and
   it either works or the device hangs up without saying why -- so
   every claim here is checked by something outside this tree.

   ── THE JUDGE THAT MATTERS ──────────────────────────────────────────

   `openssl verify -CAfile root.pem host.pem`.

   That is real signature verification by an independent
   implementation: it says the root certificate's key actually signed
   the host certificate, that the names chain, that the basic
   constraints permit it. Nothing we could write would be worth as
   much, and it is the exact check the device performs on the record
   it is being asked to remember.

   The second: `openssl x509 -noout -modulus` on the DEVICE
   certificate must print the DEVICE's modulus -- not ours. Certifying
   our own key three times would pass every structural check and fail
   on the phone.

   The third: plistlib parses the request and hands back all three
   certificates byte-for-byte.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "types.h"
#include "bignum.h"
#include "x509.h"
#include "plist.h"
#include "pair.h"
#include "../net/bignum.c"
#include "../net/der.c"
#include "../net/b64.c"
#include "../net/plist.c"
#include "../net/x509.c"
#include "../fs/gpt.c"   /* gpt_crc32 -- the tree's one CRC-32 */

/* usb/pair.c calls entropy_u32(), which lives in the kernel. A
   counter here, so that the UUIDs are reproducible AND so that the
   "two calls give two different UUIDs" check means something. */
static uint32_t g_ent = 0x12345678u;
uint32_t entropy_u32(void){ g_ent = g_ent * 1103515245u + 12345u; return g_ent; }
#include "../usb/pair.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

static int sh(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    return system(cmd) == 0;
}

/* ── loading a key out of openssl ────────────────────────────────── */
static int hex_to_bn(const char *path, bn_t *out)
{
    FILE *f = fopen(path, "rb");
    char line[4096];
    uint8_t raw[1024];
    uint32_t n = 0;
    int i;
    if(!f) return 0;
    if(!fgets(line, sizeof line, f)){ fclose(f); return 0; }
    fclose(f);
    for(i = 0; line[i] && line[i+1]; ){
        char a = line[i], b = line[i+1];
        int hi, lo;
        if(a == '\n' || a == '\r') break;
        hi = (a>='0'&&a<='9')?a-'0':(a>='A'&&a<='F')?a-'A'+10:(a>='a'&&a<='f')?a-'a'+10:-1;
        lo = (b>='0'&&b<='9')?b-'0':(b>='A'&&b<='F')?b-'A'+10:(b>='a'&&b<='f')?b-'a'+10:-1;
        if(hi<0||lo<0) break;
        if(n >= sizeof raw) return 0;
        raw[n++] = (uint8_t)((hi<<4)|lo);
        i += 2;
    }
    return n && bn_from_bytes(out, raw, n);
}

/* ── v0.8.9: _key.pem, and that is a FIXTURE FIX ──────────────────────
   This used to write the private key to /tmp/hd_pair_<tag>.pem, which
   is the path the CERTIFICATE PEM is written to further down -- so the
   key was clobbered by the certificate before anything could use it.
   Nothing noticed, because nothing had needed the key as a key until
   pair_tls_sign() had to be verified against it: `openssl pkeyutl
   -verify -inkey` on a file holding a certificate fails, and all three
   checks around it -- including the two negative controls -- passed on
   the strength of that failure. A judge that always convicts is no
   better than one that never does. */
static int make_key(const char *tag, int bits, pair_key_t *k)
{
    char pem[128], nh[128], dh[128];
    snprintf(pem, sizeof pem, "/tmp/hd_pair_%s_key.pem", tag);
    snprintf(nh,  sizeof nh,  "/tmp/hd_pair_%s_n.hex", tag);
    snprintf(dh,  sizeof dh,  "/tmp/hd_pair_%s_d.hex", tag);
    if(!sh("openssl genrsa -out %s %d >/dev/null 2>&1", pem, bits)) return 0;
    if(!sh("openssl rsa -in %s -noout -modulus 2>/dev/null "
           "| sed 's/^Modulus=//' > %s", pem, nh)) return 0;
    if(!sh("openssl rsa -in %s -text -noout 2>/dev/null "
           "| awk '/^privateExponent:/{f=1;next} /^prime1:/{f=0} f' "
           "| tr -d ' :\\n' > %s", pem, dh)) return 0;
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

static pair_key_t g_root, g_host, g_devk;
static pair_record_t g_rec;
static char g_req[16384];

int main(void)
{
    uint32_t reqlen = 0;
    int r;

    printf("\n=== test_pair: the pair request, judged by openssl ===\n");

    if(system("which openssl >/dev/null 2>&1") != 0){
        printf("\n  openssl is not installed and it verifies the chain.\n"
               "  SKIP, not a pass.\n\nSKIP\n");
        return 77;
    }

    /* ── UUIDs ───────────────────────────────────────────────────── */
    head("HostID and SystemBUID");
    {
        char a[40], b[40];
        int i, dashes = 0, hexes = 0;
        /* Filled with junk first. An uninitialised buffer on the
           stack happened to hold a zero at offset 36, so a UUID that
           was never terminated still measured 36 characters and an
           injection removing the terminator survived the suite. */
        memset(a, 'X', sizeof a);
        memset(b, 'X', sizeof b);
        ck("one is made", pair_make_uuid(a, sizeof a) == PAIR_OK, 0);
        ck("the byte after it is a terminator, not junk", a[36] == 0, a[36]);
        ck("it is 36 characters", (int)strlen(a) == 36, (long)strlen(a));
        for(i = 0; a[i]; i++){
            if(a[i] == '-') dashes++;
            else if((a[i] >= '0' && a[i] <= '9') ||
                    (a[i] >= 'A' && a[i] <= 'F')) hexes++;
        }
        ck("four dashes", dashes == 4, dashes);
        ck("thirty-two upper-case hex digits", hexes == 32, hexes);
        ck("dashes in the 8-4-4-4-12 places",
           a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-', 0);
        /* RFC 4122: version 4 in the high nibble of byte 6, variant 1
           in the top two bits of byte 8. lockdownd does not check,
           and a HostID that is not a UUID is the kind of thing that
           works until something does. */
        ck("it says version 4", a[14] == '4', a[14]);
        ck("and variant 1",
           a[19] == '8' || a[19] == '9' || a[19] == 'A' || a[19] == 'B',
           a[19]);
        ck("a second one is different",
           pair_make_uuid(b, sizeof b) == PAIR_OK && strcmp(a, b) != 0, 0);
        ck("a buffer of 36 is refused -- there is a terminator too",
           pair_make_uuid(a, 36) == PAIR_E_PARAM, 0);
        ck("a NULL buffer", pair_make_uuid(0, 40) == PAIR_E_PARAM, 0);
    }

    /* ── the keys ────────────────────────────────────────────────── */
    head("three keys from openssl");
    ck("a root key",   make_key("root", 2048, &g_root), 0);
    ck("a host key",   make_key("host", 2048, &g_host), 0);
    /* The "device" key stands in for the phone's. We have its private
       half only because openssl made it; pair_build_certs is given
       the public half alone. */
    ck("a device key", make_key("dev",  2048, &g_devk), 0);

    /* ── the certificates ───────────────────────────────────────── */
    head("building the three certificates");
    memset(&g_rec, 0, sizeof g_rec);
    r = pair_build_certs(&g_root, &g_host, &g_devk.n, &g_devk.e, &g_rec);
    ck("they build", r == PAIR_OK, r);
    ck("the root has a length",  g_rec.root_len > 400, (long)g_rec.root_len);
    ck("the host too",           g_rec.host_len > 400, (long)g_rec.host_len);
    ck("and the device's",       g_rec.dev_len  > 400, (long)g_rec.dev_len);
    /* PAIR_CERT_MAX is 1400 and the header says the real figure is
       measured rather than trusted. This is the measurement. */
    printf("          root %u, host %u, device %u bytes "
           "(PAIR_CERT_MAX is %u)\n",
           g_rec.root_len, g_rec.host_len, g_rec.dev_len, PAIR_CERT_MAX);
    ck("all three fit PAIR_CERT_MAX with room over",
       g_rec.root_len < PAIR_CERT_MAX - 100
       && g_rec.host_len < PAIR_CERT_MAX - 100
       && g_rec.dev_len  < PAIR_CERT_MAX - 100, 0);

    head("openssl reads all three");
    {
        ck("the files are written",
           write_der("/tmp/hd_pair_root.der", g_rec.root_cert, g_rec.root_len)
           && write_der("/tmp/hd_pair_host.der", g_rec.host_cert, g_rec.host_len)
           && write_der("/tmp/hd_pair_dev.der", g_rec.dev_cert, g_rec.dev_len), 0);
        ck("the root parses as X.509",
           sh("openssl x509 -inform DER -in /tmp/hd_pair_root.der -noout "
              "-text >/dev/null 2>&1"), 0);
        ck("the host does",
           sh("openssl x509 -inform DER -in /tmp/hd_pair_host.der -noout "
              "-text >/dev/null 2>&1"), 0);
        ck("the device's does",
           sh("openssl x509 -inform DER -in /tmp/hd_pair_dev.der -noout "
              "-text >/dev/null 2>&1"), 0);
    }

    /* ════════════════════════════════════════════════════════════
       THE CHECK THIS SUITE EXISTS FOR
       ════════════════════════════════════════════════════════════ */
    head("openssl VERIFIES THE CHAIN -- real signatures, not structure");
    {
        ck("the PEM forms are written",
           sh("openssl x509 -inform DER -in /tmp/hd_pair_root.der "
              "-out /tmp/hd_pair_root.pem >/dev/null 2>&1")
           && sh("openssl x509 -inform DER -in /tmp/hd_pair_host.der "
                 "-out /tmp/hd_pair_host.pem >/dev/null 2>&1")
           && sh("openssl x509 -inform DER -in /tmp/hd_pair_dev.der "
                 "-out /tmp/hd_pair_dev.pem >/dev/null 2>&1"), 0);
        /* -no_check_time because the validity window is fixed rather
            than taken from a clock -- usb/pair.c says why, and
            lockdownd does not check it either. Everything else is
            checked: the signature, the issuer name, the constraints. */
        ck("the root certificate signed the HOST certificate",
           sh("openssl verify -no_check_time "
              "-CAfile /tmp/hd_pair_root.pem "
              "/tmp/hd_pair_host.pem >/dev/null 2>&1"), 0);
        ck("the root certificate signed the DEVICE certificate",
           sh("openssl verify -no_check_time "
              "-CAfile /tmp/hd_pair_root.pem "
              "/tmp/hd_pair_dev.pem >/dev/null 2>&1"), 0);
        ck("the root verifies against itself (it is self-signed)",
           sh("openssl verify -no_check_time "
              "-CAfile /tmp/hd_pair_root.pem "
              "/tmp/hd_pair_root.pem >/dev/null 2>&1"), 0);
        /* And the negative: a DIFFERENT root must NOT verify them.
           Without this, "verify succeeded" could mean openssl was not
           checking the signature at all. */
        /* An unrelated CA must NOT verify them. Without this,
           "verify succeeded" could mean openssl was not checking the
           signature at all.

           A SEPARATE key, and a separate record: the first version of
           this check overwrote g_host to make the other CA, and the
           certificates in the message afterwards no longer matched
           the DER on disk -- which the round-trip checks caught. A
           negative control must not disturb what it is a control
           for. */
        {
            pair_key_t other;
            static pair_record_t orec;
            ck("another CA exists", make_key("other", 2048, &other), 0);
            ck("it builds its own self-signed root",
               pair_build_certs(&other, &other, &g_devk.n, &g_devk.e, &orec)
               == PAIR_OK, 0);
            ck("the other root is written",
               write_der("/tmp/hd_pair_other_root.der",
                         orec.root_cert, orec.root_len)
               && sh("openssl x509 -inform DER -in "
                     "/tmp/hd_pair_other_root.der -out "
                     "/tmp/hd_pair_other_root.pem >/dev/null 2>&1"), 0);
            ck("and it does NOT verify our host certificate",
               !sh("openssl verify -no_check_time "
                   "-CAfile /tmp/hd_pair_other_root.pem "
                   "/tmp/hd_pair_host.pem >/dev/null 2>&1"), 0);
            ck("nor our root",
               !sh("openssl verify -no_check_time "
                   "-CAfile /tmp/hd_pair_other_root.pem "
                   "/tmp/hd_pair_root.pem >/dev/null 2>&1"), 0);
        }
    }

    head("the root is a CA and the leaves are not");
    {
        ck("the root says CA:TRUE",
           sh("openssl x509 -in /tmp/hd_pair_root.pem -noout -text 2>/dev/null"
              " | grep -q 'CA:TRUE'"), 0);
        /* A leaf with CA:TRUE could sign anything in our name. */
        ck("the host does not",
           !sh("openssl x509 -in /tmp/hd_pair_host.pem -noout -text "
               "2>/dev/null | grep -q 'CA:TRUE'"), 0);
        ck("nor does the device's",
           !sh("openssl x509 -in /tmp/hd_pair_dev.pem -noout -text "
               "2>/dev/null | grep -q 'CA:TRUE'"), 0);
    }

    head("the DEVICE certificate certifies the DEVICE's key");
    {
        /* Certifying our own key three times would pass every
           structural check above and fail on the phone. */
        ck("its modulus is the device's, not the host's",
           sh("openssl x509 -in /tmp/hd_pair_dev.pem -noout -modulus "
              "2>/dev/null | sed 's/^Modulus=//' > /tmp/hd_pair_got.hex")
           && sh("openssl rsa -in /tmp/hd_pair_dev.pem -pubin -noout "
                 ">/dev/null 2>&1 || true")
           && sh("openssl rsa -in /tmp/hd_pair_dev.pem -noout -modulus "
                 ">/dev/null 2>&1 || true")
           && sh("cmp -s /tmp/hd_pair_got.hex /tmp/hd_pair_dev_n.hex"), 0);
        ck("...and is NOT the root's",
           !sh("cmp -s /tmp/hd_pair_got.hex /tmp/hd_pair_root_n.hex"), 0);
        /* v0.8.9: keyUsage on the host certificate -- openssl's
           reading of it, not ours. digitalSignature is the usage a
           TLS CertificateVerify exercises, which is what the host
           certificate is now for. */
        ck("the host certificate declares digitalSignature",
           sh("openssl x509 -in /tmp/hd_pair_host.pem -noout -text "
              "2>/dev/null | grep -q 'Digital Signature'"), 0);
        ck("and keyEncipherment",
           sh("openssl x509 -in /tmp/hd_pair_host.pem -noout -text "
              "2>/dev/null | grep -q 'Key Encipherment'"), 0);
        ck("the extension is critical",
           sh("openssl x509 -in /tmp/hd_pair_host.pem -noout -text "
              "2>/dev/null | grep -A1 'X509v3 Key Usage' "
              "| grep -q critical"), 0);
        ck("the host certificate's modulus is the host's",
           sh("openssl x509 -in /tmp/hd_pair_host.pem -noout -modulus "
              "2>/dev/null | sed 's/^Modulus=//' > /tmp/hd_pair_got2.hex")
           && sh("cmp -s /tmp/hd_pair_got2.hex /tmp/hd_pair_host_n.hex"), 0);
        ck("the subject names are distinct",
           sh("openssl x509 -in /tmp/hd_pair_dev.pem -noout -subject "
              "2>/dev/null | grep -q 'HavenDOS Device'"), 0);
        ck("the issuer is always the root",
           sh("openssl x509 -in /tmp/hd_pair_dev.pem -noout -issuer "
              "2>/dev/null | grep -q 'HavenDOS Root'"), 0);
        /* Three certificates from one authority must have three
           serial numbers. openssl verify does not care and neither
           does lockdownd, which is why an injection making them all
           1 survived the suite -- but a CA that issues duplicate
           serials is wrong, and the day something checks is not the
           day to find out. */
        ck("the three serials are distinct",
           sh("for f in root host dev; do openssl x509 -in "
              "/tmp/hd_pair_$f.pem -noout -serial; done "
              "| sort -u | wc -l | grep -qx 3"), 0);
    }

    head("certificates that cannot be built");
    {
        pair_key_t tiny;
        pair_record_t rec;
        bn_set_u32(&tiny.n, 65537); bn_set_u32(&tiny.e, 3);
        bn_set_u32(&tiny.d, 5);
        ck("a root key of 17 bits",
           pair_build_certs(&tiny, &g_host, &g_devk.n, &g_devk.e, &rec)
           == PAIR_E_KEY, 0);
        ck("a device key of 17 bits",
           pair_build_certs(&g_root, &g_host, &tiny.n, &tiny.e, &rec)
           == PAIR_E_DEVKEY, 0);
        ck("a NULL record",
           pair_build_certs(&g_root, &g_host, &g_devk.n, &g_devk.e, 0)
           == PAIR_E_PARAM, 0);
    }

    /* ── the request ─────────────────────────────────────────────── */
    head("the request, judged by plistlib");
    {
        /* The record built above is still intact -- the negative
           control uses its own key and its own record. */
        pair_make_uuid(g_rec.host_id, sizeof g_rec.host_id);
        pair_make_uuid(g_rec.system_buid, sizeof g_rec.system_buid);

        r = pair_build_request(&g_rec, g_req, sizeof g_req, &reqlen);
        ck("it builds", r == PAIR_OK, r);
        /* Three 2048-bit certificates come to about 700 DER bytes
           each, so the message is in the high threes -- measured, not
           guessed, which is why the figure is printed. What matters
           is that it is more than one mux packet, because that is the
           bound ld_send_plist() used to refuse at. */
        ck("it is over three kilobytes", reqlen > 3000, (long)reqlen);
        ck("and needs more than one mux packet",
           (reqlen + 4) > 2012, (long)reqlen);
        printf("          the request is %u bytes, %u mux packets\n",
               reqlen, (reqlen + 4 + 2011) / 2012);

        if(system("python3 -c 'import plistlib' >/dev/null 2>&1") == 0){
            FILE *f = fopen("/tmp/hd_pair_req.plist", "wb");
            if(f){ fwrite(g_req, 1, reqlen, f); fclose(f); }
            ck("plistlib parses it",
               sh("printf '%%s' 'p[\"Request\"] == \"Pair\"' "
                  "> /tmp/hd_pair_expr.txt")
               && sh("python3 -I tests/plist_judge.py "
                     "/tmp/hd_pair_req.plist /tmp/hd_pair_expr.txt "
                     ">/dev/null 2>&1"), 0);
            /* The three certificates, byte for byte, out of the
               document and compared against the DER on disk. If
               base64 or the wrapping were wrong by one character this
               is where it shows. */
            ck("the root certificate survives the round trip",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "d=open('/tmp/hd_pair_root.der','rb').read();"
                  "sys.exit(0 if p['PairRecord']['RootCertificate']==d else 1)\""), 0);
            ck("the host certificate too",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "d=open('/tmp/hd_pair_host.der','rb').read();"
                  "sys.exit(0 if p['PairRecord']['HostCertificate']==d else 1)\""), 0);
            ck("and the device's",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "d=open('/tmp/hd_pair_dev.der','rb').read();"
                  "sys.exit(0 if p['PairRecord']['DeviceCertificate']==d else 1)\""), 0);
            ck("the HostID round-trips",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "sys.exit(0 if p['PairRecord']['HostID']=='%s' else 1)\"",
                  g_rec.host_id), 0);
            ck("ExtendedPairingErrors is asked for",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "sys.exit(0 if p['PairingOptions']"
                  "['ExtendedPairingErrors'] is True else 1)\""), 0);
            /* AND the certificates openssl verified are the ones in
               the message -- the two judges checking the same bytes
               is what makes the pair of them worth more than either. */
            ck("the certificate in the message is the one openssl verified",
               sh("python3 -I -c \"import plistlib,sys;"
                  "p=plistlib.load(open('/tmp/hd_pair_req.plist','rb'));"
                  "open('/tmp/hd_pair_from_msg.der','wb')"
                  ".write(p['PairRecord']['HostCertificate'])\"")
               && sh("openssl x509 -inform DER -in /tmp/hd_pair_from_msg.der "
                     "-out /tmp/hd_pair_from_msg.pem >/dev/null 2>&1")
               && sh("openssl verify -no_check_time "
                     "-CAfile /tmp/hd_pair_root.pem "
                     "/tmp/hd_pair_from_msg.pem >/dev/null 2>&1"), 0);
        } else {
            printf("    (plistlib unavailable -- the plist half is skipped)\n");
        }
    }

    head("requests that cannot be built");
    {
        pair_record_t rec;
        char small[64];
        uint32_t n;
        memcpy(&rec, &g_rec, sizeof rec);
        ck("a buffer far too small says SPACE, not PLIST",
           pair_build_request(&rec, small, sizeof small, &n)
           == PAIR_E_SPACE, 0);
        rec.root_len = 0;
        ck("no root certificate",
           pair_build_request(&rec, g_req, sizeof g_req, &n)
           == PAIR_E_CERT, 0);
        memcpy(&rec, &g_rec, sizeof rec);
        rec.host_id[0] = 0;
        ck("no HostID",
           pair_build_request(&rec, g_req, sizeof g_req, &n)
           == PAIR_E_PARAM, 0);
        memcpy(&rec, &g_rec, sizeof rec);
        rec.system_buid[0] = 0;
        ck("no SystemBUID",
           pair_build_request(&rec, g_req, sizeof g_req, &n)
           == PAIR_E_PARAM, 0);
        ck("a NULL buffer",
           pair_build_request(&g_rec, 0, 100, &n) == PAIR_E_PARAM, 0);
    }

    /* ── the reply, which is the whole point of asking ───────────── */
    head("reading the device's reply");
    {
        static const struct { const char *xml; int want; const char *what; } T[] = {
          { "<plist><dict><key>Error</key><string>"
            "PairingDialogResponsePending</string></dict></plist>",
            PAIR_R_PENDING, "the dialog is on screen" },
          { "<plist><dict><key>Error</key><string>"
            "UserDeniedPairing</string></dict></plist>",
            PAIR_R_DENIED, "the user tapped Don't Trust" },
          { "<plist><dict><key>Error</key><string>"
            "PasswordProtected</string></dict></plist>",
            PAIR_R_PASSWORD, "the device is locked" },
          { "<plist><dict><key>Error</key><string>"
            "InvalidHostID</string></dict></plist>",
            PAIR_R_INVALID_HOSTID, "an unknown HostID" },
          { "<plist><dict><key>Error</key><string>"
            "SomethingNewInIOS30</string></dict></plist>",
            PAIR_R_OTHER, "an error we have no name for" },
          { "<plist><dict><key>Result</key><string>Success</string>"
            "</dict></plist>", PAIR_R_SUCCESS, "Result Success" },
          { "<plist><dict><key>Request</key><string>Pair</string>"
            "</dict></plist>", PAIR_R_SUCCESS,
            "a reply that just echoes the request" },
          { "<plist><dict></dict></plist>", PAIR_R_NOREPLY,
            "an empty dict" },
          { "", PAIR_R_NOREPLY, "nothing at all" },
        };
        unsigned i;
        for(i = 0; i < sizeof T / sizeof T[0]; i++)
            ck(T[i].what,
               pair_reply_status(T[i].xml, (uint32_t)strlen(T[i].xml))
               == T[i].want,
               pair_reply_status(T[i].xml, (uint32_t)strlen(T[i].xml)));

        /* An Error BEATS a Result: a reply carrying both is a refusal
           with a courtesy Result, and reading the Result would report
           a pairing that did not happen. */
        {
            const char *both =
              "<plist><dict><key>Result</key><string>Success</string>"
              "<key>Error</key><string>UserDeniedPairing</string>"
              "</dict></plist>";
            ck("an Error outranks a Result",
               pair_reply_status(both, (uint32_t)strlen(both))
               == PAIR_R_DENIED, 0);
        }
        /* The key name must be matched whole. A document that merely
           CONTAINS the word Error inside some other value must not be
           read as an error. */
        {
            /* Result IS exactly Success, and the word Error appears
               only inside another value. The first version of this
               fixture put the word inside the Result value itself, so
               the Result no longer said Success and the check passed
               for the wrong reason. */
            const char *inner =
              "<plist><dict><key>Message</key>"
              "<string>no Error occurred</string>"
              "<key>Result</key><string>Success</string></dict></plist>";
            ck("the word Error inside a value is not an Error key",
               pair_reply_status(inner, (uint32_t)strlen(inner))
               == PAIR_R_SUCCESS, 0);
        }
        /* A key with no <string> after it is not a value. */
        {
            const char *novalue =
              "<plist><dict><key>Error</key><integer>5</integer>"
              "</dict></plist>";
            ck("an Error whose value is not a string is not read",
               pair_reply_status(novalue, (uint32_t)strlen(novalue))
               == PAIR_R_NOREPLY, 0);
            /* The version that matters: a non-string value with a
               </string> LATER in the document, so a scanner that does
               not check the opening tag runs past the value it was
               asked for and returns somebody else's bytes. The
               previous fixture had no later </string>, so the
               terminator check caught it and the opening-tag check
               was never tested. */
            {
                const char *runon =
                  "<plist><dict><key>Error</key>"
                  "<data>VXNlckRlbmllZFBhaXJpbmc=</data>"
                  "<key>Note</key><string>UserDeniedPairing</string>"
                  "</dict></plist>";
                ck("...even when a </string> follows further on",
                   pair_reply_status(runon, (uint32_t)strlen(runon))
                   == PAIR_R_NOREPLY, 0);
            }
        }
        /* Whitespace and newlines between the key and its value,
            which is what a real reply has. */
        {
            const char *spaced =
              "<plist><dict>\n\t<key>Error</key>\n\t"
              "<string>UserDeniedPairing</string>\n</dict></plist>";
            ck("whitespace between the key and its value",
               pair_reply_status(spaced, (uint32_t)strlen(spaced))
               == PAIR_R_DENIED, 0);
        }
        /* Truncated: a reply cut off mid-value must not read past it. */
        {
            const char *cut =
              "<plist><dict><key>Error</key><string>UserDenied";
            ck("a reply cut off inside the value",
               pair_reply_status(cut, (uint32_t)strlen(cut))
               == PAIR_R_NOREPLY, 0);
        }
        ck("a NULL reply", pair_reply_status(0, 10) == PAIR_R_NOREPLY, 0);
        ck("every status has words for a person",
           strlen(pair_status_name(PAIR_R_PENDING)) > 10
           && strlen(pair_status_name(PAIR_R_DENIED)) > 10
           && strlen(pair_status_name(PAIR_R_PASSWORD)) > 10, 0);
    }

    head("every prefix of a real reply is read without running off it");
    {
        const char *full =
          "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
          "<key>Error</key><string>PairingDialogResponsePending</string>"
          "</dict></plist>";
        uint32_t n = (uint32_t)strlen(full), i;
        int bad = -1;
        for(i = 0; i <= n; i++){
            char *q = (char *)malloc(i ? i : 1);
            int s;
            memcpy(q, full, i);
            s = pair_reply_status(q, i);
            free(q);
            /* The only acceptable answers are "pending" once the
               whole value is there, and "nothing recognisable"
               before. Anything else means it read past the data. */
            if(s != PAIR_R_PENDING && s != PAIR_R_NOREPLY){ bad = (int)i; break; }
        }
        ck("no prefix produces a wrong status", bad < 0, bad);
        ck("and the whole thing is pending",
           pair_reply_status(full, n) == PAIR_R_PENDING, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9 -- THE STORE

       A pairing that is not saved is a pairing the user repeats, and
       repeating it costs two 2048-bit keygens and another tap on
       Trust. The thing being serialised includes PRIVATE KEYS, so a
       round trip that is almost right is a pairing that reloads
       cleanly and fails at CertificateVerify with nothing on screen
       to connect the two.

       The judge for the certificates is openssl again: the host
       certificate is pulled out of a DECODED record, wrapped in PEM,
       and verified against the root from the same record. A round
       trip that lost or shifted one byte of DER cannot survive that.
       ════════════════════════════════════════════════════════════════ */
    head("the record goes out and comes back");
    {
        static pair_store_t st, back;
        static char text[PAIR_STORE_MAX];
        uint32_t len = 0;
        int r;

        memset(&st, 0, sizeof st);
        st.rec = g_rec;                  /* built and checked above */
        strcpy(st.rec.host_id,     "3F2504E0-4F89-11D3-9A0C-0305E82C3301");
        strcpy(st.rec.system_buid, "A1B2C3D4-0000-4000-8000-0123456789AB");
        bn_copy(&st.root.n, &g_root.n); bn_copy(&st.root.e, &g_root.e);
        bn_copy(&st.root.d, &g_root.d);
        bn_copy(&st.host.n, &g_host.n); bn_copy(&st.host.e, &g_host.e);
        bn_copy(&st.host.d, &g_host.d);

        r = pair_store_encode(&st, text, sizeof text, &len);
        ck("it encodes", r == PAIR_OK, r);
        ck("and reports a length", len > 2000 && len < PAIR_STORE_MAX,
           (long)len);
        ck("the length is the string's", len == (uint32_t)strlen(text),
           (long)len);
        /* TEXT, deliberately: the first thing anybody debugging a
           failed pairing wants is the certificate, and base64 DER on
           a line of its own goes straight into `openssl x509`. */
        ck("it begins with the magic line",
           !memcmp(text, "HavenDOS-pair-record 1\n", 23), 0);
        ck("every field name is in it",
           strstr(text, "\nHostID ") && strstr(text, "\nSystemBUID ") &&
           strstr(text, "\nRootN ") && strstr(text, "\nRootE ") &&
           strstr(text, "\nRootD ") && strstr(text, "\nHostN ") &&
           strstr(text, "\nHostE ") && strstr(text, "\nHostD ") &&
           strstr(text, "\nRootCertificate ") &&
           strstr(text, "\nHostCertificate ") &&
           strstr(text, "\nDeviceCertificate ") &&
           strstr(text, "\nCRC32 ") != 0, 0);
        /* No key material in plain sight and no stray whitespace: one
           field per line, so a line count is a field count. */
        {
            uint32_t i, lines = 0;
            for(i = 0; i < len; i++) if(text[i] == '\n') lines++;
            ck("exactly thirteen lines -- magic, eleven fields, CRC",
               lines == 13, (long)lines);
        }

        memset(&back, 0, sizeof back);
        r = pair_store_decode(text, len, &back);
        ck("it decodes", r == PAIR_OK, r);
        ck("the HostID survived",
           !strcmp(back.rec.host_id, st.rec.host_id), 0);
        ck("the SystemBUID survived",
           !strcmp(back.rec.system_buid, st.rec.system_buid), 0);
        ck("the root certificate is byte for byte",
           back.rec.root_len == st.rec.root_len &&
           !memcmp(back.rec.root_cert, st.rec.root_cert, st.rec.root_len),
           (long)back.rec.root_len);
        ck("the host certificate is byte for byte",
           back.rec.host_len == st.rec.host_len &&
           !memcmp(back.rec.host_cert, st.rec.host_cert, st.rec.host_len),
           (long)back.rec.host_len);
        ck("the device certificate is byte for byte",
           back.rec.dev_len == st.rec.dev_len &&
           !memcmp(back.rec.dev_cert, st.rec.dev_cert, st.rec.dev_len),
           (long)back.rec.dev_len);
        /* THE KEYS. A record whose certificates survived and whose
           private key did not is the failure this struct exists to
           prevent: it reloads, it pairs, and it cannot sign. */
        ck("the root modulus survived",
           bn_cmp(&back.root.n, &st.root.n) == 0, 0);
        ck("the root exponent survived",
           bn_cmp(&back.root.e, &st.root.e) == 0, 0);
        ck("the root PRIVATE exponent survived",
           bn_cmp(&back.root.d, &st.root.d) == 0, 0);
        ck("the host modulus survived",
           bn_cmp(&back.host.n, &st.host.n) == 0, 0);
        ck("the host exponent survived",
           bn_cmp(&back.host.e, &st.host.e) == 0, 0);
        ck("the host PRIVATE exponent survived",
           bn_cmp(&back.host.d, &st.host.d) == 0, 0);
        ck("the two keys are not the same key",
           bn_cmp(&back.root.n, &back.host.n) != 0, 0);

        /* ── openssl, on the DECODED record ──────────────────────────
           Our own comparison above says the bytes match. This says
           they are still a certificate and still a chain, which is
           the question a phone asks. */
        head("openssl verifies the chain out of the decoded record");
        {
            write_der("/tmp/hd_ps_root.der", back.rec.root_cert,
                      back.rec.root_len);
            write_der("/tmp/hd_ps_host.der", back.rec.host_cert,
                      back.rec.host_len);
            ck("the root parses",
               sh("openssl x509 -inform DER -in /tmp/hd_ps_root.der "
                  "-out /tmp/hd_ps_root.pem >/dev/null 2>&1"), 0);
            ck("the host parses",
               sh("openssl x509 -inform DER -in /tmp/hd_ps_host.der "
                  "-out /tmp/hd_ps_host.pem >/dev/null 2>&1"), 0);
            ck("and the chain VERIFIES",
               sh("openssl verify -CAfile /tmp/hd_ps_root.pem "
                  "/tmp/hd_ps_host.pem >/dev/null 2>&1"), 0);
            /* The negative control, same as everywhere else in this
               file: a chain that verifies against anything would mean
               openssl was not checking. */
            ck("but NOT against an unrelated CA",
               !sh("openssl verify -CAfile /tmp/hd_pair_other_root.pem "
                   "/tmp/hd_ps_host.pem >/dev/null 2>&1"), 0);
        }

        /* ── the CRC ────────────────────────────────────────────────
           One flipped base64 character changes a decoded byte and
           nothing about the structure. Without a checksum that is a
           private key that is almost right. */
        head("damage is refused, not loaded");
        {
            static char bad[PAIR_STORE_MAX];
            uint32_t at;
            int caught = 0, tried = 0;
            /* Flip one character in each field line in turn. */
            /* ── 'Z', AND NOT 'A' ────────────────────────────────
               The first version of this flipped each character to
               'A' (or to 'B' if it was already 'A'), and it failed
               about half the time -- one or two positions accepted,
               varying between runs because the key varies.

               The positions were inside the CRC's own hex digits. It
               is written in LOWER case and ps_hex8() accepts either,
               so turning an 'a' into an 'A' is not damage at all: it
               is the same number spelled differently, and the record
               is still correct. The check was wrong, not the code.

               'Z' is not a hex digit and not a UUID character, so the
               change is a real one wherever it lands. */
            for(at = 0; at < len; at++){
                if(text[at] == '\n') continue;
                memcpy(bad, text, len); bad[len] = 0;
                bad[at] = (bad[at] == 'Z') ? 'Y' : 'Z';
                tried++;
                if(pair_store_decode(bad, len, &back) != PAIR_OK) caught++;
            }
            ck("every single-character change in the file is refused",
               caught == tried, (long)(tried - caught));
            ck("and there were plenty of them", tried > 2000, tried);

            /* And the one change that is NOT damage, asserted rather
               than merely excluded: the CRC's hex is case-insensitive
               because ps_hex8() reads both, which is what made the
               loop above flaky until it was understood. */
            {
                char *crc_ = strstr(bad, "\nCRC32 ");
                int k_, flipped = 0;
                memcpy(bad, text, len); bad[len] = 0;
                crc_ = strstr(bad, "\nCRC32 ");
                for(k_ = 0; crc_ && k_ < 8; k_++)
                    if(crc_[7+k_] >= 'a' && crc_[7+k_] <= 'f'){
                        crc_[7+k_] = (char)(crc_[7+k_] - 32);
                        flipped++;
                    }
                ck("the CRC's hex digits read in either case",
                   flipped == 0 ||
                   pair_store_decode(bad, len, &back) == PAIR_OK,
                   flipped);
            }
        }

        /* ── A RECORD THAT IS INTACT AND WRONG ──────────────────────
           The CRC above catches damage. It cannot catch a record that
           was WRITTEN wrong, and the injection harness needed that
           distinction: with every fixture carrying real keys, removing
           the decoder's own key checks changed nothing anything could
           see.

           So this builds a record with a field deliberately replaced
           and THE CRC RECOMPUTED -- which is what a file written by a
           version of this code that did not check would look like. The
           checksum is no defence against it; the semantic checks on
           the way in are the only ones. */
        head("a record that checksums correctly and is still not a key");
        {
            static char forged[PAIR_STORE_MAX];
            uint32_t flen;

            /* `field` gets `value`, then the CRC line is rebuilt over
               everything above it. */
            #define FORGE(field, value) do {                                              const char *src_ = text;                                                  char *at_, *eol_, *crc_;                                                  uint32_t body_;                                                           memcpy(forged, src_, len); forged[len] = 0;                               at_ = strstr(forged, "\n" field " ");                                      if(!at_) { flen = 0; break; }                                             at_ += 1 + strlen(field) + 1;                                             eol_ = strchr(at_, '\n');                                                 memmove(at_ + strlen(value), eol_,                                                strlen(eol_) + 1);                                                memcpy(at_, value, strlen(value));                                        crc_ = strstr(forged, "\nCRC32 ");                                        body_ = (uint32_t)(crc_ - forged) + 1;                                    {   uint32_t c_ = gpt_crc32(forged, body_);                                    int k_;                                                                   for(k_ = 0; k_ < 8; k_++)                                                     crc_[7 + k_] = "0123456789abcdef"                                                        [(c_ >> ((7 - k_) * 4)) & 0xF];                     }                                                                         flen = (uint32_t)strlen(forged);                                      } while(0)

            /* First: the forging itself has to produce something the
               decoder ACCEPTS when the value is legitimate, or the
               checks below would pass for the wrong reason. */
            FORGE("HostID", "11111111-2222-3333-4444-555555555555");
            ck("a forged record with a legitimate value is accepted",
               flen && pair_store_decode(forged, flen, &back) == PAIR_OK,
               (long)flen);
            ck("and carries the forged value",
               !strcmp(back.rec.host_id,
                       "11111111-2222-3333-4444-555555555555"), 0);

            /* Now the ones that must be refused on their MEANING. */
            FORGE("HostD", "AA==");          /* one zero byte */
            ck("a zero host private exponent is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_KEY, 0);
            FORGE("RootD", "AA==");
            ck("a zero root private exponent is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_KEY, 0);
            FORGE("HostE", "AA==");
            ck("a zero host public exponent is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_KEY, 0);
            FORGE("HostN", "AQAB");          /* 65537: far too small */
            ck("a host modulus too small to pair with is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_KEY, 0);
            FORGE("RootN", "AQAB");
            ck("a root modulus too small to pair with is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_KEY, 0);
            FORGE("HostCertificate", "");
            ck("an empty certificate is refused",
               pair_store_decode(forged, flen, &back) != PAIR_OK, 0);
            FORGE("SystemBUID", "not-a-uuid-at-all-not-even-close!!!!");
            ck("a SystemBUID that is not a UUID is refused",
               pair_store_decode(forged, flen, &back) == PAIR_E_FORMAT, 0);
            #undef FORGE
        }

        head("the shapes that are not a record");
        {
            static char bad[PAIR_STORE_MAX];
            char *p2;

            ck("a NULL buffer", pair_store_decode(0, 10, &back)
               == PAIR_E_PARAM, 0);
            ck("zero length", pair_store_decode(text, 0, &back)
               == PAIR_E_PARAM, 0);
            ck("no record at all",
               pair_store_decode("hello\n", 6, &back) == PAIR_E_FORMAT, 0);

            /* A LATER version is refused BY NAME. This file will grow
               a field one day, and a reader that ignored the version
               would load the first eleven fields of a twelve-field
               record and call it a pairing. */
            memcpy(bad, text, len); bad[len] = 0;
            bad[21] = '2';
            ck("a later version is refused as a version",
               pair_store_decode(bad, len, &back) == PAIR_E_VERSION, 0);

            /* Truncation at every length. A power cut during the
               write is the realistic way this file goes wrong. */
            {
                uint32_t cut;
                int bad_accepts = 0;
                /* From 1 to len-2. len-1 drops only the final
                   NEWLINE, and a record whose last line has no line
                   break is complete -- every scan here is bounded by
                   the length rather than by a terminator, which is
                   what a file read off a volume gives you. Accepting
                   that one is correct and the check says so rather
                   than pretending the boundary is elsewhere. */
                for(cut = 1; cut + 1 < len; cut++)
                    if(pair_store_decode(text, cut, &back) == PAIR_OK)
                        bad_accepts++;
                ck("no truncation of the file is accepted",
                   bad_accepts == 0, bad_accepts);
                ck("except the missing final newline, which is whole",
                   pair_store_decode(text, len - 1, &back) == PAIR_OK, 0);
            }

            /* Fields out of order. The order is the format: a reader
               that accepts any order cannot tell a record from one
               with two fields swapped -- and RootN where HostN
               belongs is a record that signs with the wrong key. */
            memcpy(bad, text, len); bad[len] = 0;
            p2 = strstr(bad, "\nRootN ");
            ck("the field is where expected", p2 != 0, 0);
            if(p2) p2[1] = 'H';                    /* RootN -> HootN */
            ck("a renamed field is refused",
               pair_store_decode(bad, len, &back) == PAIR_E_FORMAT, 0);

            /* A UUID that is not one. lockdownd does not check, which
               is exactly why this does -- include/pair.h calls that
               "works until something checks". */
            memcpy(bad, text, len); bad[len] = 0;
            p2 = strstr(bad, "\nHostID ") + 8;
            p2[8] = 'x';                           /* where a '-' goes */
            ck("a HostID with the dashes in the wrong place is refused",
               pair_store_decode(bad, len, &back) == PAIR_E_FORMAT, 0);
            memcpy(bad, text, len); bad[len] = 0;
            p2 = strstr(bad, "\nHostID ") + 8;
            p2[0] = 'Z';                           /* not hex */
            ck("a HostID with a non-hex character is refused",
               pair_store_decode(bad, len, &back) == PAIR_E_FORMAT, 0);
        }

        head("what the encoder refuses");
        {
            static pair_store_t t2;
            static char small[64];
            uint32_t n2 = 0;

            ck("a NULL record", pair_store_encode(0, text, sizeof text,
                                                  &n2) == PAIR_E_PARAM, 0);
            ck("a NULL buffer",
               pair_store_encode(&st, 0, 10, &n2) == PAIR_E_PARAM, 0);
            /* REFUSED, not truncated. A record that encoded into a
               buffer too small for it would be a file that decodes to
               a shorter key -- and net/b64.h's header makes the same
               argument about a decoder that shortens one. */
            ck("a buffer far too small is refused",
               pair_store_encode(&st, small, sizeof small, &n2)
               == PAIR_E_SPACE, 0);

            /* ── AT THE BYTE, IN A BUFFER OF EXACTLY THAT SIZE ────────
               The injection harness needed this. A 64-byte buffer is
               refused by the very first write, so it says nothing
               about the bound; and `text` is three times larger than
               any record, so nothing here came within a kilobyte of
               the edge.

               These are malloc'd to the exact length so ASAN guards
               BOTH sides, and the lengths are the two that matter:
               the record needs len+1 bytes because it is NUL
               terminated as well as counted -- vfs_write() takes the
               count, and anything reading the file as a string takes
               the terminator. A writer that kept no byte back for it
               would pass every check with a generous buffer and write
               one byte past a tight one. */
            {
                char *exact = (char *)malloc(len);
                char *plus1 = (char *)malloc(len + 1);
                uint32_t n3 = 0, n4 = 0;
                ck("exactly the record's length is REFUSED -- the "
                   "terminator needs one more",
                   exact && pair_store_encode(&st, exact, len, &n3)
                   == PAIR_E_SPACE, (long)n3);
                ck("and one byte more is accepted",
                   plus1 && pair_store_encode(&st, plus1, len + 1, &n4)
                   == PAIR_OK, (long)n4);
                ck("at the same length as before",
                   n4 == len, (long)n4);
                ck("terminated inside its own buffer",
                   plus1 && plus1[len] == 0, 0);
                /* And every length below it refused, not truncated:
                   a short record that reported success would be a
                   file holding a key that is almost right. */
                {
                    uint32_t cap2;
                    int accepted = 0;
                    char *buf2 = (char *)malloc(len + 1);
                    for(cap2 = 1; cap2 <= len; cap2++)
                        if(pair_store_encode(&st, buf2, cap2, &n3)
                           == PAIR_OK) accepted++;
                    ck("no capacity below len+1 is accepted",
                       accepted == 0, accepted);
                    free(buf2);
                }
                free(exact); free(plus1);
            }

            t2 = st; t2.rec.host_id[0] = 0;
            ck("no HostID", pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_PARAM, 0);
            t2 = st; t2.rec.system_buid[0] = 0;
            ck("no SystemBUID",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_PARAM, 0);
            t2 = st; t2.rec.host_len = 0;
            ck("a missing certificate",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_CERT, 0);
            /* The one that would look complete and fail later. */
            t2 = st; bn_zero(&t2.host.d);
            ck("a record with no host private key is refused",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_KEY, 0);
            t2 = st; bn_zero(&t2.root.d);
            ck("nor one with no root private key",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_KEY, 0);
            t2 = st; bn_set_u32(&t2.host.n, 65537);
            ck("nor one with a key too small to pair with",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_KEY, 0);
            /* A PUBLIC exponent of zero is not a key either, and it is
               the one field the size and private-key checks above say
               nothing about. The injection harness asked for this:
               without it, a zero exponent encoded as a zero-length
               field, which is a record that writes cleanly and will
               not read back. */
            t2 = st; bn_zero(&t2.host.e);
            ck("nor one with no public exponent",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_KEY, 0);
            t2 = st; bn_zero(&t2.root.e);
            ck("nor one with no root public exponent",
               pair_store_encode(&t2, text, sizeof text, &n2)
               == PAIR_E_KEY, 0);
        }

        /* ── the signature net/tls.c will ask for ────────────────────
           Signed with the HOST key, because the host certificate is
           what certifies it. One struct field away from a signature
           the device refuses while every certificate in the chain
           checks out -- which is the defect
           tests/test_tlscc.c's negative control is also aimed at. */
        head("pair_tls_sign signs with the host key, and openssl agrees");
        {
            uint8_t dg[32], sig[512];
            uint32_t slen = 0;
            uint32_t k = (bn_bits(&back.host.n) + 7u) / 8u;
            int rr;

            sha256((const uint8_t *)"the handshake transcript", 24, dg);
            rr = pair_tls_sign(&back.host, dg, sig, sizeof sig, &slen);
            ck("it signs", rr == PAIR_OK, rr);
            ck("at exactly the modulus length, as I2OSP requires",
               slen == k, (long)slen);

            write_der("/tmp/hd_ps_dg.bin", dg, 32);
            write_der("/tmp/hd_ps_sig.bin", sig, slen);
            /* openssl's verdict, against the key openssl generated.
               `-pkeyopt digest:sha256` makes it the PKCS#1 v1.5
               DigestInfo check rather than a raw modexp. */
            ck("openssl verifies it against the host key",
               sh("openssl pkeyutl -verify -inkey /tmp/hd_pair_host_key.pem "
                  "-pkeyopt digest:sha256 -sigfile /tmp/hd_ps_sig.bin "
                  "-in /tmp/hd_ps_dg.bin >/dev/null 2>&1"), 0);
            /* And NOT against the root key, which is the wrong-field
               defect stated as a control. */
            ck("but NOT against the root key",
               !sh("openssl pkeyutl -verify -inkey /tmp/hd_pair_root_key.pem "
                   "-pkeyopt digest:sha256 -sigfile /tmp/hd_ps_sig.bin "
                   "-in /tmp/hd_ps_dg.bin >/dev/null 2>&1"), 0);
            /* A judge that never convicts: one byte of the digest
               changed must fail. */
            dg[7] ^= 0x01;
            write_der("/tmp/hd_ps_dg.bin", dg, 32);
            ck("and not over a different digest",
               !sh("openssl pkeyutl -verify -inkey /tmp/hd_pair_host_key.pem "
                   "-pkeyopt digest:sha256 -sigfile /tmp/hd_ps_sig.bin "
                   "-in /tmp/hd_ps_dg.bin >/dev/null 2>&1"), 0);

            ck("a NULL key is refused",
               pair_tls_sign(0, dg, sig, sizeof sig, &slen)
               == PAIR_E_PARAM, 0);
            ck("a buffer too small for the signature is refused",
               pair_tls_sign(&back.host, dg, sig, 8, &slen)
               == PAIR_E_KEY, 0);
        }
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
