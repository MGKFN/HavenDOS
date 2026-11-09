/* ================================================================
   net/x509.c -- see include/x509.h.
   ================================================================ */
#include "x509.h"
#include "der.h"
#include "bignum.h"
#include "string.h"

extern void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* ── object identifiers ──────────────────────────────────────────
   Written as arcs rather than as pre-encoded bytes so der_oid() does
   the 40a+b and the base-128 continuation, which is the part that is
   easy to get wrong by hand and impossible to see wrong in a dump. */
static const uint32_t OID_RSA_ENC[]     = {1,2,840,113549,1,1,1};
static const uint32_t OID_SHA256_RSA[]  = {1,2,840,113549,1,1,11};
static const uint32_t OID_CN[]          = {2,5,4,3};
static const uint32_t OID_BASIC_CONSTR[]= {2,5,29,19};
static const uint32_t OID_KEY_USAGE[]   = {2,5,29,15};

/* DigestInfo for SHA-256, RFC 8017 section 9.2 note 1. The fixed
   prefix is used rather than re-encoded every time: it is a constant
   in the standard, every implementation hard-codes it, and an
   encoder bug here would produce a signature that no other
   implementation accepts while verifying perfectly against itself. */
static const uint8_t SHA256_DIGESTINFO[19] = {
    0x30,0x31, 0x30,0x0d, 0x06,0x09, 0x60,0x86,0x48,0x01,0x65,0x03,
    0x04,0x02,0x01, 0x05,0x00, 0x04,0x20
};

int pkcs1_pad_sha256(uint32_t k, const uint8_t digest[32],
                     uint8_t *out, uint32_t outcap)
{
    uint32_t tlen = sizeof SHA256_DIGESTINFO + 32u;
    uint32_t i, ps;

    if(!out || !digest) return X509_E_PARAM;
    if(outcap < k) return X509_E_SPACE;
    /* RFC 8017: at least eight octets of padding, so the modulus has
       to be at least tLen + 11 bytes. A 512-bit key is 64 bytes and
       tLen is 51, so even that fits -- but the check is here because
       the failure mode without it is a signature that is silently
       truncated rather than refused. */
    if(k < tlen + 11u) return X509_E_KEY;

    ps = k - tlen - 3u;
    out[0] = 0x00;
    out[1] = 0x01;
    for(i = 0; i < ps; i++) out[2 + i] = 0xFF;
    out[2 + ps] = 0x00;
    memcpy(out + 3 + ps, SHA256_DIGESTINFO, sizeof SHA256_DIGESTINFO);
    memcpy(out + 3 + ps + sizeof SHA256_DIGESTINFO, digest, 32);
    return X509_OK;
}

int pkcs1_sign_sha256(const bn_t *n, const bn_t *d,
                      const uint8_t digest[32],
                      uint8_t *out, uint32_t outcap, uint32_t *outlen)
{
    static uint8_t em[BN_BITS / 8];
    bn_t m, s;
    uint32_t k;
    int r;

    if(!n || !d || !digest || !out) return X509_E_PARAM;
    k = (bn_bits(n) + 7u) / 8u;
    if(!k || k > sizeof em) return X509_E_KEY;
    if(outcap < k) return X509_E_SPACE;

    r = pkcs1_pad_sha256(k, digest, em, sizeof em);
    if(r != X509_OK) return r;

    if(!bn_from_bytes(&m, em, k)) return X509_E_SIGN;
    if(!bn_modexp(&s, &m, d, n)) return X509_E_SIGN;

    /* LEFT-PADDED TO THE MODULUS LENGTH, ALWAYS.
       bn_to_bytes() with a length writes that many bytes big-endian
       with leading zeroes, which is what RFC 8017 I2OSP means. Asking
       it for the signature's own byte length instead would produce a
       signature one byte short roughly one time in 256 -- rare enough
       to pass a suite that signs once and fail on somebody's phone. */
    if(!bn_to_bytes(&s, out, k)) return X509_E_SIGN;
    if(outlen) *outlen = k;
    return X509_OK;
}

/* ── pieces of a certificate ─────────────────────────────────────── */

/* AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters NULL }
   The NULL is not optional for rsaEncryption and sha256WithRSA:
   RFC 4055 says it MUST be present and MUST be NULL, and leaving it
   out produces a certificate that some parsers take and others
   reject. */
static void alg_id(der_t *d, const uint32_t *oid, uint32_t n)
{
    uint32_t m = der_seq_begin(d, DER_SEQUENCE);
    der_oid(d, oid, n);
    der_null(d);
    der_seq_end(d, m);
}

/* Name ::= SEQUENCE OF RelativeDistinguishedName, and we use exactly
   one RDN with one attribute: CN. */
static void name_cn(der_t *d, const char *cn)
{
    uint32_t seq = der_seq_begin(d, DER_SEQUENCE);
    uint32_t set = der_seq_begin(d, DER_SET);
    uint32_t av  = der_seq_begin(d, DER_SEQUENCE);
    der_oid(d, OID_CN, 4);
    der_string(d, DER_UTF8STRING, cn);
    der_seq_end(d, av);
    der_seq_end(d, set);
    der_seq_end(d, seq);
}

/* SubjectPublicKeyInfo: the RSAPublicKey SEQUENCE is encoded into a
   scratch buffer and spliced in as the BIT STRING's contents, because
   a BIT STRING's value is an octet string and not a nested TLV the
   outer writer can nest into. */
static int spki(der_t *d, const bn_t *n, const bn_t *e)
{
    static uint8_t inner[BN_BITS / 4];
    der_t id;
    uint32_t m, b;

    der_init(&id, inner, sizeof inner);
    b = der_seq_begin(&id, DER_SEQUENCE);
    der_integer_bn(&id, n);
    der_integer_bn(&id, e);
    der_seq_end(&id, b);
    if(!der_ok(&id)) return X509_E_DER;

    m = der_seq_begin(d, DER_SEQUENCE);
    alg_id(d, OID_RSA_ENC, 7);
    der_bitstring(d, inner, der_len(&id));
    der_seq_end(d, m);
    return X509_OK;
}

/* Extension ::= SEQUENCE { extnID OID, critical BOOLEAN DEFAULT FALSE,
                            extnValue OCTET STRING }
   extnValue holds the DER of the extension's own value, so each one
   is encoded into scratch and spliced, the same as the SPKI above. */
static void ext_basic_constraints(der_t *d, int is_ca)
{
    uint8_t  inner[16];
    der_t    v;
    uint32_t m, b;

    der_init(&v, inner, sizeof inner);
    b = der_seq_begin(&v, DER_SEQUENCE);
    /* DEFAULT FALSE: a DER encoder must OMIT a field at its default,
       so CA:FALSE is an EMPTY sequence and not an explicit FALSE. An
       explicit one is still parsed by everything, and is still not
       DER. */
    if(is_ca) der_bool(&v, 1);
    der_seq_end(&v, b);

    m = der_seq_begin(d, DER_SEQUENCE);
    der_oid(d, OID_BASIC_CONSTR, 4);
    der_bool(d, 1);                       /* critical */
    der_tlv(d, DER_OCTET_STRING, inner, der_len(&v));
    der_seq_end(d, m);
}

static void ext_key_usage(der_t *d, uint8_t bits)
{
    uint8_t inner[8];
    uint8_t ku[2];
    der_t   v;
    uint32_t m;
    int unused = 0, i;

    /* KeyUsage is a BIT STRING, and its leading octet is the number
       of UNUSED bits in the last byte -- which for a one-byte value
       is however many trailing zeroes it has. der_bitstring() always
       writes 0 there, which is right for a signature and wrong here:
       a KeyUsage of 0xA0 with "0 unused" says nine bits of usage.
       So this one is built by hand. */
    for(i = 0; i < 8; i++){
        if(bits & (1u << i)) break;
        unused++;
    }
    if(unused > 7) unused = 7;
    ku[0] = (uint8_t)unused;
    ku[1] = bits;

    der_init(&v, inner, sizeof inner);
    der_tlv(&v, DER_BIT_STRING, ku, 2);

    m = der_seq_begin(d, DER_SEQUENCE);
    der_oid(d, OID_KEY_USAGE, 4);
    der_bool(d, 1);                       /* critical */
    der_tlv(d, DER_OCTET_STRING, inner, der_len(&v));
    der_seq_end(d, m);
}

int x509_build(const x509_req_t *q, uint8_t *out, uint32_t cap,
               uint32_t *len)
{
    static uint8_t tbs[BN_BITS];        /* the TBSCertificate alone     */
    static uint8_t sig[BN_BITS / 8];
    der_t    t, c;
    uint32_t m, v, val, tbs_len, siglen = 0;
    uint8_t  digest[32];
    int      r;

    if(!q || !out || !q->subject_cn || !q->issuer_cn) return X509_E_PARAM;
    if(!q->subject_n || !q->subject_e) return X509_E_PARAM;
    if(!q->issuer_n || !q->issuer_d)   return X509_E_PARAM;
    if(!q->not_before || !q->not_after) return X509_E_PARAM;

    /* ── TBSCertificate ─────────────────────────────────────────── */
    der_init(&t, tbs, sizeof tbs);
    m = der_seq_begin(&t, DER_SEQUENCE);

    v = der_seq_begin(&t, DER_CTX(0));      /* [0] EXPLICIT version    */
    der_integer_u32(&t, 2);                 /* 2 means v3              */
    der_seq_end(&t, v);

    der_integer_u32(&t, q->serial);
    alg_id(&t, OID_SHA256_RSA, 7);
    name_cn(&t, q->issuer_cn);

    v = der_seq_begin(&t, DER_SEQUENCE);    /* Validity                */
    der_string(&t, DER_UTCTIME, q->not_before);
    der_string(&t, DER_UTCTIME, q->not_after);
    der_seq_end(&t, v);

    name_cn(&t, q->subject_cn);

    r = spki(&t, q->subject_n, q->subject_e);
    if(r != X509_OK) return r;

    v = der_seq_begin(&t, DER_CTX(3));      /* [3] EXPLICIT extensions */
    val = der_seq_begin(&t, DER_SEQUENCE);
    ext_basic_constraints(&t, q->is_ca);
    if(q->key_usage) ext_key_usage(&t, q->key_usage);
    der_seq_end(&t, val);
    der_seq_end(&t, v);

    der_seq_end(&t, m);
    if(!der_ok(&t)) return X509_E_DER;
    tbs_len = der_len(&t);

    /* ── sign THE BYTES WE JUST WROTE ───────────────────────────── */
    sha256(tbs, tbs_len, digest);
    r = pkcs1_sign_sha256(q->issuer_n, q->issuer_d, digest,
                          sig, sizeof sig, &siglen);
    if(r != X509_OK) return r;

    /* ── Certificate ────────────────────────────────────────────── */
    der_init(&c, out, cap);
    m = der_seq_begin(&c, DER_SEQUENCE);
    der_raw(&c, tbs, tbs_len);
    alg_id(&c, OID_SHA256_RSA, 7);
    der_bitstring(&c, sig, siglen);
    der_seq_end(&c, m);
    if(!der_ok(&c)) return X509_E_SPACE;

    if(len) *len = der_len(&c);
    return X509_OK;
}

const char *x509_strerror(int code)
{
    switch(code){
    case X509_OK:        return "ok";
    case X509_E_SPACE:   return "the buffer is too small";
    case X509_E_PARAM:   return "bad parameters";
    case X509_E_KEY:     return "the key is unusable for this signature";
    case X509_E_SIGN:    return "the RSA operation failed";
    case X509_E_DER:     return "the DER encoder gave up";
    default:             return "unknown error";
    }
}
