#pragma once
#include "types.h"
#define TLS_ERR_HANDSHAKE -1
#define TLS_ERR_ALERT     -2
#define TLS_ERR_CERT      -3
/* v0.7.6: split out of the generic handshake error so wget/curl can say
   something actionable.  A modern server that has disabled static-RSA key
   exchange lands on TLS_ERR_CIPHER or sends alert 40; either way the user
   now sees why rather than "TLS handshake failed". */
#define TLS_ERR_CIPHER    -4
#define TLS_ERR_FINISHED  -5
/* v0.8.9: the peer asked for a client certificate and this side could not
   produce one it would accept.  Distinct from TLS_ERR_HANDSHAKE because the
   remedy is different and specific: lockdownd asks every host for one, so a
   session that dies here is a pair record that is missing or unusable, not a
   network fault.  See tls_connect_ex(). */
#define TLS_ERR_CLIENTCERT -6

/* v0.7.5 FIX: these were 1 and 2 — wrong.  TLS ContentType values are
   defined by RFC 5246 A.1 and net/tls.c had to shadow them with local
   #defines (producing "redefined" warnings) to work at all.  Any other
   translation unit that included this header got the bogus values. */
#define TLS_CHANGE_CIPHER_SPEC 20
#define TLS_ALERT              21
#define TLS_HANDSHAKE          22
#define TLS_APPLICATION_DATA   23

/* ── HOW FAR THE HANDSHAKE GOT ───────────────────────────────────────
   v0.8.9: these lived in net/tls.c, and shell/shell_v076.c carried a
   SECOND copy of the list -- as bare numbers in a switch, with a
   hardcoded "/9" and a hardcoded `>= 9` for "established".  Inserting
   the three client-certificate stages below would have silently
   renamed every stage on the `tlsinfo` screen: stage 5 would still
   have read "ClientKeyExchange sent" while meaning
   "ServerHelloDone received".

   One list, here.  tls_stage_name() is the only place that turns a
   number into words, and tools/check_tls_stages.py refuses a build in
   which a second copy appears.  Same argument as include/version.h:
   two copies of a list of names drift, and this one drifts silently. */
#define TLS_STAGE_NONE         0
#define TLS_STAGE_HELLO_SENT   1
#define TLS_STAGE_SRV_HELLO    2
#define TLS_STAGE_CERT         3
#define TLS_STAGE_CERT_REQ     4   /* the peer wants a client cert    */
#define TLS_STAGE_DONE_RECV    5
#define TLS_STAGE_CLIENT_CERT  6   /* our chain went out             */
#define TLS_STAGE_KEYEX        7
#define TLS_STAGE_CERT_VERIFY  8   /* we proved we hold the key       */
#define TLS_STAGE_CCS_SENT     9
#define TLS_STAGE_FIN_SENT     10
#define TLS_STAGE_SRV_CCS      11
#define TLS_STAGE_ESTABLISHED  12

/* The one name table.  Never a switch on bare numbers anywhere else. */
const char *tls_stage_name(int stage);

/* ── THE TRANSPORT ───────────────────────────────────────────────────
   v0.8.9.  net/tls.c spoke to net/tcp.c by name -- tcp_send(),
   tcp_recv(), tcp_eof(), tcp_close() -- in nine places, and pairing
   needs TLS over a usbmux channel instead, which is not a socket and
   has no fd.

   So the four calls go through this, and `handle` is whatever the
   transport wants it to be: a TCP fd for net/tcp.c, a channel number
   for usb/lockdown.c.  It is NOT interpreted by net/tls.c except as
   the key that selects a session slot, so two transports must never
   hand out the same handle at the same time -- usb/lockdown.c uses a
   constant well outside the fd range and says so.

   `recv` returns bytes read, 0 for "nothing yet, ask again", or
   negative for a connection that is gone.  That is net/tcp.c's
   existing contract, kept so the tcp transport is a straight
   forwarding and cannot be where a bug is introduced.

   `send` returns NEGATIVE on failure and anything non-negative on
   success, and THE COUNT IS NOT MEANINGFUL -- net/tcp.c's tcp_send()
   returns 0 for a complete write, so a caller comparing the result
   against the length it asked for would reject every successful
   send. Partial writes are therefore the transport's problem and not
   this layer's: a transport that cannot deliver the whole record must
   report failure, and usb/lockdown.c's does, by name and with a latch
   (half a record leaves the peer reading the next one at the wrong
   offset and nothing reports an error). This paragraph exists because
   the injection harness proposed a defect -- a transport claiming a
   byte count it had not sent -- that nothing could detect, and the
   reason nothing could was that the contract had never been written
   down.

   `close` may be NULL -- a transport whose teardown belongs to its
   owner (usb/lockdown.c owns the mux channel) supplies none, and
   tls_close() then sends its close_notify and stops there. */
typedef struct {
    const char *name;
    int  (*send )(int handle, const uint8_t *data, uint16_t len);
    int  (*recv )(int handle, uint8_t *buf, uint16_t max,
                  uint32_t timeout_ms);
    int  (*eof  )(int handle);
    void (*close)(int handle);           /* may be NULL */
} tls_transport_t;

/* net/tcp.c, wrapped.  What tls_connect() uses. */
extern const tls_transport_t tls_transport_tcp;

/* ── CLIENT CERTIFICATES ─────────────────────────────────────────────
   RFC 5246 7.4.6 and 7.4.8.  A server that sends CertificateRequest
   gets, from us, a Certificate message and -- if we actually have a
   key -- a CertificateVerify signed over every handshake byte so far.

   WHY SIGNING IS A CALLBACK AND NOT A KEY.  The private key for
   pairing is a bn_t pair held by shell/paircmd.c, and net/tls.c has
   never known what a bignum is.  Handing it a function keeps the RSA
   out of the TLS code, and -- the reason that matters -- lets a test
   suite sign with openssl instead, so what the suite proves is that
   the TRANSCRIPT and the FRAMING are right rather than that our RSA
   agrees with itself.

   `sign` gets the SHA-256 of the handshake transcript and must
   produce a PKCS#1 v1.5 signature of exactly the modulus length.
   Return 0 for success, negative to abort the handshake.

   The chain is leaf FIRST, which is the order RFC 5246 requires and
   the opposite of the order a pair record lists them in. */
#define TLS_MAX_CHAIN 4

typedef int (*tls_sign_fn)(void *ctx, const uint8_t digest[32],
                           uint8_t *sig, uint32_t sigcap,
                           uint32_t *siglen);

typedef struct {
    const uint8_t *cert[TLS_MAX_CHAIN];   /* DER, leaf first        */
    uint32_t       cert_len[TLS_MAX_CHAIN];
    int            n;                     /* 0 is legal: see below  */
    tls_sign_fn    sign;                  /* NULL with n==0 only    */
    void          *sign_ctx;
} tls_client_cert_t;

/* The plain TCP client.  Unchanged behaviour: no client certificate,
   and a CertificateRequest is answered with an EMPTY Certificate
   message, which RFC 5246 7.4.6 requires of a client that has none.
   Before v0.8.9 it was not answered at all, and a server that asks
   politely and then insists would hang the handshake rather than
   refusing it. */
int  tls_connect(int tcp_fd, const char *hostname, uint32_t seed);

/* The full form.  `hostname` may be NULL or "" -- then no SNI
   extension is sent, which is what a connection to a device over USB
   needs: there is no name to send, and an SNI carrying an empty
   string is malformed rather than absent.

   `cc` may be NULL (no certificate, as above).  A `cc` with n==0 and
   sign==NULL means the same thing explicitly. */
int  tls_connect_ex(int handle, const tls_transport_t *tr,
                    const char *hostname, uint32_t seed,
                    const tls_client_cert_t *cc);

int  tls_send(int fd, const uint8_t *data, uint16_t len);
int  tls_recv(int fd, uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms);
int  tls_eof(int fd);
void tls_close(int fd);

/* v0.7.6 diagnostics, read by the `tlsinfo` shell command. */
extern int g_tls_last_alert_level;
extern int g_tls_last_alert_desc;
extern int g_tls_last_stage;
extern int g_tls_peer_cipher;

/* v0.8.9 diagnostics.  Three separate facts, because on hardware they
   fail apart: the peer may not ask at all (then pairing cannot be the
   problem), it may ask and we may have nothing to send, or we may
   send a chain and have the signature refused. */
extern int g_tls_cert_requested;   /* peer sent CertificateRequest   */
extern int g_tls_cert_sent;        /* certificates we put on the wire */
extern int g_tls_cert_verified;    /* a CertificateVerify went out    */
