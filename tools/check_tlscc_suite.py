#!/usr/bin/env python3
"""check_tlscc_suite.py -- prove tests/test_tlscc.c can fail.

net/tls.c grew a client-certificate path in v0.8.9 because lockdownd
will not open a session without one. The thing being produced is a
CertificateVerify: a signature over every handshake byte so far, in
order. There is exactly one way to be wrong about which bytes those
are, and it produces a message that is structurally perfect and
verifies nowhere.

tests/test_tlscc.c hands the handshake to `openssl s_server -Verify`,
which is the same decision the phone makes. This harness is what
decides whether that suite would notice if the handshake were wrong.

── THE FINDING THIS HARNESS'S OWN SUITE MADE FIRST ───────────────────

Worth recording here because it is the same class of problem one level
up. The suite's negative control -- "the same chain against an
UNRELATED CA must be refused" -- failed on its first run: openssl
ACCEPTED it. `s_server -Verify` installs a verify callback that prints
"verify error" and then returns 1 anyway, so the handshake completes
and the -www page comes back naming a certificate openssl had just
failed to verify. `-verify_return_error` is what makes the refusal
real.

So for one run, the suite's main case -- the whole reason the file
exists -- was passing against a judge that was not judging. The
negative control found it. That is what negative controls are for, and
it is why every crypto suite in this tree has one.

Run:  python3 tools/check_tlscc_suite.py

NEVER in a background process. This patches the tree and restores it
in a `finally`; a container that kills it mid-run leaves an injected
defect in the source looking like real code.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/tls.c"
TEST = "tests/test_tlscc.c"
CC = ["gcc", "-std=gnu99", "-g", "-fsanitize=address,undefined",
      "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "net/tls_crypto.c"]

BUGS = [
    # ── THE TRANSCRIPT, which is the whole of a CertificateVerify ─────
    ("CertificateVerify is sent BEFORE ClientKeyExchange",
     "RFC 5246 7.3's flight order, and it is not cosmetic: the "
     "signature covers every handshake byte SO FAR, so moving it one "
     "message earlier signs a transcript that is missing the "
     "ClientKeyExchange. Every byte of the message is still correct "
     "and the peer's verification fails -- which looks like a broken "
     "key and is a broken order",
     """    /* → ClientKeyExchange */
    if(send_client_key_exchange(fd)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_KEYEX;""",
     """    if(got_certreq && g_cc && g_cc->n > 0){
        if(send_cert_verify(fd)<0) return TLS_ERR_CLIENTCERT;
    }
    /* → ClientKeyExchange */
    if(send_client_key_exchange(fd)<0) return TLS_ERR_HANDSHAKE;
    g_tls_last_stage=TLS_STAGE_KEYEX;"""),

    ("the Certificate is sent AFTER the ClientKeyExchange",
     "same flight, the other message. The peer is reading a "
     "ClientKeyExchange where a Certificate is required",
     """    if(got_certreq){
        if(send_client_cert(fd)<0) return TLS_ERR_CLIENTCERT;
        g_tls_last_stage=TLS_STAGE_CLIENT_CERT;
    }""",
     """    ;"""),

    ("the running transcript hash is finalised instead of copied",
     "sha256_final() consumes the context. Signing with the live one "
     "leaves every later hs_update() appending to a finished hash, so "
     "the Finished message is computed over nothing -- and the symptom "
     "appears two messages later, at the Finished, not here",
     """    memcpy(ctx_copy, g_sess.hs_ctx, SHA256_CTX_BYTES);
    sha256_final(ctx_copy, hash);""",
     """    sha256_final(g_sess.hs_ctx, hash);"""),

    # ── THE FRAMING ───────────────────────────────────────────────────
    ("the certificate list length counts itself",
     "two nested 24-bit lengths three bytes apart. Three too many, and "
     "the peer reads a fourth certificate out of whatever follows",
     """    put_u24(body, pos - 3);""",
     """    put_u24(body, pos);"""),

    ("each certificate's own length is omitted",
     "the list is a sequence of length-prefixed certificates, not a "
     "concatenation. Without the prefix the peer reads the first three "
     "bytes of DER as a length",
     """        put_u24(body + pos, clen); pos += 3;
        memcpy(body + pos, g_cc->cert[i], clen); pos += clen;""",
     """        memcpy(body + pos, g_cc->cert[i], clen); pos += clen;"""),

    ("the chain goes out root first",
     "RFC 5246 7.4.2: the sender's own certificate FIRST, each "
     "certifying the one before. Reversed, the peer's first "
     "certificate is a CA it was not asked about",
     """    for(i = 0; i < n; i++){
        uint32_t clen = g_cc->cert_len[i];""",
     """    for(i = n - 1; i >= 0; i--){
        uint32_t clen = g_cc->cert_len[i];"""),

    ("the signature is written at the wrong offset in the body",
     "two bytes of algorithm and two of length come first. Writing the "
     "signature two bytes early overwrites the length field with the "
     "signature's own first bytes",
     """    if(g_cc->sign(g_cc->sign_ctx, hash, body + 4,
                  (uint32_t)sizeof body - 4, &siglen) < 0) return -1;""",
     """    if(g_cc->sign(g_cc->sign_ctx, hash, body + 2,
                  (uint32_t)sizeof body - 4, &siglen) < 0) return -1;"""),

    ("the signature's length field is one too long",
     "the peer reads one byte of padding into the signature",
     """    put_u16(body + 2, (uint16_t)siglen);""",
     """    put_u16(body + 2, (uint16_t)(siglen + 1));"""),

    ("CertificateVerify claims SHA-1",
     "HashAlgorithm 2 is sha1 and 4 is sha256. The signature IS over a "
     "SHA-256 digest, so this is a message that describes itself "
     "wrongly -- and a peer that believes the description computes a "
     "different digest and refuses",
     """    body[0] = 4;                       /* HashAlgorithm.sha256       */""",
     """    body[0] = 2;                       /* HashAlgorithm.sha256       */"""),

    ("CertificateVerify claims DSA",
     "SignatureAlgorithm 1 is rsa. 2 is dsa, and the bytes are an RSA "
     "signature either way",
     """    body[1] = 1;                       /* SignatureAlgorithm.rsa     */""",
     """    body[1] = 2;                       /* SignatureAlgorithm.rsa     */"""),

    ("the CertificateVerify is sent as a Certificate",
     "handshake type 15, not 11. The peer reads a Certificate message "
     "whose first byte is a hash algorithm",
     """    return send_hs(fd, HS_CERT_VERIFY, body, 4 + siglen);""",
     """    return send_hs(fd, HS_CERTIFICATE, body, 4 + siglen);"""),

    # ── signature_algorithms, which RFC 5246 makes load-bearing ───────
    ("the signature_algorithms extension is dropped",
     "RFC 5246 7.4.1.4.1: a TLS 1.2 client that omits it is TAKEN to "
     "have offered {sha1, rsa}, and SHA-256 is the only thing this "
     "stack can sign with. It was omitted before v0.8.9, which cost "
     "nothing only because no CertificateVerify was ever sent",
     """    put_u16(hello+pos,0x000D); pos+=2;   /* signature_algorithms */
    put_u16(hello+pos,4);      pos+=2;   /* ext data length       */
    put_u16(hello+pos,2);      pos+=2;   /* list length, in bytes */
    hello[pos++]=4;                      /* HashAlgorithm.sha256  */
    hello[pos++]=1;                      /* SignatureAlgorithm.rsa */""",
     """    ;"""),

    ("signature_algorithms offers sha1 instead of sha256",
     "offering an algorithm this stack cannot produce",
     """    hello[pos++]=4;                      /* HashAlgorithm.sha256  */
    hello[pos++]=1;                      /* SignatureAlgorithm.rsa */""",
     """    hello[pos++]=2;                      /* HashAlgorithm.sha256  */
    hello[pos++]=1;                      /* SignatureAlgorithm.rsa */"""),

    ("an SNI extension is sent with an empty name",
     "RFC 6066 requires a non-empty HostName. The usbmux connection "
     "has no name at all, so the extension must be ABSENT rather than "
     "present and empty",
     """    if(hlen){
        /* SNI extension (type 0x0000) */""",
     """    if(1){
        /* SNI extension (type 0x0000) */"""),

    # ── THE REQUEST ───────────────────────────────────────────────────
    ("a CertificateRequest is ignored, as it was before v0.8.9",
     "RFC 5246 7.4.6 requires a Certificate message in reply, empty if "
     "there is nothing to send. A client that sends none leaves a "
     "server that asked waiting -- and lockdownd always asks",
     """                if(got_certreq) *got_certreq=1;
                g_tls_cert_requested=1;""",
     """                if(got_certreq) *got_certreq=0;
                g_tls_cert_requested=0;"""),

    ("a CertificateVerify is sent with an empty chain",
     "there is no key to prove possession of, and a signature from a "
     "key the peer has no certificate for is worse than silence",
     """    if(got_certreq && g_cc && g_cc->n > 0){""",
     """    if(got_certreq){"""),

    # ── THE TRANSPORT ─────────────────────────────────────────────────
    ("records are sent through net/tcp.c whatever transport was given",
     "the indirection exists so pairing can run TLS over a usbmux "
     "channel. A session reading one channel and writing another is a "
     "failure both sides report as success",
     """    if(g_tr->send(fd,hdr,5)<0) return -1;
    if(len && g_tr->send(fd,data,(uint16_t)len)<0) return -1;""",
     """    if(tcp_send(fd,hdr,5)<0) return -1;
    if(len && tcp_send(fd,data,(uint16_t)len)<0) return -1;"""),

    ("reads come from net/tcp.c whatever transport was given",
     "the other direction of the same defect",
     """        int r=g_tr->recv(fd,dst+total,(uint16_t)(want-total),500);""",
     """        int r=tcp_recv(fd,dst+total,(uint16_t)(want-total),500);"""),

    ("the transport is remembered globally rather than per session",
     "two sessions on two transports is exactly the case pairing "
     "creates: an HTTPS fetch and a lockdown session open at once",
     """    g_tr=tr; g_cc=cc;""",
     """    g_tr=&tls_transport_tcp; g_cc=cc;"""),

    # ── REFUSING RATHER THAN TRUNCATING ───────────────────────────────
    ("an oversize chain is truncated to fit",
     "a shortened chain is refused by the peer for a reason nowhere "
     "near the real one. net/b64.c's header makes the same argument "
     "about a decoder that silently shortens a key",
     """        if(pos + 3 + clen > sizeof body) return -1;""",
     """        if(pos + 3 + clen > sizeof body) clen = (uint32_t)sizeof body - pos - 3;"""),

    ("more certificates than the chain holds are read anyway",
     "cert[] has TLS_MAX_CHAIN entries and n comes from the caller",
     """    if(n < 0 || n > TLS_MAX_CHAIN) return -1;""",
     """    if(n < 0) return -1;"""),

    ("a signature longer than the body buffer is believed",
     "the callback reports how much it wrote and the buffer is this "
     "side's",
     """    if(siglen == 0 || siglen > (uint32_t)sizeof body - 4) return -1;""",
     """    ;"""),

    ("a chain with no signer is sent bare",
     "the certificates go out, no CertificateVerify follows, and the "
     "peer refuses a client that proved nothing",
     """    if(!g_cc || !g_cc->sign) return -1;""",
     """    if(!g_cc) return -1;"""),

    ("a transport with no send function is accepted",
     "the first record dereferences a null pointer",
     """    if(!tr || !tr->send || !tr->recv || !tr->eof) return TLS_ERR_HANDSHAKE;""",
     """    if(!tr) return TLS_ERR_HANDSHAKE;"""),

    # ── THE DIAGNOSTICS, which are what a hardware test reads ─────────
    ("the certificate counter is set before anything is sent",
     "`tlsinfo` and `usb lockdown` report it on a machine nobody can "
     "attach a debugger to. A count that is right when the send failed "
     "sends the next person looking at the signature",
     """    {
        int r = send_hs(fd, HS_CERTIFICATE, body, pos);
        if(r < 0) return r;
        g_tls_cert_sent = n;
        return r;
    }""",
     """    g_tls_cert_sent = n;
    return send_hs(fd, HS_CERTIFICATE, body, pos);"""),

    ("the diagnostics are not cleared between handshakes",
     "a failed pairing after a successful one would report the "
     "previous session's certificate count",
     """    g_tls_cert_requested=0; g_tls_cert_sent=0; g_tls_cert_verified=0;""",
     """    ;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """const char *tls_stage_name(int stage){""",
    """/* the one name table */
const char *tls_stage_name(int stage){""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "tlscc", SRC, TEST, CC, BUGS, HARMLESS, timeout=600,
        preamble="Proving tests/test_tlscc.c can fail.\n"
                 "Every verdict in that suite is openssl's, through "
                 "`s_server -Verify`, which is\nthe same check the "
                 "phone makes. Under ASAN.",
        checks=(["python3", "tools/check_tls_stages.py"],)))
