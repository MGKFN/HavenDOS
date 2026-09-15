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

extern void sleep_ms(uint32_t ms);
extern void net_poll(void);

/* ── TLS record types ────────────────────────────────────────── */
#define TLS_CHANGE_CIPHER_SPEC  20
#define TLS_ALERT               21
#define TLS_HANDSHAKE           22
#define TLS_APPLICATION_DATA    23

/* ── Handshake message types ────────────────────────────────── */
#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE          11
#define HS_SERVER_HELLO_DONE    14
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20

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

static tls_session_t g_sess;

/* ── Pseudo-RNG (simple, seeded from timer) ──────────────────── */
static uint32_t g_prng = 0xDEADBEEFu;
static uint8_t prng_byte(void){
    g_prng ^= g_prng<<13;
    g_prng ^= g_prng>>17;
    g_prng ^= g_prng<<5;
    return (uint8_t)(g_prng & 0xFF);
}
static void prng_seed(uint32_t s){ g_prng = s ? s : 0xCAFEBABEu; }
static void prng_fill(uint8_t *buf, int len){
    for(int i=0;i<len;i++) buf[i]=prng_byte();
}

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
    if(tcp_send(fd,hdr,5)<0) return -1;
    if(len && tcp_send(fd,data,(uint16_t)len)<0) return -1;
    return 0;
}

/* ── Receive one TLS record ──────────────────────────────────── */
#define TLS_MAX_RECORD  (16384+512)
static uint8_t g_recv_buf[TLS_MAX_RECORD];

static int tls_recv_record(int fd, uint8_t *type_out, uint8_t **data_out, uint16_t *len_out){
    uint8_t hdr[5];
    int got=tcp_recv(fd,hdr,5,10000);
    if(got<5) return -1;
    *type_out=hdr[0];
    uint16_t len=get_u16(hdr+3);
    if(len>TLS_MAX_RECORD) return -1;
    int total=0;
    while(total<(int)len){
        int r=tcp_recv(fd,g_recv_buf+total,(uint16_t)(len-total),5000);
        if(r<=0) break;
        total+=r;
    }
    *data_out=g_recv_buf;
    *len_out=(uint16_t)total;
    return 0;
}

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
    /* Build full handshake message for record */
    static uint8_t hs_buf[8192];
    hs_buf[0]=hs_type; put_u24(hs_buf+1,body_len);
    if(body_len && body_len<sizeof(hs_buf)-4) memcpy(hs_buf+4,body,body_len);
    return tls_send_raw(fd,TLS_HANDSHAKE,hs_buf,(uint16_t)(4+body_len));
}

/* ── Build and send ClientHello ──────────────────────────────── */
static int send_client_hello(int fd, const char *hostname){
    /* client_random: 4 bytes time + 28 random */
    g_sess.client_random[0]=0; g_sess.client_random[1]=0;
    g_sess.client_random[2]=0; g_sess.client_random[3]=0;
    prng_fill(g_sess.client_random+4,28);

    int hlen=(int)strlen(hostname);
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
    /* SNI extension (type 0x0000) */
    put_u16(hello+pos,0x0000); pos+=2;  /* type: SNI */
    put_u16(hello+pos,(uint16_t)(hlen+5)); pos+=2;  /* ext data len */
    put_u16(hello+pos,(uint16_t)(hlen+3)); pos+=2;  /* server_name_list length */
    hello[pos++]=0;                                   /* name_type: host_name */
    put_u16(hello+pos,(uint16_t)hlen); pos+=2;
    memcpy(hello+pos,hostname,hlen); pos+=hlen;
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

static int parse_certificate(const uint8_t *body, uint16_t blen){
    if(blen<3) return -1;
    uint32_t list_len=get_u24(body);
    if(list_len+3>blen) return -1;
    /* First cert */
    uint32_t cert_len=get_u24(body+3);
    const uint8_t *cert=body+6;
    if(cert_len+6>blen) return -1;

    /* Scan for RSA OID */
    for(uint32_t i=0;i+sizeof(rsa_oid)<cert_len;i++){
        if(memcmp(cert+i,rsa_oid,sizeof(rsa_oid))==0){
            /* Skip past OID tag (06), len, oid bytes, NULL params */
            const uint8_t *p=cert+i+sizeof(rsa_oid);
            uint32_t remain=(uint32_t)(cert_len-(uint32_t)(p-cert));
            /* Skip NULL (05 00) if present */
            if(remain>=2&&p[0]==0x05&&p[1]==0x00){ p+=2; remain-=2; }
            /* Expect BIT STRING (03) */
            if(remain<2||p[0]!=0x03) continue;
            int lc=0;
            int bslen=der_read_len(p+1,(int)remain-1,&lc);
            if(bslen<2) continue;
            p+=1+lc;
            /* Skip unused bits byte */
            p++; bslen--;
            /* Now should have SEQUENCE (30) for RSAPublicKey */
            if(p[0]!=0x30) continue;
            int seqc=0;
            int seqlen=der_read_len(p+1,bslen-1,&seqc);
            if(seqlen<4) continue;
            p+=1+seqc;
            /* INTEGER (02) = modulus */
            if(p[0]!=0x02) continue;
            int mc=0;
            int mlen=der_read_len(p+1,seqlen,&mc);
            if(mlen<4||mlen>512) continue;
            p+=1+mc;
            /* skip leading zero if any */
            if(p[0]==0x00){ p++; mlen--; }
            if(mlen<4||mlen>512) continue;
            g_sess.rsa_mod_len=mlen;
            memcpy(g_sess.rsa_mod,p,mlen);
            p+=mlen;
            /* INTEGER (02) = exponent */
            if(p[0]!=0x02) return -1;
            int ec=0;
            int elen=der_read_len(p+1,32,&ec);
            if(elen<1||elen>8) return -1;
            p+=1+ec;
            g_sess.rsa_exp_len=elen;
            memcpy(g_sess.rsa_exp,p,elen);
            return 0;   /* success */
        }
    }
    return -1;   /* no RSA key found */
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
    memset(plain+48,0x10,16);
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
    if(len-16>sizeof(plain)) return -1;
    uint16_t ct_len=(uint16_t)(len-16);
    const uint8_t *iv=data;
    memcpy(plain,data+16,ct_len);
    aes128_cbc_decrypt(g_sess.server_write_key,iv,plain,ct_len);
    /* Remove PKCS7 padding — pad byte value = count of padding bytes */
    uint8_t pad=plain[ct_len-1];
    if(pad<1||pad>16||pad>ct_len) return -1;
    uint16_t plainlen=(uint16_t)(ct_len-pad);
    if(plainlen<16+12) return -1;
    /* First 4 bytes: type + length */
    if(plain[0]!=HS_FINISHED) return -1;
    uint32_t vlen=get_u24(plain+1);
    if(vlen!=12) return -1;
    /* Compute expected verify_data */
    uint8_t expected[12];
    compute_finished("server finished",expected);
    return memcmp(plain+4,expected,12);
}

/* ── TLS handshake ───────────────────────────────────────────── */
int tls_connect(int fd, const char *hostname, uint32_t seed){
    memset(&g_sess,0,sizeof(g_sess));
    g_sess.fd=fd;
    prng_seed(seed);

    /* Init handshake transcript */
    sha256_init(g_sess.hs_ctx);

    /* → ClientHello */
    if(send_client_hello(fd,hostname)<0) return TLS_ERR_HANDSHAKE;

    /* ← ServerHello, Certificate, ServerHelloDone */
    int got_hello=0,got_cert=0,got_done=0;
    for(int attempt=0;attempt<20&&!(got_hello&&got_cert&&got_done);attempt++){
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;

        if(rtype==TLS_ALERT){
            return TLS_ERR_ALERT;
        }
        if(rtype!=TLS_HANDSHAKE) continue;

        /* A single TLS record may contain multiple handshake messages */
        int pos=0;
        while(pos+4<=rlen){
            uint8_t hs_type=rdata[pos];
            uint32_t hs_len=get_u24(rdata+pos+1);
            if(pos+4+(int)hs_len>rlen) break;

            /* Update transcript */
            hs_update(rdata+pos,4+hs_len);

            const uint8_t *body=rdata+pos+4;
            uint16_t body_len=(uint16_t)hs_len;

            if(hs_type==HS_SERVER_HELLO){
                if(parse_server_hello(body,body_len)<0) return TLS_ERR_HANDSHAKE;
                got_hello=1;
            } else if(hs_type==HS_CERTIFICATE){
                if(parse_certificate(body,body_len)<0) return TLS_ERR_CERT;
                got_cert=1;
            } else if(hs_type==HS_SERVER_HELLO_DONE){
                got_done=1;
            }
            pos+=(int)(4+hs_len);
        }
    }

    if(!got_hello||!got_cert||!got_done) return TLS_ERR_HANDSHAKE;

    /* → ClientKeyExchange */
    if(send_client_key_exchange(fd)<0) return TLS_ERR_HANDSHAKE;

    /* Derive session keys */
    derive_keys();

    /* → ChangeCipherSpec */
    if(send_change_cipher_spec(fd)<0) return TLS_ERR_HANDSHAKE;

    /* → Finished (encrypted) */
    if(send_finished(fd)<0) return TLS_ERR_HANDSHAKE;

    /* ← ChangeCipherSpec */
    {
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;
        if(rtype!=TLS_CHANGE_CIPHER_SPEC) return TLS_ERR_HANDSHAKE;
    }

    /* ← Finished (encrypted) */
    {
        uint8_t rtype; uint8_t *rdata; uint16_t rlen;
        if(tls_recv_record(fd,&rtype,&rdata,&rlen)<0) return TLS_ERR_HANDSHAKE;
        if(rtype!=TLS_HANDSHAKE) return TLS_ERR_HANDSHAKE;
        if(verify_server_finished(rdata,rlen)<0) return TLS_ERR_HANDSHAKE;
        g_sess.server_seq++;
    }

    g_sess.handshake_done=1;
    return 0;
}

/* ── Send application data (encrypted) ──────────────────────── */
int tls_send(int fd, const uint8_t *data, uint16_t len){
    if(!g_sess.handshake_done||fd!=g_sess.fd) return -1;

    /* MAC input: seq_num(8)+type(1)+ver(2)+len(2)+data */
    static uint8_t mac_input[8+5+16384];
    for(int i=7;i>=0;i--) mac_input[i]=(uint8_t)(g_sess.client_seq>>(56-i*8));
    mac_input[8]=TLS_APPLICATION_DATA;
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
    uint8_t pad_byte=(uint8_t)(16-plen%16);
    if(pad_byte==0) pad_byte=16;
    /* BUG FIX: PKCS7 requires each padding byte = count of padding bytes.
       Was pad_byte-1, which produced wrong values that servers rejected. */
    memset(plain+plen,pad_byte,pad_byte);
    plen+=pad_byte;

    /* IV */
    uint8_t iv[16]; memcpy(iv,g_sess.client_write_iv,16);
    iv[0]^=(uint8_t)g_sess.client_seq;
    iv[15]^=(uint8_t)(g_sess.client_seq>>8);

    aes128_cbc_encrypt(g_sess.client_write_key,iv,plain,plen);

    static uint8_t rec[16+16384+32+16];
    memcpy(rec,iv,16);
    memcpy(rec+16,plain,plen);
    g_sess.client_seq++;

    return tls_send_raw(fd,TLS_APPLICATION_DATA,rec,(uint16_t)(16+plen));
}

/* ── Receive application data (decrypt) ──────────────────────── */
int tls_recv(int fd, uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms){
    if(!g_sess.handshake_done||fd!=g_sess.fd) return -1;

    uint8_t rtype; uint8_t *rdata; uint16_t rlen;
    /* Poll with timeout — retry every 5ms */
    uint32_t elapsed=0;
    while(elapsed<timeout_ms){
        rtype=0; rdata=NULL; rlen=0;
        int r=tls_recv_record(fd,&rtype,&rdata,&rlen);
        if(r==0) break;
        sleep_ms(5); elapsed+=5;
    }
    if(rtype!=TLS_APPLICATION_DATA||rlen<16+1) return 0;

    /* Decrypt */
    static uint8_t plain[TLS_MAX_RECORD];
    uint16_t ct_len=(uint16_t)(rlen-16);
    if(ct_len>sizeof(plain)) return -1;
    const uint8_t *iv=rdata;
    memcpy(plain,rdata+16,ct_len);
    aes128_cbc_decrypt(g_sess.server_write_key,iv,plain,ct_len);

    /* Remove PKCS7 padding */
    /* BUG FIX: was plain[ct_len-1]+1 which removed one extra byte.
       PKCS7: last byte IS the count. If server sends 5 bytes of 0x05, pad=5. */
    uint8_t pad=plain[ct_len-1];
    if(pad<1||pad>16||pad>ct_len) return -1;
    uint16_t plainlen=(uint16_t)(ct_len-pad);

    /* Remove MAC (32 bytes at end) */
    if(plainlen<32) return -1;
    uint16_t datalen=(uint16_t)(plainlen-32);

    g_sess.server_seq++;

    if(datalen>maxlen) datalen=maxlen;
    memcpy(buf,plain,datalen);
    return (int)datalen;
}

void tls_close(int fd){
    if(fd==g_sess.fd) memset(&g_sess,0,sizeof(g_sess));
    tcp_close(fd);
}
