#pragma once
#include "types.h"
#include "bignum.h"

/* ════════════════════════════════════════════════════════════════════
   include/pair.h -- the pair request: the message that makes an iPhone
                     ask "Trust this computer?"

   Stage 3, piece four, first half. Everything it is built from now
   exists, which is the only reason this file is short:

     net/bignum.c, net/rsa.c   a 2048-bit keypair, in seconds
     net/x509.c                a signed certificate, verified by openssl
     net/der_read.c            the device's public key, out of PEM
     net/plist.c               the message, judged by plistlib
     usb/usbmux.c              a request that does not fit one packet
     kernel/entropy.c          the two UUIDs

   ── WHAT PAIRING ACTUALLY IS ────────────────────────────────────────

   Not a password exchange. The host invents a small certificate
   authority of its own, and asks the device to remember it:

     RootCertificate    self-signed, CA:TRUE. The authority.
     HostCertificate    the host's own key, signed by the root.
     DeviceCertificate  THE DEVICE'S OWN PUBLIC KEY, signed by the
                        root -- which is the part that surprises
                        people. The host certifies the device to
                        itself, so that the TLS session afterwards has
                        both ends vouched for by the same authority.

   The device public key comes from `GetValue DevicePublicKey`, which
   usb/lockdown.c has done since stage 2.

   Then: HostID and SystemBUID, two UUIDs the host invents once and
   must keep forever -- a different HostID is a different computer as
   far as the phone is concerned, and pairing again with a new one
   asks the user to tap Trust again.

   ── WHAT THIS FILE DOES NOT DO ──────────────────────────────────────

   - IT DOES NOT GENERATE THE KEYS. rsa_keygen_step() is a state
     machine precisely so that the loop belongs to shell/, where
     kernel_poll() and a keypress that aborts are allowed. See
     include/rsa.h: this file lives in usb/ and may not yield.
   - IT DOES NOT STORE THE PAIR RECORD. A record that is not saved is
     a pairing the user has to repeat, and where to put it is a
     filesystem question for a later release. This produces the
     record; keeping it is somebody else's job and is noted in
     HANDOFF.md rather than half-done here.
   - IT DOES NOT START THE TLS SESSION. StartSession followed by TLS
     with client certificates is the second half of piece four, and
     net/tls.c has no client-certificate path yet.

   So what this delivers is the request, and the reply to it: the
   phone puts the dialog on screen and the user taps Trust. That is a
   visible, testable milestone, and it is where pairing either works
   or says why in words.
   ════════════════════════════════════════════════════════════════════ */

#define PAIR_OK           0
#define PAIR_E_PARAM    (-1)
#define PAIR_E_KEY      (-2)   /* a key that cannot be used          */
#define PAIR_E_CERT     (-3)   /* a certificate would not build      */
#define PAIR_E_SPACE    (-4)   /* the message does not fit           */
#define PAIR_E_PLIST    (-5)   /* the writer refused the document    */
#define PAIR_E_DEVKEY   (-6)   /* DevicePublicKey would not parse    */

/* Reply states, which are the whole point of asking. */
#define PAIR_R_SUCCESS        0
#define PAIR_R_PENDING        1   /* the dialog is on screen         */
#define PAIR_R_DENIED         2   /* the user said no                */
#define PAIR_R_PASSWORD       3   /* the device has a passcode set   */
#define PAIR_R_INVALID_HOSTID 4
#define PAIR_R_OTHER          5   /* an Error we do not have a name for */
#define PAIR_R_NOREPLY        6   /* nothing recognisable in it      */

/* A keypair. The private exponent is here because signing needs it;
   nothing in this file sends it anywhere. */
typedef struct {
    bn_t n, e, d;
} pair_key_t;

/* 1180 is comfortably over what a 2048-bit certificate with one CN
   and three extensions comes to -- tests/test_pair.c measures the
   real figure rather than trusting this one. */
#define PAIR_CERT_MAX 1400

typedef struct {
    char     host_id[40];
    char     system_buid[40];
    uint8_t  root_cert[PAIR_CERT_MAX]; uint32_t root_len;
    uint8_t  host_cert[PAIR_CERT_MAX]; uint32_t host_len;
    uint8_t  dev_cert [PAIR_CERT_MAX]; uint32_t dev_len;
} pair_record_t;

/* A version-4 UUID in the canonical 36-character form, NUL
   terminated. `out` needs 37 bytes. */
int pair_make_uuid(char *out, uint32_t cap);

/* The three certificates. `dev_n`/`dev_e` are the device's public key
   as net/der_read.c produced it. Fills rec->*_cert and the lengths;
   does NOT touch rec's UUIDs, so a caller re-pairing with a stored
   HostID keeps it. */
int pair_build_certs(const pair_key_t *root, const pair_key_t *host,
                     const bn_t *dev_n, const bn_t *dev_e,
                     pair_record_t *rec);

/* The Pair request, as XML, NUL terminated. */
int pair_build_request(const pair_record_t *rec,
                       char *out, uint32_t cap, uint32_t *len);

/* What the device said. Takes the reply's XML. */
int pair_reply_status(const char *xml, uint32_t len);

/* ════════════════════════════════════════════════════════════════════
   KEEPING IT -- v0.8.9, stage 3's second half

   v0.8.8.11's version of this header said storing the record was
   "somebody else's job". This is that job, and the reason it could not
   stay undone is not convenience: the HostID is the host's identity.
   A new one is a new computer as far as the phone is concerned, so a
   reboot without a stored record means tapping Trust again -- and,
   worse, it means two 2048-bit keygens again before the question can
   even be asked.

   ── WHAT HAS TO BE KEPT, AND WHAT MUST NOT BE LOST ──────────────────

   The record the device remembers is the three certificates and the
   two UUIDs. But a certificate is useless without the key it
   certifies: the TLS session afterwards proves we hold the HOST
   private key by signing the handshake with it. So the private keys
   are part of the stored thing, and a store that saved the record and
   not the keys would look complete, reload cleanly, and fail at
   CertificateVerify with nothing on screen to connect the two.

   That is why this is one struct and one file rather than a record
   plus "the keys somewhere".

   ── IT IS A PRIVATE KEY ON A FAT VOLUME ─────────────────────────────

   Unencrypted, because there is nothing to encrypt it WITH: this
   kernel has no keyring, no user secret and no secure element, and a
   key encrypted under a constant in the kernel image is plaintext
   with extra steps. What that key can do is bounded and worth stating
   plainly: it pairs with phones that have already been paired from
   this machine. Anyone who can read the HavenDOS partition can
   already read the kernel, replace the loader and own the machine
   outright. `pair forget` deletes it.

   ── WHY A TEXT FORMAT ───────────────────────────────────────────────

   Because the first thing anybody debugging a failed pairing will
   want is the certificate, and base64 DER on a line of its own can be
   wrapped in PEM delimiters and handed to `openssl x509` with no tool
   in between. A packed binary struct would also have made the
   serialiser's bugs invisible to a reader.

   The format is strict about everything except that: fields in order,
   each exactly once, no unknown fields, and a CRC-32 over everything
   above it. A record half-written by a power cut is refused rather
   than loaded as a key that is almost right.
   ════════════════════════════════════════════════════════════════════ */

#define PAIR_E_FORMAT   (-7)   /* the stored record is not one       */
#define PAIR_E_CRC      (-8)   /* it is damaged                      */
#define PAIR_E_VERSION  (-9)   /* a later HavenDOS wrote it          */

/* Where it lives. A flat name, because that is what fs/vfs.c takes. */
#define PAIR_STORE_FILE  ".pairrec"

/* What pair_store_encode() needs at most: three certificates and two
   2048-bit keys, base64, plus the field names. Measured by
   tests/test_pair.c rather than guessed -- see pair_store_size(). */
#define PAIR_STORE_MAX   10240

typedef struct {
    pair_record_t rec;
    pair_key_t    root;
    pair_key_t    host;
} pair_store_t;

/* Text out. NUL terminated, and *len is the length without it. */
int pair_store_encode(const pair_store_t *st, char *out, uint32_t cap,
                      uint32_t *len);

/* Text in. Fills *st completely or touches nothing worth keeping;
   a partial parse is a refusal, never a half-loaded key. */
int pair_store_decode(const char *in, uint32_t len, pair_store_t *st);

/* ── the signature the TLS session needs ─────────────────────────────
   net/tls.c asks for a PKCS#1 v1.5 signature over the handshake
   transcript and knows nothing about RSA; this is the callback that
   answers, with a pair_key_t as its context. It lives here because
   the key does.

   Matches tls_sign_fn in include/tls.h. The prototype is written out
   rather than taken from that header so that usb/pair.c does not have
   to include the TLS layer -- and tests/test_pair.c can call it
   directly. */
int pair_tls_sign(void *ctx, const uint8_t digest[32],
                  uint8_t *sig, uint32_t sigcap, uint32_t *siglen);
const char *pair_status_name(int status);
const char *pair_strerror(int code);
