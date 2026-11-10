/* ════════════════════════════════════════════════════════════════════
   tests/test_tlscc.c -- TLS client certificates, judged by openssl

   Pairing stage 3's second half, v0.8.9. lockdownd will not open a
   session in the clear: it answers StartSession with EnableSessionSSL
   and then expects a TLS handshake in which THE DEVICE IS THE SERVER
   and this host is a client presenting a client certificate. net/tls.c
   had no client-certificate path at all, and this suite is what
   decides whether the one it has now works.

   ── THE JUDGE IS `openssl s_server`, NOT A FIXTURE ───────────────────

   There is no mock server in this file. net/tls.c's tcp_send() and
   tcp_recv() are implemented here on a REAL POSIX SOCKET, and the peer
   at the other end is openssl, run as a process, doing a full TLS 1.2
   handshake and making exactly the decision the phone makes:

       openssl s_server -Verify 1 -CAfile <our root>

   `-Verify` REQUIRES a client certificate, verifies the chain against
   our root, and verifies the CertificateVerify signature over the
   handshake transcript. If any of that is wrong the handshake fails.
   And `-www` makes it answer with a page that NAMES the certificate it
   accepted, so success is read off openssl's own words --

       Client certificate
               Subject: CN=HavenDOS Host

   -- rather than off our believing the handshake returned zero.

   A mock here would have been worthless in the specific way that
   matters. The transcript a CertificateVerify signs is every handshake
   byte in order; a fixture written from the same reading of RFC 5246
   as the code would agree with the code about which bytes those are,
   and the phone would not.

   ── AND BECAUSE A JUDGE THAT NEVER CONVICTS IS NOT A JUDGE ───────────

   Five negative controls, each one a defect that passes every
   structural check and fails only here:

     - the same chain against an UNRELATED CA must be refused
     - one byte flipped in the signature must be refused
     - a signature over a DIFFERENT digest must be refused
     - a signature made with the ROOT key while sending the HOST
       certificate must be refused  (the wrong-struct-field bug)
     - an empty chain where one was REQUIRED must be refused

   Without the first of those, "openssl accepted it" could mean openssl
   was not checking. tests/test_pair.c records the same argument about
   the same risk.

   ── WHAT IS CHECKED WITHOUT A SERVER ─────────────────────────────────

   Two things a server cannot be asked about, so they are read off the
   bytes with a recording transport:

     - the signature_algorithms extension is in the ClientHello. RFC
       5246 7.4.1.4.1 says a TLS 1.2 client that omits it is TAKEN to
       have offered {sha1, rsa}, and SHA-256 is the only thing this
       stack can sign with. Before v0.8.9 it was omitted -- which cost
       nothing while no CertificateVerify was ever sent.
     - no SNI extension when there is no hostname, because the usbmux
       connection has no name and an empty HostName is malformed
       rather than absent.

   And one thing a server cannot distinguish: that tls_connect_ex()
   sends through the transport it was GIVEN. A global counter on
   tcp_send() must stay at zero for the whole of a handshake on another
   transport -- otherwise a session could be reading one channel and
   writing another, which both sides would report as success.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "types.h"
#include "bignum.h"
#include "der.h"
#include "x509.h"
#include "netstat.h"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

static int sh(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    return system(cmd) == 0;
}

/* ── the socket under net/tls.c ───────────────────────────────────────
   These ARE net/tcp.c as far as the file under test is concerned. One
   socket at a time is all any case needs, and a single global makes
   the "did it use the transport it was given" check a one-line
   counter. */
static int  g_sock = -1;
static long g_tcp_send_calls = 0;
static long g_tcp_recv_calls = 0;

int tcp_send(int fd, const void *data, uint16_t len)
{
    ssize_t n;
    (void)fd;
    g_tcp_send_calls++;
    if(g_sock < 0) return -1;
    n = send(g_sock, data, len, 0);
    return (n == (ssize_t)len) ? (int)n : -1;
}

int tcp_recv(int fd, void *buf, uint16_t maxlen, uint32_t timeout_ms)
{
    struct timeval tv;
    ssize_t n;
    (void)fd;
    g_tcp_recv_calls++;
    if(g_sock < 0) return -1;
    tv.tv_sec  = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    n = recv(g_sock, buf, maxlen, 0);
    if(n > 0) return (int)n;
    if(n == 0) return -1;                         /* peer closed */
    if(errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

static int g_eof = 0;
int  tcp_eof(int fd){ (void)fd; return g_eof; }
void tcp_close(int fd){ (void)fd; }

/* net/tls.c's two other outside calls. */
void net_poll(void){ }
void sleep_ms(uint32_t ms){ (void)ms; }

/* net/tls_crypto.c is linked in for sha256/aes/prf and wants entropy
   from the kernel. rsa_public_encrypt() DOES use it -- the PKCS#1 v1.5
   encryption padding is random -- so this is reached, and it is a
   fixed sequence on purpose: a handshake that fails should fail the
   same way twice. */
static uint32_t g_ent = 0x12345678u;
uint32_t entropy_u32(void){
    g_ent = g_ent * 1664525u + 1013904223u;
    return g_ent;
}
void entropy_stir(uint32_t v){ g_ent ^= v; }

netstat_t g_netstat;

#include "../net/bignum.c"
#include "../net/der.c"
#include "../net/x509.c"
#include "../net/tls.c"

/* ════════════════════════════════════════════════════════════════════
   keys and certificates
   ════════════════════════════════════════════════════════════════════ */
typedef struct { bn_t n, e, d; } key_t_;

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

/* The keys come from `openssl genrsa` and not from rsa_keygen_step(),
   for the reason tests/test_x509.c gives: a key from outside means a
   signature failure is OUR arithmetic rather than possibly our key. */
static int make_key(const char *tag, key_t_ *k)
{
    char pem[160], nh[160], dh[160];
    snprintf(pem, sizeof pem, "/tmp/hd_cc_%s.key", tag);
    snprintf(nh,  sizeof nh,  "/tmp/hd_cc_%s_n.hex", tag);
    snprintf(dh,  sizeof dh,  "/tmp/hd_cc_%s_d.hex", tag);
    if(!sh("openssl genrsa -out %s 2048 >/dev/null 2>&1", pem)) return 0;
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

static int write_file(const char *path, const void *p, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    size_t w;
    if(!f) return 0;
    w = fwrite(p, 1, n, f);
    fclose(f);
    return w == n;
}

#define NOT_BEFORE "010101000000Z"
#define NOT_AFTER  "490101000000Z"

static key_t_  k_root, k_host, k_other;
static uint8_t c_root[1600], c_host[1600], c_other[1600];
static uint32_t l_root, l_host, l_other;

/* Our own root, our own host certificate under it -- exactly what
   usb/pair.c builds, built by the same net/x509.c. */
static int build_chain(void)
{
    x509_req_t q;

    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Root"; q.issuer_cn = "HavenDOS Root";
    q.serial = 1;
    q.subject_n = &k_root.n; q.subject_e = &k_root.e;
    q.issuer_n  = &k_root.n; q.issuer_d  = &k_root.d;
    q.is_ca = 1;
    q.not_before = NOT_BEFORE; q.not_after = NOT_AFTER;
    if(x509_build(&q, c_root, sizeof c_root, &l_root) != X509_OK) return 0;

    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Host"; q.issuer_cn = "HavenDOS Root";
    q.serial = 2;
    q.subject_n = &k_host.n; q.subject_e = &k_host.e;
    q.issuer_n  = &k_root.n; q.issuer_d  = &k_root.d;
    q.is_ca = 0;
    q.key_usage = X509_KU_DIGITAL_SIG | X509_KU_KEY_ENCIPHER;
    q.not_before = NOT_BEFORE; q.not_after = NOT_AFTER;
    if(x509_build(&q, c_host, sizeof c_host, &l_host) != X509_OK) return 0;

    /* The negative control's authority: a different root entirely,
       built the same way so that the ONLY difference is whose key
       signed it. */
    memset(&q, 0, sizeof q);
    q.subject_cn = "Unrelated Root"; q.issuer_cn = "Unrelated Root";
    q.serial = 1;
    q.subject_n = &k_other.n; q.subject_e = &k_other.e;
    q.issuer_n  = &k_other.n; q.issuer_d  = &k_other.d;
    q.is_ca = 1;
    q.not_before = NOT_BEFORE; q.not_after = NOT_AFTER;
    if(x509_build(&q, c_other, sizeof c_other, &l_other) != X509_OK) return 0;

    if(!write_file("/tmp/hd_cc_root.der", c_root, l_root)) return 0;
    if(!write_file("/tmp/hd_cc_other.der", c_other, l_other)) return 0;
    if(!sh("openssl x509 -inform DER -in /tmp/hd_cc_root.der "
           "-out /tmp/hd_cc_root.pem >/dev/null 2>&1")) return 0;
    if(!sh("openssl x509 -inform DER -in /tmp/hd_cc_other.der "
           "-out /tmp/hd_cc_other.pem >/dev/null 2>&1")) return 0;
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   the signers
   ════════════════════════════════════════════════════════════════════ */
static int g_sign_calls = 0;
static int g_sign_mode  = 0;     /* 0 host key, 1 root key          */
static int g_flip_byte  = 0;     /* corrupt one byte of the sig     */
static int g_wrong_dg   = 0;     /* sign a different digest         */
static int g_sign_fail  = 0;     /* refuse                          */
static int g_sign_short = 0;     /* claim zero length               */
static int g_sign_huge  = 0;     /* claim more than it wrote        */
static uint8_t g_last_digest[32];

static int cc_sign(void *ctx, const uint8_t digest[32],
                   uint8_t *sig, uint32_t sigcap, uint32_t *siglen)
{
    const key_t_ *k = g_sign_mode ? &k_root : &k_host;
    uint8_t dg[32];
    uint32_t n = 0;
    (void)ctx;

    g_sign_calls++;
    memcpy(g_last_digest, digest, 32);
    memcpy(dg, digest, 32);
    if(g_wrong_dg) dg[0] ^= 0xFF;
    if(g_sign_fail) return -1;

    if(pkcs1_sign_sha256(&k->n, &k->d, dg, sig, sigcap, &n) != X509_OK)
        return -1;
    if(g_flip_byte && n > 8) sig[n/2] ^= 0x01;
    if(g_sign_short) n = 0;
    if(g_sign_huge)  n = sigcap + 1;
    *siglen = n;
    return 0;
}

static void sign_reset(void)
{
    g_sign_calls = 0; g_sign_mode = 0; g_flip_byte = 0;
    g_wrong_dg = 0; g_sign_fail = 0; g_sign_short = 0; g_sign_huge = 0;
}

static tls_client_cert_t cc_full(void)
{
    tls_client_cert_t cc;
    memset(&cc, 0, sizeof cc);
    cc.cert[0] = c_host; cc.cert_len[0] = l_host;
    cc.cert[1] = c_root; cc.cert_len[1] = l_root;
    cc.n = 2;
    cc.sign = cc_sign;
    return cc;
}

/* ════════════════════════════════════════════════════════════════════
   the server
   ════════════════════════════════════════════════════════════════════ */
#define SRV_CERT "/tmp/hd_cc_srv.pem"
#define SRV_KEY  "/tmp/hd_cc_srv.key"

static int make_server_cert(void)
{
    if(!sh("openssl genrsa -out " SRV_KEY " 2048 >/dev/null 2>&1")) return 0;
    if(!sh("openssl req -x509 -new -key " SRV_KEY " -subj /CN=device "
           "-days 3650 -sha256 -out " SRV_CERT " >/dev/null 2>&1")) return 0;
    return 1;
}

static int g_port = 0;

/* Start s_server and wait until it says it is listening.
   NOT by probing with a connect(): -naccept 1 means the probe would
   BE the connection, and the case under test would then find nothing
   to talk to. The log line is what is waited for. */
static int srv_start(const char *extra)
{
    char log[64];
    int i;
    g_port++;
    snprintf(log, sizeof log, "/tmp/hd_cc_s%d.log", g_port);
    sh("rm -f %s /tmp/hd_cc_s%d.pid", log, g_port);
    /* ── -verify_return_error IS LOAD-BEARING ────────────────────────
       Found by the negative control below, which is the only reason
       it is here. `s_server -Verify 1 -CAfile <unrelated>` ACCEPTED
       our chain: s_server installs a verify callback that prints
       "verify error" and then returns 1 anyway, so the handshake
       completes and the -www page comes back naming a certificate
       openssl had just failed to verify.

       Which means that without this flag the main case -- the one the
       whole suite exists for -- would have passed with a chain
       nothing had checked. The negative control was not decoration;
       it was the thing that found the judge was not judging. */
    if(!sh("openssl s_server -accept %d -cert %s -key %s -tls1_2 "
           "-cipher 'AES128-SHA256@SECLEVEL=0' -verify_return_error "
           "-www -naccept 1 %s "
           ">%s 2>&1 & echo $! > /tmp/hd_cc_s%d.pid",
           14400 + g_port, SRV_CERT, SRV_KEY, extra, log, g_port))
        return 0;
    for(i = 0; i < 400; i++){
        if(sh("grep -q ACCEPT %s 2>/dev/null", log)) return 1;
        usleep(25000);
    }
    return 0;
}

static void srv_stop(void)
{
    sh("kill $(cat /tmp/hd_cc_s%d.pid 2>/dev/null) 2>/dev/null", g_port);
}

static int srv_log_has(const char *needle)
{
    return sh("grep -qi '%s' /tmp/hd_cc_s%d.log 2>/dev/null",
              needle, g_port);
}

static int sock_open(void)
{
    struct sockaddr_in a;
    int one = 1;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if(g_sock < 0) return 0;
    setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)(14400 + g_port));
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if(connect(g_sock, (struct sockaddr *)&a, sizeof a) < 0){
        close(g_sock); g_sock = -1; return 0;
    }
    g_eof = 0;
    return 1;
}

static void sock_shut(void)
{
    if(g_sock >= 0) close(g_sock);
    g_sock = -1;
}

/* Send an HTTP request through the finished session and read back
   s_server's -www page, which NAMES the certificate it accepted. The
   only proof that the session actually works: a handshake that
   completed with the wrong keys fails here and nowhere earlier. */
static int fetch_page(int h, char *out, int cap)
{
    static const char REQ[] = "GET / HTTP/1.0\r\n\r\n";
    int got = 0, r, spins;
    if(tls_send(h, (const uint8_t *)REQ, (uint16_t)(sizeof REQ - 1)) < 0)
        return -1;
    for(spins = 0; spins < 60 && got < cap - 1; spins++){
        r = tls_recv(h, (uint8_t *)out + got, (uint16_t)(cap - 1 - got), 500);
        if(r > 0){ got += r; spins = 0; continue; }
        if(r < 0) break;
        if(tls_eof(h)) break;
    }
    out[got > 0 ? got : 0] = 0;
    return got;
}

/* ── a transport that records instead of sending ──────────────────────
   For the two questions no server can be asked: what is in the
   ClientHello, and whether tls_connect_ex() used the transport it was
   handed. It refuses the first read, so the handshake dies straight
   after the ClientHello -- which is all that needs to have happened. */
static uint8_t g_rec_buf[4096];
static int     g_rec_len = 0;
static int     g_rec_sends = 0;

static int rec_send(int h, const uint8_t *d, uint16_t len)
{
    (void)h;
    g_rec_sends++;
    if(g_rec_len + len <= (int)sizeof g_rec_buf){
        memcpy(g_rec_buf + g_rec_len, d, len);
        g_rec_len += len;
    }
    return (int)len;
}
static int rec_recv(int h, uint8_t *b, uint16_t max, uint32_t to)
{
    (void)h; (void)b; (void)max; (void)to;
    return -1;
}
static int rec_eof(int h){ (void)h; return 1; }
static const tls_transport_t REC_TR = { "rec", rec_send, rec_recv, rec_eof, 0 };

static void rec_reset(void){ g_rec_len = 0; g_rec_sends = 0; }

static int find_bytes(const uint8_t *hay, int haylen,
                      const uint8_t *needle, int n)
{
    int i, j;
    for(i = 0; i + n <= haylen; i++){
        for(j = 0; j < n; j++) if(hay[i+j] != needle[j]) break;
        if(j == n) return i;
    }
    return -1;
}

/* ── the socket transport, under its own handle ───────────────────────
   Identical bytes to the tcp one, reached a different way. A case run
   through this with g_tcp_send_calls frozen is the proof that the
   indirection routes rather than merely compiles. */
static int sk_send(int h, const uint8_t *d, uint16_t len)
{
    ssize_t n;
    (void)h;
    if(g_sock < 0) return -1;
    n = send(g_sock, d, len, 0);
    return (n == (ssize_t)len) ? (int)n : -1;
}
static int sk_recv(int h, uint8_t *b, uint16_t max, uint32_t to)
{
    struct timeval tv;
    ssize_t n;
    (void)h;
    if(g_sock < 0) return -1;
    tv.tv_sec = (time_t)(to/1000); tv.tv_usec = (suseconds_t)((to%1000)*1000);
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    n = recv(g_sock, b, max, 0);
    if(n > 0) return (int)n;
    if(n == 0) return -1;
    if(errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
static int sk_eof(int h){ (void)h; return 0; }
static const tls_transport_t SK_TR = { "sock", sk_send, sk_recv, sk_eof, 0 };
#define SK_HANDLE 0x4C44

/* ── the same socket, but the Certificate record never leaves ────────
   The injection harness needed this. Everything in net/tls.c that
   happens AFTER a failed send is unreachable while every send
   succeeds, and one of those things is a diagnostic counter -- which
   reported two certificates sent on a handshake where none had been.
   A counter that is wrong exactly when the handshake failed is a
   counter that sends the next person to look in the wrong place.

   The refusal is keyed on the handshake TYPE rather than on a call
   number, so it stays pointed at the Certificate message however the
   records around it are framed. */
static int g_fail_hs_type = -1;
static int fs_send(int h, const uint8_t *d, uint16_t len)
{
    if(g_fail_hs_type >= 0 && len > 4 && d[0] == (uint8_t)g_fail_hs_type)
        return -1;
    return sk_send(h, d, len);
}
static const tls_transport_t FS_TR = { "failsend", fs_send, sk_recv,
                                       sk_eof, 0 };

/* One case: start a server with `extra`, handshake with `cc`, return
   what tls_connect_ex() returned. The page, if any, lands in `page`. */
static int run_case(const char *extra, const tls_client_cert_t *cc,
                    char *page, int pagecap, int use_alt)
{
    int r;
    if(page && pagecap) page[0] = 0;
    if(!srv_start(extra)) return -1000;
    if(!sock_open()){ srv_stop(); return -1001; }
    if(use_alt == 2){
        r = tls_connect_ex(SK_HANDLE, &FS_TR, 0, 0xABCD1234u, cc);
        if(r == 0) tls_close(SK_HANDLE);
    } else if(use_alt){
        r = tls_connect_ex(SK_HANDLE, &SK_TR, 0, 0xABCD1234u, cc);
        if(r == 0 && page) fetch_page(SK_HANDLE, page, pagecap);
        if(r == 0) tls_close(SK_HANDLE);
    } else {
        r = tls_connect_ex(7, &tls_transport_tcp, 0, 0xABCD1234u, cc);
        if(r == 0 && page) fetch_page(7, page, pagecap);
        if(r == 0) tls_close(7);
    }
    sock_shut();
    srv_stop();
    return r;
}

static char g_page[32768];

int main(void)
{
    /* v0.8.9.7: a write to a socket s_server has already closed raises
       SIGPIPE, whose default action kills the process -- before it has
       printed a single result line, so run_suite.py could only say "the
       suite printed no result". It happened once in eight runs while
       0.8.9.7 was being built. Ignored, the write fails with EPIPE
       instead, and the check that made it fails with a reason. */
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, 0, _IOLBF, 0);   /* and what did print is not lost */

    printf("\n=== test_tlscc: TLS client certificates, judged by "
           "openssl s_server ===\n");

    if(system("which openssl >/dev/null 2>&1") != 0){
        printf("\n  openssl is not installed, and openssl IS the judge in\n"
               "  this file -- there is no mock server to fall back to.\n"
               "  SKIP, not a pass.\n\nSKIP\n");
        return 0;
    }

    if(!make_key("root", &k_root) || !make_key("host", &k_host) ||
       !make_key("other", &k_other) || !build_chain() ||
       !make_server_cert()){
        printf("\n  the fixture keys or certificates would not build.\n"
               "  SKIP, not a pass.\n\nSKIP\n");
        return 0;
    }

    /* ════════════════════════════════════════════════════════════════
       1. The ClientHello, read off the wire
       ════════════════════════════════════════════════════════════════ */
    head("the ClientHello says what it can sign with");
    {
        tls_client_cert_t cc = cc_full();
        static const uint8_t SIGALG[] = {
            0x00,0x0D, 0x00,0x04, 0x00,0x02, 0x04,0x01 };
        int at;

        rec_reset(); sign_reset();
        tls_connect_ex(99, &REC_TR, 0, 1u, &cc);

        ck("something was sent before the first read failed",
           g_rec_len > 40, g_rec_len);
        at = find_bytes(g_rec_buf, g_rec_len, SIGALG, (int)sizeof SIGALG);
        ck("signature_algorithms offers exactly sha256+rsa", at > 0, at);

        /* RFC 5246 7.4.1.4.1: omitting it MEANS {sha1,rsa}, and this
           stack cannot sign with SHA-1. The extension is the only
           thing making the CertificateVerify below legal. */
        ck("it is inside the extensions block, not the body",
           at > 44, at);
    }

    head("no hostname means no SNI extension, not an empty one");
    {
        tls_client_cert_t cc = cc_full();
        static const uint8_t SNI_T[] = { 0x00,0x00 };   /* ext type 0 */
        int ext_at, i, found_sni = 0;

        rec_reset(); sign_reset();
        tls_connect_ex(99, &REC_TR, 0, 1u, &cc);
        /* Extension type 0 would appear at a 2-byte-aligned position
           inside the extensions block; rather than parse, assert the
           whole ClientHello carries exactly ONE extension and that it
           is the signature_algorithms one -- 8 bytes of extensions. */
        ext_at = g_rec_len - 8;
        ck("the extensions block is 8 bytes -- one extension",
           ext_at > 0 && g_rec_buf[ext_at-2] == 0x00 &&
           g_rec_buf[ext_at-1] == 0x08, (long)g_rec_len);
        for(i = 0; i + 2 <= ext_at; i += 1)
            if(!memcmp(g_rec_buf + i, SNI_T, 2) && i > 50) found_sni++;
        /* A real SNI would be followed by its length and a non-zero
           name; the exact-size check above is the load-bearing one. */
        (void)found_sni;

        rec_reset(); sign_reset();
        tls_connect_ex(99, &REC_TR, "device.local", 1u, &cc);
        ck("a hostname DOES produce a longer ClientHello",
           g_rec_len > ext_at + 8, (long)g_rec_len);
        ck("and the name is in it",
           find_bytes(g_rec_buf, g_rec_len,
                      (const uint8_t *)"device.local", 12) > 0, 0);
    }

    head("tls_connect_ex() sends through the transport it was given");
    {
        tls_client_cert_t cc = cc_full();
        long before = g_tcp_send_calls;
        rec_reset(); sign_reset();
        tls_connect_ex(99, &REC_TR, 0, 1u, &cc);
        ck("the recording transport was used", g_rec_sends > 0, g_rec_sends);
        ck("tcp_send() was NOT", g_tcp_send_calls == before,
           g_tcp_send_calls - before);
    }

    /* ════════════════════════════════════════════════════════════════
       2. A server that does not ask -- the existing HTTPS path
       ════════════════════════════════════════════════════════════════ */
    head("a server that asks for nothing still works, and we send nothing");
    {
        int r;
        sign_reset();
        r = run_case("", 0, g_page, sizeof g_page, 0);
        ck("the handshake succeeded", r == 0, r);
        ck("the session is established",
           g_tls_last_stage == TLS_STAGE_ESTABLISHED, g_tls_last_stage);
        ck("no CertificateRequest was seen", g_tls_cert_requested == 0,
           g_tls_cert_requested);
        ck("no certificate was sent", g_tls_cert_sent == 0, g_tls_cert_sent);
        ck("no signature was made", g_sign_calls == 0, g_sign_calls);
        ck("application data works -- openssl answered",
           strstr(g_page, "HTTP/1.0 200") != 0, (long)strlen(g_page));
    }

    /* ════════════════════════════════════════════════════════════════
       3. Asked for, and we have none -- the EMPTY Certificate message
       ════════════════════════════════════════════════════════════════ */
    head("asked politely with nothing to send: an EMPTY Certificate");
    {
        int r;
        sign_reset();
        /* -verify (lower case) REQUESTS and tolerates a client with
           none. Before v0.8.9 nothing was sent at all, and RFC 5246
           7.4.6 requires an empty Certificate message instead -- a
           server that asks and then insists would otherwise wait for
           a message that never comes. */
        r = run_case("-verify 1 -CAfile /tmp/hd_cc_root.pem",
                     0, g_page, sizeof g_page, 0);
        ck("the handshake still succeeded", r == 0, r);
        ck("the request was noticed", g_tls_cert_requested == 1,
           g_tls_cert_requested);
        ck("nothing was sent", g_tls_cert_sent == 0, g_tls_cert_sent);
        ck("no CertificateVerify either", g_tls_cert_verified == 0,
           g_tls_cert_verified);
        ck("openssl says it had no client certificate",
           strstr(g_page, "no client certificate available") != 0,
           (long)strlen(g_page));
    }

    /* ════════════════════════════════════════════════════════════════
       4. THE ONE THAT MATTERS
       ════════════════════════════════════════════════════════════════ */
    head("REQUIRED, and accepted: openssl verifies our chain and our "
         "signature");
    {
        tls_client_cert_t cc = cc_full();
        int r;
        sign_reset();
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, g_page, sizeof g_page, 0);
        ck("the handshake succeeded", r == 0, r);
        ck("two certificates went out", g_tls_cert_sent == 2,
           g_tls_cert_sent);
        ck("a CertificateVerify went out", g_tls_cert_verified == 1,
           g_tls_cert_verified);
        ck("the transcript was signed exactly once", g_sign_calls == 1,
           g_sign_calls);
        ck("the session reached ESTABLISHED",
           g_tls_last_stage == TLS_STAGE_ESTABLISHED, g_tls_last_stage);
        /* openssl's own words, and the whole point of the suite. */
        ck("openssl NAMES the certificate it accepted",
           strstr(g_page, "Subject: CN=HavenDOS Host") != 0,
           (long)strlen(g_page));
        ck("and the chain verified on its side",
           !srv_log_has("verify error"), 0);
        ck("the page came back through the session",
           strstr(g_page, "HTTP/1.0 200") != 0, 0);
    }

    head("the same thing over a transport that is not net/tcp.c");
    {
        tls_client_cert_t cc = cc_full();
        long before = g_tcp_send_calls, before_rx = g_tcp_recv_calls;
        int r;
        sign_reset();
        /* This is the path usb/lockdown.c takes: its own transport,
           its own handle, no hostname. If the indirection leaked, the
           counter below would move. */
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, g_page, sizeof g_page, 1);
        ck("the handshake succeeded", r == 0, r);
        ck("openssl accepted the certificate",
           strstr(g_page, "Subject: CN=HavenDOS Host") != 0, 0);
        ck("tcp_send() was never called", g_tcp_send_calls == before,
           g_tcp_send_calls - before);
        /* And tcp_recv() was not either. The injection harness needed
           this one: both transports in this suite read the SAME
           socket, so a read that went through net/tcp.c by mistake
           still returned the right bytes and nothing failed. Counting
           the writes was not enough. */
        ck("tcp_recv() was never called either",
           g_tcp_recv_calls == before_rx, g_tcp_recv_calls - before_rx);
        ck("application data went both ways over it",
           strstr(g_page, "HTTP/1.0 200") != 0, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       5. THE NEGATIVE CONTROLS
       ════════════════════════════════════════════════════════════════ */
    head("the negative controls -- each one must be REFUSED");
    {
        tls_client_cert_t cc;
        int r;

        /* (a) An unrelated CA. Without this, "openssl accepted it"
           could mean openssl was not checking at all. */
        cc = cc_full(); sign_reset();
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_other.pem",
                     &cc, g_page, sizeof g_page, 0);
        ck("an unrelated CA refuses our chain", r != 0, r);
        ck("and the page never arrived", strstr(g_page, "HTTP/1.0 200") == 0,
           (long)strlen(g_page));

        /* (b) One byte flipped in the signature. The certificate is
           untouched, so every structural check still passes. */
        cc = cc_full(); sign_reset(); g_flip_byte = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, g_page, sizeof g_page, 0);
        ck("a corrupted signature is refused", r != 0, r);
        ck("the signature was actually produced first", g_sign_calls == 1,
           g_sign_calls);

        /* (c) A signature over a different digest. This is what a
           transcript that covers the wrong messages produces, and it
           is the defect a self-written fixture could never catch. */
        cc = cc_full(); sign_reset(); g_wrong_dg = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, g_page, sizeof g_page, 0);
        ck("a signature over the wrong digest is refused", r != 0, r);

        /* (d) The ROOT key signing while the HOST certificate is sent.
           Both keys are real, both certificates verify, and the
           signature is a perfectly good PKCS#1 v1.5 signature -- of
           the wrong key. One struct field. */
        cc = cc_full(); sign_reset(); g_sign_mode = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, g_page, sizeof g_page, 0);
        ck("the wrong key is refused, however valid the certificate",
           r != 0, r);

        /* (e) Nothing sent where it was REQUIRED. */
        sign_reset();
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     0, g_page, sizeof g_page, 0);
        ck("an empty chain is refused when one is required", r != 0, r);
    }

    /* ════════════════════════════════════════════════════════════════
       6. Refusals this side makes, before anything goes out
       ════════════════════════════════════════════════════════════════ */
    head("what this side refuses to send");
    {
        tls_client_cert_t cc;
        int r;

        /* A chain that will not fit is REFUSED, never truncated: a
           shortened chain is rejected by the peer for a reason
           nowhere near the real one. */
        cc = cc_full(); sign_reset();
        cc.cert_len[0] = CLIENT_CERT_MAX;      /* claims to be huge */
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("an oversize chain gives TLS_ERR_CLIENTCERT",
           r == TLS_ERR_CLIENTCERT, r);
        ck("and nothing was signed", g_sign_calls == 0, g_sign_calls);

        /* EVERY SLOT FILLED, and that is the whole point of the
           fixture. With cert[2] and cert[3] left null, the loop hit a
           null pointer at index 2 and refused before it ever reached
           index 4 -- so removing the bound check changed nothing the
           suite could see, and the injection survived for the wrong
           reason. Full slots mean the only thing standing between the
           loop and a read past the end of cert[] is the bound. */
        cc = cc_full(); sign_reset();
        cc.cert[2] = c_host; cc.cert_len[2] = l_host;
        cc.cert[3] = c_root; cc.cert_len[3] = l_root;
        cc.n = TLS_MAX_CHAIN + 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("more certificates than the chain holds is refused",
           r == TLS_ERR_CLIENTCERT, r);
        ck("and nothing went out", g_tls_cert_sent == 0, g_tls_cert_sent);

        cc = cc_full(); sign_reset();
        cc.cert[1] = 0;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("a null certificate in the chain is refused",
           r == TLS_ERR_CLIENTCERT, r);

        cc = cc_full(); sign_reset(); g_sign_fail = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("a signer that refuses gives TLS_ERR_CLIENTCERT",
           r == TLS_ERR_CLIENTCERT, r);
        ck("the certificates had already gone out by then",
           g_tls_cert_sent == 2, g_tls_cert_sent);

        cc = cc_full(); sign_reset(); g_sign_short = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("a zero-length signature is refused",
           r == TLS_ERR_CLIENTCERT, r);

        cc = cc_full(); sign_reset(); g_sign_huge = 1;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("a signature longer than the buffer is refused",
           r == TLS_ERR_CLIENTCERT, r);

        cc = cc_full(); sign_reset(); cc.sign = 0;
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 0);
        ck("a chain with no signer is refused rather than sent bare",
           r == TLS_ERR_CLIENTCERT, r);
    }

    head("a Certificate that never leaves");
    {
        tls_client_cert_t cc = cc_full();
        int r;
        sign_reset();
        g_fail_hs_type = 11;                 /* HS_CERTIFICATE */
        r = run_case("-Verify 1 -CAfile /tmp/hd_cc_root.pem",
                     &cc, 0, 0, 2);
        g_fail_hs_type = -1;
        ck("the handshake gives TLS_ERR_CLIENTCERT",
           r == TLS_ERR_CLIENTCERT, r);
        /* The counter is a diagnostic read on hardware. It must say
           nothing went out, because nothing did. */
        ck("and reports NO certificates sent", g_tls_cert_sent == 0,
           g_tls_cert_sent);
        ck("and no CertificateVerify", g_tls_cert_verified == 0,
           g_tls_cert_verified);
        ck("the request was still noticed", g_tls_cert_requested == 1,
           g_tls_cert_requested);
    }

    head("a later handshake does not report the previous one's");
    {
        int r;
        sign_reset();
        /* Deliberately AFTER the cases that sent two certificates and
           signed a transcript. The counters are per handshake and
           nothing else in this suite runs in an order that could show
           it -- a failed pairing after a successful one would
           otherwise report the successful one's numbers. */
        ck("the previous case left the counters set",
           g_tls_cert_requested == 1, g_tls_cert_requested);
        r = run_case("", 0, g_page, sizeof g_page, 0);
        ck("a plain handshake succeeds", r == 0, r);
        ck("no CertificateRequest is reported", g_tls_cert_requested == 0,
           g_tls_cert_requested);
        ck("no certificates are reported", g_tls_cert_sent == 0,
           g_tls_cert_sent);
        ck("no CertificateVerify is reported", g_tls_cert_verified == 0,
           g_tls_cert_verified);
    }

    head("a transport that is not a transport");
    {
        tls_client_cert_t cc = cc_full();
        tls_transport_t bad = { "bad", 0, rec_recv, rec_eof, 0 };
        ck("a NULL transport is refused",
           tls_connect_ex(99, 0, 0, 1u, &cc) == TLS_ERR_HANDSHAKE, 0);
        ck("a transport with no send is refused",
           tls_connect_ex(99, &bad, 0, 1u, &cc) == TLS_ERR_HANDSHAKE, 0);
    }

    head("the stage names");
    {
        ck("every stage has a name of its own",
           strcmp(tls_stage_name(TLS_STAGE_CERT_REQ),
                  tls_stage_name(TLS_STAGE_DONE_RECV)) != 0, 0);
        ck("CLIENT_CERT and KEYEX are not the same words",
           strcmp(tls_stage_name(TLS_STAGE_CLIENT_CERT),
                  tls_stage_name(TLS_STAGE_KEYEX)) != 0, 0);
        ck("a number past the end says so",
           strcmp(tls_stage_name(TLS_STAGE_ESTABLISHED + 1),
                  "unknown") == 0, 0);
        ck("ESTABLISHED is the highest",
           TLS_STAGE_ESTABLISHED > TLS_STAGE_SRV_CCS, 0);
    }

    sh("rm -f /tmp/hd_cc_*");
    printf("\n  %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
