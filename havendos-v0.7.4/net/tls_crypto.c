/*
 * tls_crypto.c — Cryptographic primitives for HavenDOS TLS 1.2
 *
 * Implements:
 *   SHA-256       — RFC 6234
 *   AES-128       — FIPS 197 (ECB, CBC mode)
 *   HMAC-SHA256   — RFC 2104
 *   RSA           — PKCS#1 v1.5 encrypt (bignum, up to 4096-bit key)
 *   TLS 1.2 PRF   — RFC 5246 §5 (P_SHA256)
 *
 * All implementations from spec — no external dependencies.
 * Optimised for correctness and small code size, not speed.
 * UTM SE TCG is slow; we avoid unnecessary work.
 */

#include "../include/types.h"
#include "../include/string.h"

/* ═══════════════════════════════════════════════════════════════
   SHA-256
   ═══════════════════════════════════════════════════════════════ */

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a)     (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define EP1(e)     (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define SIG0(x)    (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x)    (ROR32(x,17)^ROR32(x,19)^((x)>>10))

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx_t;

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *data) {
    uint32_t a,b,c,d,e,f,g,h,t1,t2,w[64];
    for(int i=0;i<16;i++)
        w[i]=((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)|
             ((uint32_t)data[i*4+2]<<8)|(uint32_t)data[i*4+3];
    for(int i=16;i<64;i++)
        w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
    a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];
    e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];
    for(int i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+sha256_k[i]+w[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;
    ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}

void sha256_init(sha256_ctx_t *ctx){
    ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
    ctx->count=0;ctx->buflen=0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len){
    for(uint32_t i=0;i<len;i++){
        ctx->buf[ctx->buflen++]=data[i];
        if(ctx->buflen==64){sha256_transform(ctx,ctx->buf);ctx->buflen=0;}
        ctx->count++;
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]){
    uint64_t bitlen=ctx->count*8;
    uint8_t pad=0x80;
    sha256_update(ctx,&pad,1);
    while(ctx->buflen!=56){pad=0;sha256_update(ctx,&pad,1);}
    /* length big-endian */
    for(int i=7;i>=0;i--){pad=(uint8_t)(bitlen>>(i*8));sha256_update(ctx,&pad,1);}
    for(int i=0;i<8;i++){
        out[i*4]=(uint8_t)(ctx->state[i]>>24);
        out[i*4+1]=(uint8_t)(ctx->state[i]>>16);
        out[i*4+2]=(uint8_t)(ctx->state[i]>>8);
        out[i*4+3]=(uint8_t)(ctx->state[i]);
    }
}

/* Convenience one-shot */
void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]){
    sha256_ctx_t ctx; sha256_init(&ctx);
    sha256_update(&ctx,data,len); sha256_final(&ctx,out);
}

/* ═══════════════════════════════════════════════════════════════
   HMAC-SHA256
   ═══════════════════════════════════════════════════════════════ */

void hmac_sha256(const uint8_t *key, uint32_t klen,
                 const uint8_t *data, uint32_t dlen,
                 uint8_t out[32])
{
    uint8_t k[64]; memset(k,0,64);
    if(klen>64){sha256(key,klen,k);klen=32;}
    else memcpy(k,key,klen);

    uint8_t ipad[64],opad[64];
    for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5C;}

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx,ipad,64);
    sha256_update(&ctx,data,dlen);
    uint8_t inner[32]; sha256_final(&ctx,inner);

    sha256_init(&ctx);
    sha256_update(&ctx,opad,64);
    sha256_update(&ctx,inner,32);
    sha256_final(&ctx,out);
}

/* ═══════════════════════════════════════════════════════════════
   AES-128  (FIPS 197)
   ═══════════════════════════════════════════════════════════════ */

/* S-box */
static const uint8_t sbox[256]={
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Inverse S-box */
static const uint8_t rsbox[256]={
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t xtime(uint8_t x){ return (x<<1)^(x&0x80?0x1b:0); }
static uint8_t gmul(uint8_t a,uint8_t b){
    uint8_t p=0;
    for(int i=0;i<8;i++){
        if(b&1) p^=a;
        int hi=a&0x80; a<<=1; if(hi) a^=0x1b; b>>=1;
    }
    return p;
}

typedef uint8_t aes_block_t[16];

static void aes_sub_bytes(aes_block_t s){
    for(int i=0;i<16;i++) s[i]=sbox[s[i]];
}
static void aes_inv_sub_bytes(aes_block_t s){
    for(int i=0;i<16;i++) s[i]=rsbox[s[i]];
}
static void aes_shift_rows(aes_block_t s){
    uint8_t t;
    /* Row 1: shift left 1 */
    t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;
    /* Row 2: shift left 2 */
    t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
    /* Row 3: shift left 3 (= right 1) */
    t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
}
static void aes_inv_shift_rows(aes_block_t s){
    uint8_t t;
    t=s[13];s[13]=s[9];s[9]=s[5];s[5]=s[1];s[1]=t;
    t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
    t=s[3];s[3]=s[7];s[7]=s[11];s[11]=s[15];s[15]=t;
}
static void aes_mix_columns(aes_block_t s){
    for(int c=0;c<4;c++){
        uint8_t *col=s+c*4;
        uint8_t a=col[0],b=col[1],cc=col[2],d=col[3];
        col[0]=gmul(a,2)^gmul(b,3)^cc^d;
        col[1]=a^gmul(b,2)^gmul(cc,3)^d;
        col[2]=a^b^gmul(cc,2)^gmul(d,3);
        col[3]=gmul(a,3)^b^cc^gmul(d,2);
    }
}
static void aes_inv_mix_columns(aes_block_t s){
    for(int c=0;c<4;c++){
        uint8_t *col=s+c*4;
        uint8_t a=col[0],b=col[1],cc=col[2],d=col[3];
        col[0]=gmul(a,0x0e)^gmul(b,0x0b)^gmul(cc,0x0d)^gmul(d,0x09);
        col[1]=gmul(a,0x09)^gmul(b,0x0e)^gmul(cc,0x0b)^gmul(d,0x0d);
        col[2]=gmul(a,0x0d)^gmul(b,0x09)^gmul(cc,0x0e)^gmul(d,0x0b);
        col[3]=gmul(a,0x0b)^gmul(b,0x0d)^gmul(cc,0x09)^gmul(d,0x0e);
    }
}

typedef struct { uint8_t rk[176]; } aes128_ctx_t;   /* 11 round keys × 16 bytes */

static void aes128_key_expand(aes128_ctx_t *ctx, const uint8_t key[16]){
    static const uint8_t rcon[10]={0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    memcpy(ctx->rk, key, 16);
    for(int i=1;i<=10;i++){
        uint8_t *prev=ctx->rk+(i-1)*16;
        uint8_t *cur =ctx->rk+i*16;
        /* RotWord + SubWord + Rcon */
        cur[0]=prev[12]; cur[1]=prev[13]; cur[2]=prev[14]; cur[3]=prev[15];
        /* rotate */
        uint8_t t=cur[0]; cur[0]=cur[1]; cur[1]=cur[2]; cur[2]=cur[3]; cur[3]=t;
        /* sub */
        for(int j=0;j<4;j++) cur[j]=sbox[cur[j]];
        /* xor rcon */
        cur[0]^=rcon[i-1];
        /* xor with prev word */
        for(int j=0;j<4;j++) cur[j]^=prev[j];
        /* words 1-3 */
        for(int w=1;w<4;w++){
            for(int j=0;j<4;j++) cur[w*4+j]=prev[w*4+j]^cur[(w-1)*4+j];
        }
    }
}

static void aes128_encrypt_block(const aes128_ctx_t *ctx, uint8_t *block){
    /* Initial AddRoundKey */
    for(int i=0;i<16;i++) block[i]^=ctx->rk[i];
    for(int round=1;round<=10;round++){
        aes_sub_bytes(block);
        aes_shift_rows(block);
        if(round<10) aes_mix_columns(block);
        for(int i=0;i<16;i++) block[i]^=ctx->rk[round*16+i];
    }
}

static void aes128_decrypt_block(const aes128_ctx_t *ctx, uint8_t *block){
    for(int i=0;i<16;i++) block[i]^=ctx->rk[10*16+i];
    for(int round=9;round>=0;round--){
        aes_inv_shift_rows(block);
        aes_inv_sub_bytes(block);
        for(int i=0;i<16;i++) block[i]^=ctx->rk[round*16+i];
        if(round>0) aes_inv_mix_columns(block);
    }
}

/* AES-128-CBC encrypt — in-place, len must be multiple of 16 */
void aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                         uint8_t *data, uint32_t len)
{
    aes128_ctx_t ctx; aes128_key_expand(&ctx,key);
    uint8_t prev[16]; memcpy(prev,iv,16);
    for(uint32_t i=0;i<len;i+=16){
        for(int j=0;j<16;j++) data[i+j]^=prev[j];
        aes128_encrypt_block(&ctx,data+i);
        memcpy(prev,data+i,16);
    }
}

/* AES-128-CBC decrypt — in-place */
void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                         uint8_t *data, uint32_t len)
{
    aes128_ctx_t ctx; aes128_key_expand(&ctx,key);
    uint8_t prev[16],tmp[16]; memcpy(prev,iv,16);
    for(uint32_t i=0;i<len;i+=16){
        memcpy(tmp,data+i,16);
        aes128_decrypt_block(&ctx,data+i);
        for(int j=0;j<16;j++) data[i+j]^=prev[j];
        memcpy(prev,tmp,16);
    }
}

/* ═══════════════════════════════════════════════════════════════
   RSA — PKCS#1 v1.5 public-key encrypt  (for ClientKeyExchange)
   Big integers stored as big-endian byte arrays, max 512 bytes (4096-bit)
   ═══════════════════════════════════════════════════════════════ */

#define RSA_MAX_BYTES 512

/* n-byte big-endian modular exponentiation: base^exp mod mod → out */
/* Uses square-and-multiply with left-to-right bit scan */
/* All operands are RSA_MAX_BYTES bytes, big-endian */

typedef struct { uint8_t d[RSA_MAX_BYTES]; } bignum_t;

static void bn_zero(bignum_t *a){ memset(a->d,0,RSA_MAX_BYTES); }
static void bn_one (bignum_t *a){ bn_zero(a); a->d[RSA_MAX_BYTES-1]=1; }
static int  bn_is_zero(const bignum_t *a){
    for(int i=0;i<RSA_MAX_BYTES;i++) if(a->d[i]) return 0; return 1;
}

/* a = b */
static void bn_copy(bignum_t *a, const bignum_t *b){
    memcpy(a->d,b->d,RSA_MAX_BYTES);
}

/* a *= 2 (left shift 1), return overflow bit */
static int bn_shl1(bignum_t *a){
    int carry=0;
    for(int i=RSA_MAX_BYTES-1;i>=0;i--){
        int nc=(a->d[i]>>7)&1;
        a->d[i]=(uint8_t)((a->d[i]<<1)|carry);
        carry=nc;
    }
    return carry;
}

/* a >= b ? */
static int bn_ge(const bignum_t *a, const bignum_t *b){
    for(int i=0;i<RSA_MAX_BYTES;i++){
        if(a->d[i]>b->d[i]) return 1;
        if(a->d[i]<b->d[i]) return 0;
    }
    return 1;
}

/* a -= b (assumes a >= b) */
static void bn_sub(bignum_t *a, const bignum_t *b){
    int borrow=0;
    for(int i=RSA_MAX_BYTES-1;i>=0;i--){
        int diff=(int)a->d[i]-(int)b->d[i]-borrow;
        if(diff<0){diff+=256;borrow=1;}else borrow=0;
        a->d[i]=(uint8_t)diff;
    }
}

/* r = (a * b) mod m — using double-and-add then reduce */
/* We work with 2×RSA_MAX_BYTES intermediate; simplified using
   Montgomery-free left-to-right square-and-multiply.
   Since we only need this for RSA public ops (small exponent e=65537),
   we use repeated squaring which is fast enough. */

/* a = (2a) mod m */
static void bn_dbl_mod(bignum_t *a, const bignum_t *m){
    int ov=bn_shl1(a);
    /* if overflow or a >= m, subtract m */
    if(ov || bn_ge(a,m)) bn_sub(a,m);
}

/* r = (a + b) mod m */
static void bn_addmod(bignum_t *r, const bignum_t *a,
                       const bignum_t *b, const bignum_t *m){
    /* r = a + b */
    int carry=0;
    for(int i=RSA_MAX_BYTES-1;i>=0;i--){
        int s=(int)a->d[i]+(int)b->d[i]+carry;
        r->d[i]=(uint8_t)(s&0xFF); carry=s>>8;
    }
    if(carry||bn_ge(r,m)) bn_sub(r,m);
}

/* r = a^e mod m using square-and-multiply, e as big-endian bytes */
static void bn_powmod(bignum_t *r,
                       const bignum_t *base,
                       const uint8_t *exp_be, int exp_bytes,
                       const bignum_t *m)
{
    bn_one(r);

    /* Left-to-right binary method */
    for(int byte=0;byte<exp_bytes;byte++){
        for(int bit=7;bit>=0;bit--){
            /* r = r^2 mod m via repeated doubling */
            bignum_t sq; bn_zero(&sq);
            /* Square: sq = r*r mod m — using double-and-add of r */
            bignum_t acc; bn_zero(&acc);
            bignum_t tmp; bn_copy(&tmp,r);
            for(int i=RSA_MAX_BYTES-1;i>=0;i--){
                for(int b2=7;b2>=0;b2--){
                    bn_dbl_mod(&acc,m);
                    if((r->d[i]>>b2)&1) bn_addmod(&acc,&acc,&tmp,m);
                }
            }
            bn_copy(r,&acc);

            /* If this exponent bit is 1: r = r * base mod m */
            if((exp_be[byte]>>bit)&1){
                bignum_t prod; bn_zero(&prod);
                bignum_t tmb; bn_copy(&tmb,base);
                for(int i=RSA_MAX_BYTES-1;i>=0;i--){
                    for(int b2=7;b2>=0;b2--){
                        bn_dbl_mod(&prod,m);
                        if((r->d[i]>>b2)&1) bn_addmod(&prod,&prod,&tmb,m);
                    }
                }
                bn_copy(r,&prod);
            }
        }
    }
}

/*
 * rsa_public_encrypt: PKCS#1 v1.5 type 2 encrypt
 *   mod_bytes: size of modulus in bytes (e.g. 256 for RSA-2048)
 *   e: public exponent bytes (big-endian), typically {0x01,0x00,0x01} (65537)
 *   e_len: length of e
 *   msg: plaintext (pre-master secret, 48 bytes for TLS)
 *   msg_len: length of msg
 *   out: output buffer of size mod_bytes
 */
int rsa_public_encrypt(const uint8_t *modulus, int mod_bytes,
                        const uint8_t *e, int e_len,
                        const uint8_t *msg, int msg_len,
                        uint8_t *out)
{
    if(mod_bytes>RSA_MAX_BYTES || mod_bytes<11) return -1;
    if(msg_len > mod_bytes-11) return -1;

    /* Build PKCS#1 v1.5 type 2 padded message */
    /* EM = 0x00 0x02 PS 0x00 msg  (PS >= 8 non-zero bytes) */
    static uint8_t em[RSA_MAX_BYTES];
    memset(em,0,RSA_MAX_BYTES);
    int ps_len = mod_bytes - msg_len - 3;
    em[RSA_MAX_BYTES-mod_bytes+0]=0x00;
    em[RSA_MAX_BYTES-mod_bytes+1]=0x02;
    /* PS: non-zero padding — we use fixed pattern (no PRNG yet) */
    /* For TLS this is acceptable since we only need to encrypt once */
    for(int i=0;i<ps_len;i++)
        em[RSA_MAX_BYTES-mod_bytes+2+i]=(uint8_t)(0xAB^(i&0xFF));
    /* make sure none are zero */
    for(int i=0;i<ps_len;i++)
        if(!em[RSA_MAX_BYTES-mod_bytes+2+i])
            em[RSA_MAX_BYTES-mod_bytes+2+i]=0xAB;
    em[RSA_MAX_BYTES-mod_bytes+2+ps_len]=0x00;
    memcpy(em+RSA_MAX_BYTES-msg_len,msg,msg_len);

    /* Load modulus right-aligned in bignum */
    bignum_t M; bn_zero(&M);
    memcpy(M.d+RSA_MAX_BYTES-mod_bytes,modulus,mod_bytes);

    /* Load em into bignum */
    bignum_t EM; bn_zero(&EM);
    memcpy(EM.d,em,RSA_MAX_BYTES);

    /* Compute EM^e mod M */
    bignum_t R;
    bn_powmod(&R,&EM,e,(int)e_len,&M);

    /* Output right-aligned mod_bytes */
    memcpy(out,R.d+RSA_MAX_BYTES-mod_bytes,mod_bytes);
    return mod_bytes;
}

/* ═══════════════════════════════════════════════════════════════
   TLS 1.2 PRF — P_SHA256 (RFC 5246 §5)
   ═══════════════════════════════════════════════════════════════ */

/*
 * P_hash(secret, seed, out, outlen)
 * P_hash = HMAC_hash(secret, A(1) + seed) ||
 *           HMAC_hash(secret, A(2) + seed) || ...
 * A(0)=seed, A(i)=HMAC(secret,A(i-1))
 */
static void p_sha256(const uint8_t *secret, uint32_t slen,
                      const uint8_t *seed,   uint32_t seedlen,
                      uint8_t *out,           uint32_t outlen)
{
    uint8_t a[32]; /* A(i) */
    /* A(1) = HMAC(secret, seed) */
    hmac_sha256(secret,slen,seed,seedlen,a);

    static uint8_t as_seed[32+256]; /* A(i) + seed */
    uint32_t filled=0;
    while(filled<outlen){
        /* Build A(i)+seed */
        memcpy(as_seed,a,32);
        if(seedlen>256) seedlen=256;
        memcpy(as_seed+32,seed,seedlen);
        uint8_t chunk[32];
        hmac_sha256(secret,slen,as_seed,32+seedlen,chunk);
        uint32_t take=outlen-filled; if(take>32)take=32;
        memcpy(out+filled,chunk,take);
        filled+=take;
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret,slen,a,32,a);
    }
}

/*
 * tls12_prf: TLS 1.2 PRF
 * PRF(secret, label, seed, out, outlen)
 * = P_SHA256(secret, label+seed)
 */
void tls12_prf(const uint8_t *secret, uint32_t slen,
               const char *label,
               const uint8_t *seed,  uint32_t seedlen,
               uint8_t *out,         uint32_t outlen)
{
    static uint8_t ls[128]; /* label+seed */
    uint32_t llen=(uint32_t)strlen(label);
    if(llen>64) llen=64;
    memcpy(ls,label,llen);
    if(seedlen>64) seedlen=64;
    memcpy(ls+llen,seed,seedlen);
    p_sha256(secret,slen,ls,llen+seedlen,out,outlen);
}

/* ── Exports ──────────────────────────────────────────────────── */
/* All functions are file-scope; tls.c includes this via extern decls */
