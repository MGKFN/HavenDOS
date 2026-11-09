/*
 * tls.c — TLS 1.2 client for HavenDOS v0.6.8
 *
 * Cipher suite: TLS_RSA_WITH_AES_128_CBC_SHA256 (0x003C)
 * No certificate validation — no CA store on a hobby OS.
 * Provides encryption against passive sniffing.
 *
 * Handshake flow:
 *   → ClientHello
 *   ← ServerHello
 *   ← Certificate
 *   ← ServerHelloDone
 *   → ClientKeyExchange  (RSA encrypt 48-byte pre-master secret)
 *   → ChangeCipherSpec
 *   → Finished
 *   ← ChangeCipherSpec
 *   ← Finished
 *   [application data via tls_send / tls_recv]
 */

#include "../include/tls.h"
#include "../include/tcp.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/netstat.h"
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

/* ── Crypto externs (from tls_crypto.c) ─────────────────────── */
extern void sha256_init  (void *ctx);
extern void sha256_update(void *ctx, const uint8_t *d, uint32_t len);
extern void sha256_final (void *ctx, uint8_t out[32]);
extern void sha256       (const uint8_t *d, uint32_t len, uint8_t out[32]);
extern void hmac_sha256  (const uint8_t *key, uint32_t klen,
                           const uint8_t *data,uint32_t dlen, uint8_t out[32]);
extern void aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                                uint8_t *data, uint32_t len);
extern void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                                uint8_t *data, uint32_t len);
extern void tls12_prf    (const uint8_t *secret, uint32_t slen,
                           const char *label,
                           const uint8_t *seed, uint32_t seedlen,
                           uint8_t *out, uint32_t outlen);
extern int  rsa_public_encrypt(const uint8_t *mod, int mod_bytes,
                                const uint8_t *e, int e_len,
                                const uint8_t *msg, int msg_len,
                                uint8_t *out);

extern void net_poll(void);

/* TLS ContentType values now live in include/tls.h (v0.7.5). */

/* ── Handshake message types ────────────────────────────────── */
#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_NEW_SESSION_TICKET   4
#define HS_CERTIFICATE          11
#define HS_SERVER_KEY_EXCHANGE  12
#define HS_CERT_REQUEST         13
#define HS_SERVER_HELLO_DONE    14
#define HS_CERT_VERIFY          15
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20

/* v0.7.6 diagnostics.  `tlsinfo` in the shell reads these so a failed
   handshake on real hardware says something more useful than "TLS error". */
int g_tls_last_alert_level = 0;
int g_tls_last_alert_desc  = -1;
int g_tls_last_stage       = 0;   /* how far the handshake got */
int g_tls_peer_cipher      = -1;  /* cipher suite the server picked */

/* v0.8.9: the TLS_STAGE_* numbers moved to include/tls.h, because the
   shell carried a second copy of the names. See that header. */

/* v0.8.9 diagnostics for the client-certificate path. */
int g_tls_cert_requested = 0;
int g_tls_cert_sent      = 0;
int g_tls_cert_verified  = 0;

/* THE ONLY place a stage number becomes words.
   tools/check_tls_stages.py refuses a build with a second one. */
const char *tls_stage_name(int stage){
    switch(stage){
        case TLS_STAGE_NONE:        return "nothing attempted";
        case TLS_STAGE_HELLO_SENT:  return "ClientHello sent";
        case TLS_STAGE_SRV_HELLO:   return "ServerHello received";
        case TLS_STAGE_CERT:        return "Certificate parsed";
        case TLS_STAGE_CERT_REQ:    return "CertificateRequest received";
        case TLS_STAGE_DONE_RECV:   return "ServerHelloDone received";
        case TLS_STAGE_CLIENT_CERT: return "client Certificate sent";
        case TLS_STAGE_KEYEX:       return "ClientKeyExchange sent";
        case TLS_STAGE_CERT_VERIFY: return "CertificateVerify sent";
        case TLS_STAGE_CCS_SENT:    return "ChangeCipherSpec sent";
        case TLS_STAGE_FIN_SENT:    return "Finished sent";
        case TLS_STAGE_SRV_CCS:     return "server ChangeCipherSpec received";
        case TLS_STAGE_ESTABLISHED: return "session established";
        default:                    return "unknown";
    }
}

/* ── TLS version ────────────────────────────────────────────── */
#define TLS_VER_MAJOR  3
#define TLS_VER_MINOR  3   /* TLS 1.2 */

/* ── Cipher suite ───────────────────────────────────────────── */
/* TLS_RSA_WITH_AES_128_CBC_SHA256 = 0x003C */
#define CIPHER_HI  0x00
#define CIPHER_LO  0x3C

/* ── SHA256 context size (must match tls_crypto.c struct) ───── */
#define SHA256_CTX_BYTES  (8*4 + 8 + 64 + 4)   /* 108 bytes */

/* ── TLS session state ───────────────────────────────────────── */
typedef struct {
    int      fd;
    int      handshake_done;

    /* Key material */
    uint8_t  master_secret[48];
    uint8_t  client_write_key[16];
    uint8_t  server_write_key[16];
    uint8_t  client_write_mac[32];
    uint8_t  server_write_mac[32];
    uint8_t  client_write_iv [16];
    uint8_t  server_write_iv [16];

    /* Sequence numbers */
    uint64_t client_seq;
    uint64_t server_seq;

    /* Handshake transcript (SHA-256 running hash) */
    uint8_t  hs_ctx[SHA256_CTX_BYTES];   /* opaque sha256_ctx_t */

    /* Stored random values */
    uint8_t  client_random[32];
    uint8_t  server_random[32];

    /* RSA public key from server cert */
    uint8_t  rsa_mod[512];
    int      rsa_mod_len;
    uint8_t  rsa_exp[8];
    int      rsa_exp_len;

} tls_session_t;

/* ── v0.7.13: per-connection TLS sessions ────────────────────────────────
 *
 * There was one global `g_sess`, referenced 56 times, so exactly one TLS
 * connection could exist at a time.  With multi-socket TCP underneath, that
 * became the new bottleneck.
 *
 * Rather than rewrite 56 references (and risk missing one, which would
 * silently mix two connections' keys), the session is now selected by fd and
 * `g_sess` is a macro for the current one.  Every public entry point calls
 * sess_bind(fd) first; everything below keeps reading and writing `g_sess`
 * exactly as it did.
 *
 * The plaintext holding buffer, its cursors and the EOF flag move into the
 * session too — they carry state BETWEEN calls, so sharing them across
 * connections would hand one connection's decrypted bytes to another.
 *
 * g_recv_buf and g_hs_acc stay shared: both are scratch used strictly within a
 * single synchronous call (tls_recv_record) or within one handshake
 * (tls_connect), neither of which interleaves.
 * ────────────────────────────────────────────────────────────────────────── */
#define TLS_MAX_RECORD  (16384+512)
#define TLS_MAX_SESS 3

typedef struct {
    int           in_use;
    int           fd;
    /* v0.8.9: which transport this session's bytes travel on, and the
       client certificate it was opened with. Both belong to the SLOT
       and not to a global: two sessions can be open on two different
       transports, and a global would send one session's records down
       the other's channel -- silently, because both calls succeed. */
    const tls_transport_t  *tr;
    const tls_client_cert_t *cc;
    tls_session_t s;
    uint8_t       plain[TLS_MAX_RECORD];
    uint16_t      plain_len;    /* bytes of valid plaintext */
    uint16_t      plain_head;   /* read cursor */
    int           eof;          /* peer sent close_notify / FIN */
} tls_slot_t;

static tls_slot_t  g_slots[TLS_MAX_SESS];
static tls_slot_t *g_cur = &g_slots[0];

#define g_sess       (g_cur->s)
#define g_plain      (g_cur->plain)
#define g_plain_len  (g_cur->plain_len)
#define g_plain_head (g_cur->plain_head)
#define g_tls_eof    (g_cur->eof)
#define g_tr         (g_cur->tr)
#define g_cc         (g_cur->cc)

/* Select the slot owning this fd.  Returns 0 if there is none. */
static int sess_bind(int fd){
    for(int i=0;i<TLS_MAX_SESS;i++)
        if(g_slots[i].in_use && g_slots[i].fd == fd){ g_cur = &g_slots[i]; return 1; }
    return 0;
}

/* Claim a slot for a new handshake on this fd. */
static int sess_open(int fd){
    for(int i=0;i<TLS_MAX_SESS;i++)
        if(g_slots[i].in_use && g_slots[i].fd == fd){
            g_cur = &g_slots[i];
            memset(g_cur, 0, sizeof(*g_cur));
            g_cur->in_use = 1; g_cur->fd = fd;
            return 1;
        }
    for(int i=0;i<TLS_MAX_SESS;i++)
        if(!g_slots[i].in_use){
            g_cur = &g_slots[i];
            memset(g_cur, 0, sizeof(*g_cur));
            g_cur->in_use = 1; g_cur->fd = fd;
            return 1;
        }
    return 0;
}

static void sess_release(void){
    if(g_cur) memset(g_cur, 0, sizeof(*g_cur));
}

/* ── net/tcp.c as a transport ─────────────────────────────────────────
   v0.8.9. Straight forwarding, deliberately: every one of these is the
   call net/tls.c used to make by name, so a bug introduced by the
   indirection would have to be in four one-line functions.

   The signatures differ only in const-ness and void* versus uint8_t*,
   which is why these exist at all rather than casting function
   pointers -- a cast would compile and then pass arguments in whatever
   order the two prototypes happened to disagree about. */
static int tcp_tr_send(int h, const uint8_t *d, uint16_t len){
    return tcp_send(h, d, len);
}
static int tcp_tr_recv(int h, uint8_t *b, uint16_t max, uint32_t to){
    return tcp_recv(h, b, max, to);
}
static int tcp_tr_eof(int h){ return tcp_eof(h); }
static void tcp_tr_close(int h){ tcp_close(h); }

const tls_transport_t tls_transport_tcp = {
    "tcp", tcp_tr_send, tcp_tr_recv, tcp_tr_eof, tcp_tr_close
};

int tls_session_count(void){
    int n=0; for(int i=0;i<TLS_MAX_SESS;i++) if(g_slots[i].in_use) n++;
    return n;
}

/* ── Pseudo-RNG (simple, seeded from timer) ──────────────────── */
static uint32_t g_prng = 0xDEADBEEFu;
static uint8_t prng_byte(void){
    g_prng ^= g_prng<<13;
    g_prng ^= g_prng>>17;
    g_prng ^= g_prng<<5;
    return (uint8_t)(g_prng & 0xFF);
}
extern uint32_t entropy_u32(void);          /* kernel/entropy.c */
extern void     entropy_stir(uint32_t v);
/* v0.7.11: the caller's `seed` was get_ticks(), which on a cold boot is very
   nearly the same number every time - the client_random and the pre-master
   secret were drawn from a keyspace you could enumerate.  Fold in the real
   entropy pool, and re-stir on every draw so successive bytes are not a pure
   function of the initial seed. */
static void prng_seed(uint32_t s){
    g_prng = s ^ entropy_u32();
    if(!g_prng) g_prng = 0xCAFEBABEu;
}
static void prng_fill(uint8_t *buf, int len){
    entropy_stir((uint32_t)len);
    for(int i=0;i<len;i++){
        if((i & 7)==0) g_prng ^= entropy_u32();
        buf[i]=prng_byte();
    }
}

static int verify_record_mac(uint8_t rtype, const uint8_t *frag,
                             uint16_t fraglen, const uint8_t *mac,
                             uint64_t seq);   /* v0.7.11 */

/* ── Write u24 big-endian ────────────────────────────────────── */
static void put_u24(uint8_t *p, uint32_t v){
    p[0]=(v>>16)&0xFF; p[1]=(v>>8)&0xFF; p[2]=v&0xFF;
}
static uint32_t get_u24(const uint8_t *p){
    return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
}
static void put_u16(uint8_t *p, uint16_t v){
    p[0]=(v>>8)&0xFF; p[1]=v&0xFF;
}
static uint16_t get_u16(const uint8_t *p){
    return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);
}

/* ── Send a TLS record ───────────────────────────────────────── */
static int tls_send_raw(int fd, uint8_t type, const uint8_t *data, uint16_t len){
    uint8_t hdr[5];
    hdr[0]=type;
    hdr[1]=TLS_VER_MAJOR; hdr[2]=TLS_VER_MINOR;
    put_u16(hdr+3,len);
    if(g_tr->send(fd,hdr,5)<0) return -1;
    if(len && g_tr->send(fd,data,(uint16_t)len)<0) return -1;
    NS_INC(tls_rec_tx);
    return 0;
}

/* ── Receive one TLS record ──────────────────────────────────── */
static uint8_t g_recv_buf[TLS_MAX_RECORD];

/* v0.7.5 FIX: the old version issued a single tcp_recv() for the 5-byte
   record header and gave up if fewer than 5 bytes arrived — but those bytes
   were already consumed from the TCP ring, permanently desynchronising the
   record stream.  It also returned 0 (success) on a short body read, so a
   truncated record was decrypted as if complete.  Both now loop to
   completion and report failure honestly. */
static int tls_read_exact(int fd, uint8_t *dst, int want, uint32_t timeout_ms){
    int total=0;
    uint32_t elapsed=0;
    while(total<want){
        int r=g_tr->recv(fd,dst+total,(uint16_t)(want-total),500);
        if(r>0){ total+=r; continue; }
        if(r<0) return total;            /* connection gone */
        if(g_tr->eof(fd)) return total;  /* clean EOF — no more will arrive */
        elapsed+=500;
        if(elapsed>=timeout_ms) return total;
    }
    return total;
}

static int tls_recv_record(int fd, uint8_t *type_out, uint8_t **data_out, uint16_t *len_out){
    uint8_t hdr[5];
    if(tls_read_exact(fd,hdr,5,10000)<5) return -1;
    *type_out=hdr[0];
    uint16_t len=get_u16(hdr+3);
    if(len>TLS_MAX_RECORD) return -1;
    int total=tls_read_exact(fd,g_recv_buf,(int)len,10000);
    if(total<(int)len){ NS_INC(tls_short_rec); return -1; }
    NS_INC(tls_rec_rx);
    *data_out=g_recv_buf;
    *len_out=(uint16_t)total;
    return 0;
}

/* ── Plaintext holding buffer (see tls_recv below) ───────────── */
/* v0.7.13: g_plain / g_plain_len / g_plain_head / g_tls_eof now live in
   the per-fd slot above — see the sess_bind() note. */

/* ── Update handshake transcript ─────────────────────────────── */
static void hs_update(const uint8_t *data, uint32_t len){
    sha256_update(g_sess.hs_ctx, data, len);
}

/* ── Send a handshake message (also updates transcript) ──────── */
static int send_hs(int fd, uint8_t hs_type, const uint8_t *body, uint32_t body_len){
    uint8_t hdr[4];
    hdr[0]=hs_type;
    put_u24(hdr+1,body_len);
    hs_update(hdr,4);
    if(body_len) hs_update(body,body_len);
    /* Build full handshake message for record.
       v0.7.6: the old code skipped the memcpy when body_len was too large but
       still transmitted 4+body_len bytes, i.e. it sent uninitialised .bss on
       the wire.  Refuse outright instead. */
    static uint8_t hs_buf[8192];
    if(body_len > sizeof(hs_buf)-4) return -1;
    hs_buf[0]=hs_type; put_u24(hs_buf+1,body_len);
    if(body_len) memcpy(hs_buf+4,body,body_len);
    return tls_send_raw(fd,TLS_HANDSHAKE,hs_buf,(uint16_t)(4+body_len));
}

/* ── Build and send ClientHello ──────────────────────────────── */
static int send_client_hello(int fd, const char *hostname){
    /* client_random: 4 bytes time + 28 random */
    g_sess.client_random[0]=0; g_sess.client_random[1]=0;
    g_sess.client_random[2]=0; g_sess.client_random[3]=0;
    prng_fill(g_sess.client_random+4,28);

    /* v0.8.9: no name is a legal state now. A connection to a device
       over USB has no hostname at all, and an SNI extension carrying a
       zero-length name is malformed rather than absent -- RFC 6066
       requires a non-empty HostName -- so the extension is omitted
       instead of sent empty. */
    int hlen = hostname ? (int)strlen(hostname) : 0;
    if(hlen>255) hlen=255;

    /* Build ClientHello body */
    static uint8_t hello[512];
    int pos=0;

    /* ProtocolVersion */
    hello[pos++]=TLS_VER_MAJOR; hello[pos++]=TLS_VER_MINOR;
    /* Random */
    memcpy(hello+pos,g_sess.client_random,32); pos+=32;
    /* SessionID length = 0 */
    hello[pos++]=0;
    /* CipherSuites: 1 suite */
    put_u16(hello+pos,2); pos+=2;
    hello[pos++]=CIPHER_HI; hello[pos++]=CIPHER_LO;
    /* Compression: null only */
    hello[pos++]=1; hello[pos++]=0;
    /* Extensions */
    int ext_start=pos; pos+=2; /* placeholder for extensions length */
    if(hlen){
        /* SNI extension (type 0x0000) */
        put_u16(hello+pos,0x0000); pos+=2;  /* type: SNI */
        put_u16(hello+pos,(uint16_t)(hlen+5)); pos+=2;  /* ext data len */
        put_u16(hello+pos,(uint16_t)(hlen+3)); pos+=2;  /* server_name_list length */
        hello[pos++]=0;                                   /* name_type: host_name */
        put_u16(hello+pos,(uint16_t)hlen); pos+=2;
        memcpy(hello+pos,hostname,hlen); pos+=hlen;
    }

    /* ── signature_algorithms (type 0x000D) ──────────────────────────
       v0.8.9, and it is not decoration. RFC 5246 7.4.1.4.1: a TLS 1.2
       client that sends NO signature_algorithms extension is TAKEN TO
       HAVE SAID {sha1, rsa}, and a server is then entitled to refuse a
       CertificateVerify signed with SHA-256 -- which is the only kind
       this stack can produce, because pkcs1_sign_sha256() is the only
       signer in the tree.

       So before this release the omission cost nothing (we never sent
       a CertificateVerify) and the first client certificate would have
       been rejected for a reason nowhere in the certificate. One
       entry, sha256+rsa, which is exactly what we can do: offering
       more would be claiming an algorithm we would then fail to use. */
    put_u16(hello+pos,0x000D); pos+=2;   /* signature_algorithms */
    put_u16(hello+pos,4);      pos+=2;   /* ext data length       */
    put_u16(hello+pos,2);      pos+=2;   /* list length, in bytes */
    hello[pos++]=4;                      /* HashAlgorithm.sha256  */
    hello[pos++]=1;                      /* SignatureAlgorithm.rsa */

    /* Fill extensions length */
    put_u16(hello+ext_start,(uint16_t)(pos-ext_start-2));

    return send_hs(fd,HS_CLIENT_HELLO,hello,(uint32_t)pos);
}

/* ── Parse ServerHello ───────────────────────────────────────── */
static int parse_server_hello(const uint8_t *body, uint16_t len){
    if(len<35) return -1;
    /* Check version */
    if(body[0]!=TLS_VER_MAJOR||body[1]!=TLS_VER_MINOR) return -1;
    /* Server random */
    memcpy(g_sess.server_random,body+2,32);
    /* Session ID */
    int sid_len=body[34]; int pos=35+sid_len;
    if(pos+2>len) return -1;
    /* Cipher suite */
    g_tls_peer_cipher = ((int)body[pos]<<8)|body[pos+1];
    if(body[pos]!=CIPHER_HI||body[pos+1]!=CIPHER_LO) return -2; /* unsupported */
    return 0;
}

/* ── Parse Certificate — extract RSA public key ──────────────── */
/*
 * We do minimal ASN.1 DER parsing to extract the RSA modulus and exponent.
 * We skip validation entirely — no CA chain, no hostname check.
 *
 * Structure (simplified):
 *   Certificate message: 3-byte length + list of certs
 *   Each cert: 3-byte length + DER-encoded X.509
 *   X.509 is a SEQUENCE containing TBSCertificate which contains
 *   SubjectPublicKeyInfo which contains the RSA key.
 *
 * We find the RSA public key by scanning for the RSA OID
 * 1.2.840.113549.1.1.1 = 2A 86 48 86 F7 0D 01 01 01
 * followed by the BIT STRING containing the RSAPublicKey SEQUENCE.
 */

static int der_read_len(const uint8_t *p, int avail, int *consumed){
    if(avail<1) return -1;
    if(!(p[0]&0x80)){ *consumed=1; return p[0]; }
    int nb=p[0]&0x7F;
    if(nb>4||nb+1>avail){ *consumed=0; return -1; }
    int len=0;
    for(int i=0;i<nb;i++) len=(len<<8)|p[1+i];
    *consumed=1+nb;
    return len;
}

/* RSA OID bytes */
static const uint8_t rsa_oid[]={0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01};

/* ── v0.7.11 (roadmap 0.7.7): bounds-checked certificate parsing ────────
 * The old parser scanned the whole DER blob for the RSA OID byte pattern and
 * then walked forward from wherever it matched, reading tags and lengths with
 * almost no bounds checks:
 *
 *   - `if(list_len+3>blen)` and `if(cert_len+6>blen)` add a uint32 read from
 *     the wire to a small constant; a crafted length near 2^32 wraps and the
 *     check passes.
 *   - after the OID match, `p[0]`, `p+=1+lc`, `p+=mlen` and so on walked
 *     forward with only the *declared* lengths as a guide, never re-checking
 *     against the end of the certificate.
 *   - the OID pattern can legitimately appear inside a subject name, an
 *     extension, or any other opaque field, so the parser could start walking
 *     from the middle of unrelated data.
 *
 * A hostile or merely unusual certificate could therefore read past the end of
 * the receive buffer.  Every step below is now bounded against the actual end
 * of the certificate, and the RSA OID is only accepted where it is followed by
 * a well-formed AlgorithmIdentifier and SubjectPublicKeyInfo.
 * ─────────────────────────────────────────────────────────────────────── */
static int parse_certificate(const uint8_t *body, uint16_t blen){
    if(blen<6) return -1;

    uint32_t list_len = get_u24(body);
    if(list_len > (uint32_t)blen || list_len + 3u > (uint32_t)blen) return -1;

    uint32_t cert_len = get_u24(body+3);
    if(cert_len < 8) return -1;
    if(cert_len > (uint32_t)blen) return -1;
    if(cert_len + 6u > (uint32_t)blen) return -1;

    const uint8_t *cert = body+6;
    const uint8_t *end  = cert + cert_len;      /* hard limit for every read */

    #define AVAIL(ptr) ((ptr) < end ? (uint32_t)(end-(ptr)) : 0u)

    for(uint32_t i=0; i+sizeof(rsa_oid) <= cert_len; i++){
        if(memcmp(cert+i,rsa_oid,sizeof(rsa_oid))!=0) continue;

        const uint8_t *p = cert + i + sizeof(rsa_oid);
        if(AVAIL(p) < 4) continue;

        /* optional NULL parameters */
        if(AVAIL(p) >= 2 && p[0]==0x05 && p[1]==0x00) p += 2;

        /* BIT STRING wrapping the SubjectPublicKeyInfo */
        if(AVAIL(p) < 2 || p[0]!=0x03) continue;
        int lc=0;
        int bslen = der_read_len(p+1,(int)AVAIL(p)-1,&lc);
        if(lc<=0 || bslen<8) continue;
        p += 1+lc;
        if(AVAIL(p) < (uint32_t)bslen) continue;      /* declared > actual */

        /* unused-bits octet must be 0 for a key */
        if(AVAIL(p) < 1 || p[0]!=0x00) continue;
        p++; bslen--;

        /* SEQUENCE { modulus INTEGER, exponent INTEGER } */
        if(AVAIL(p) < 2 || p[0]!=0x30) continue;
        int seqc=0;
        int seqlen = der_read_len(p+1,bslen-1,&seqc);
        if(seqc<=0 || seqlen<8) continue;
        p += 1+seqc;
        if(AVAIL(p) < (uint32_t)seqlen) continue;

        /* modulus */
        if(AVAIL(p) < 2 || p[0]!=0x02) continue;
        int mc=0;
        int mlen = der_read_len(p+1,(int)AVAIL(p)-1,&mc);
        if(mc<=0 || mlen<64) continue;                /* < 512-bit RSA: reject */
        p += 1+mc;
        if(AVAIL(p) < (uint32_t)mlen) continue;
        if(mlen>0 && p[0]==0x00){ p++; mlen--; }      /* strip sign byte */
        if(mlen<64 || mlen>(int)sizeof(g_sess.rsa_mod)) continue; /* 512..4096 bit */
        if(AVAIL(p) < (uint32_t)mlen) continue;

        g_sess.rsa_mod_len = mlen;
        memcpy(g_sess.rsa_mod,p,(uint32_t)mlen);
        p += mlen;

        /* exponent */
        if(AVAIL(p) < 2 || p[0]!=0x02) continue;
        int ec=0;
        int elen = der_read_len(p+1,(int)AVAIL(p)-1,&ec);
        if(ec<=0 || elen<1 || elen>8) continue;
        p += 1+ec;
        if(AVAIL(p) < (uint32_t)elen) continue;

        int nonzero=0;
        for(int k=0;k<elen;k++) if(p[k]) { nonzero=1; break; }
        if(!nonzero) continue;
        memcpy(g_sess.rsa_exp,p,(uint32_t)elen);
        g_sess.rsa_exp_len = elen;
        return 0;
    }
    #undef AVAIL
    return -1;
}

/* ── Send ClientKeyExchange ──────────────────────────────────── */
static int send_client_key_exchange(int fd){
    /* Generate 48-byte pre-master secret */
    uint8_t pms[48];
    pms[0]=TLS_VER_MAJOR; pms[1]=TLS_VER_MINOR;
    prng_fill(pms+2,46);

    /* RSA encrypt the pre-master secret */
    static uint8_t encrypted[512];
    int enc_len=rsa_public_encrypt(
        g_sess.rsa_mod, g_sess.rsa_mod_len,
        g_sess.rsa_exp, g_sess.rsa_exp_len,
        pms, 48, encrypted);
    if(enc_len<0) return -1;

    /* ClientKeyExchange body: 2-byte length + encrypted pms */
    static uint8_t cke_body[514];
    put_u16(cke_body,(uint16_t)enc_len);
    memcpy(cke_body+2,encrypted,enc_len);

    int ret=send_hs(fd,HS_CLIENT_KEY_EXCHANGE,cke_body,(uint32_t)(2+enc_len));

    /* Derive master secret from pre-master secret */
    uint8_t seed[64];
    memcpy(seed,g_sess.client_random,32);
    memcpy(seed+32,g_sess.server_random,32);
    tls12_prf(pms,48,"master secret",seed,64,g_sess.master_secret,48);

    /* Zero pre-master secret */
    memset(pms,0,48);
    return ret;
}

/* ── Derive key material ─────────────────────────────────────── */
static void derive_keys(void){
    /* key_block = PRF(master_secret, "key expansion",
                       server_random + client_random, needed_bytes) */
    uint8_t seed[64];
    memcpy(seed,g_sess.server_random,32);
    memcpy(seed+32,g_sess.client_random,32);
    /* For AES-128-CBC-SHA256:
       mac_key_len=32, enc_key_len=16, iv_len=16
       Total: 2×32 + 2×16 + 2×16 = 128 bytes */
    uint8_t kb[128];
    tls12_prf(g_sess.master_secret,48,"key expansion",seed,64,kb,128);
    memcpy(g_sess.client_write_mac,kb+0, 32);
    memcpy(g_sess.server_write_mac,kb+32,32);
    memcpy(g_sess.client_write_key,kb+64,16);
    memcpy(g_sess.server_write_key,kb+80,16);
    memcpy(g_sess.client_write_iv, kb+96,16);
    memcpy(g_sess.server_write_iv, kb+112,16);
    g_sess.client_seq=0;
    g_sess.server_seq=0;
}

/* ── Send ChangeCipherSpec ───────────────────────────────────── */
static int send_change_cipher_spec(int fd){
    uint8_t ccs=1;
    return tls_send_raw(fd,TLS_CHANGE_CIPHER_SPEC,&ccs,1);
}

/* ── Compute Finished verify_data ────────────────────────────── */
static void compute_finished(const char *label, uint8_t out[12]){
    /* Get current handshake hash */
    uint8_t hs_hash[32];
    /* We need a copy of the SHA256 context to not disturb ongoing hash */
    uint8_t ctx_copy[SHA256_CTX_BYTES];
    memcpy(ctx_copy,g_sess.hs_ctx,SHA256_CTX_BYTES);
    sha256_final(ctx_copy,hs_hash);

    /* verify_data = PRF(master_secret, label, hs_hash, 12) */
    tls12_prf(g_sess.master_secret,48,label,hs_hash,32,out,12);
}

/* ── Send Finished (encrypted) ───────────────────────────────── */
static int send_finished(int fd){
    uint8_t vdata[12];
    compute_finished("client finished",vdata);

    /* Finished handshake message: type(1)+len(3)+vdata(12) = 16 bytes */
    uint8_t hs_msg[16];
    hs_msg[0]=HS_FINISHED; put_u24(hs_msg+1,12);
    memcpy(hs_msg+4,vdata,12);

    /* Update transcript with Finished message */
    hs_update(hs_msg,16);

    /* Build TLS record with MAC and padding */
    /* Record: seq_num(8)+type(1)+ver(2)+len(2) for MAC input */
    uint8_t mac_input[8+1+2+2+16];
    for(int i=7;i>=0;i--){ mac_input[i]=(uint8_t)(g_sess.client_seq>>(56-i*8)); }
    mac_input[8]=TLS_HANDSHAKE;
    mac_input[9]=TLS_VER_MAJOR; mac_input[10]=TLS_VER_MINOR;
    put_u16(mac_input+11,16);
    memcpy(mac_input+13,hs_msg,16);
    uint8_t mac[32];
    hmac_sha256(g_sess.client_write_mac,32,mac_input,(uint32_t)sizeof(mac_input),mac);

    /* Plaintext = hs_msg(16) + mac(32) = 48 bytes. Pad to AES block (48 is divisible by 16). */
    uint8_t plain[64]; /* 48 bytes + 16 byte IV prefix room */
    memcpy(plain,hs_msg,16);
    memcpy(plain+16,mac,32);
    /* PKCS#7 padding: 48 is already multiple of 16, so add a full padding block.
       PKCS7 always adds at least 1 block of padding. 16 bytes of padding → each byte = 0x0F.
       BUG FIX: was 0x0F (= 15), must be 0x0F because PKCS7 padding byte value =
       (number of padding bytes - 1)? NO — RFC 5652: each byte equals the count of padding bytes.
       16 padding bytes → each byte = 0x10 (16 decimal). */
    /* v0.7.5 FIX: TLS 1.2 padding is padding_length bytes of value
       padding_length, FOLLOWED BY the padding_length byte itself
       (RFC 5246 6.2.3.2) — not PKCS#7.  Plaintext is 48 bytes (already a
       block multiple), so we add a full block: 15 bytes of 0x0F plus the
       0x0F length byte = 16.  The old code wrote 0x10 sixteen times, which
       every real server rejects — the handshake could never complete. */
    memset(plain+48,0x0F,16);
    uint32_t plain_len=64;

    /* Random IV (use client_write_iv XOR seq) */
    uint8_t iv[16]; memcpy(iv,g_sess.client_write_iv,16);
    iv[0]^=(uint8_t)(g_sess.client_seq);

    /* Encrypt */
    aes128_cbc_encrypt(g_sess.client_write_key,iv,plain,plain_len);

    /* Send: IV(16) + ciphertext(64) */
    static uint8_t record[80];
    memcpy(record,iv,16);
    memcpy(record+16,plain,plain_len);
    g_sess.client_seq++;

    return tls_send_raw(fd,TLS_HANDSHAKE,record,(uint16_t)(16+plain_len));
}

/* ── Verify server Finished ──────────────────────────────────── */
static int verify_server_finished(const uint8_t *data, uint16_t len){
    if(len<16+1+12) return -1; /* IV + type + vdata minimum */
    /* Decrypt */
    static uint8_t plain[256];
    /* v0.7.12: `len-16` was promoted to int and compared against a size_t, so
       the check relied on the guard above for its safety rather than standing
       on its own.  Compare in a single unsigned domain. */
    if((uint32_t)(len-16) > (uint32_t)sizeof(plain)) return -1;
    uint16_t ct_len=(uint16_t)(len-16);
    /* v0.7.6: aes128_cbc_decrypt() walks the buffer 16 bytes at a time and
       would read past the end on a non-block-multiple length. */
    if(ct_len==0||(ct_len%16)) return -1;
    const uint8_t *iv=data;
    memcpy(plain,data+16,ct_len);
    aes128_cbc_decrypt(g_sess.server_write_key,iv,plain,ct_len);
    /* Remove PKCS7 padding — pad byte value = count of padding bytes */
    /* v0.7.5 FIX: strip pad+1 bytes (padding + the padding_length byte). */
    uint8_t pad=plain[ct_len-1];
    if(pad>15||(uint16_t)(pad+1)>=ct_len) return -1;
    uint16_t plainlen=(uint16_t)(ct_len-(pad+1));
    if(plainlen<16+32) return -1;   /* Finished(16) + MAC(32) */
    /* v0.7.11: the server Finished record carries a MAC like any other and was
       likewise never checked. */
    if(verify_record_mac(TLS_HANDSHAKE, plain, (uint16_t)(plainlen-32),
                         plain+(plainlen-32), g_sess.server_seq) != 0){
        NS_INC(tls_bad_mac);
        return -1;
    }
    /* First 4 bytes: type + length */
    if(plain[0]!=HS_FINISHED) return -1;
    uint32_t vlen=get_u24(plain+1);
    if(vlen!=12) return -1;
    /* Compute expected verify_data */
    uint8_t expected[12];
    compute_finished("server finished",expected);
    return memcmp(plain+4,expected,12);
}

/* ── v0.7.11: record MAC verification ─────────────────────────────────────
 * Up to v0.7.6 the receive path stripped the trailing 32-byte HMAC and threw
 * it away WITHOUT CHECKING IT.  That meant the connection was encrypted but
 * completely unauthenticated: anything that could modify bytes in flight could
 * flip ciphertext blocks and we would hand the result to the HTTP parser as
 * though the server had sent it.  Confidentiality without integrity is not
 * much use, and it was the single largest gap left in the TLS stack.
 *
 * RFC 5246 6.2.3.1 defines the MAC input as
 *     seq_num(8) || type(1) || version(2) || length(2) || fragment
 * where `length` is the length of the fragment BEFORE the MAC and padding are
 * appended.  Mirrors the construction in tls_send().
 */
static int verify_record_mac(uint8_t rtype, const uint8_t *frag,
                             uint16_t fraglen, const uint8_t *mac,
                             uint64_t seq)
{
    static uint8_t mi[8+5+16384+64];
    if(fraglen > 16384) return -1;
    for(int i=7;i>=0;i--) mi[i]=(uint8_t)(seq>>(56-i*8));
    mi[8]=rtype;
    mi[9]=TLS_VER_MAJOR; mi[10]=TLS_VER_MINOR;
    put_u16(mi+11,fraglen);
    if(fraglen) memcpy(mi+13,frag,fraglen);
    uint8_t want[32];
    hmac_sha256(g_sess.server_write_mac,32,mi,(uint32_t)(13+fraglen),want);
    /* constant-time compare - never leak which byte differed */
    uint8_t diff=0;
    for(int i=0;i<32;i++) diff |= (uint8_t)(want[i]^mac[i]);
    return diff?-1:0;
}

/* ── v0.7.12: handshake message reassembly ───────────────────────────────
 *
 * A TLS handshake MESSAGE and a TLS RECORD are not the same unit.  RFC 5246
 * 6.2.1 is explicit that a message may be fragmented across several records,
 * and that several messages may be coalesced into one.  The old code only
 * handled the second case:
 *
 *     while(pos+4<=rlen){
 *         hs_len = get_u24(rdata+pos+1);
 *         if(pos+4+hs_len > rlen) break;      // <-- silently abandons the tail
 *         hs_update(rdata+pos, 4+hs_len);
 *         ...
 *     }
 *
 * When a message did not fit, the loop simply stopped.  Two things then went
 * wrong at once.  First, the trailing bytes were never fed to hs_update(), so
 * the transcript hash diverged from the server's and the Finished check was
 * guaranteed to fail.  Second, the next record was parsed from byte 0 as if it
 * were a fresh message header, so its first four bytes — actually the middle of
 * a certificate — were read as a type and a length.
 *
 * The record ceiling is 16384 bytes, and a certificate chain with an
 * intermediate routinely exceeds that, so this hit real servers.  It presents
 * as TLS_ERR_FINISHED or a generic handshake failure against hosts whose chain
 * happens to be large, while smaller ones connect — which makes it look like an
 * intermittent or server-specific fault rather than a stack bug.
 *
 * Handshake bytes now accumulate here and are consumed only as whole messages.
 * ───────────────────────────────────────────────────────────────────────── */
#define HS_ACC_MAX 24576
static uint8_t  g_hs_acc[HS_ACC_MAX];
static uint32_t g_hs_acc_len = 0;

/* Feed one record's worth of handshake bytes.  Dispatches every COMPLETE
   message and leaves any partial tail buffered for the next record.
   dispatch==0 means transcript-only (the pre-CCS NewSessionTicket window).
   Returns 0, or a negative TLS_ERR_* code. */
static int hs_feed(const uint8_t *frag, uint16_t fraglen, int dispatch,
                   int *got_hello, int *got_cert, int *got_done,
                   int *got_certreq)
{
    if(fraglen){
        if(g_hs_acc_len + (uint32_t)fraglen > HS_ACC_MAX) return TLS_ERR_HANDSHAKE;
        memcpy(g_hs_acc + g_hs_acc_len, frag, fraglen);
        g_hs_acc_len += fraglen;
    }

    while(g_hs_acc_len >= 4){
        uint32_t hs_len = get_u24(g_hs_acc + 1);
        if(hs_len > HS_ACC_MAX - 4) return TLS_ERR_HANDSHAKE;
        if(g_hs_acc_len < 4 + hs_len) break;          /* need another record */

        uint8_t        hs_type  = g_hs_acc[0];
        const uint8_t *body     = g_hs_acc + 4;
        uint16_t       body_len = (uint16_t)hs_len;   /* < HS_ACC_MAX, fits */

        hs_update(g_hs_acc, 4 + hs_len);

        if(dispatch){
            if(hs_type==HS_SERVER_HELLO){
                int r=parse_server_hello(body,body_len);
                if(r==-2) return TLS_ERR_CIPHER;
                if(r<0)   return TLS_ERR_HANDSHAKE;
                if(got_hello) *got_hello=1;
                g_tls_last_stage=TLS_STAGE_SRV_HELLO;
            } else if(hs_type==HS_CERTIFICATE){
                if(parse_certificate(body,body_len)<0) return TLS_ERR_CERT;
                if(got_cert) *got_cert=1;
                g_tls_last_stage=TLS_STAGE_CERT;
            } else if(hs_type==HS_SERVER_HELLO_DONE){
                if(got_done) *got_done=1;
                g_tls_last_stage=TLS_STAGE_DONE_RECV;
            } else if(hs_type==HS_CERT_REQUEST){
                /* v0.8.9. The BODY is deliberately not parsed: it
                   carries the certificate types, the signature
                   algorithms the server will accept, and a list of
                   acceptable CA names, and acting on any of them means
                   choosing between certificates -- of which this host
                   has exactly one. What matters is THAT it was asked,
                   because RFC 5246 7.4.6 then requires a Certificate
                   message from us, empty if we have nothing, and a
                   client that sends none leaves the server waiting.

                   lockdownd asks every host for one, so this is the
                   normal path and not an edge case. */
                if(got_certreq) *got_certreq=1;
                g_tls_cert_requested=1;
                g_tls_last_stage=TLS_STAGE_CERT_REQ;
            }
            /* ServerKeyExchange is hashed above and otherwise ignored --
               static RSA only. */
        }

        uint32_t consumed = 4 + hs_len;
        g_hs_acc_len -= consumed;
        if(g_hs_acc_len) memmove(g_hs_acc, g_hs_acc + consumed, g_hs_acc_len);
    }
    return 0;
}

/* ── Certificate (ours) ──────────────────────────────────────────────
   v0.8.9. RFC 5246 7.4.6: a client that has been sent a
   CertificateRequest MUST reply with a Certificate message, and "if no
   suitable certificate is available, the client MUST send a
   certificate message containing no certificates". An empty one is a
   refusal the server can act on; sending nothing at all is a server
   that waits.

   The list is leaf first, each certificate preceded by its own 24-bit
   length, inside a 24-bit length for the whole list. Two nested
   lengths three bytes apart is the kind of framing that is wrong by
   three and still parses as far as the first certificate, which is why
   tests/test_tlscc.c compares the bytes against what openssl's own
   client puts on the wire rather than against our reading of them. */
#define CLIENT_CERT_MAX 6144
static int send_client_cert(int fd){
    static uint8_t body[CLIENT_CERT_MAX];
    uint32_t pos = 3;                    /* list length goes in last */
    int i, n = 0;

    if(g_cc) n = g_cc->n;
    if(n < 0 || n > TLS_MAX_CHAIN) return -1;

    for(i = 0; i < n; i++){
        uint32_t clen = g_cc->cert_len[i];
        if(!g_cc->cert[i] || !clen) return -1;
        /* Refused, never truncated: a chain that does not fit must not
           become a shorter chain that the peer then rejects for a
           reason that is nowhere near the real one. */
        if(pos + 3 + clen > sizeof body) return -1;
        put_u24(body + pos, clen); pos += 3;
        memcpy(body + pos, g_cc->cert[i], clen); pos += clen;
    }
    put_u24(body, pos - 3);

    /* The counter is set AFTER the send, and the injection harness is
       why. `tlsinfo` and `usb lockdown` print it on a machine nobody
       can attach a debugger to, so "2 certificates sent" when the
       send failed is a line that sends the next person to look at the
       signature. It is only a diagnostic, which is exactly the kind
       of thing that stays wrong for releases. */
    {
        int r = send_hs(fd, HS_CERTIFICATE, body, pos);
        if(r < 0) return r;
        g_tls_cert_sent = n;
        return r;
    }
}

/* ── CertificateVerify ───────────────────────────────────────────────
   v0.8.9. RFC 5246 7.4.8. Proof that we hold the private key for the
   certificate we just sent: a signature over EVERY handshake byte so
   far, which at this point is ClientHello through ClientKeyExchange
   inclusive.

   Two things about the transcript are worth stating, because both are
   off-by-one-message mistakes that produce a signature verifying
   nothing:

     - It covers the ClientKeyExchange. So this must be sent AFTER it,
       and RFC 5246 7.3's flight order says the same.
     - It does NOT cover itself. compute_finished() already takes a
       COPY of the running SHA-256 context for exactly this reason;
       this does the same, and send_hs() folds this message into the
       transcript afterwards so the Finished hash still includes it.

   The signature is produced by the caller's callback -- see
   include/tls.h on why net/tls.c holds no key. */
static int send_cert_verify(int fd){
    uint8_t  hash[32];
    uint8_t  ctx_copy[SHA256_CTX_BYTES];
    static uint8_t body[4 + 512];
    uint32_t siglen = 0;

    if(!g_cc || !g_cc->sign) return -1;

    memcpy(ctx_copy, g_sess.hs_ctx, SHA256_CTX_BYTES);
    sha256_final(ctx_copy, hash);

    if(g_cc->sign(g_cc->sign_ctx, hash, body + 4,
                  (uint32_t)sizeof body - 4, &siglen) < 0) return -1;
    if(siglen == 0 || siglen > (uint32_t)sizeof body - 4) return -1;

    body[0] = 4;                       /* HashAlgorithm.sha256       */
    body[1] = 1;                       /* SignatureAlgorithm.rsa     */
    put_u16(body + 2, (uint16_t)siglen);

    g_tls_cert_verified = 1;
    return send_hs(fd, HS_CERT_VERIFY, body, 4 + siglen);
}

/* ── TLS handshake ───────────────────────────────────────────── */
int tls_connect(int fd, const char *hostname, uint32_t seed){
    return tls_connect_ex(fd, &tls_transport_tcp, hostname, seed, 0);
}

int tls_connect_ex(int fd, const tls_transport_t *tr, const char *hostname,
                   uint32_t seed, const tls_client_cert_t *cc){
    /* v0.7.13: claim a per-fd slot before touching any session state. */
    if(!tr || !tr->send || !tr->recv || !tr->eof) return TLS_ERR_HANDSHAKE;
    if(!sess_open(fd)) return TLS_ERR_HANDSHAKE;   /* all slots busy */
    g_sess.fd=fd;                                  /* slot already zeroed */
    g_tr=tr; g_cc=cc;
    g_tls_cert_requested=0; g_tls_cert_sent=0; g_tls_cert_verified=0;
    prng_seed(seed);
    g_tls_last_alert_level=0; g_tls_last_alert_desc=-1;
    g_tls_last_stage=TLS_STAGE_NONE; g_tls_peer_cipher=-1;
    g_plain_len=0; g_plain_head=0; g_tls_eof=0;
    g_hs_acc_len=0;

    /* Init handshake transcript */
    sha256_init(g_sess.hs_ctx);

    /* → ClientHello */
    if(send_client_hello(fd,hostname)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_HELLO_SENT;

    /* ← ServerHello, Certificate, ServerHelloDone */
    int got_hello=0,got_cert=0,got_done=0,got_certreq=0;
    for(int attempt=0;attempt<20&&!(got_hello&&got_cert&&got_done);attempt++){
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;

        if(rtype==TLS_ALERT){
            /* v0.7.6: keep the alert so the user can be told WHY.
               level 2 = fatal; desc 40 = handshake_failure (usually "server
               refuses static-RSA key exchange"), 70 = protocol_version. */
            if(rlen>=2){ g_tls_last_alert_level=rdata[0];
                         g_tls_last_alert_desc =rdata[1]; }
            return TLS_ERR_ALERT;
        }
        if(rtype!=TLS_HANDSHAKE) continue;

        /* Records carry handshake BYTES, not whole messages — see hs_feed(). */
        int hr = hs_feed(rdata, rlen, 1, &got_hello, &got_cert, &got_done,
                         &got_certreq);
        if(hr < 0) return hr;
    }

    if(!got_hello||!got_cert||!got_done) return TLS_ERR_HANDSHAKE;

    /* → Certificate, if one was asked for.
       Before ClientKeyExchange: RFC 5246 7.3's flight order, and the
       CertificateVerify below signs a transcript that depends on it. */
    if(got_certreq){
        if(send_client_cert(fd)<0) return TLS_ERR_CLIENTCERT;
        g_tls_last_stage=TLS_STAGE_CLIENT_CERT;
    }

    /* → ClientKeyExchange */
    if(send_client_key_exchange(fd)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_KEYEX;

    /* → CertificateVerify, only if we actually sent a certificate.
       A client that sent an EMPTY Certificate message must not send
       this -- there is no key to prove possession of, and a signature
       from a key the server has no certificate for is worse than
       silence. */
    if(got_certreq && g_cc && g_cc->n > 0){
        if(send_cert_verify(fd)<0) return TLS_ERR_CLIENTCERT;
        g_tls_last_stage=TLS_STAGE_CERT_VERIFY;
    }

    /* Derive session keys */
    derive_keys();

    /* → ChangeCipherSpec */
    if(send_change_cipher_spec(fd)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_CCS_SENT;

    /* → Finished (encrypted) */
    if(send_finished(fd)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_FIN_SENT;

    /* ← [NewSessionTicket] ← ChangeCipherSpec
     *
     * v0.7.6 CRITICAL FIX.  The old code read exactly one record here and
     * bailed out unless it was a ChangeCipherSpec.  But RFC 5077 puts
     * NewSessionTicket in the server's flight BEFORE its CCS, in the clear,
     * and it must also be folded into the Finished transcript hash.  Any
     * server with session tickets enabled — which is most of them — made the
     * handshake fail here with a generic error even though everything up to
     * that point had succeeded.
     *
     * We now accept plaintext handshake records until the CCS arrives,
     * hashing each one so the server Finished still verifies. */
    {
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        int spins=0;
        for(;;){
            if(++spins>8) return TLS_ERR_HANDSHAKE;
            if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;
            if(rtype==TLS_CHANGE_CIPHER_SPEC) break;
            if(rtype==TLS_ALERT){
                if(rlen>=2){ g_tls_last_alert_level=rdata[0];
                             g_tls_last_alert_desc =rdata[1]; }
                return TLS_ERR_ALERT;
            }
            if(rtype==TLS_HANDSHAKE){
                /* NewSessionTicket (or anything else still in the clear):
                   add to the transcript and keep waiting for the CCS.  A
                   ticket can be several KB and fragment exactly like a
                   certificate, so it goes through the same reassembly. */
                int hr = hs_feed(rdata, rlen, 0, 0, 0, 0, 0);
                if(hr < 0) return hr;
                continue;
            }
            return TLS_ERR_HANDSHAKE;
        }
        g_tls_last_stage=TLS_STAGE_SRV_CCS;
    }

    /* ← Finished (encrypted) */
    {
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;
        if(rtype==TLS_ALERT){
            if(rlen>=2){ g_tls_last_alert_level=rdata[0];
                         g_tls_last_alert_desc =rdata[1]; }
            return TLS_ERR_ALERT;
        }
        if(rtype!=TLS_HANDSHAKE) return TLS_ERR_HANDSHAKE;
        if(verify_server_finished(rdata,rlen)<0) return TLS_ERR_FINISHED;
        g_sess.server_seq++;
    }

    g_sess.handshake_done=1;
    g_tls_last_stage=TLS_STAGE_ESTABLISHED;
    return 0;
}

/* ── Send application data (encrypted) ──────────────────────── */
#define TLS_SEND_MAX 16384
/* v0.7.11: the encrypt-and-send body was hardwired to TLS_APPLICATION_DATA.
   It now takes the content type so tls_close() can emit an encrypted
   close_notify alert through exactly the same path. */
static int tls_send_typed(int fd, uint8_t ctype, const uint8_t *data, uint16_t len){
    if(!g_sess.handshake_done||fd!=g_sess.fd) return -1;
    /* v0.7.6: mac_input[] and plain[] are sized for 16384 bytes of payload but
       len is a uint16_t, so a caller could ask for 65535 and overflow both.
       No current caller does, but the guard costs nothing. */
    if(len>TLS_SEND_MAX) return -1;

    /* MAC input: seq_num(8)+type(1)+ver(2)+len(2)+data */
    static uint8_t mac_input[8+5+16384];
    for(int i=7;i>=0;i--) mac_input[i]=(uint8_t)(g_sess.client_seq>>(56-i*8));
    mac_input[8]=ctype;
    mac_input[9]=TLS_VER_MAJOR; mac_input[10]=TLS_VER_MINOR;
    put_u16(mac_input+11,len);
    memcpy(mac_input+13,data,len);
    uint8_t mac[32];
    hmac_sha256(g_sess.client_write_mac,32,mac_input,(uint32_t)(13+len),mac);

    /* Build plaintext: data + mac + pkcs7 padding */
    static uint8_t plain[16384+32+16];
    memcpy(plain,data,len);
    memcpy(plain+len,mac,32);
    uint32_t plen=len+32;
    /* v0.7.5 FIX: TLS 1.2 (RFC 5246 6.2.3.2), not PKCS#7.  We append
       pad_total bytes all equal to (pad_total-1); the last of them IS the
       padding_length field.  The old code stored pad_total, so the receiver
       computed the wrong plaintext length and the MAC never matched. */
    uint32_t pad_total=16-(plen%16);
    if(pad_total==0) pad_total=16;
    memset(plain+plen,(uint8_t)(pad_total-1),pad_total);
    plen+=pad_total;

    /* IV */
    uint8_t iv[16]; memcpy(iv,g_sess.client_write_iv,16);
    iv[0]^=(uint8_t)g_sess.client_seq;
    iv[15]^=(uint8_t)(g_sess.client_seq>>8);

    aes128_cbc_encrypt(g_sess.client_write_key,iv,plain,plen);

    static uint8_t rec[16+16384+32+16];
    memcpy(rec,iv,16);
    memcpy(rec+16,plain,plen);
    g_sess.client_seq++;

    return tls_send_raw(fd,ctype,rec,(uint16_t)(16+plen));
}

/* Public entry point: application data. */
int tls_send(int fd, const uint8_t *data, uint16_t len){
    if(!sess_bind(fd)) return -1;

    return tls_send_typed(fd, TLS_APPLICATION_DATA, data, len);
}

/* ── Receive application data (decrypt) ──────────────────────────────
 *
 * v0.7.5 CRITICAL FIX — this is why HTTPS never worked.
 *
 * The old tls_recv() decrypted one whole TLS record, copied min(datalen,
 * maxlen) bytes to the caller and THREW THE REST AWAY.  http.c reads
 * response headers one byte at a time (tls_recv(fd,tbuf,1,...)), so a
 * 1400-byte record yielded 1 usable byte and 1399 discarded ones.  Every
 * HTTPS response was shredded beyond recovery.
 *
 * TLS is record-oriented, the caller API is stream-oriented, so a
 * plaintext holding buffer between the two is mandatory.  Records are now
 * decrypted once into g_plain[] and drained across as many calls as it
 * takes.
 */
/* Decrypt exactly one record into g_plain[].  Returns 1 on data ready,
   0 on clean end-of-stream, -1 on error. */
static int tls_fill(int fd, uint32_t timeout_ms){
    uint8_t rtype=0; uint8_t *rdata=NULL; uint16_t rlen=0;

    if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0){
        g_tls_eof=1;
        return 0;
    }
    (void)timeout_ms;

    if(rtype==TLS_ALERT){
        NS_INC(tls_alerts);
        if(rlen>=2){ g_tls_last_alert_level=rdata[0]; g_tls_last_alert_desc=rdata[1]; }
        g_tls_eof=1; return 0;                        /* close_notify or fatal */
    }
    if(rtype!=TLS_APPLICATION_DATA) return -1;       /* unexpected — skip */
    if(rlen<16+16) return -1;                        /* IV + one AES block */

    uint16_t ct_len=(uint16_t)(rlen-16);
    if(ct_len%16) return -1;                         /* not a CBC multiple */
    if(ct_len>sizeof(g_plain)) return -1;

    const uint8_t *iv=rdata;
    memcpy(g_plain,rdata+16,ct_len);
    aes128_cbc_decrypt(g_sess.server_write_key,iv,g_plain,ct_len);

    /* RFC 5246 §6.2.3.2: TLS padding is NOT plain PKCS#7.  The final byte
       is padding_length, and it is preceded by padding_length bytes of the
       same value — so pad+1 bytes are stripped in total, not pad.  The old
       code stripped only `pad`, leaving a stray byte on every record. */
    uint8_t pad=g_plain[ct_len-1];
    if(pad>15||(uint16_t)(pad+1)>=ct_len){ NS_INC(tls_bad_pad); return -1; }
    uint16_t plainlen=(uint16_t)(ct_len-(pad+1));

    if(plainlen<32) return -1;                       /* must hold the MAC */
    uint16_t fraglen = (uint16_t)(plainlen-32);

    /* v0.7.11: actually verify it.  A record that fails the MAC is a fatal
       protocol error - RFC 5246 7.2.2 says bad_record_mac must terminate the
       connection, so we tear down rather than resynchronise. */
    if(verify_record_mac(rtype, g_plain, fraglen,
                         g_plain+fraglen, g_sess.server_seq) != 0){
        NS_INC(tls_bad_mac);
        g_tls_last_alert_level = 2;
        g_tls_last_alert_desc  = 20;   /* bad_record_mac */
        g_tls_eof = 1;
        g_plain_len = 0; g_plain_head = 0;
        return -1;
    }

    g_plain_len  = fraglen;                          /* MAC verified, strip */
    g_plain_head = 0;
    g_sess.server_seq++;
    return g_plain_len ? 1 : 0;
}

int tls_recv(int fd, uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms){
    if(!sess_bind(fd)) return -1;

    if(!g_sess.handshake_done||fd!=g_sess.fd) return -1;
    if(maxlen==0) return 0;

    /* Refill only when the holding buffer is drained.
       v0.7.6: bounded.  tls_fill() returning -1 for an unexpected record type
       normally still consumes a record, so this made progress — but a server
       emitting a run of empty or malformed records could pin the CPU with no
       way out.  64 consecutive non-productive records is far beyond anything
       legitimate. */
    int spins=0;
    while(g_plain_head>=g_plain_len){
        if(g_tls_eof) return 0;                      /* genuine EOF */
        if(++spins>64) return -1;
        int r=tls_fill(fd,timeout_ms);
        if(r<0) continue;                            /* skip a bad record */
        if(r==0 && g_tls_eof) return 0;
        if(r==0) continue;                           /* empty record — keep reading */
    }

    uint16_t avail=(uint16_t)(g_plain_len-g_plain_head);
    if(avail>maxlen) avail=maxlen;
    memcpy(buf,g_plain+g_plain_head,avail);
    g_plain_head=(uint16_t)(g_plain_head+avail);
    return (int)avail;
}

/* TLS-layer end of stream.  http.c must use this instead of tcp_eof():
   buffered plaintext can still be pending long after the TCP FIN lands. */
int tls_eof(int fd){
    if(!sess_bind(fd)) return 1;

    if(fd!=g_sess.fd) return 1;
    if(g_plain_head<g_plain_len) return 0;           /* plaintext still buffered */
    return g_tls_eof || g_tr->eof(fd);
}

void tls_close(int fd){
    /* v0.8.9: with no session bound there is no transport to ask, so
       this keeps closing the fd as a TCP socket -- which is what every
       caller that can reach this line is holding. A usbmux channel is
       closed by usb/lockdown.c and never arrives here: see the note on
       tls_transport_t::close. */
    if(!sess_bind(fd)){ tcp_close(fd); return; }

    /* v0.7.11: send close_notify before tearing down.  Without it the peer sees
       a bare TCP FIN, which is indistinguishable from a truncation attack -
       well-behaved servers log it and some reset the connection, which is why
       a second request to the same host could fail after a clean first one. */
    if(g_sess.handshake_done && !g_tls_eof){
        uint8_t alert[2]={1,0};                 /* warning, close_notify */
        tls_send_typed(fd,TLS_ALERT,alert,2);
    }
    /* v0.7.13: free the whole slot, not just the key material — otherwise the
       slot stays claimed for this fd and the third connection cannot open. */
    {   /* Read the transport out BEFORE the slot is wiped. */
        const tls_transport_t *tr = g_tr;
        sess_release();
        if(tr && tr->close) tr->close(fd);
    }
}
