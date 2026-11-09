/* usb/pair.c -- see include/pair.h. */
#include "../include/pair.h"
#include "../include/x509.h"
#include "../include/plist.h"
#include "../include/string.h"

extern uint32_t entropy_u32(void);

static const char HEX[] = "0123456789abcdef";

int pair_make_uuid(char *out, uint32_t cap)
{
    uint8_t b[16];
    int i, o = 0;
    if (!out || cap < 37) return PAIR_E_PARAM;

    for (i = 0; i < 16; i += 4) {
        uint32_t v = entropy_u32();
        b[i+0] = (uint8_t)(v >> 24);
        b[i+1] = (uint8_t)(v >> 16);
        b[i+2] = (uint8_t)(v >> 8);
        b[i+3] = (uint8_t)v;
    }
    /* Version 4, variant 1 -- RFC 4122. Not decoration: lockdownd
       does not check, but a HostID that is not a UUID is the kind of
       thing that works until the day something does check it. */
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);

    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = HEX[b[i] >> 4];
        out[o++] = HEX[b[i] & 0x0F];
    }
    out[o] = 0;
    /* Upper case, which is what every pair record on disk uses and
       what libimobiledevice writes. */
    for (i = 0; i < o; i++)
        if (out[i] >= 'a' && out[i] <= 'f') out[i] = (char)(out[i] - 32);
    return PAIR_OK;
}

/* The validity window. NOT taken from a clock: this kernel's idea of
   the date is whatever the RTC says and a certificate whose bytes
   depend on when it was built cannot be compared against a fixture.
   lockdownd does not check the dates on a pair record -- the record
   IS the trust -- and a fixed window is honest about that rather than
   pretending to an accuracy nothing uses. 2001 to 2049 covers the
   life of any device this will ever talk to. */
#define NOT_BEFORE "010101000000Z"
#define NOT_AFTER  "490101000000Z"

int pair_build_certs(const pair_key_t *root, const pair_key_t *host,
                     const bn_t *dev_n, const bn_t *dev_e,
                     pair_record_t *rec)
{
    x509_req_t q;
    int r;

    if (!root || !host || !dev_n || !dev_e || !rec) return PAIR_E_PARAM;
    if (bn_bits(&root->n) < 1024 || bn_bits(&host->n) < 1024)
        return PAIR_E_KEY;
    if (bn_bits(dev_n) < 1024) return PAIR_E_DEVKEY;

    /* ── the root: self-signed, and a CA ─────────────────────────────
       Subject and issuer are the same key and the same name, which is
       what self-signed means. CA:TRUE, because the other two
       certificates are signed by it and a device that checked would
       refuse a chain whose root is not one. */
    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Root";
    q.issuer_cn  = "HavenDOS Root";
    q.serial     = 1;
    q.subject_n  = &root->n;  q.subject_e = &root->e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 1;
    q.not_before = NOT_BEFORE;
    q.not_after  = NOT_AFTER;
    r = x509_build(&q, rec->root_cert, sizeof rec->root_cert,
                   &rec->root_len);
    if (r != 0) return PAIR_E_CERT;

    /* ── the host: our own key, signed by the root ───────────────── */
    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Host";
    q.issuer_cn  = "HavenDOS Root";
    q.serial     = 2;
    q.subject_n  = &host->n;  q.subject_e = &host->e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 0;
    /* ── v0.8.9: keyUsage, which include/x509.h said all along ───────
       That header has described the host certificate as "CA:FALSE,
       keyUsage digitalSignature + keyEncipherment, critical" since
       v0.8.8.9, taken from libimobiledevice's userpref.c -- and this
       function never set it. Nothing noticed because nothing used the
       certificate for anything: a header stating an intent the code
       does not carry out is a discrepancy that waits.

       It stopped waiting this release. The host certificate is now a
       TLS CLIENT certificate, and digitalSignature is precisely the
       usage a CertificateVerify exercises. Absent, the extension
       places no restriction and a peer that checks has nothing to
       check; present, it says what the key is for and matches what
       the phone has been handed by every other host it has paired
       with. */
    q.key_usage  = X509_KU_DIGITAL_SIG | X509_KU_KEY_ENCIPHER;
    q.not_before = NOT_BEFORE;
    q.not_after  = NOT_AFTER;
    r = x509_build(&q, rec->host_cert, sizeof rec->host_cert,
                   &rec->host_len);
    if (r != 0) return PAIR_E_CERT;

    /* ── the device: ITS key, signed by OUR root ─────────────────────
       The part that surprises people. The host certifies the device
       to itself, so that in the TLS session afterwards both ends are
       vouched for by the same authority -- the device presents this
       certificate back to us and we recognise our own signature on
       it. We do not have the device's private key and do not need it:
       a certificate is a signature over a PUBLIC key. */
    memset(&q, 0, sizeof q);
    q.subject_cn = "HavenDOS Device";
    q.issuer_cn  = "HavenDOS Root";
    q.serial     = 3;
    q.subject_n  = dev_n;     q.subject_e = dev_e;
    q.issuer_n   = &root->n;  q.issuer_d  = &root->d;
    q.is_ca      = 0;
    q.not_before = NOT_BEFORE;
    q.not_after  = NOT_AFTER;
    r = x509_build(&q, rec->dev_cert, sizeof rec->dev_cert, &rec->dev_len);
    if (r != 0) return PAIR_E_CERT;

    return PAIR_OK;
}

int pair_build_request(const pair_record_t *rec,
                       char *out, uint32_t cap, uint32_t *len)
{
    plist_t p;

    if (!rec || !out || !cap) return PAIR_E_PARAM;
    if (!rec->root_len || !rec->host_len || !rec->dev_len)
        return PAIR_E_CERT;
    if (!rec->host_id[0] || !rec->system_buid[0]) return PAIR_E_PARAM;

    /* ── "too small" is answered BEFORE writing, not after ───────────
       The first version inferred it afterwards from plist_len() --
       "if the length reached the capacity it must have run out" --
       and that is wrong in the case it most needed to be right: a
       buffer so small that the very first write is refused leaves the
       length at zero, so a hopeless capacity reported itself as a bug
       in the writer. tests/test_pair.c caught it.

       The requirement is arithmetic: three certificates at four
       base64 characters per three bytes, a newline every 68
       characters, and about a kilobyte of elements, keys, UUIDs and
       the DOCTYPE. */
    {
        uint32_t der = rec->root_len + rec->host_len + rec->dev_len;
        uint32_t b64 = (der + 2) / 3 * 4;
        uint32_t need = b64 + b64 / 68 + 1024;
        if (cap < need) return PAIR_E_SPACE;
    }

    plist_init(&p, out, cap);
    plist_begin(&p);
    plist_dict_begin(&p);
      plist_kv_string(&p, "Label",   "HavenDOS");
      plist_kv_string(&p, "Request", "Pair");
      plist_key(&p, "PairRecord");
      plist_dict_begin(&p);
        /* The order libimobiledevice uses. A dict is unordered by the
           format and lockdownd does not care, and matching it anyway
           means a diff against a known-good request is readable --
           which is the only debugging tool there is when the reply is
           a bare error. */
        plist_kv_data(&p, "DeviceCertificate", rec->dev_cert, rec->dev_len);
        plist_kv_data(&p, "HostCertificate",   rec->host_cert, rec->host_len);
        plist_kv_string(&p, "HostID",          rec->host_id);
        plist_kv_data(&p, "RootCertificate",   rec->root_cert, rec->root_len);
        plist_kv_string(&p, "SystemBUID",      rec->system_buid);
      plist_dict_end(&p);
      plist_key(&p, "PairingOptions");
      plist_dict_begin(&p);
        /* Without this, a device that refuses says "PairingFailed"
           and nothing else. With it, it says which of the dozen
           reasons it was -- and the whole value of this request is in
           its reply. */
        plist_kv_bool(&p, "ExtendedPairingErrors", 1);
      plist_dict_end(&p);
      plist_kv_string(&p, "ProtocolVersion", "2");
    plist_dict_end(&p);
    plist_end(&p);

    /* Past the precheck, a refusal is a bug in this function rather
       than a buffer the caller sized wrongly, and it is reported as
       one. */
    if (!plist_ok(&p)) return PAIR_E_PLIST;
    if (len) *len = plist_len(&p);
    return PAIR_OK;
}

/* ── the reply ────────────────────────────────────────────────────────
   A bounded scan for the <string> that follows a given <key>, which
   is what usb/lockdown.c already does for GetValue. Not a plist
   reader: this looks for two named keys in a document the device just
   sent, and a parser general enough to read any plist is a parser
   nobody can check -- the same argument net/der_read.c's header
   makes.

   What it must not do is find a key inside somebody else's value. The
   scan therefore matches the full "<key>NAME</key>" and then requires
   the next element to be a <string>, rather than searching for NAME
   anywhere. */
static int find_string(const char *xml, uint32_t len, const char *key,
                       const char **val, uint32_t *vlen)
{
    char pat[64];
    uint32_t klen, i;

    klen = 0;
    pat[klen++] = '<'; pat[klen++] = 'k'; pat[klen++] = 'e';
    pat[klen++] = 'y'; pat[klen++] = '>';
    while (*key && klen < sizeof pat - 8) pat[klen++] = *key++;
    pat[klen++] = '<'; pat[klen++] = '/'; pat[klen++] = 'k';
    pat[klen++] = 'e'; pat[klen++] = 'y'; pat[klen++] = '>';

    if (len < klen) return 0;
    for (i = 0; i + klen <= len; i++) {
        uint32_t j = i + klen;
        if (memcmp(xml + i, pat, klen) != 0) continue;
        /* Skip whitespace to the next element. */
        while (j < len && (xml[j] == ' ' || xml[j] == '\t' ||
                           xml[j] == '\n' || xml[j] == '\r')) j++;
        if (j + 8 > len) return 0;
        if (memcmp(xml + j, "<string>", 8) != 0) return 0;
        j += 8;
        {
            uint32_t s = j;
            while (j + 9 <= len && memcmp(xml + j, "</string>", 9) != 0) j++;
            if (j + 9 > len) return 0;
            *val = xml + s;
            *vlen = j - s;
            return 1;
        }
    }
    return 0;
}

static int eq(const char *a, uint32_t alen, const char *b)
{
    uint32_t n = (uint32_t)strlen(b);
    return alen == n && memcmp(a, b, n) == 0;
}

int pair_reply_status(const char *xml, uint32_t len)
{
    const char *v = 0;
    uint32_t vlen = 0;

    if (!xml || !len) return PAIR_R_NOREPLY;

    /* An Error, if there is one, decides. ExtendedPairingErrors is
       what makes these strings worth reading rather than all being
       "PairingFailed". */
    if (find_string(xml, len, "Error", &v, &vlen)) {
        if (eq(v, vlen, "PairingDialogResponsePending")) return PAIR_R_PENDING;
        if (eq(v, vlen, "UserDeniedPairing"))            return PAIR_R_DENIED;
        if (eq(v, vlen, "PasswordProtected"))            return PAIR_R_PASSWORD;
        if (eq(v, vlen, "InvalidHostID"))                return PAIR_R_INVALID_HOSTID;
        return PAIR_R_OTHER;
    }
    /* No Error and a Result of Success is a pairing. */
    if (find_string(xml, len, "Result", &v, &vlen) && eq(v, vlen, "Success"))
        return PAIR_R_SUCCESS;
    /* An EscrowBag is only ever in a successful reply, and some iOS
       versions send it without a Result. */
    if (find_string(xml, len, "Request", &v, &vlen) && eq(v, vlen, "Pair")) {
        /* A reply that echoes the request and carries no error is the
           other shape of success. */
        return PAIR_R_SUCCESS;
    }
    return PAIR_R_NOREPLY;
}

const char *pair_status_name(int status)
{
    switch (status) {
    case PAIR_R_SUCCESS: return "paired";
    case PAIR_R_PENDING: return "the device is asking the user -- tap Trust";
    case PAIR_R_DENIED:  return "the user tapped Don't Trust";
    case PAIR_R_PASSWORD:
        return "unlock the device with its passcode first";
    case PAIR_R_INVALID_HOSTID:
        return "the device does not know this HostID";
    case PAIR_R_OTHER:   return "the device refused and named a reason";
    default:             return "nothing recognisable in the reply";
    }
}

const char *pair_strerror(int code)
{
    switch (code) {
    case PAIR_OK:        return "ok";
    case PAIR_E_PARAM:   return "bad argument";
    case PAIR_E_KEY:     return "a key too small to pair with";
    case PAIR_E_CERT:    return "a certificate would not build";
    case PAIR_E_SPACE:   return "the request does not fit the buffer";
    case PAIR_E_PLIST:   return "the plist writer refused the document";
    case PAIR_E_DEVKEY:  return "DevicePublicKey would not parse";
    default:             return "unknown";
    }
}

/* ════════════════════════════════════════════════════════════════════
   WHAT THIS WAS PROVED AGAINST

   27 defects injected one at a time, with openssl verifying the
   certificate chain. 24 caught on the first run; three survived, and
   all three were blind fixtures rather than correct code:

     - MAKING ALL THREE SERIAL NUMBERS 1 survived. `openssl verify`
       does not care for a two-certificate chain and lockdownd does
       not either, so nothing noticed. A certificate authority that
       issues duplicate serials is wrong anyway, and the day
       something checks is not the day to discover it. The suite now
       requires three distinct serials.

     - REMOVING THE UUID's TERMINATOR survived, because the test's
       `char a[40]` happened to hold a zero at offset 36. The buffer
       is filled with 'X' first now, and the byte after the UUID is
       asserted to be the terminator.

     - NOT REQUIRING THE VALUE TO BE A <string> survived, because the
       fixture had no later </string> for a loose scanner to run to,
       so the terminator check caught it instead. The fixture is now
       a <data> value followed by a real <string> -- which is what a
       scanner that skips the opening tag would return.

   The twenty-four include every one that would have shipped a
   pairing that cannot work: a root that is not a CA, a leaf that is,
   the host certificate self-signed instead of root-signed, an issuer
   name that does not chain, and -- the one that would have looked
   perfect in every structural check -- THE DEVICE CERTIFICATE
   CARRYING OUR OWN MODULUS instead of the device's.

   ── THE NEGATIVE CONTROL ────────────────────────────────────────────

   A separate certificate authority, built from its own key, must NOT
   verify our certificates. Without it, "openssl verify succeeded"
   could mean openssl was not checking the signature at all -- and the
   first version of that control overwrote the host key to make the
   other CA, which quietly broke the round-trip checks downstream. A
   negative control that disturbs what it is a control for is not one.
   ════════════════════════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════════════════════════
   THE STORE -- v0.8.9, stage 3's second half

   See include/pair.h for why the private keys are part of the stored
   thing and why it is text. This half of the file is a serialiser and
   nothing else: no filesystem call appears in it, because usb/ has no
   business opening files and because a serialiser that needs a volume
   to be tested is a serialiser that does not get tested. The
   vfs_write() is in shell/paircmd.c, which is where the record's
   lifetime is decided anyway.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/b64.h"
/* The tree has exactly ONE CRC-32, and it is named for its first
   caller rather than for what it is. That is a smaller wrong than a
   second implementation -- which is the whole argument of
   include/b64.h, written when base64 had reached four copies. */
#include "../include/gpt.h"

#define PS_MAGIC   "HavenDOS-pair-record"
#define PS_VERSION 1

/* The fields, in the order they are written and the only order that is
   accepted. A reader that accepts any order is a reader that cannot
   tell a record from a record with one field substituted for
   another. */
#define PS_F_HOSTID   0
#define PS_F_BUID     1
#define PS_F_ROOT_N   2
#define PS_F_ROOT_E   3
#define PS_F_ROOT_D   4
#define PS_F_HOST_N   5
#define PS_F_HOST_E   6
#define PS_F_HOST_D   7
#define PS_F_ROOTCERT 8
#define PS_F_HOSTCERT 9
#define PS_F_DEVCERT  10
#define PS_F_COUNT    11

static const char *const PS_NAME[PS_F_COUNT] = {
    "HostID", "SystemBUID",
    "RootN", "RootE", "RootD",
    "HostN", "HostE", "HostD",
    "RootCertificate", "HostCertificate", "DeviceCertificate"
};

/* ── small, bounded text helpers ──────────────────────────────────────
   Not a printf: this file is in usb/ and the one thing a serialiser
   must never do is write one byte more than it was given. Every one of
   these takes the capacity and the cursor and refuses rather than
   truncating, and `ok` latches off exactly like net/der.c's and
   net/plist.c's do -- so the caller checks once at the end instead of
   writing eleven `if`s and getting ten right. */
typedef struct {
    char    *buf;
    uint32_t cap, len;
    int      ok;
} ps_w;

static void psw_init(ps_w *w, char *buf, uint32_t cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->ok = (buf && cap) ? 1 : 0;
}

static void psw_raw(ps_w *w, const char *s, uint32_t n)
{
    if (!w->ok) return;
    /* One byte held back for the terminator, always. */
    if (n + 1 > w->cap - w->len) { w->ok = 0; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void psw_str(ps_w *w, const char *s)
{
    psw_raw(w, s, (uint32_t)strlen(s));
}

static void psw_u32(ps_w *w, uint32_t v)
{
    char t[12];
    int i = 0;
    if (!v) { psw_raw(w, "0", 1); return; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) psw_raw(w, &t[i], 1);
}

static void psw_hex8(ps_w *w, uint32_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        char c = HEX[(v >> (i * 4)) & 0xF];
        psw_raw(w, &c, 1);
    }
}

/* A field: "Name <base64>\n". Unwrapped, deliberately -- one field per
   line is what makes the file greppable and what lets the certificate
   lines be pasted between PEM delimiters. */
static void psw_b64_field(ps_w *w, const char *name,
                          const uint8_t *data, uint32_t dlen)
{
    int n;
    if (!w->ok) return;
    psw_str(w, name);
    psw_raw(w, " ", 1);
    if (!w->ok) return;
    n = b64_encode(data, (int)dlen, w->buf + w->len,
                   (int)(w->cap - w->len - 1), 0);
    if (n < 0) { w->ok = 0; return; }
    w->len += (uint32_t)n;
    psw_raw(w, "\n", 1);
}

/* A bignum, as the fewest big-endian bytes that hold it. Leading
   zeroes are not written and not needed: bn_from_bytes() reads any
   length, and a modulus is identified by its value and not by how many
   bytes somebody chose to spell it in. */
static void psw_bn_field(ps_w *w, const char *name, const bn_t *a)
{
    uint8_t tmp[(BN_LIMBS * 4) + 4];
    uint32_t nb;
    if (!w->ok) return;
    nb = (bn_bits(a) + 7) / 8;
    if (nb == 0) nb = 1;                 /* zero is one zero byte */
    if (nb > sizeof tmp) { w->ok = 0; return; }
    if (!bn_to_bytes(a, tmp, nb)) { w->ok = 0; return; }
    psw_b64_field(w, name, tmp, nb);
}

int pair_store_encode(const pair_store_t *st, char *out, uint32_t cap,
                      uint32_t *len)
{
    ps_w w;
    uint32_t crc;

    if (!st || !out || !cap) return PAIR_E_PARAM;
    if (!st->rec.host_id[0] || !st->rec.system_buid[0]) return PAIR_E_PARAM;
    if (!st->rec.root_len || !st->rec.host_len || !st->rec.dev_len)
        return PAIR_E_CERT;
    /* A record whose keys are absent would encode, decode and then
       fail at CertificateVerify -- which is the failure include/pair.h
       says this struct exists to make impossible. Refused here, where
       there is something to say about it. */
    if (bn_bits(&st->root.n) < 1024 || bn_bits(&st->host.n) < 1024 ||
        bn_is_zero(&st->root.d)     || bn_is_zero(&st->host.d) ||
        bn_is_zero(&st->root.e)     || bn_is_zero(&st->host.e))
        return PAIR_E_KEY;

    psw_init(&w, out, cap);
    psw_str(&w, PS_MAGIC " ");
    psw_u32(&w, PS_VERSION);
    psw_raw(&w, "\n", 1);

    psw_str(&w, PS_NAME[PS_F_HOSTID]); psw_raw(&w, " ", 1);
    psw_str(&w, st->rec.host_id);      psw_raw(&w, "\n", 1);
    psw_str(&w, PS_NAME[PS_F_BUID]);   psw_raw(&w, " ", 1);
    psw_str(&w, st->rec.system_buid);  psw_raw(&w, "\n", 1);

    psw_bn_field(&w, PS_NAME[PS_F_ROOT_N], &st->root.n);
    psw_bn_field(&w, PS_NAME[PS_F_ROOT_E], &st->root.e);
    psw_bn_field(&w, PS_NAME[PS_F_ROOT_D], &st->root.d);
    psw_bn_field(&w, PS_NAME[PS_F_HOST_N], &st->host.n);
    psw_bn_field(&w, PS_NAME[PS_F_HOST_E], &st->host.e);
    psw_bn_field(&w, PS_NAME[PS_F_HOST_D], &st->host.d);

    psw_b64_field(&w, PS_NAME[PS_F_ROOTCERT],
                  st->rec.root_cert, st->rec.root_len);
    psw_b64_field(&w, PS_NAME[PS_F_HOSTCERT],
                  st->rec.host_cert, st->rec.host_len);
    psw_b64_field(&w, PS_NAME[PS_F_DEVCERT],
                  st->rec.dev_cert, st->rec.dev_len);

    if (!w.ok) return PAIR_E_SPACE;

    /* The CRC covers everything written so far, which is everything
       except the line that carries it. */
    crc = gpt_crc32(out, w.len);
    psw_str(&w, "CRC32 ");
    psw_hex8(&w, crc);
    psw_raw(&w, "\n", 1);
    if (!w.ok) return PAIR_E_SPACE;

    out[w.len] = 0;
    if (len) *len = w.len;
    return PAIR_OK;
}

/* ── reading it back ─────────────────────────────────────────────────
   Line by line, in order, each field exactly once. Anything else is a
   refusal: this is a private key and a half-understood one is worse
   than none.

   The input is NOT required to be NUL terminated -- a file read off a
   volume is a length and a buffer -- so every scan here is bounded by
   `len`. */

static uint32_t ps_line(const char *in, uint32_t len, uint32_t pos,
                        uint32_t *end)
{
    uint32_t e = pos;
    while (e < len && in[e] != '\n') e++;
    *end = e;                            /* exclusive, no newline */
    return (e < len) ? e + 1 : e;        /* next line's start     */
}

/* Does the line begin with `name` followed by exactly one space?
   Returns the offset of the value, or 0. Full-token matching: "Root"
   must not match the line "RootN ...". */
static uint32_t ps_match(const char *in, uint32_t s, uint32_t e,
                         const char *name)
{
    uint32_t n = (uint32_t)strlen(name);
    if (e - s < n + 1) return 0;
    if (memcmp(in + s, name, n) != 0) return 0;
    if (in[s + n] != ' ') return 0;
    return s + n + 1;
}

static int ps_hex8(const char *p, uint32_t avail, uint32_t *out)
{
    uint32_t v = 0, i;
    if (avail < 8) return 0;
    for (i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static int ps_u32(const char *p, uint32_t avail, uint32_t *out)
{
    uint32_t v = 0, i = 0;
    if (!avail) return 0;
    for (i = 0; i < avail; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        if (v > 0xFFFFFFFFu / 10u) return 0;
        v = v * 10u + (uint32_t)(p[i] - '0');
    }
    *out = v;
    return 1;
}

/* A UUID field: 36 characters of the canonical form, and nothing else.
   Checked by SHAPE rather than only by length -- a 36-character line
   of anything would otherwise become a HostID, and a HostID that is
   not one is include/pair.h's "works until something checks". */
static int ps_uuid(const char *p, uint32_t n, char *out)
{
    uint32_t i;
    if (n != 36) return 0;
    for (i = 0; i < 36; i++) {
        char c = p[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                     (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    memcpy(out, p, 36);
    out[36] = 0;
    return 1;
}

int pair_store_decode(const char *in, uint32_t len, pair_store_t *st)
{
    pair_store_t t;
    uint32_t pos = 0, s, e, v, next;
    uint32_t ver = 0, crc_said = 0, crc_is;
    uint32_t body_end = 0;
    int field = 0;
    uint8_t  scratch[PAIR_CERT_MAX];

    if (!in || !st || !len) return PAIR_E_PARAM;
    memset(&t, 0, sizeof t);

    /* ── the magic line ─────────────────────────────────────────── */
    s = pos; next = ps_line(in, len, pos, &e);
    v = ps_match(in, s, e, PS_MAGIC);
    if (!v) return PAIR_E_FORMAT;
    if (!ps_u32(in + v, e - v, &ver)) return PAIR_E_FORMAT;
    /* A LATER version is refused by name. This file will grow a field
       one day, and a reader that ignores the version would load the
       first eleven fields of a twelve-field record and call it a
       pairing. */
    if (ver != PS_VERSION) return PAIR_E_VERSION;
    pos = next;

    /* ── the fields, in order ───────────────────────────────────── */
    for (field = 0; field < PS_F_COUNT; field++) {
        if (pos >= len) return PAIR_E_FORMAT;
        s = pos; next = ps_line(in, len, pos, &e);
        v = ps_match(in, s, e, PS_NAME[field]);
        if (!v) return PAIR_E_FORMAT;
        pos = next;

        switch (field) {
        case PS_F_HOSTID:
            if (!ps_uuid(in + v, e - v, t.rec.host_id))
                return PAIR_E_FORMAT;
            break;
        case PS_F_BUID:
            if (!ps_uuid(in + v, e - v, t.rec.system_buid))
                return PAIR_E_FORMAT;
            break;
        default: {
            int n = b64_decode((const uint8_t *)(in + v), (int)(e - v),
                               scratch, (int)sizeof scratch);
            if (n <= 0) return PAIR_E_FORMAT;
            switch (field) {
            case PS_F_ROOT_N:
                if (!bn_from_bytes(&t.root.n, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_ROOT_E:
                if (!bn_from_bytes(&t.root.e, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_ROOT_D:
                if (!bn_from_bytes(&t.root.d, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_HOST_N:
                if (!bn_from_bytes(&t.host.n, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_HOST_E:
                if (!bn_from_bytes(&t.host.e, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_HOST_D:
                if (!bn_from_bytes(&t.host.d, scratch, (uint32_t)n))
                    return PAIR_E_FORMAT;
                break;
            case PS_F_ROOTCERT:
                memcpy(t.rec.root_cert, scratch, (uint32_t)n);
                t.rec.root_len = (uint32_t)n;
                break;
            case PS_F_HOSTCERT:
                memcpy(t.rec.host_cert, scratch, (uint32_t)n);
                t.rec.host_len = (uint32_t)n;
                break;
            case PS_F_DEVCERT:
                memcpy(t.rec.dev_cert, scratch, (uint32_t)n);
                t.rec.dev_len = (uint32_t)n;
                break;
            }
            break;
        }
        }
    }

    /* Everything above the CRC line is what the CRC covers. */
    body_end = pos;

    if (pos >= len) return PAIR_E_FORMAT;
    s = pos; (void)ps_line(in, len, pos, &e);
    v = ps_match(in, s, e, "CRC32");
    if (!v) return PAIR_E_FORMAT;
    if (!ps_hex8(in + v, e - v, &crc_said)) return PAIR_E_FORMAT;

    crc_is = gpt_crc32(in, body_end);
    if (crc_is != crc_said) return PAIR_E_CRC;

    /* The same refusals the encoder makes, on the way in. A stored
       record that does not carry a usable key is a record that would
       pair and then fail at the signature. */
    if (bn_bits(&t.root.n) < 1024 || bn_bits(&t.host.n) < 1024 ||
        bn_is_zero(&t.root.d)     || bn_is_zero(&t.host.d) ||
        bn_is_zero(&t.root.e)     || bn_is_zero(&t.host.e))
        return PAIR_E_KEY;

    *st = t;
    return PAIR_OK;
}

/* ── the signature net/tls.c asks for ────────────────────────────────
   include/tls.h's tls_sign_fn. The digest is the SHA-256 of the whole
   handshake transcript; this signs it with the HOST key, which is the
   key the host certificate certifies -- and getting that wrong by one
   struct field would produce a signature the device rejects while
   every certificate in the chain checked out. */
int pair_tls_sign(void *ctx, const uint8_t digest[32],
                  uint8_t *sig, uint32_t sigcap, uint32_t *siglen)
{
    const pair_key_t *k = (const pair_key_t *)ctx;
    if (!k || !digest || !sig) return PAIR_E_PARAM;
    if (pkcs1_sign_sha256(&k->n, &k->d, digest, sig, sigcap, siglen)
        != X509_OK)
        return PAIR_E_KEY;
    return PAIR_OK;
}
