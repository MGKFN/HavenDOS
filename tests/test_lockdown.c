/* ════════════════════════════════════════════════════════════════════
   test_lockdown.c -- stage 1: does lockdownd answer QueryType.

   ── WHAT THIS SUITE RUNS AGAINST ─────────────────────────────────────

   Everything below the file under test is REAL: usb/core.c, the
   descriptor walk, usb/usbmux.c's version exchange and its TCP
   connection. tests/muxdev.inc supplies the phone -- the same one
   tests/test_usbmux.c uses, in one file, because two copies of a
   device model drift apart.

   On top of that this file adds lockdownd: a service on port 62078
   that speaks length-prefixed XML plists the way libimobiledevice's
   common/property_list_service.c frames them.

   ── AND THE HONESTY THIS FILE OWES, WHICH IS THE SAME AS LAST TIME ───

   tests/test_usbmux.c passed 79 of 79 against a sixteen-byte mux
   header the phone has never used, because the simulated device and
   the driver were written by the same person on the same afternoon
   from the same guess. A suite can disagree with the code. It cannot
   disagree with the world.

   What is different here is where the framing came from. It was read
   out of libimobiledevice's source, not reasoned about:

       internal_plist_send()     nlen = htobe32(length);
                                 service_send(&nlen, 4);
                                 service_send(content, length);

   Two facts fall out of that, and the fixture below carries BOTH
   deliberately, because they are the two most likely ways this fails
   on hardware:

     1. THE LENGTH DOES NOT COUNT ITSELF. The mux header four bytes
        below it in the same byte stream does. A driver that got this
        wrong by four leaves lockdownd waiting for a request that never
        finishes, and the symptom is a device that does not answer --
        which is the one symptom this release exists to tell apart from
        the others.

     2. IT ARRIVES IN TWO WRITES, so the four length bytes and the XML
        are separate TCP packets. The fixture SENDS THEM SEPARATELY,
        because a reader written for one packet reads four bytes,
        concludes the reply is empty, and passes every check a
        single-packet fixture could make.

   What a boot on the HP can still refute: whether lockdownd accepts
   the request as one packet rather than two, and whether it answers at
   all without a trust record. Both are printed raw before this kernel
   interprets anything -- `usb lockdown`'s last lines exist for that.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

#include "../include/types.h"
#include "../include/usb.h"
#include "../include/klog.h"
#include "../include/tls.h"

static uint32_t g_ms;
uint32_t klog_now_ms(void){ return g_ms; }
static int g_logs;
/* ── WARNINGS ARE COUNTED SEPARATELY, v0.8.7.8 ────────────────────────
   g_logs counts every line, so "it said something" cannot distinguish
   a healthy boot's running commentary from a complaint. A SPURIOUS
   WARNING is a real defect: it buries the one line that matters under
   thousands that do not, and the magic check that v0.8.7.8 turned from
   a refusal into a report would otherwise warn on every packet of a
   working connection with nothing to notice it. */
static int g_warns;
void klog(int level, const char *fmt, ...){
    (void)fmt; g_logs++; if (level == KLOG_WARN) g_warns++; }

/* The phone, its controller and its mux. MUXDEV_SERVICE says this
   suite models something listening on the port. */
#define MUXDEV_SERVICE 1
#include "muxdev.inc"

/* ── lockdownd ────────────────────────────────────────────────────────

   A service that reads a four-byte big-endian length and then that
   many bytes, and answers with a plist framed the same way. It is as
   STRICT as the real one can be shown to be:

     - a length prefix that counts itself is four too many, so the
       request never completes and it answers nothing. That is what
       lockdownd does, and it is why the symptom of getting this wrong
       is silence rather than an error.
     - a request it cannot find a Request key in gets an Error reply,
       not a refusal: lockdownd answers a bad request with a plist.

   It keeps its own accumulation buffer, because the host's request
   may arrive in several packets too and a service that assumed one
   would be a service this driver cannot be tested against. */
static struct {
    /* ── v0.8.9.0: ROOM FOR A REQUEST THAT DOES NOT FIT A PACKET ─────
       4096 and 2048 were enough while every request lockdown sent was
       a few hundred bytes. Stage 3's Pair request runs past five
       kilobytes and arrives in seven packets, and the question it
       asks of this fixture is the one only a byte-for-byte comparison
       answers: did lockdownd see ONE request, whole and in order, or
       several with a segment missing. Both buffers are sized for
       LD_REQ (12288) with room over. */
    uint8_t  in[20480];
    int      in_len;
    int      too_big;           /* it gave up reassembling            */
    int      want;              /* 4 + the declared length, -1 unknown  */
    int      requests;          /* complete requests it has read        */
    int      replies;

    /* knobs */
    int      split_reply;       /* send length and body separately      */
    int      one_packet;        /* ...or as one, to show the difference */
    int      answer_type;       /* 0 lockdown, 1 something else, 2 none */
    int      answer_error;      /* answer with an Error key             */
    int      prefix_counts_self;/* the bug, from the device's side      */
    int      body_short;        /* declare more than it sends           */
    int      silent;            /* read the request, answer nothing     */
    int      huge_reply;        /* declare a reply far too big to hold  */
    int      chunk;             /* bytes per body packet, 0 = all       */
    int      stub_reply;        /* answer with two bytes and stop       */
    int      rst_after_prefix;  /* send the prefix, then reset          */
    int      typeish_reply;     /* a key whose NAME contains "Type"     */
    int      getvalue;          /* 1 <data>, 2 <string>, 3 bad base64   */

    /* ── v0.8.9: the session ─────────────────────────────────────────
       `session` picks which StartSession reply comes back. `ssl` is
       the device's own idea of whether the conversation is encrypted,
       and it is what makes the routing testable: see the note above
       ld_xor(). */
    int      session;           /* 1 ok+SSL, 2 ok no SSL, 3 no flag,
                                   4 no SessionID, 5 InvalidHostID,
                                   6 a boolean that is a <string>     */
    int      ssl;               /* the device is encrypting            */

    /* ── v0.8.9.2: THE DEVICE SENDS A PACKET TWICE ───────────────────
       Nothing in this fixture had ever duplicated a packet, and that
       is the whole reason 5883 checks and 65 injections missed a
       transport that delivered duplicates as new data. A
       retransmission is not an exotic event -- it is what TCP does
       whenever an ACK is late -- and on Kyan's HP it put every
       lockdown reply one request behind.

       dup_every retransmits every data packet; dup_once does it for
       the next one only. Same sequence number, same bytes, queued
       immediately behind the original, which is what a retransmission
       looks like from this side. */
    int      dup_every, dup_once, dups_sent;

    /* v0.8.9.3: the length prefix and then SILENCE. Not an RST -- the
       device is simply still working on the body. The reply is going
       to arrive, which is what makes abandoning it dangerous. */
    int      stall_after_prefix;
    int      cleartext_seen;    /* a request arrived in the clear      */
    int      stop_session;      /* the last request was StopSession    */
    /* Empty polls before each packet. A reply that arrives instantly
       cannot show whether the poll budget is reset by progress -- the
       budget is never near spent. With this, it is. */
    int      hold, held;

    int      bad_requests;      /* requests it could not parse          */

    char     last_request[16384];
    int      last_request_len;
} ld;

static void ld_reset(void)
{
    memset(&ld, 0, sizeof ld);
    ld.want        = -1;
    ld.split_reply = 1;         /* what libimobiledevice does */
}

/* The reply bodies, written out rather than built, so the strings this
   suite checks for are literally in the file. */
static const char REPLY_LOCKDOWN[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Request</key>\n\t<string>QueryType</string>\n"
    "\t<key>Type</key>\n\t<string>com.apple.mobile.lockdown</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_OTHER[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Type</key>\n\t<string>com.apple.mobile.something</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_NOTYPE[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Request</key>\n\t<string>QueryType</string>\n"
    "</dict>\n</plist>\n";
/* ── FOR THE SCANNER, and chosen rather than transcribed ──────────────
   A key whose NAME CONTAINS "Type" and comes BEFORE the Type key.

   REPLY_LOCKDOWN already puts Request/QueryType ahead of Type, and an
   unanchored search for "Type" lands inside "QueryType" -- from where
   the next <string> happens to be the right one. So that reply cannot
   show the anchoring works; it passes either way, by luck. The harness
   said so.

   This one cannot. An unanchored search finds "ProtocolType", and the
   next <string> after it is "2". The shape is picked to test the
   property, which is stated here rather than left to look like a
   transcript of something a phone sent. */
static const char REPLY_TYPEISH[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>ProtocolType</key>\n\t<string>2</string>\n"
    "\t<key>Type</key>\n\t<string>com.apple.mobile.lockdown</string>\n"
    "</dict>\n</plist>\n";
/* ── STAGE 2's REPLY, and the wrapping is the point ───────────────────
   GetValue returns the value as <data>, not <string>: base64, wrapped
   across lines with a TAB on each. That wrapping is what separates a
   decoder that works from one that returns a sixth of the key --
   shell/shell_v073.c's b64_decode_buf() walks a NUL-terminated string
   and stops at the first character outside the alphabet, which is the
   newline at the end of line one.

   The base64 below is real: it decodes to the 174-byte PEM in
   PEM_EXPECTED, and the suite checks that it does rather than trusting
   either side's idea of what base64 is. */
static const char REPLY_GETVALUE[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Key</key>\n\t<string>DevicePublicKey</string>\n"
    "\t<key>Request</key>\n\t<string>GetValue</string>\n"
    "\t<key>Value</key>\n\t<data>\n"
    "\tLS0tLS1CRUdJTiBSU0EgUFVCTElDIEtFWS0tLS0tCk1JSUJDZ0tDQVFFQXhH\n"
    "\tVm5RM1o2Y0VGaFFtTkVaVVpuU0dsS2EweHRUbTlRY1ZKelZIVldkMWg1V2pB\n"
    "\teE1qTTAKTlRZM09EbGhZbU5rWldabmFHbHFhMnh0Ym05d2NYSnpkSFYyZDNo\n"
    "\tNWVnSURBUUFCCi0tLS0tRU5EIFJTQSBQVUJMSUMgS0VZLS0tLS0K\n"
    "\t</data>\n"
    "</dict>\n</plist>\n";

static const char PEM_EXPECTED[] =
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIIBCgKCAQEAxGVnQ3Z6cEFhQmNEZUZnSGlKa0xtTm9QcVJzVHVWd1h5WjAxMjM0\n"
    "NTY3ODlhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5egIDAQAB\n"
    "-----END RSA PUBLIC KEY-----\n";

/* ── CHOSEN SHAPES, not transcripts ───────────────────────────────────
   Three replies picked to make a property testable, each labelled so
   nobody reads them as something a phone sent.

   TWODATA: a <data> value BEFORE the one being asked for. REPLY_GETVALUE
   has exactly one <data> in it, so an unanchored search finds the right
   one by luck and the anchoring cannot be shown -- the harness said so.
   EscrowBag is a real lockdownd value and it is binary, so a reply
   carrying both is plausible as well as useful.

   SHORT: a value far shorter than the one before it. Without a
   terminator after the decoded bytes, the tail of the PREVIOUS value
   is still in the buffer and lockdown_value() runs straight into it.

   ERRVAL: an Error AND a Value in one reply. lockdownd does not send
   this, and that is the point -- the rule being pinned is a precedence
   one. If the device says no, no key is reported, whatever else is in
   the document. Stage 3 would sign something with that key. */
static const char REPLY_GETVALUE_TWODATA[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>EscrowBag</key>\n\t<data>\n\tWlhOamNtOTNZbUZu\n\t</data>\n"
    "\t<key>Value</key>\n\t<data>\n"
    "\tLS0tLS1CRUdJTiBSU0EgUFVCTElDIEtFWS0tLS0tCk1JSUJDZ0tDQVFFQXhH\n"
    "\tVm5RM1o2Y0VGaFFtTkVaVVpuU0dsS2EweHRUbTlRY1ZKelZIVldkMWg1V2pB\n"
    "\teE1qTTAKTlRZM09EbGhZbU5rWldabmFHbHFhMnh0Ym05d2NYSnpkSFYyZDNo\n"
    "\tNWVnSURBUUFCCi0tLS0tRU5EIFJTQSBQVUJMSUMgS0VZLS0tLS0K\n"
    "\t</data>\n"
    "</dict>\n</plist>\n";

static const char REPLY_GETVALUE_SHORT[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Value</key>\n\t<data>\n\tYWJj\n\t</data>\n"
    "</dict>\n</plist>\n";

static const char REPLY_GETVALUE_ERRVAL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Error</key>\n\t<string>InvalidRequest</string>\n"
    "\t<key>Value</key>\n\t<data>\n\tYWJj\n\t</data>\n"
    "</dict>\n</plist>\n";

/* A value that comes back as a <string>, which some keys do --
   ProductVersion, for one. DevicePublicKey does not, and a driver
   that only handled one shape would be wrong for half the keys. */
static const char REPLY_GETVALUE_STR[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Key</key>\n\t<string>ProductVersion</string>\n"
    "\t<key>Value</key>\n\t<string>17.5.1</string>\n"
    "</dict>\n</plist>\n";

/* <data> that is not valid base64 -- a reply whose value region is not
   what this driver thinks it is. */
static const char REPLY_GETVALUE_BAD[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Value</key>\n\t<data>\n"
    "\tnot base64 at all !!!! ####\n"
    "\t</data>\n"
    "</dict>\n</plist>\n";

/* ── v0.8.9: StartSession ─────────────────────────────────────────────
   The shapes a real lockdownd answers with, written out so the strings
   this suite looks for are literally in the file. */
static const char REPLY_SESSION[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>EnableSessionSSL</key>\n\t<true/>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "\t<key>SessionID</key>\n"
    "\t<string>0BD1C4E7-6A9F-4A1D-9C3E-5A1B2C3D4E5F</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_SESSION_NOSSL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>EnableSessionSSL</key>\n\t<false/>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "\t<key>SessionID</key>\n\t<string>PLAINTEXT-SESSION</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_SESSION_NOFLAG[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "\t<key>SessionID</key>\n\t<string>NO-FLAG-AT-ALL</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_SESSION_NOID[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>EnableSessionSSL</key>\n\t<true/>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "</dict>\n</plist>\n";
static const char REPLY_INVALID_HOSTID[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Error</key>\n\t<string>InvalidHostID</string>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "</dict>\n</plist>\n";
/* The reply ld_key_bool() has to get right and a looser scanner
   would not: the key it is asked about carries a <string>, and the
   NEXT key carries the <true/>. A scanner that skipped forward to the
   first boolean after the key name would report SSL requested when
   the device said something else entirely. */
static const char REPLY_SESSION_BOOLISH[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>EnableSessionSSL</key>\n\t<string>maybe</string>\n"
    "\t<key>SomethingElse</key>\n\t<true/>\n"
    "\t<key>Request</key>\n\t<string>StartSession</string>\n"
    "\t<key>SessionID</key>\n\t<string>BOOLISH</string>\n"
    "</dict>\n</plist>\n";

static const char REPLY_ERROR[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<plist version=\"1.0\">\n<dict>\n"
    "\t<key>Error</key>\n\t<string>InvalidRequest</string>\n"
    "</dict>\n</plist>\n";

/* Hand one TCP packet of the reply to the mux, and move the device's
   sequence on by what it carried. queue_tcp() holds one packet, so a
   split reply is delivered a packet per poll -- which is exactly how
   it arrives from a real device and the reason lockdown.c accumulates. */
static void ld_emit(const void *p, int n)
{
    queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, p, n);
    if (ld.dup_every || ld.dup_once) {
        ld.dup_once = 0;
        ld.dups_sent++;
        /* THE SAME SEQUENCE NUMBER. That is what makes it a
           retransmission rather than more data -- and it is the only
           thing a receiver can use to tell them apart. */
        queue_tcp2(F_ACK, ph.dev_seq, ph.exp_host_seq, p, n);
    }
    ph.dev_seq += (uint32_t)n;
}

/* ── DELIVERY LIVES IN ONE PLACE ──────────────────────────────────────
   What is still to be sent of the current reply, drained a packet at a
   time as the host polls. queue_tcp() holds one packet, so this is
   what makes a reply arrive the way a real one does.

   ld_answer() used to emit the length prefix itself, immediately, and
   leave only the body to this. That made the delay knob useless: there
   was effectively ONE wait rather than two, so a poll budget spent
   across the whole exchange and a budget reset by progress could not
   be told apart, and the harness reported the reset as unreachable.

   Every packet goes out from here now, the first one included. */
static struct { uint8_t buf[4096]; int len, at; } pend;

static void ld_pump(void)
{
    int n;
    if (pend.at >= pend.len) return;
    if (ph.in_len) return;              /* a packet is already waiting */
    /* A device that takes its time. */
    if (ld.hold && ++ld.held < ld.hold) return;
    ld.held = 0;

    /* The connection dies after the length prefix. Four bytes have
       arrived and are worth keeping: they are what `usb lockdown`
       would print, and the only evidence of how far it got. */
    if (ld.stall_after_prefix && pend.at == 4) return;
    if (ld.rst_after_prefix && pend.at == 4) {
        queue_tcp(F_RST, ph.dev_seq, ph.exp_host_seq, 0, 0);
        pend.at = pend.len;
        return;
    }
    if (ld.stub_reply) {
        /* TWO BYTES. Not even a length prefix, which is the one case
           where the reader never learns how much to expect. */
        ld_emit(pend.buf, 2);
        pend.at = pend.len;
        return;
    }
    if (ld.one_packet) {
        /* NOT what libimobiledevice does. Here to show that the
           accumulation is what makes the split case work rather than
           something that happens to pass either way. */
        ld_emit(pend.buf, pend.len);
        pend.at = pend.len;
        return;
    }
    if (pend.at == 0) {
        /* THE FOUR LENGTH BYTES ALONE, as their own packet, because
           property_list_service.c writes them separately. A reader
           written for one packet gets these four and nothing else. */
        ld_emit(pend.buf, 4);
        pend.at = 4;
        return;
    }
    n = pend.len - pend.at;
    if (ld.chunk && n > ld.chunk) n = ld.chunk;
    ld_emit(pend.buf + pend.at, n);
    pend.at += n;
}

/* Both directions of the fixture's cipher, in one place. */
static void ld_xor(uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++) p[i] ^= 0x5A;
}

static void ld_answer(const char *body)
{
    int n = (int)strlen(body);
    uint32_t declared = (uint32_t)n;

    if (ld.prefix_counts_self) declared += 4;   /* the classic error */
    if (ld.huge_reply)         declared = 100000u;
    if (ld.body_short)         n -= 10;

    memset(&pend, 0, sizeof pend);
    wr32(pend.buf, declared);
    memcpy(pend.buf + 4, body, (size_t)n);
    pend.len = 4 + n;
    /* The length prefix is INSIDE the encrypted stream, not beside
       it -- so it is encrypted too. A fixture that left the prefix in
       the clear would let a driver reading plaintext still find the
       length and look half-right. */
    if (ld.ssl) ld_xor(pend.buf, pend.len);
    ld.replies++;
    /* Nothing is emitted here. ld_pump() delivers all of it. */
}

/* Length-bounded search, used by the service and by the checks. It is
   written here rather than taken from lockdown.c's ld_find(): a suite
   that checks a scanner with the scanner cannot disagree with it. */
static int contains_(const char *hay, int haylen, const char *needle)
{
    int n = (int)strlen(needle), i, j;
    if (n == 0 || n > haylen) return 0;
    for (i = 0; i + n <= haylen; i++) {
        for (j = 0; j < n; j++) if (hay[i+j] != needle[j]) break;
        if (j == n) return 1;
    }
    return 0;
}

static void dev_service(const uint8_t *data, int len)
{
    /* Counted, not silent: a fixture that quietly stops reassembling
       makes a correct driver look like one that sent nothing. */
    if (ld.in_len + len > (int)sizeof ld.in) { ld.too_big++; return; }
    memcpy(ld.in + ld.in_len, data, (size_t)len);
    if (ld.ssl) ld_xor(ld.in + ld.in_len, len);
    ld.in_len += len;

    if (ld.want < 0 && ld.in_len >= 4)
        ld.want = 4 + (int)rd32(ld.in);
    if (ld.want < 0 || ld.in_len < ld.want) return;

    /* A complete request. */
    ld.requests++;
    ld.last_request_len = ld.want - 4;
    if (ld.last_request_len > 0 &&
        ld.last_request_len < (int)sizeof ld.last_request)
        memcpy(ld.last_request, ld.in + 4, (size_t)ld.last_request_len);
    ld.in_len = 0;
    ld.want   = -1;

    /* ── IT READS THE REQUEST, it does not just count it ─────────────
       libplist parses the body. A request whose declared length is
       four short arrives as a document with its closing tag cut off,
       and a service that only counted bytes would accept it and
       answer -- which is what this fixture did, and what let the
       harness's "the length prefix is four too few" injection through.

       The minimum a parser would insist on: it begins with an XML
       declaration and it ends with a closing plist tag. Anything else
       gets an Error, which is what lockdownd answers a request it
       cannot act on. */
    if (ld.last_request_len < 12 ||
        !contains_(ld.last_request, ld.last_request_len, "<?xml") ||
        !contains_(ld.last_request, ld.last_request_len, "</plist>")) {
        ld.bad_requests++;
        if (!ld.silent) ld_answer(REPLY_ERROR);
        return;
    }

    if (ld.silent) return;

    /* ── v0.8.9 ──────────────────────────────────────────────────────
       The request is READ to decide which reply to send, so a
       StopSession that arrives where a StartSession was expected is
       visible rather than answered identically. */
    if (contains_(ld.last_request, ld.last_request_len, "StopSession")) {
        ld.stop_session++;
        ld_answer(REPLY_LOCKDOWN);
        return;
    }
    if (ld.session && contains_(ld.last_request, ld.last_request_len,
                                "StartSession")) {
        if (ld.session == 1) ld_answer(REPLY_SESSION);
        else if (ld.session == 2) ld_answer(REPLY_SESSION_NOSSL);
        else if (ld.session == 3) ld_answer(REPLY_SESSION_NOFLAG);
        else if (ld.session == 4) ld_answer(REPLY_SESSION_NOID);
        else if (ld.session == 5) ld_answer(REPLY_INVALID_HOSTID);
        else                      ld_answer(REPLY_SESSION_BOOLISH);
        return;
    }

    /* ── v0.8.9.2: ANSWER THE QUESTION THAT WAS ASKED ────────────────
       The getvalue modes used to answer every request with the
       GetValue reply, whatever had been asked. That is fine for a
       suite that sends one request per connection -- and it makes an
       ALTERNATING conversation impossible to model, which is the only
       shape in which a reply arriving one request late can be seen at
       all. A fixture that answers every question identically cannot
       tell you the answers got swapped. */
    if (ld.getvalue && contains_(ld.last_request, ld.last_request_len,
                                 "<string>QueryType</string>")) {
        ld_answer(REPLY_LOCKDOWN);
        return;
    }
    if (ld.getvalue == 1)       { ld_answer(REPLY_GETVALUE); return; }
    if (ld.getvalue == 4)       { ld_answer(REPLY_GETVALUE_TWODATA); return; }
    if (ld.getvalue == 5)       { ld_answer(REPLY_GETVALUE_SHORT); return; }
    if (ld.getvalue == 6)       { ld_answer(REPLY_GETVALUE_ERRVAL); return; }
    if (ld.getvalue == 2)       { ld_answer(REPLY_GETVALUE_STR); return; }
    if (ld.getvalue == 3)       { ld_answer(REPLY_GETVALUE_BAD); return; }
    if (ld.answer_error)        ld_answer(REPLY_ERROR);
    else if (ld.answer_type == 1) ld_answer(REPLY_OTHER);
    else if (ld.answer_type == 2) ld_answer(REPLY_NOTYPE);
    else if (ld.typeish_reply)    ld_answer(REPLY_TYPEISH);
    else                          ld_answer(REPLY_LOCKDOWN);
}

/* The mux's poll has to drain a pending reply, or a split reply stops
   after its first packet. Wrapping the controller op is the honest
   place for it: the device is what has more to send. */
/* ── A CLOCK, because the budget is now TIME ─────────────────────────
   v0.8.9.3. ld_recv_plist() used to poll a fixed NUMBER of times, so
   a suite with no clock could test it: twenty thousand polls is
   twenty thousand polls wherever it runs. That was the bug -- on the
   HP those twenty thousand polls took under ten milliseconds, the
   GetValue timed out, and its reply arrived in time to be read as the
   answer to the next request.

   The budget is ticks now, so the fixture needs a clock, and the
   honest place to advance it is the poll: asking the device whether
   it has anything yet is what takes the time. One tick -- 10 ms --
   per poll, which makes ld.hold a number of ticks and makes a device
   that is too slow actually too slow. */
static uint32_t g_ticks;
uint32_t get_ticks(void) { return g_ticks; }

static int ld_intr_poll(struct usb_hc *h, const usb_dev_t *d, uint8_t ep,
                        int in, void *data, int len, int mps)
{
    g_ticks++;
    if (!ph.in_len) ld_pump();
    return s_intr_poll(h, d, ep, in, data, len, mps);
}

static const usb_hc_ops_t LD_OPS = {
    .name = "SIM", .port_count = s_port_count,
    .port_connected = s_port_connected, .attach = s_attach,
    .control = s_control, .bulk = s_bulk, .intr = s_bulk,
    .reset_toggle = s_reset_toggle, .intr_poll = ld_intr_poll,
    .configure = s_configure,
};

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

/* v0.8.8.4: usb/core.c prints WHERE an xHCI attach failed, not just
   the code, because "no answer (-4)" named four different bugs at
   once. This suite stubs the controller, so the stub says so rather
   than linking to an empty string. */
const char *xhci_attach_why(void){ return "(xHCI stubbed in this suite)"; }

#include "../usb/core.c"
#include "../usb/usbmux.c"
#include "../include/b64.h"
#include "../net/b64.c"
#include "../include/plist.h"
#include "../net/plist.c"

/* ════════════════════════════════════════════════════════════════════
   v0.8.9: A TLS THAT IS NOT TLS, AND WHY THAT IS THE RIGHT FIXTURE

   usb/lockdown.c does not implement TLS -- net/tls.c does, and
   tests/test_tlscc.c judges that against `openssl s_server`, which is
   the only honest judge for a handshake. What THIS file has to decide
   is different and it is not about cryptography:

     does the plist framing go down the ENCRYPTED path once the
     session is encrypted, and the plaintext path before it

   A fake TLS that merely counted calls could not answer that: a
   request that took the wrong branch would still arrive, still parse,
   and still be answered. So the fake XORs every byte with 0x5A, and
   THE DEVICE MODEL XORS TOO. A request that goes out in the clear
   while the device is decrypting arrives as rubbish and lands in
   ld.bad_requests; one that goes out encrypted before the handshake
   does the same. The branch cannot be taken wrongly and pass.

   The handshake is modelled as what it actually is: the moment both
   ends start encrypting. tls_connect_ex() sets ld.ssl, and tls_close()
   clears it.
   ════════════════════════════════════════════════════════════════════ */
#define LD_XOR 0x5A

static struct {
    int conn, closes, sends, recvs;
    int handshake_fail;
    int send_fail;                /* tls_send() refuses              */
    int close_saw_connected;      /* was the mux still up at close?   */
    int handle;
    const tls_transport_t  *tr;
    const char             *host;
    const tls_client_cert_t *cc;
    uint32_t seed;
    int calls;
} ft;

int g_tls_last_alert_level = 0;
int g_tls_last_alert_desc  = -1;
int g_tls_last_stage       = 0;
int g_tls_peer_cipher      = -1;
int g_tls_cert_requested   = 0;
int g_tls_cert_sent        = 0;
int g_tls_cert_verified    = 0;

const char *tls_stage_name(int stage)
{
    return stage >= TLS_STAGE_ESTABLISHED ? "session established"
                                          : "somewhere short of it";
}

int tls_connect_ex(int h, const tls_transport_t *tr, const char *host,
                   uint32_t seed, const tls_client_cert_t *cc)
{
    ft.calls++;
    ft.handle = h; ft.tr = tr; ft.host = host; ft.cc = cc; ft.seed = seed;
    if (!tr || !tr->send || !tr->recv || !tr->eof) return TLS_ERR_HANDSHAKE;
    if (ft.handshake_fail) {
        g_tls_last_stage = TLS_STAGE_CERT_REQ;
        return TLS_ERR_CLIENTCERT;
    }
    ft.conn = 1;
    ld.ssl  = 1;                        /* both ends, together */
    g_tls_last_stage   = TLS_STAGE_ESTABLISHED;
    g_tls_cert_sent    = cc ? cc->n : 0;
    g_tls_cert_verified = (cc && cc->n) ? 1 : 0;
    return 0;
}

int tls_connect(int fd, const char *host, uint32_t seed)
{ return tls_connect_ex(fd, 0, host, seed, 0); }

int tls_send(int h, const uint8_t *d, uint16_t len)
{
    static uint8_t tmp[20480];
    int i;
    if (!ft.conn || h != ft.handle) return -1;
    if (ft.send_fail) return -1;
    if (len > (int)sizeof tmp) return -1;
    for (i = 0; i < (int)len; i++) tmp[i] = (uint8_t)(d[i] ^ LD_XOR);
    ft.sends++;
    return ft.tr->send(h, tmp, len);
}

int tls_recv(int h, uint8_t *b, uint16_t max, uint32_t to)
{
    int r, i;
    if (!ft.conn || h != ft.handle) return -1;
    r = ft.tr->recv(h, b, max, to);
    if (r > 0) { for (i = 0; i < r; i++) b[i] ^= LD_XOR; ft.recvs++; }
    return r;
}

int tls_eof(int h){ return (!ft.conn || h != ft.handle) ? 1 : ft.tr->eof(h); }

void tls_close(int h)
{
    if (h != ft.handle) return;
    ft.closes++;
    /* ── The order, measured THROUGH THE TRANSPORT ────────────────────
       lockdown_close() promises the close_notify travels down a
       channel that is still open; lockdown_forget() promises the
       opposite, because the device may be the one that was just
       unplugged.

       The first version of this asked usbmux_connected(), and that is
       true in BOTH cases -- forget() deliberately does not disconnect
       the mux, and close() has not got there yet. So it measured
       nothing and the check passed in close() by luck. What
       distinguishes them is whether the transport would carry a byte,
       which is exactly what tls_close() is about to try: ld_tr_eof()
       is 0 when lockdown still owns the connection and 1 when it has
       already let go. */
    ft.close_saw_connected = (ft.tr && !ft.tr->eof(h)) ? 1 : 0;
    ft.conn = 0;
    ld.ssl  = 0;
}

static void ft_reset(void){ memset(&ft, 0, sizeof ft); ft.handle = -1; }

#include "../usb/lockdown.c"

static void machine(void)
{
    phone_reset();
    ld_reset();
    ft_reset();
    g_ticks = 0;
    memset(&pend, 0, sizeof pend);
    lockdown_forget();
    usbmux_close();
    usb_enumerate();
}

#define contains contains_

int main(void)
{
    printf("lockdown: stage 1, QueryType over the mux\n");
    usb_register_hc(&LD_OPS, 0, 0, USB_HC_NONE);

    /* ════════════════════════════════════════════════════════════════
       1. Getting there
       ════════════════════════════════════════════════════════════════ */
    head("the fixture frames a reply the way libimobiledevice does");
    {
        /* The fixture's OWN framing, checked before it is used to
           judge anything. A length prefix that counted itself would
           make every test below agree with a driver that was four
           bytes out, in the same direction. */
        ld_reset();
        ld_answer(REPLY_LOCKDOWN);
        ck("the prefix is the body's length, not the packet's",
           (int)rd32(pend.buf) == (int)strlen(REPLY_LOCKDOWN),
           (long)rd32(pend.buf));
        ck("...which is four less than what is sent in all",
           pend.len == (int)rd32(pend.buf) + 4, pend.len);
        /* ld_answer() queues; ld_pump() delivers. One pump, and the
           first thing on the wire is the four prefix bytes alone --
           which is the shape the real one has and the reason
           lockdown.c accumulates. */
        ld_pump();
        ck("and the first packet is the four prefix bytes alone",
           pend.at == 4 && ph.in_len > 0, pend.at);
        ck("the body is XML", contains((const char *)pend.buf + 4,
           pend.len - 4, "<plist"), 0);
    }

    head("lockdown_connect opens the channel and reaches port 62078");
    machine();
    {
        int r;
        ck("nothing is connected yet", !usbmux_connected(), 0);
        r = lockdown_connect();
        ck("it connects", r == USB_OK, r);
        /* The transport is the kernel's own all the way down: the
           interface was claimed, the version exchanged, the SETUP sent
           and the three-way handshake completed, and the DEVICE agrees
           it is established rather than this file saying so. */
        ck("the device reached its established state",
           ph.tcp_state == 2, ph.tcp_state);
        ck("the version exchange happened once", ph.setup_seen == 1,
           ph.setup_seen);
        ck("and it refused nothing on the way",
           ph.reject_seq == 0 && ph.reject_len == 0 &&
           ph.reject_ports == 0 && ph.reject_flags == 0 &&
           ph.reject_magic == 0, ph.reject_seq);
        ck("to port 62078", ph.reply_port == 62078, ph.reply_port);
    }

    head("connecting twice does not open a second connection");
    {
        /* COUNT THE SYNS. The first version of this compared
           ph.tcp_state before and after, and tcp_state is 2 either
           way -- a reconnection ends established just as the first
           one did. A fixture value that cannot tell two things apart
           is the shape v0.8.7's NIC suite had four of, and the
           harness said so here too.

           Reopening matters: it tears down a conversation in progress
           and restarts the sequence numbers underneath whatever was
           using it. */
        ck("it is already connected", lockdown_connect() == USB_OK, 0);
        ck("and the device saw exactly ONE SYN", ph.syns == 1, ph.syns);
        ck("no second SETUP went out", ph.setup_seen == 1, ph.setup_seen);
    }

    head("a device with no mux interface cannot be reached");
    machine();
    ph.no_mux = 1;
    {
        ck("connect refuses", lockdown_connect() < 0, 0);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("A CLOSED PORT is distinguishable from a channel that broke");
    machine();
    ph.refuse_port = 1;
    {
        int r = lockdown_connect();
        ck("connect refuses", r < 0, r);
        /* "lockdownd is not listening" and "the transport failed" send
           the next person to different places. `usb lockdown` prints
           different screens for them, so the flag has to survive out
           to the shell. */
        ck("and it is recorded as the PORT refusing", usbmux_refused(), 0);
    }

    head("a query with no connection is refused BY NAME");
    machine();
    {
        /* ld_send_plist() refuses a closed connection too and returns
           the same USB_ENODEV, so the return value cannot tell the
           two apart -- the harness reported the outer guard as
           unreachable until this check existed. The log is the
           difference, and it is the line that tells somebody reading
           `dmesg` which layer said no. */
        int logs = g_logs;
        ck("it is refused", lockdown_query_type() == USB_ENODEV, 0);
        ck("...and said so, rather than being refused silently below",
           g_logs > logs, g_logs - logs);
        ck("and nothing reached the device", ld.requests == 0,
           ld.requests);
    }

    /* ════════════════════════════════════════════════════════════════
       2. THE REQUEST, as lockdownd reads it
       ════════════════════════════════════════════════════════════════ */
    head("THE QUERYTYPE REQUEST, as the service received it");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r = lockdown_query_type();
        ck("the device answered with the lockdown type", r == 1, r);
        ck("the service read exactly one complete request",
           ld.requests == 1, ld.requests);
        /* ── THE LENGTH PREFIX ──────────────────────────────────────
           The service only completes a request when it has four plus
           the declared length. A prefix counting itself leaves it four
           short for ever, so `requests == 1` is already the check --
           but assert the arithmetic too, so a failure says which. */
        ck("...and its declared length matched the XML it got",
           ld.last_request_len == (int)strlen(ld.last_request),
           ld.last_request_len);
        ck("the request is a plist",
           contains(ld.last_request, ld.last_request_len, "<plist"), 0);
        ck("asking for QueryType",
           contains(ld.last_request, ld.last_request_len,
                    "<key>Request</key>"), 0);
        ck("...by that name",
           contains(ld.last_request, ld.last_request_len,
                    "<string>QueryType</string>"), 0);
        /* The Label is what the phone shows the user in the trust
           prompt. On a release whose whole purpose is finding out
           whether the phone trusts this host, the prompt saying who is
           asking is worth having. */
        ck("and it says who is asking",
           contains(ld.last_request, ld.last_request_len,
                    "<string>HavenDOS</string>"), 0);
        ck("the mux refused none of it", ph.reject_seq == 0,
           ph.reject_seq);
        /* ── AND THE SERVICE COULD PARSE IT ──────────────────────────
           The byte count agreeing is not enough: a length prefix four
           SHORT delivers a document with its closing tag cut off, and
           the count still agrees with itself. The service reads the
           body now, which is what a real one does with libplist. */
        ck("...and the service could parse it", ld.bad_requests == 0,
           ld.bad_requests);
        ck("the document is complete, closing tag and all",
           contains(ld.last_request, ld.last_request_len, "</plist>"), 0);
    }

    head("...and the reply was READ ACROSS PACKETS");
    {
        /* The thing a single-packet fixture cannot show. The device
           sent four bytes, then the body; a reader written for one
           packet would have four bytes and an empty plist. */
        /* COUNT THE PACKETS. "ld.replies == 1" was the first version
           of this check and it says only that the service answered
           once -- it would pass against a reply that arrived whole, so
           it could not show that the accumulation ran at all. The
           driver's own count is the test. */
        ck("THE REPLY ARRIVED IN MORE THAN ONE PACKET",
           g_ld.packets >= 2, g_ld.packets);
        ck("...and the service sent exactly one reply", ld.replies == 1,
           ld.replies);
        ck("the Type is the lockdown one",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
        ck("and there is no Error", lockdown_error()[0] == 0, 0);
        ck("the dump reports the connection", lockdown_dump() == 1, 0);
    }

    head("a reply arriving as ONE packet also works");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.one_packet = 1;
    {
        /* Not what libimobiledevice does, and that is why it is here:
           if only the one-packet case worked, the accumulation would
           be untested rather than unnecessary. */
        int r = lockdown_query_type();
        ck("it still answers", r == 1, r);
        ck("in ONE packet this time, which is the contrast",
           g_ld.packets == 1, g_ld.packets);
        ck("with the same Type",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
    }

    head("A REPLY IN MANY SMALL PACKETS is accumulated");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.chunk = 7;              /* seven bytes at a time */
    {
        int r = lockdown_query_type();
        ck("it answers", r == 1, r);
        ck("after many packets, not two",
           g_ld.packets > 2, g_ld.packets);
        ck("with the whole Type, not a fragment of it",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
        /* Every packet had to be acknowledged, or the device stops
           sending and the reply stalls half way. The mux's own
           sequence check is what proves the acks were right. */
        ck("and the device refused no acknowledgement",
           ph.reject_seq == 0, ph.reject_seq);
    }

    /* ════════════════════════════════════════════════════════════════
       3. What it refuses to believe
       ════════════════════════════════════════════════════════════════ */
    head("A SERVICE THAT NEVER ANSWERS is reported, not waited on");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.silent = 1;
    {
        int r = lockdown_query_type();
        ck("it reports a timeout", r == USB_ETIMEOUT, r);
        /* The request DID complete at the service, so this is not a
           framing failure -- and the distinction matters, because the
           fix for "lockdownd ignored us" is a trust prompt on the
           phone and the fix for "the framing is wrong" is this file. */
        ck("...although the request itself arrived whole",
           ld.requests == 1, ld.requests);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("A REPLY THAT STOPS PART WAY keeps what arrived");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.body_short = 1;         /* declares ten more bytes than it sends */
    {
        int r = lockdown_query_type();
        ck("the query fails", r < 0, r);
        /* The packet most worth looking at is the one that did not
           parse. `usb lockdown` prints it, so it must survive the
           failure -- recording after validation would throw away
           exactly the evidence. */
        ck("but the dump still has something to show",
           lockdown_dump() == 1, 0);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("A REPLY TOO BIG TO HOLD is refused by name, not truncated");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.huge_reply = 1;         /* declares a hundred thousand bytes */
    {
        int logs = g_logs;
        int r = lockdown_query_type();
        /* A truncated plist that still contains the string being
           looked for is the worst outcome available: it would report
           success on a reply nobody read. */
        ck("refused", r < 0, r);
        /* ── AND REFUSED AS THE RIGHT THING ────────────────────────
           Without the size guard this still fails, just later and for
           another reason: the accumulation fills the buffer, the
           reply never completes, and it comes back USB_EXFER -- "the
           reply stopped part way". It did not stop; it was never
           going to fit. Two different faults with two different
           fixes, and one error number for both is the x_control()
           mistake three layers down. */
        ck("as USB_EPARAM -- too big to hold, not stopped part way",
           r == USB_EPARAM, r);
        ck("and the Type was NOT filled in from a partial reply",
           lockdown_type()[0] == 0, 0);
        ck("and it said so", g_logs > logs, g_logs - logs);
    }

    head("A REPLY WITH NO LENGTH PREFIX AT ALL");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.stub_reply = 1;         /* two bytes, and then nothing */
    {
        /* Fewer than four bytes arrived, so the length was never read
           and the reader never learns how much to expect. Without the
           guard the code falls through to `need - 4` with need still
           -1 and returns -5, which is USB_EPARAM: "a bad argument".
           Nobody passed a bad argument. */
        int r = lockdown_query_type();
        ck("it is refused", r < 0, r);
        ck("as USB_EXFER, not as somebody's bad argument",
           r == USB_EXFER, r);
        ck("and the two bytes were kept", g_ld.reply_len == 2,
           g_ld.reply_len);
        ck("so the dump can show them", lockdown_dump() == 1, 0);
    }

    head("A CONNECTION THAT DIES MID-REPLY keeps what arrived");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.rst_after_prefix = 1;
    {
        /* The length prefix lands, then the device resets. This is the
           path where usbmux_recv() itself returns an error, and it is
           the ONLY path that reaches the keep-what-arrived branch in
           ld_recv_plist() -- a reply that merely stops produces no
           error at all, just a budget that runs out. The harness
           reported that branch as unreachable until this existed.

           Four bytes is not much, but it is the difference between
           `usb lockdown` printing "length prefix: 00 00 00 FA = 250"
           and printing nothing: between "the reply was starting to
           arrive" and "the device never said anything". */
        int r = lockdown_query_type();
        ck("the query fails", r < 0, r);
        ck("THE FOUR PREFIX BYTES WERE KEPT", g_ld.reply_len == 4,
           g_ld.reply_len);
        ck("and the dump has them to show", lockdown_dump() == 1, 0);
        ck("the connection is down, as the device said",
           !usbmux_connected(), 1);
    }

    head("A SLOW REPLY is not abandoned between packets");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.hold = 700;             /* ticks of silence before each packet */
    {
        /* v0.8.9.3: re-expressed in TICKS, because the budget is time
           now rather than a poll count. The first reply of a
           connection gets LD_FIRST_TICKS (1000). The reply comes in
           two packets 700 ticks apart, so the FIRST arrives inside
           the budget and the pair of them do not -- which is the
           whole point: only a loop whose budget is reset BY PROGRESS
           gets both. One that spends a single budget across the
           exchange gives up during the second wait, with the prefix
           in hand and the body still coming.

           Without the hold every packet is there on the first poll
           and the budget is never near spent, so the reset could not
           be observed -- which is what the harness reported. A test
           whose subject never comes under load is not testing it. */
        int r = lockdown_query_type();
        ck("it still answers", r == 1, r);
        ck("with the whole Type",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
        ck("and it really did take two packets", g_ld.packets >= 2,
           g_ld.packets);
    }

    head("a device whose length prefix counts itself");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.prefix_counts_self = 1;
    {
        /* The error in the other direction: the DEVICE declaring four
           too many. It never completes, which is what makes getting
           this wrong present as silence. Recorded because somebody
           reading `usb lockdown`'s "no reply" screen needs to know
           that a wrong prefix looks exactly like it. */
        int r = lockdown_query_type();
        ck("the reply never completes", r < 0, r);
        ck("and the prefix is visible in the dump",
           lockdown_dump() == 1, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       4. The answer, when it is not the one wanted
       ════════════════════════════════════════════════════════════════ */
    head("AN ERROR REPLY is a framing success and a request failure");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.answer_error = 1;
    {
        int r = lockdown_query_type();
        /* Zero, not negative: every byte of framing worked and
           lockdownd refused the request itself. Collapsing that into
           the transport's error number sends the next person to
           re-check a framing that was perfect. */
        ck("it is not a failure", r == 0, r);
        ck("the Error is carried out",
           !strcmp(lockdown_error(), "InvalidRequest"), 0);
        ck("and no Type was invented", lockdown_type()[0] == 0, 0);
    }

    head("a Type that is not lockdownd's is reported as itself");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.answer_type = 1;
    {
        int r = lockdown_query_type();
        ck("it is not a success", r == 0, r);
        ck("and the Type that DID arrive is carried out",
           !strcmp(lockdown_type(), "com.apple.mobile.something"), 0);
        ck("with no Error", lockdown_error()[0] == 0, 0);
    }

    head("a reply with no Type at all");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.answer_type = 2;
    {
        int r = lockdown_query_type();
        ck("it is not a success", r == 0, r);
        ck("and nothing was filled in", lockdown_type()[0] == 0, 0);
        /* The reply had a Request key and a <string> after it. A
           scanner that took the first <string> in the document rather
           than the one after <key>Type</key> would report
           "QueryType" as the type. */
        ck("...and certainly not the first string in the document",
           strcmp(lockdown_type(), "QueryType") != 0, 0);
        ck("and it said so", g_logs > 0, g_logs);
    }

    /* ════════════════════════════════════════════════════════════════
       5. The scanner
       ════════════════════════════════════════════════════════════════ */
    head("THE SCANNER takes the string after the KEY, not the first one");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        /* REPLY_LOCKDOWN puts Request/QueryType BEFORE Type, on
           purpose. A scanner that found <string> without anchoring on
           the key would read "QueryType" and call it the type -- and
           because that is a real lockdownd string, the result would
           look like a plausible answer rather than a bug. */
        ck("it answers", lockdown_query_type() == 1, 0);
        ck("the Type is the one after <key>Type</key>",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
        ck("and not the QueryType string ahead of it",
           strcmp(lockdown_type(), "QueryType") != 0, 0);
        ck("the reply really did contain both",
           contains(REPLY_LOCKDOWN, (int)strlen(REPLY_LOCKDOWN),
                    "<string>QueryType</string>") &&
           contains(REPLY_LOCKDOWN, (int)strlen(REPLY_LOCKDOWN),
                    "<string>com.apple.mobile.lockdown</string>"), 0);
    }

    head("...AND IT IS ANCHORED ON THE KEY TAG, not on the word");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    ld.typeish_reply = 1;
    {
        /* The check above cannot show this and the harness said so.
           In REPLY_LOCKDOWN the first "Type" is inside "QueryType",
           and the next <string> after THAT point happens to be the
           right one -- so an unanchored search gets the right answer
           by luck and the test passes either way.

           REPLY_TYPEISH cannot be passed by luck: its first "Type" is
           inside a key NAME, "ProtocolType", whose value is "2". A
           scanner looking for the bare word reports the protocol
           version as the device type. */
        ck("it answers", lockdown_query_type() == 1, 0);
        ck("the Type is the one after <key>Type</key>",
           !strcmp(lockdown_type(), "com.apple.mobile.lockdown"), 0);
        ck("and NOT the value of the key that merely contains the word",
           strcmp(lockdown_type(), "2") != 0, 0);
        ck("the reply really does carry that trap",
           contains(REPLY_TYPEISH, (int)strlen(REPLY_TYPEISH),
                    "<key>ProtocolType</key>"), 0);
    }

    head("the state is cleared between queries, not carried over");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        ck("a good query first", lockdown_query_type() == 1, 0);
        ck("the Type is set", lockdown_type()[0] != 0, 0);
        ld.answer_error = 1;
        /* A second query whose reply has no Type must not report the
           FIRST one's. That is the stale-buffer shape
           tests/test_usbmux.c carries a check for one layer down, and
           it is worth one here because the consequence is a screen
           saying lockdownd answered when it did not. */
        ck("a second query answers with an Error", lockdown_query_type() == 0, 0);
        ck("and the stale Type is GONE, not reported again",
           lockdown_type()[0] == 0, 0);
        ck("while the Error is the new one",
           !strcmp(lockdown_error(), "InvalidRequest"), 0);
        ck("and the packet count is THIS reply's, not both added up",
           g_ld.packets <= 2, g_ld.packets);
    }
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        /* ── AND A FAILED QUERY MUST NOT SHOW THE LAST GOOD REPLY ───
           The Type and Error are cleared by the scanner on its way in,
           so clearing them in ld_recv_plist() is belt and braces --
           the harness reported it as unreachable through them.

           `have_reply` is not. A second query that times out with
           nothing at all would leave the FIRST reply's bytes in the
           buffer, and `usb lockdown` would print them as though they
           had just arrived: a screen saying lockdownd answered, on a
           boot where it said nothing. */
        ck("a good query first", lockdown_query_type() == 1, 0);
        ck("there is a reply to show", g_ld.reply_len > 0,
           g_ld.reply_len);
        ld.silent = 1;
        ck("a second query times out", lockdown_query_type() < 0, 0);
        ck("AND THE OLD REPLY IS GONE", g_ld.reply_len == 0,
           g_ld.reply_len);
        ck("so the dump says there is nothing, not the last one",
           g_ld.have_reply == 0, g_ld.have_reply);
    }

    /* ════════════════════════════════════════════════════════════════
       6. Bounds, and letting go
       ════════════════════════════════════════════════════════════════ */
    head("every transfer fits the back-end's buffers");
    ck("nothing was refused as too long for a bulk write",
       ph.sync_refused == 0, ph.sync_refused);
    ck("nor for a poll", ph.async_refused == 0, ph.async_refused);
    {
        /* LD_BUF holds a whole REPLY, accumulated; the mux's bound is
           what ONE PACKET can carry and is smaller. Two different
           numbers, and the relationship is asserted rather than left
           to agree by luck -- 0.8.7 shipped a driver whose transfer
           size and whose back-end's bound were each written
           separately. */
        ck("a reply buffer is bigger than one packet's payload",
           LD_BUF > usbmux_max_payload(), LD_BUF);
        ck("and one packet's payload fits the mux buffer",
           usbmux_max_payload() == MUX_BUF - MUX_HDR_FULL - 20,
           usbmux_max_payload());
        ck("the request fits in one packet, which is why it is sent as one",
           (int)strlen(LD_QUERYTYPE) + 4 <= usbmux_max_payload(),
           (long)strlen(LD_QUERYTYPE));
    }

    /* ════════════════════════════════════════════════════════════════
       7. STAGE 2 -- GetValue, where the value is <data>
       ════════════════════════════════════════════════════════════════ */

    head("the fixture's base64 really is base64");
    {
        /* Checked BEFORE it is used to judge anything. A fixture whose
           base64 is wrong would agree with a decoder that is wrong in
           the same direction, which is the mistake this whole file
           carries two scars from. The expected PEM is written out
           separately and compared byte for byte. */
        ck("the reply carries a <data> element",
           contains(REPLY_GETVALUE, (int)strlen(REPLY_GETVALUE),
                    "<data>"), 0);
        ck("...and NOT a <string> value",
           !contains(REPLY_GETVALUE, (int)strlen(REPLY_GETVALUE),
                     "<key>Value</key>\n\t<string>"), 0);
        /* THE WRAPPING IS THE POINT. A <data> element is broken across
           lines with a tab on each, and a decoder that stops at the
           first non-alphabet character returns a sixth of the key. */
        ck("the data is wrapped across lines, with tabs",
           contains(REPLY_GETVALUE, (int)strlen(REPLY_GETVALUE), "\n\t"), 0);
        ck("the expected PEM is a public key",
           contains(PEM_EXPECTED, (int)strlen(PEM_EXPECTED),
                    "BEGIN RSA PUBLIC KEY"), 0);
    }

    head("STAGE 2: GetValue DevicePublicKey");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        ck("stage 1 first", lockdown_query_type() == 1, 0);
        ld.getvalue = 1;
        r = lockdown_get_value("DevicePublicKey");
        ck("the device returned a value", r == 1, r);
        /* BYTE FOR BYTE against the PEM the base64 was made from --
           not "it starts with BEGIN", which a decoder that stopped
           after the first line would also satisfy. */
        ck("and it is the whole key, byte for byte",
           lockdown_value_len() == (int)strlen(PEM_EXPECTED) &&
           memcmp(lockdown_value(), PEM_EXPECTED,
                  strlen(PEM_EXPECTED)) == 0, lockdown_value_len());
        ck("...which is a PEM RSA public key",
           contains(lockdown_value(), lockdown_value_len(),
                    "-----BEGIN RSA PUBLIC KEY-----"), 0);
        ck("...with its closing line, so nothing was cut short",
           contains(lockdown_value(), lockdown_value_len(),
                    "-----END RSA PUBLIC KEY-----"), 0);
        ck("no Error came with it", lockdown_error()[0] == 0, 0);
        ck("and the mux refused nothing", ph.reject_seq == 0,
           ph.reject_seq);
    }

    head("...and the request named the key it was asked for");
    {
        ck("it is a GetValue",
           contains(ld.last_request, ld.last_request_len,
                    "<string>GetValue</string>"), 0);
        ck("with a Key",
           contains(ld.last_request, ld.last_request_len,
                    "<key>Key</key>"), 0);
        ck("naming DevicePublicKey",
           contains(ld.last_request, ld.last_request_len,
                    "<string>DevicePublicKey</string>"), 0);
        ck("the service could parse it", ld.bad_requests == 0,
           ld.bad_requests);
        /* The key is interpolated, so the length prefix has to be
           computed rather than fixed -- a request whose declared
           length came from the QueryType string would be wrong for
           every key of a different length. */
        ck("and its declared length matched the XML",
           ld.last_request_len == (int)strlen(ld.last_request),
           ld.last_request_len);
    }

    head("STAGES 1 AND 2 ARE ONE CONVERSATION");
    {
        /* Two requests, one connection, one SYN. A second connection
           would be a second chance for the thing under test to be the
           transport rather than the request. */
        ck("two requests reached the service", ld.requests == 2,
           ld.requests);
        ck("over ONE connection", ph.syns == 1, ph.syns);
        ck("and the version was exchanged once", ph.setup_seen == 1,
           ph.setup_seen);
    }

    head("A VALUE THAT IS A <string> is read too");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        ld.getvalue = 2;
        /* Some keys answer with a string -- ProductVersion is one.
           DevicePublicKey is not, and a driver that handled only the
           shape stage 2 needs would be wrong for half of GetValue. */
        r = lockdown_get_value("ProductVersion");
        ck("it returns a value", r == 1, r);
        ck("and it is the string, not empty",
           !strcmp(lockdown_value(), "17.5.1"), 0);
        ck("with the right length",
           lockdown_value_len() == 6, lockdown_value_len());
    }

    head("<data> THAT IS NOT BASE64 IS REFUSED, not half-decoded");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r, logs = g_logs;
        ld.getvalue = 3;
        r = lockdown_get_value("DevicePublicKey");
        /* Skipping the characters it did not recognise would turn a
           wrong guess about the framing into a SHORTER KEY THAT STILL
           LOOKS LIKE A KEY -- and stage 3 would sign something with
           it. */
        ck("no value is reported", r == 0, r);
        ck("and nothing was left in the buffer",
           lockdown_value_len() == 0, lockdown_value_len());
        ck("and it said so", g_logs > logs, g_logs - logs);
    }

    head("a GetValue the device refuses");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        ld.answer_error = 1;
        r = lockdown_get_value("DevicePublicKey");
        /* Zero, not negative: the framing worked and lockdownd said
           no. On a real phone an Error here naming pairing or trust
           is the answer that means stage 2 needs stage 3. */
        ck("it is not a transport failure", r == 0, r);
        ck("the Error is carried out",
           !strcmp(lockdown_error(), "InvalidRequest"), 0);
        ck("and no value was invented", lockdown_value_len() == 0,
           lockdown_value_len());
    }

    head("the value does not survive the next request");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        ld.getvalue = 1;
        ck("a good GetValue first",
           lockdown_get_value("DevicePublicKey") == 1, 0);
        ck("there is a value", lockdown_value_len() > 0,
           lockdown_value_len());
        ld.getvalue = 0;
        ld.answer_error = 1;
        /* A stale key reported after a request that returned none is
           a screen saying stage 2 worked on a boot where it did not --
           and stage 3 would build a pair record around it. */
        ck("a second request answers with an Error",
           lockdown_get_value("DevicePublicKey") == 0, 0);
        ck("AND THE OLD KEY IS GONE", lockdown_value_len() == 0,
           lockdown_value_len());
        ck("not merely emptied of its first byte",
           lockdown_value()[0] == 0, 0);
    }

    head("THE VALUE IS ANCHORED TO ITS KEY, not the first <data> there is");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        /* REPLY_GETVALUE has exactly one <data> in it, so an
           unanchored search finds the right one by luck and the test
           above passes either way -- the harness said so. This reply
           carries an EscrowBag <data> ahead of the Value, so a
           scanner that took the first one reports six bytes of
           something else as the device key. */
        ld.getvalue = 4;
        ck("it returns a value",
           lockdown_get_value("DevicePublicKey") == 1, 0);
        ck("and it is the KEY, not the data ahead of it",
           lockdown_value_len() == (int)strlen(PEM_EXPECTED) &&
           memcmp(lockdown_value(), PEM_EXPECTED,
                  strlen(PEM_EXPECTED)) == 0, lockdown_value_len());
        ck("the reply really did carry that trap",
           contains(REPLY_GETVALUE_TWODATA,
                    (int)strlen(REPLY_GETVALUE_TWODATA),
                    "<key>EscrowBag</key>"), 0);
    }

    head("A SHORTER VALUE does not run into the last one");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        ld.getvalue = 1;
        ck("a long value first",
           lockdown_get_value("DevicePublicKey") == 1, 0);
        ck("it is long", lockdown_value_len() > 100,
           lockdown_value_len());
        ld.getvalue = 5;
        ck("then a short one",
           lockdown_get_value("DevicePublicKey") == 1, 0);
        ck("three bytes", lockdown_value_len() == 3,
           lockdown_value_len());
        /* Without a terminator after the decoded bytes, the tail of
           the long value is still sitting there and the string runs
           straight into it -- so the dump would print a key, and the
           length beside it would say 3. */
        ck("AND THE STRING ENDS THERE TOO",
           (int)strlen(lockdown_value()) == 3,
           (long)strlen(lockdown_value()));
        ck("with the right three bytes",
           !memcmp(lockdown_value(), "abc", 3), 0);
    }

    head("AN ERROR WINS over a value in the same reply");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        /* lockdownd does not send both, and the rule is worth pinning
           anyway: if the device said no, no key is reported whatever
           else is in the document. The alternative is stage 3 signing
           a pair record with something out of a refused reply. */
        ld.getvalue = 6;
        ck("it is not reported as a success",
           lockdown_get_value("DevicePublicKey") == 0, 0);
        ck("the Error is carried out",
           !strcmp(lockdown_error(), "InvalidRequest"), 0);
        ck("AND NO VALUE WAS REPORTED", lockdown_value_len() == 0,
           lockdown_value_len());
        ck("though the reply really did contain one",
           contains(REPLY_GETVALUE_ERRVAL,
                    (int)strlen(REPLY_GETVALUE_ERRVAL), "<data>"), 0);
    }

    head("GetValue with no connection, and with no key");
    machine();
    {
        int logs = g_logs;
        ck("no connection is refused",
           lockdown_get_value("DevicePublicKey") == USB_ENODEV, 0);
        ck("...by name", g_logs > logs, g_logs - logs);
    }
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        ck("an empty key is refused", lockdown_get_value("") == USB_EPARAM, 0);
        ck("a null key is refused", lockdown_get_value(0) == USB_EPARAM, 0);
        /* The request buffer is 512 and the key is interpolated into
           it. A key longer than the bound is refused rather than
           overrunning -- the one place in this file where a caller's
           string reaches a fixed buffer. */
        {
            static char longkey[200];
            memset(longkey, 'K', sizeof longkey - 1);
            longkey[sizeof longkey - 1] = 0;
            ck("an absurd key is refused",
               lockdown_get_value(longkey) == USB_EPARAM, 0);
        }
        ck("and none of them reached the device", ld.requests == 0,
           ld.requests);
    }

    head("THE DECODER SKIPS WHITESPACE and refuses rubbish");
    {
        /* ld_b64() is reached through the driver above, but its edges
           are worth pinning directly: these are the cases that decide
           whether a wrapped <data> element comes back whole. */
        uint8_t out[64];
        int n;
        n = ld_b64((const uint8_t *)"aGVsbG8=", 8, out, sizeof out);
        ck("plain base64 decodes", n == 5 && !memcmp(out, "hello", 5), n);
        n = ld_b64((const uint8_t *)"aGVs\n\tbG8=", 10, out, sizeof out);
        ck("...and so does the same split across lines with a tab",
           n == 5 && !memcmp(out, "hello", 5), n);
        n = ld_b64((const uint8_t *)"aGVsbG8", 7, out, sizeof out);
        ck("unpadded base64 still decodes", n == 5, n);
        n = ld_b64((const uint8_t *)"aGV!bG8=", 8, out, sizeof out);
        ck("a character outside the alphabet is REFUSED", n < 0, n);
        n = ld_b64((const uint8_t *)"aGVsbG8=X", 9, out, sizeof out);
        ck("...and so is anything after the padding", n < 0, n);
        /* TWO characters is not truncation: 12 bits carry one byte
           with four left over, which is what an unpadded single byte
           looks like. The first version of this check called it a
           cut-short group and was wrong about base64, not about the
           code. ONE character is the impossible case -- six bits and
           nothing to put them in. */
        n = ld_b64((const uint8_t *)"aG", 2, out, sizeof out);
        ck("two characters are one byte, not a truncated group",
           n == 1, n);
        n = ld_b64((const uint8_t *)"a", 1, out, sizeof out);
        ck("ONE character is refused -- six bits and no byte", n < 0, n);
        n = ld_b64((const uint8_t *)"aGVsbG8=aGVs", 12, out, sizeof out);
        ck("and a second group after the padding is refused", n < 0, n);
        n = ld_b64((const uint8_t *)"aGVsbG8=", 8, out, 2);
        ck("and a value too big for the buffer is refused, not cut",
           n < 0, n);
        n = ld_b64((const uint8_t *)"", 0, out, sizeof out);
        ck("empty decodes to nothing", n == 0, n);
    }

    head("closing tells the device; forgetting does not");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int calls;
        lockdown_close();
        ck("the device saw the reset", ph.tcp_state == 0, ph.tcp_state);
        ck("the interface is still claimed", usbmux_present(), 0);
        ck("a query is refused", lockdown_query_type() == USB_ENODEV, 0);

        ck("and it reconnects", lockdown_connect() == USB_OK, 0);
        calls = ph.out_calls;
        /* `usb rescan` calls this before usb_enumerate() rewalks the
           bus. The device it would say goodbye to may be the one that
           was unplugged, which is usually why the rescan is
           happening -- so a farewell packet there waits for a device
           that is not there. */
        lockdown_forget();
        ck("forgetting sends NOTHING", ph.out_calls == calls,
           ph.out_calls - calls);
        ck("and a query is refused after it",
           lockdown_query_type() == USB_ENODEV, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9.0: A REQUEST THAT DOES NOT FIT A PACKET

       ld_send_plist() used to refuse anything over usbmux_max_payload()
       by name, with the refusal saying "not written yet" about exactly
       this. usbmux_send_stream() now splits it. What is tested here is
       not the splitting -- tests/test_usbmux.c does that against the
       transport -- but the thing the splitting is FOR: that lockdownd
       on the other side reads ONE request, whole, in order, with the
       four-byte prefix still meaning what it meant.

       The fixture's dev_service() reassembles across packets and then
       insists the body begins with an XML declaration and ends with a
       closing plist tag, so a request missing its middle or its tail
       is counted as bad rather than answered.
       ════════════════════════════════════════════════════════════════ */
    head("a five-kilobyte request, built by net/plist.c, sent whole");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        static char doc[LD_REQ];
        static uint8_t cert[1180];
        plist_t pl;
        int i, r, body;

        for (i = 0; i < (int)sizeof cert; i++)
            cert[i] = (uint8_t)((i * 7 + 1) & 0xFF);

        /* The real Pair request, as net/plist.c builds it. Not a
           string of the right length: a document of the right SHAPE,
           so that what crosses the wire is what will cross it on the
           HP. */
        plist_init(&pl, doc, sizeof doc);
        plist_begin(&pl);
        plist_dict_begin(&pl);
          plist_kv_string(&pl, "Label",   "HavenDOS");
          plist_kv_string(&pl, "Request", "Pair");
          plist_key(&pl, "PairRecord");
          plist_dict_begin(&pl);
            plist_kv_data(&pl, "DeviceCertificate", cert, 300);
            plist_kv_data(&pl, "HostCertificate",   cert, 1120);
            plist_kv_data(&pl, "RootCertificate",   cert, 1180);
            plist_kv_string(&pl, "HostID",
                            "C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B");
            plist_kv_string(&pl, "SystemBUID",
                            "7F8E9D0C-1B2A-4958-8674-5F4E3D2C1B0A");
          plist_dict_end(&pl);
        plist_dict_end(&pl);
        plist_end(&pl);

        ck("the document built", plist_ok(&pl), 0);
        body = (int)plist_len(&pl);
        /* The whole point: this is bigger than one mux packet. If the
           payload bound ever grows past it, this check says so rather
           than the suite quietly testing the single-packet path. */
        ck("it is larger than one packet can carry",
           body + 4 > usbmux_max_payload(), body + 4);
        ck("...and fits what lockdown will stage", body + 4 <= LD_REQ,
           body + 4);

        r = ld_send_plist(doc);
        ck("it sends", r == USB_OK, r);
        ck("it took several packets", ph.host_segs > 1, ph.host_segs);
        ck("exactly as many as the bound requires",
           ph.host_segs == (body + 4 + usbmux_max_payload() - 1)
                           / usbmux_max_payload(), ph.host_segs);

        /* lockdownd's side. */
        ck("the fixture reassembled it", !ld.too_big, ld.too_big);
        ck("lockdownd saw ONE request, not several",
           ld.requests == 1, ld.requests);
        ck("...and none it could not parse",
           ld.bad_requests == 0, ld.bad_requests);
        ck("the declared length matched the body",
           ld.last_request_len == body, ld.last_request_len);
        /* THE CHECK THE REST OF THIS BLOCK EXISTS FOR. A duplicated
           or dropped segment keeps the length right and the XML
           well-formed, and shows up only here. */
        ck("every byte of the document arrived, in order",
           memcmp(ld.last_request, doc, (size_t)body) == 0, 0);
        ck("the device rejected no sequence number",
           ph.reject_seq == 0, ph.reject_seq);
        ck("the connection is still up", usbmux_connected(), 0);
    }

    head("a request bigger than lockdown can stage is refused by name");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        static char too_big[LD_REQ + 64];
        int logs = g_logs, i;
        for (i = 0; i < (int)sizeof too_big - 1; i++) too_big[i] = 'x';
        too_big[sizeof too_big - 1] = 0;
        ck("it is refused", ld_send_plist(too_big) == USB_EPARAM, 0);
        ck("by name", g_logs > logs, g_logs - logs);
        ck("and nothing went out", ph.host_segs == 0, ph.host_segs);
        ck("the connection is untouched", usbmux_connected(), 0);
    }

    head("the staging bound, at the byte");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        static char doc[LD_REQ + 8];
        int i;
        ld.silent = 1;          /* no answer wanted, only the send   */
        for (i = 0; i < (int)sizeof doc; i++) doc[i] = 'z';

        /* LD_REQ holds the four-byte prefix AND the body, so the
           largest body that fits is LD_REQ - 4. An off-by-four here
           writes the prefix, then copies the body four bytes past the
           end of a static array -- and a bound of `len > sizeof out`
           accepts exactly these four lengths and no others, which is
           why this is checked at the byte rather than with a document
           that is obviously too big. */
        doc[LD_REQ - 4] = 0;
        ck("a body of exactly LD_REQ-4 is accepted",
           ld_send_plist(doc) == USB_OK, 0);
        ck("...and went out", ph.host_bytes == (uint32_t)LD_REQ,
           (long)ph.host_bytes);

        machine(); lockdown_connect(); ld.silent = 1;
        doc[LD_REQ - 4] = 'z';
        doc[LD_REQ - 3] = 0;
        ck("one byte more is refused",
           ld_send_plist(doc) == USB_EPARAM, 0);
        ck("...and nothing went out", ph.host_bytes == 0u,
           (long)ph.host_bytes);

        machine(); lockdown_connect(); ld.silent = 1;
        doc[LD_REQ - 3] = 'z';
        doc[LD_REQ] = 0;
        ck("and four bytes more is still refused",
           ld_send_plist(doc) == USB_EPARAM, 0);
        ck("...with nothing on the wire", ph.host_bytes == 0u,
           (long)ph.host_bytes);
    }

    /* ── the partial send ────────────────────────────────────────────
       The failure worth designing for. Once segments are out,
       lockdownd is part way through a length prefix: every later
       request on that connection is read at the wrong offset, and
       nothing anywhere reports an error. So the connection is torn
       down rather than reused. */
    head("a request that breaks part way through drops the connection");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        static char doc[4096];
        int i, r, logs;
        for (i = 0; i < (int)sizeof doc - 1; i++) doc[i] = 'y';
        doc[sizeof doc - 1] = 0;

        /* The connection took four bulk OUTs; the request's first
           segment is the fifth, a full 2048-byte packet, so the sixth
           is its zero-length packet (v0.8.9.7) and the second segment
           is the seventh. */
        ph.stall_out_on = 7;
        logs = g_logs;
        r = ld_send_plist(doc);
        ck("it fails", r < 0, r);
        ck("it said so", g_logs > logs, g_logs - logs);
        ck("some of it went out", ph.host_bytes > 0, (long)ph.host_bytes);
        ck("but not all of it",
           ph.host_bytes < (uint32_t)sizeof doc, (long)ph.host_bytes);
        /* The whole point. A connection left up here is a connection
           that answers the wrong question for the rest of its life. */
        ck("the mux connection is down", !usbmux_connected(), 0);
        ck("and lockdown knows it",
           lockdown_query_type() == USB_ENODEV, 0);
        ck("lockdownd never saw a complete request",
           ld.requests == 0, ld.requests);
        ph.stall_out_on = 0;
    }

    /* ── a failure with NOTHING on the wire is different ─────────────
       A request whose FIRST segment never left is a request lockdownd
       has not seen a byte of. The stream is still aligned, so the
       connection is fine and tearing it down would throw away a
       working channel for no reason. The distinction is the whole
       reason ld_send_plist() looks at how many bytes went out instead
       of just at the error code. */
    head("a request that fails before anything goes out keeps the line");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        static char doc[4096];
        int i, r;
        for (i = 0; i < (int)sizeof doc - 1; i++) doc[i] = 'y';
        doc[sizeof doc - 1] = 0;

        /* The FIRST segment this time: the connection took four bulk
           OUTs, so the request's first is the fifth. */
        ph.stall_out_on = 5;
        r = ld_send_plist(doc);
        ck("it fails", r < 0, r);
        ck("not a byte went out", ph.host_bytes == 0u, (long)ph.host_bytes);
        ck("the mux connection is STILL UP", usbmux_connected(), 0);
        /* g_ld.connected is static in the driver; this suite
           includes it, so the flag itself is the assertion rather
           than a predicate invented for the test. */
        ck("and lockdown still has it", g_ld.connected, 0);
        ph.stall_out_on = 0;
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9.3 -- A REPLY THAT ARRIVES AFTER WE GAVE UP

       THE BUG, as the HP's kernel log recorded it:

           lockdown: GetValue DevicePublicKey sent, 334 bytes
           lockdown: no reply at all              <- same 10 ms tick
           lockdown: QueryType sent, 285 bytes
           lockdown: a 914-byte reply with no Type in it

       The 914 bytes were the GetValue answer, arriving a moment late
       and being read as the reply to the QueryType that followed.
       From then on every reply was one request behind, each one
       perfectly framed and containing the answer to the previous
       question.

       The cause was a budget counted in POLLS rather than in time:
       twenty thousand empty bulk-INs finish in under ten milliseconds
       on that machine. A phone answers QueryType at once and takes
       noticeably longer over DevicePublicKey, because it has to go and
       fetch the key -- so stage 1 always worked and stage 2 never did.

       No suite could see it, because a poll count is the same number
       everywhere and this fixture had no clock. It has one now: see
       ld_intr_poll().
       ════════════════════════════════════════════════════════════════ */
    head("a device slower than the budget does not poison the next request");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        /* Comfortably past LD_FIRST_TICKS. The device IS going to
           answer -- that is the point. A device that never answers at
           all cannot produce this bug. */
        ld.hold = 4000;
        ld.one_packet = 1;
        r = lockdown_query_type();
        ck("the request times out", r == USB_ETIMEOUT, r);
        /* ── AND THE CHANNEL IS GONE ─────────────────────────────────
           Lengthening the budget makes this rare; it cannot make it
           impossible, because a device busy enough beats any budget.
           So the only safe thing is to stop using a stream whose
           position the two ends no longer agree on. */
        ck("and the channel was dropped, not left to desynchronise",
           !usbmux_connected(), 1);
        ck("the driver knows it is not connected", !lockdown_ssl(), 0);
    }

    /* The next request must reconnect and get ITS OWN answer. On the
       HP it got the previous one's, for ever. */
    {
        int r;
        ld.hold = 0;
        ld.one_packet = 0;
        ck("a reconnect is needed", lockdown_connect() == USB_OK, 0);
        r = lockdown_query_type();
        ck("and the next request answers correctly", r == 1, r);
        ck("with its OWN reply", !strcmp(lockdown_type(), LOCKDOWN_TYPE),
           0);
    }

    head("a reply that stops after its prefix drops the channel too");
    machine();
    lockdown_connect();
    {
        int r;
        /* The prefix arrives; the body does not, and NO RST follows --
           the device is simply still working on it. So the body is
           still coming, and the next request would read it. Same
           desynchronisation as a timeout, through a different door. */
        ld.stall_after_prefix = 1;
        r = lockdown_query_type();
        ck("it fails", r < 0, r);
        ck("the four prefix bytes were kept as evidence",
           g_ld.reply_len == 4, g_ld.reply_len);
        ck("the dump still shows them", lockdown_dump() == 1, 0);
        ck("and the channel was dropped", !usbmux_connected(), 1);
    }

    head("a reply just inside the budget is still waited for");
    machine();
    lockdown_connect();
    {
        int r;
        ld.hold = 900;             /* < LD_FIRST_TICKS (1000) */
        ld.one_packet = 1;
        r = lockdown_query_type();
        /* The old code's budget was twenty thousand POLLS, which this
           device would have blown through in no time at all. The
           point of the fix is that a slow device is waited for rather
           than abandoned. */
        ck("a slow device is waited for", r == 1, r);
        ck("and answers correctly",
           !strcmp(lockdown_type(), LOCKDOWN_TYPE), 0);
        ck("the channel is still up", usbmux_connected(), 0);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9.2 -- A PACKET THAT ARRIVES TWICE

       THE BUG THIS IS FOR, seen on the HP and not by any suite. Until
       v0.8.9.2 the receive path never looked at a packet's sequence
       number: it added the payload length to the acknowledgement and
       handed the bytes up. So a retransmission was delivered as new
       data, and from that moment every request got the PREVIOUS
       request's reply --

           QueryType  -> the GetValue reply
           GetValue   -> the QueryType reply

       -- each of which parses perfectly and contains the wrong thing,
       so an entire stage of pairing reported itself as a protocol
       failure.

       Nothing here could have found it, because this fixture had
       never sent the same packet twice. That is the lesson and it is
       the same one tests/test_xhci.c records about slots: a property
       that only holds while something never happens needs a test in
       which it happens.
       ════════════════════════════════════════════════════════════════ */
    head("a reply that arrives twice is delivered once");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        ld.dup_every = 1;
        r = lockdown_query_type();
        ck("QueryType still answers correctly", r == 1, r);
        ck("with the right type",
           !strcmp(lockdown_type(), LOCKDOWN_TYPE), 0);
        ck("the device really did retransmit", ld.dups_sent > 0,
           ld.dups_sent);
        /* The transport must have REFUSED them rather than passed
           them up. Checked at the counter because a duplicate that is
           dropped and one that never arrived look identical from
           above -- which is exactly how this went unnoticed. */
        ck("and the duplicates were dropped", g_m.dup_drops > 0,
           (long)g_m.dup_drops);
    }

    /* ── THE SYMPTOM ITSELF ──────────────────────────────────────────
       The check that would have caught it. A second request after a
       duplicated first one must get ITS OWN answer. */
    head("the request after a duplicate gets its own reply");
    {
        int r;
        ld.getvalue = 1;
        r = lockdown_get_value("DevicePublicKey");
        ck("GetValue answers", r == 1, r);
        ck("with a VALUE, not the QueryType reply",
           lockdown_value()[0] != 0, 0);
        /* And specifically not the previous reply's content. On the
           HP this came back as com.apple.mobile.lockdown -- the right
           bytes for the wrong question. */
        ck("and not the lockdown type string",
           strcmp(lockdown_value(), LOCKDOWN_TYPE) != 0, 0);
    }

    head("a whole conversation with every packet duplicated");
    machine();
    lockdown_connect();
    {
        int i, ok = 0;
        ld.dup_every = 1;
        ld.getvalue = 1;
        /* Alternating requests, because the failure was an OFF-BY-ONE
           in the stream: one exchange can be right by luck and the
           next one tells you. */
        for (i = 0; i < 4; i++) {
            if (lockdown_query_type() == 1 &&
                !strcmp(lockdown_type(), LOCKDOWN_TYPE) &&
                lockdown_get_value("DevicePublicKey") == 1 &&
                lockdown_value()[0]) ok++;
        }
        ck("four QueryType/GetValue pairs, all correct", ok == 4, ok);
        ck("lockdownd never saw a malformed request",
           ld.bad_requests == 0, ld.bad_requests);
        ck("duplicates were sent throughout", ld.dups_sent >= 4,
           ld.dups_sent);
    }

    head("the acknowledgement follows the stream, not a byte count");
    machine();
    lockdown_connect();
    {
        uint32_t ack_before, ack_after;
        usbmux_conn_state(0, 0, 0, &ack_before, 0);
        ld.dup_once = 1;
        lockdown_query_type();
        usbmux_conn_state(0, 0, 0, &ack_after, 0);
        /* The ack must have advanced by what the DEVICE actually
           sent, not by what arrived. Adding the length of every
           packet -- which is what the old code did -- walks the
           acknowledgement past the end of the device's stream, and
           every number after that is wrong. */
        ck("the ack advanced by the reply's length exactly",
           ack_after - ack_before ==
               (uint32_t)(4 + (int)strlen(REPLY_LOCKDOWN)),
           (long)(ack_after - ack_before));
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9 -- STAGE 3's SECOND HALF: the session

       What is NOT tested here: TLS. net/tls.c owns that and
       tests/test_tlscc.c judges it against `openssl s_server`, which
       is the only honest judge for a handshake. What IS tested here is
       the thing only this file can answer -- that the plist framing
       takes the encrypted path once the session is encrypted and the
       plaintext path before it. See the note above ld_xor(): the
       fixture's cipher is what makes a wrong branch fail rather than
       pass.
       ════════════════════════════════════════════════════════════════ */
    head("StartSession asks the right question");
    machine();
    ck("connected", lockdown_connect() == USB_OK, 0);
    {
        int r;
        ld.session = 1;
        r = lockdown_start_session("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE",
                                   "11111111-2222-3333-4444-555555555555");
        ck("it returns a session", r == 1, r);
        ck("the request named StartSession",
           contains(ld.last_request, ld.last_request_len,
                    "<string>StartSession</string>"), 0);
        ck("it carried the HostID",
           contains(ld.last_request, ld.last_request_len,
                    "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"), 0);
        ck("and the SystemBUID",
           contains(ld.last_request, ld.last_request_len,
                    "11111111-2222-3333-4444-555555555555"), 0);
        ck("it is a complete plist",
           contains(ld.last_request, ld.last_request_len, "<?xml") &&
           contains(ld.last_request, ld.last_request_len, "</plist>"), 0);
        ck("lockdownd could parse it", ld.bad_requests == 0,
           ld.bad_requests);
        /* The two are separate facts and the driver keeps them apart:
           a session exists, and SSL was ASKED FOR. */
        ck("the SessionID came back",
           !strcmp(lockdown_session_id(),
                   "0BD1C4E7-6A9F-4A1D-9C3E-5A1B2C3D4E5F"),
           0);
        ck("there is a session", lockdown_session() == 1, 0);
        ck("SSL was requested", lockdown_ssl_wanted() == 1, 0);
        /* And NOT yet done. A flag set from what the device asked for
           would route the next plaintext request into a session that
           does not exist. */
        ck("but the handshake has NOT happened", lockdown_ssl() == 0, 0);
    }

    head("the EnableSessionSSL boolean, in the shapes it comes in");
    {
        machine();
        lockdown_connect(); ld.session = 2;
        lockdown_start_session("A-B", "C-D");
        ck("<false/> is read as false", lockdown_ssl_wanted() == 0, 0);
        ck("the session still exists", lockdown_session() == 1, 0);

        machine();
        lockdown_connect(); ld.session = 3;
        lockdown_start_session("A-B", "C-D");
        ck("a reply with no flag at all means no SSL",
           lockdown_ssl_wanted() == 0, 0);
        ck("and still a session", lockdown_session() == 1, 0);

        /* THE ONE A LOOSER SCANNER WOULD GET WRONG. The key it is
           asked about carries a <string>, and the NEXT key carries the
           <true/>. A scanner that searched forward for the first
           boolean would report SSL requested. */
        machine();
        lockdown_connect(); ld.session = 6;
        lockdown_start_session("A-B", "C-D");
        ck("a <string> value is not mistaken for a boolean",
           lockdown_ssl_wanted() == 0, 0);
        ck("nor is the NEXT key's <true/>", lockdown_ssl_wanted() == 0, 0);
        ck("the session is still read correctly",
           !strcmp(lockdown_session_id(), "BOOLISH"), 0);
    }

    head("StartSession refused, and the refusals that differ");
    {
        machine();
        lockdown_connect(); ld.session = 5;
        ck("InvalidHostID returns 0 rather than an error",
           lockdown_start_session("A-B", "C-D") == 0, 0);
        ck("and names itself", !strcmp(lockdown_error(), "InvalidHostID"),
           0);
        ck("no session was recorded", lockdown_session() == 0, 0);
        ck("and no SessionID left behind",
           lockdown_session_id()[0] == 0, 0);

        machine();
        lockdown_connect(); ld.session = 4;
        ck("a reply with no SessionID returns 0",
           lockdown_start_session("A-B", "C-D") == 0, 0);
        ck("and records no session", lockdown_session() == 0, 0);

        machine();
        ck("with no connection it says so",
           lockdown_start_session("A-B", "C-D") == USB_ENODEV, 0);

        machine();
        lockdown_connect();
        ck("a NULL HostID is refused",
           lockdown_start_session(0, "C-D") == USB_EPARAM, 0);
        ck("an empty one too",
           lockdown_start_session("", "C-D") == USB_EPARAM, 0);
        {
            char longid[200];
            memset(longid, 'Z', sizeof longid - 1);
            longid[sizeof longid - 1] = 0;
            ck("one too long for the request buffer is refused",
               lockdown_start_session(longid, "C-D") == USB_EPARAM, 0);
            ck("and nothing was sent", ld.requests == 0, ld.requests);
        }
    }

    head("SSL cannot be turned on without a session");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect();
        ck("it refuses", lockdown_enable_ssl(&cc, 1u) == USB_EPARAM, 0);
        ck("the handshake was never attempted", ft.calls == 0, ft.calls);
        machine();
        ck("and with no connection it refuses differently",
           lockdown_enable_ssl(&cc, 1u) == USB_ENODEV, 0);
    }

    head("what gets handed to the TLS layer");
    {
        tls_client_cert_t cc;
        uint8_t fake_cert[4] = { 0x30, 0x02, 0x05, 0x00 };
        memset(&cc, 0, sizeof cc);
        cc.cert[0] = fake_cert; cc.cert_len[0] = 4; cc.n = 1;

        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        ck("the handshake succeeds", lockdown_enable_ssl(&cc, 0x1234u) == 1,
           0);
        ck("it was asked exactly once", ft.calls == 1, ft.calls);
        /* NO HOSTNAME. There is no name to put in an SNI extension
           here, and an empty one is malformed rather than absent. */
        ck("with no hostname", ft.host == 0, 0);
        ck("the client certificate was passed through",
           ft.cc == (const tls_client_cert_t *)&cc, 0);
        ck("the seed was the caller's", ft.seed == 0x1234u, 0);
        /* The handle must not be an fd: net/tcp.c hands out small
           integers and a collision would put two sessions in one
           slot. */
        ck("the handle is well clear of the fd range",
           ft.handle == LD_TLS_HANDLE && ft.handle > 1000, ft.handle);
        ck("a transport was supplied", ft.tr != 0, 0);
        ck("with all three functions",
           ft.tr && ft.tr->send && ft.tr->recv && ft.tr->eof, 0);
        ck("and no close -- the mux belongs to lockdown",
           ft.tr && ft.tr->close == 0, 0);
        ck("it is named", ft.tr && !strcmp(ft.tr->name, "usbmux"), 0);
        ck("the driver now says the session is encrypted",
           lockdown_ssl() == 1, 0);
    }

    /* ── THE CHECK THE XOR EXISTS FOR ────────────────────────────────
       A request sent in the clear on an encrypted session arrives as
       rubbish and lockdownd cannot parse it. Nothing else in this
       suite could tell the two branches apart. */
    head("once encrypted, the framing goes through TLS and not beside it");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        ck("encrypted", lockdown_enable_ssl(&cc, 1u) == 1, 0);
        {
            int before_bad = ld.bad_requests;
            int r = lockdown_query_type();
            ck("QueryType inside the session is answered", r == 1, r);
            ck("the device answered with the lockdown type",
               !strcmp(lockdown_type(), LOCKDOWN_TYPE), 0);
            ck("lockdownd parsed it -- so it was NOT sent in the clear",
               ld.bad_requests == before_bad, ld.bad_requests);
            ck("it went through the TLS layer", ft.sends > 0, ft.sends);
            ck("and came back through it", ft.recvs > 0, ft.recvs);
            /* The reply still arrives in two writes, encrypted. The
               accumulation is the same loop and it still has to
               work. */
            ck("the reply still arrived in more than one packet",
               ld.replies > 0 && g_ld.packets >= 2, g_ld.packets);
        }
    }

    head("a handshake that FAILS leaves the session in the clear");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        ft.handshake_fail = 1;
        ck("it reports the failure",
           lockdown_enable_ssl(&cc, 1u) == TLS_ERR_CLIENTCERT, 0);
        /* The flag is set from the handshake's SUCCESS and from
           nothing else. A flag set on the device's request would send
           the next request encrypted into a device that is not
           decrypting. */
        ck("the driver does NOT claim an encrypted session",
           lockdown_ssl() == 0, 0);
        ck("the session itself survives", lockdown_session() == 1, 0);
        {
            int r = lockdown_query_type();
            ck("a later request still works, in the clear", r == 1, r);
            ck("and lockdownd could parse it", ld.bad_requests == 0,
               ld.bad_requests);
            ck("nothing went through the TLS layer", ft.sends == 0,
               ft.sends);
        }
    }

    /* ── THE FLAG COMES FROM THE HANDSHAKE, NOT FROM THE REQUEST ─────
       The injection harness needed this case. Every other test here
       has a device that asked for SSL, so ssl_on and ssl_wanted agree
       and `ssl_on = ssl_wanted` is indistinguishable from
       `ssl_on = 1`. A device that declined SSL and a handshake done
       anyway is the one arrangement that tells them apart -- and the
       defect it would hide sends plaintext into an encrypted session,
       which is answered by nothing. */
    head("the encrypted flag is the handshake's, not the device's wish");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 2;        /* <false/> */
        lockdown_start_session("A-B", "C-D");
        ck("the device declined SSL", lockdown_ssl_wanted() == 0, 0);
        ck("the handshake still succeeds when asked for",
           lockdown_enable_ssl(&cc, 1u) == 1, 0);
        ck("and the session IS encrypted regardless",
           lockdown_ssl() == 1, 0);
        {
            int r = lockdown_query_type();
            ck("a request afterwards is answered", r == 1, r);
            ck("so it went out encrypted", ld.bad_requests == 0,
               ld.bad_requests);
            ck("through the TLS layer", ft.sends > 0, ft.sends);
        }
    }

    head("a new StartSession does not inherit the last one");
    {
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        ck("there is a session with SSL",
           lockdown_session() == 1 && lockdown_ssl_wanted() == 1, 0);
        ld.session = 5;                            /* InvalidHostID */
        ck("the next one is refused",
           lockdown_start_session("A-B", "C-D") == 0, 0);
        /* Nothing of the previous session may survive: a StopSession
           sent with a stale SessionID is a request about a session
           the device closed. */
        ck("no SessionID is left", lockdown_session_id()[0] == 0, 0);
        ck("no session is claimed", lockdown_session() == 0, 0);
        ck("and the SSL answer was reset", lockdown_ssl_wanted() == 0, 0);
    }

    head("an encrypted send that fails is reported, not swallowed");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        {
            int reqs_before = ld.requests;
            int r;
            ft.send_fail = 1;
            r = lockdown_query_type();
            /* The failure must be the SEND's, reported immediately.
               Reporting success and going on to read leaves the caller
               waiting out a poll budget for a reply to a request
               lockdownd never saw -- and the error it finally gets
               says "no reply", which sends the next person looking at
               the device. */
            ck("it fails", r < 0, r);
            ck("and not as a timeout", r == USB_EXFER, r);
            ck("lockdownd never saw the request",
               ld.requests == reqs_before, ld.requests - reqs_before);
            ft.send_fail = 0;
        }
    }

    head("StopSession travels INSIDE the session");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        {
            int sends_before = ft.sends;
            int r = lockdown_stop_session();
            ck("it succeeds", r == 1, r);
            ck("the device saw a StopSession", ld.stop_session == 1,
               ld.stop_session);
            ck("it carried the SessionID",
               contains(ld.last_request, ld.last_request_len,
                        "0BD1C4E7-6A9F-4A1D-9C3E-5A1B2C3D4E5F"), 0);
            /* ENCRYPTED, which is why the TLS teardown is after the
               request and not before it. */
            ck("it went out through TLS", ft.sends > sends_before,
               ft.sends);
            ck("lockdownd could parse it", ld.bad_requests == 0,
               ld.bad_requests);
            ck("and the TLS session was closed afterwards",
               ft.closes == 1, ft.closes);
            ck("the driver has no session left", lockdown_session() == 0,
               0);
            ck("nor an encrypted one", lockdown_ssl() == 0, 0);
            ck("nor a SessionID", lockdown_session_id()[0] == 0, 0);
        }
        ck("a second StopSession does nothing",
           lockdown_stop_session() == 0, 0);
    }

    head("close and forget tear the session down differently");
    {
        tls_client_cert_t cc;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        lockdown_close();
        ck("close() shut the TLS session", ft.closes == 1, ft.closes);
        /* The ORDER: the close_notify has to travel down a channel
           that is still open. */
        ck("and the mux was still up when it did",
           ft.close_saw_connected == 1, 0);
        ck("no session is left", lockdown_session() == 0, 0);
        ck("and no encrypted one", lockdown_ssl() == 0, 0);

        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        lockdown_forget();
        ck("forget() also releases the TLS slot", ft.closes == 1,
           ft.closes);
        /* The OPPOSITE order, deliberately: `usb rescan` calls this
           because the device may have been unplugged, and a farewell
           packet to a device that is not there burns the poll budget.
           The slot still has to be freed or the third handshake finds
           none. */
        ck("but the connection was already gone",
           ft.close_saw_connected == 0, 0);
        ck("and nothing is left", lockdown_session() == 0 &&
           lockdown_ssl() == 0, 0);
    }

    head("the transport this file hands to net/tls.c");
    {
        tls_client_cert_t cc;
        uint8_t buf[64];
        const tls_transport_t *tr;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        tr = ft.tr;

        ck("eof is false while the channel is up", tr->eof(0) == 0, 0);
        /* Success is any NON-NEGATIVE value and the count is not
           compared -- include/tls.h says why: net/tcp.c returns 0 for
           a complete write, so a caller comparing the result against
           the length would reject every successful send. The sign is
           the whole of the signal. */
        {
            uint8_t probe[8] = { 1,2,3,4,5,6,7,8 };
            ck("a healthy send is non-negative",
               tr->send(0, probe, 8) >= 0, 0);

            /* ── THE LATCH, ON A HEALTHY CHANNEL ──────────────────────
               Set directly, which is the only way to isolate it. The
               real latch is set by a partial send -- and a partial
               send also drops the connection, so down there the
               "connected" guard refuses first and removing the latch
               changes nothing the suite can see. The injection
               harness needed this.

               g_ld is static in the driver and this suite includes
               it, so the flag itself is the assertion rather than a
               predicate invented for the test -- the same argument
               the stall checks above make. */
            g_ld.ssl_broken = 1;
            ck("a send refuses while the stream is known misaligned",
               tr->send(0, probe, 8) < 0, 0);
            g_ld.ssl_broken = 0;
            ck("and sends again once the latch is cleared",
               tr->send(0, probe, 8) >= 0, 0);
        }
        /* A read with nothing waiting is NOT an error. A transport
           that reported a timeout as a dead connection would turn a
           slow device into a broken one -- net/tls.c's read loop asks
           again and counts the time itself. */
        ck("a read with nothing waiting returns 0, not an error",
           tr->recv(0, buf, (uint16_t)sizeof buf, 1) == 0, 0);

        /* And once the channel is gone, every one of them refuses --
           which is what makes tls_close()'s close_notify safe to
           attempt after lockdown_forget() has already dropped the
           connection. */
        g_ld.connected = 0;
        ck("with no connection, send refuses",
           tr->send(0, buf, 4) < 0, 0);
        ck("recv refuses", tr->recv(0, buf, 4, 1) < 0, 0);
        ck("and eof is true", tr->eof(0) == 1, 0);
        g_ld.connected = 1;
    }

    head("a TLS record that goes out HALF WAY cannot be recovered from");
    {
        tls_client_cert_t cc;
        const tls_transport_t *tr;
        static uint8_t big[8192];
        memset(&cc, 0, sizeof cc);
        memset(big, 'Q', sizeof big);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        tr = ft.tr;

        /* Half a record leaves the peer reading the next one at the
           wrong offset, and nothing reports an error: it decrypts to
           rubbish and fails a MAC check several messages later.
           ld_send_plist() has the same rule one layer down.

           THE STALL IS RELATIVE. ph.stall_out_on counts bulk OUTs
           since the machine was reset, and by this point the
           connection, the StartSession and the handshake have all
           used some -- the first version of this check used a fixed 8,
           which was long past, so nothing stalled and the send failed
           for an unrelated reason while the check still read as
           "fails". Counting from here means the stall lands INSIDE
           the transfer: +2 is past the first segment, so bytes
           genuinely go out before it breaks. */
        {
            uint32_t bytes_before = ph.host_bytes;
            /* +3 since v0.8.9.7: the first segment is a full packet,
               so +2 is its zero-length packet. Stalling THAT leaves the
               first transfer unterminated, so the device never takes
               its bytes -- which is what a real phone would do, and is
               not the case this check is about. */
            ph.stall_out_on = ph.out_calls + 3;
            ck("the send fails",
               tr->send(0, big, (uint16_t)sizeof big) < 0, 0);
            ck("but some bytes HAD gone out first",
               ph.host_bytes > bytes_before,
               (long)(ph.host_bytes - bytes_before));
        }
        ck("the encrypted session is abandoned", lockdown_ssl() == 0, 0);
        ck("and the mux connection with it", g_ld.connected == 0, 0);
        /* LATCHED, and tested WITH THE CONNECTION PUT BACK. The
           injection harness needed that: with g_ld.connected left at
           zero the first guard in ld_tr_send() refuses anyway, so
           removing the latch changed nothing the suite could see.
           Restoring the connection leaves the latch as the only thing
           standing between a later send and a stream that is
           permanently misaligned. */
        ph.stall_out_on = 0;
        g_ld.connected = 1;
        ck("the latch is set", g_ld.ssl_broken == 1, g_ld.ssl_broken);
        ck("a later send refuses even with the channel back",
           tr->send(0, big, 16) < 0, 0);
        /* No "and now it works again" check here: the partial send
           disconnected the mux, so ld_tr_send()'s first guard refuses
           whatever the latch says. The sign contract is asserted
           above, on a healthy channel. */
    }

    /* ── v0.8.9.9: THE TRANSPORT, READ THE WAY net/tls.c READS ────────
       Every TLS check above runs against a FAKE TLS layer, which reads
       whatever a packet holds in one call -- and that is why none of
       them could see 0.8.9.8's handshake die on the HP. The real
       net/tls.c asks the transport for FIVE bytes (a record header),
       then for the body. The phone answers a ClientHello with about a
       kilobyte in one packet. This drives the real ld_tr_recv() over
       the real usbmux.c exactly so: 5, then the rest. */
    head("the TLS transport hands a big packet over five bytes first");
    {
        tls_client_cert_t cc;
        const tls_transport_t *tr;
        static uint8_t flight[900], got[900];
        int i, r, have = 0, guard = 0;
        memset(&cc, 0, sizeof cc);
        machine();
        lockdown_connect(); ld.session = 1;
        lockdown_start_session("A-B", "C-D");
        lockdown_enable_ssl(&cc, 1u);
        tr = ft.tr;
        for (i = 0; i < (int)sizeof flight; i++) flight[i] = (uint8_t)(i ^ 0x5A);
        flight[0] = 22; flight[1] = 3; flight[2] = 3;   /* handshake, TLS 1.2 */
        flight[3] = (uint8_t)((sizeof flight - 5) >> 8);
        flight[4] = (uint8_t)((sizeof flight - 5) & 0xFF);
        queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, flight, (int)sizeof flight);
        ph.dev_seq += sizeof flight;
        r = tr ? tr->recv(0, got, 5, 500) : -99;
        ck("five bytes of record header, not a dead connection", r == 5, r);
        ck("the header is the record's", got[0] == 22 && got[3] ==
           (uint8_t)((sizeof flight - 5) >> 8), got[0]);
        have = r > 0 ? r : 0;
        while (tr && have < (int)sizeof flight && guard++ < 100) {
            r = tr->recv(0, got + have, (uint16_t)(sizeof flight - have), 500);
            if (r < 0) break;
            have += r;
        }
        ck("then the body, all of it", have == (int)sizeof flight, have);
        ck("byte for byte", memcmp(got, flight, sizeof flight) == 0, 0);
        ck("and the channel is still up", g_ld.connected && usbmux_connected(),
           g_ld.connected);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
