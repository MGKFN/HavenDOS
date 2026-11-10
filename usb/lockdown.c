/* ════════════════════════════════════════════════════════════════════
   usb/lockdown.c -- lockdownd, stage 1: does it answer QueryType.

   include/lockdown.h carries the argument. In one line: the phone's
   Ethernet interface is offered without a trust handshake and is not
   usable without one, pairing goes through lockdownd, and lockdownd is
   a plist conversation over the mux connection usb/usbmux.c opened on
   26 October.

   This file sends one request and reads one reply. It does not pair.
   It does not have an XML parser and does not pretend to -- stage 1 is
   a single string and a scanner that knows it is a scanner cannot
   misread a structure it was never looking at.

   ── WHERE THE FRAMING COMES FROM ─────────────────────────────────────

   libimobiledevice, common/property_list_service.c:

     internal_plist_send()            nlen = htobe32(length);
                                      service_send(&nlen, 4);
                                      service_send(content, length);

     internal_plist_receive_timeout()  read 4; pktlen = be32toh(pktlen);
                                       read pktlen into content

   Two things fall out of that and both matter more than they look:

     - THE LENGTH DOES NOT COUNT ITSELF, which is the opposite of the
       mux header four bytes below it in the same stream.
     - THE DEVICE SENDS IT IN TWO WRITES, so the four length bytes and
       the XML arrive as separate packets. This accumulates.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/usbmux.h"
#include "../include/lockdown.h"
#include "../include/b64.h"
#include "../include/klog.h"
/* v0.8.9: usb/ reaching into net/ for the first time, which is worth a
   sentence. The session SSL lockdownd asks for is TLS, net/tls.c is
   the only TLS in the tree, and the alternative -- a second TLS inside
   a USB driver -- is not a trade worth considering. The dependency is
   one way: net/tls.c knows nothing about usbmux and is handed a
   transport. usb/ethernet.c already includes net/netdev.h for the same
   kind of reason. */
#include "../include/tls.h"
/* get_ticks(), in 10 ms units. Included rather than hand-declared:
   include/pit.h exists precisely because seven files used to carry
   their own copy of that extern and one of them called it through an
   implicit declaration. */
#include "../include/pit.h"

/* ── sizes, and what each one is actually bounded by ──────────────────

   LD_BUF holds ONE REPLY, accumulated across packets. It is not
   bounded by what a packet can carry -- usbmux_max_payload() is that,
   and it is smaller -- so the two are separate numbers on purpose and
   tests/test_lockdown.c asserts the relationship rather than either of
   them agreeing by luck.

   A QueryType reply is about 250 bytes. Stage 2's GetValue for
   DevicePublicKey is a 2048-bit RSA key in base64 inside an XML
   plist, around 800. 4096 leaves room for stage 2 without pretending
   this can hold a reply of any size: a bigger one is REFUSED by name,
   because a truncated plist that still contains the string being
   looked for is the worst outcome available.

   WHY LD_BUF IS NOT IN tools/check_usb_buffers.py's TABLE. That check
   compares a driver's transfer constants against the bounds its own
   back-end enforces, and LD_BUF is not a transfer constant: nothing
   is ever handed to the controller with this size. usbmux_recv() is
   given `LD_BUF - have` as the room REMAINING, and the transfer
   itself is bounded by MUX_BUF, which is in that table. The
   relationship that does matter -- LD_BUF holds more than one packet,
   and a packet fits MUX_BUF -- is asserted in tests/test_lockdown.c
   rather than left to agree by luck. */
#define LD_BUF   4096

/* ── stage 2's decoded value ──────────────────────────────────────────
   A DevicePublicKey is a PEM RSA public key, around 420 bytes, which
   arrives base64'd inside the reply's <data> element at about 570. A
   kilobyte leaves room without pretending this holds a value of any
   size: a bigger one is refused by ld_b64() rather than truncated,
   because half a key that still looks like a key is the worst outcome
   available. */
#define LD_VAL   1024

/* lockdownd answers in milliseconds, but nothing in this kernel runs
   on a timer, so this is a bounded poll like every other wait here.
   The reply arrives in at least two packets, so the budget is per
   RECEIVE rather than for the whole exchange -- a single count shared
   across both would be spent by the first one. */
/* ── HOW LONG TO WAIT FOR A REPLY -- v0.8.9.3 ────────────────────────
   This was LD_POLL_TRIES 20000, a COUNT of polls, and the comment
   beside the usbmux transport even described it as "a ten-second
   budget". It was nothing of the kind. A usbmux_recv() with nothing
   waiting is one bulk-IN that NAKs and returns immediately, so on
   Kyan's HP twenty thousand of them finished in under ten
   milliseconds -- the kernel log has the GetValue going out and "no
   reply at all" at the same 10 ms tick.

   An iPhone answers QueryType almost at once and takes appreciably
   longer over GetValue DevicePublicKey, because it has to go and get
   the key. So stage 1 worked, stage 2 always timed out, and the
   reply turned up a moment later -- in time to be read as the answer
   to THE NEXT REQUEST. From then on every reply was one request
   behind, for ever:

       QueryType -> the GetValue reply   (918 bytes)
       GetValue  -> the QueryType reply  (301 bytes)

   A budget measured in polls is a budget whose length depends on how
   fast the host controller says no. Time is what was meant and time
   is what is counted now. get_ticks() is 10 ms UNITS -- this project
   has got that wrong twice, so it is spelled out at every use. */
#define LD_REPLY_TICKS   500     /* 500 * 10 ms = 5 seconds */
#define LD_FIRST_TICKS   1000    /* 10 s for the first reply of a
                                    connection: the device may still be
                                    waking the service up */

/* ── the request ──────────────────────────────────────────────────────

   What libimobiledevice's lockdownd_query_type() builds: a dict with
   a Label and Request. plist_to_xml() emits the DOCTYPE and tabs;
   this is the same document written out by hand, because a plist
   WRITER is stage 3's problem and one fixed request does not need one.

   The Label is the client's name and lockdownd only shows it to the
   user in the trust prompt. It is ours to choose, and choosing
   something recognisable means the prompt on the phone says who is
   asking -- which, on a release whose whole purpose is finding out
   whether the phone trusts this host, is worth more than brevity. */
static const char LD_QUERYTYPE[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Label</key>\n"
    "\t<string>HavenDOS</string>\n"
    "\t<key>Request</key>\n"
    "\t<string>QueryType</string>\n"
    "</dict>\n"
    "</plist>\n";

/* v0.8.9: the handle net/tls.c files this session under. NOT an fd --
   see the transport note further down -- and chosen so it cannot
   collide with the small integers net/tcp.c hands out. 'L','D'. */
#define LD_TLS_HANDLE  0x4C44

typedef struct {
    int      connected;

    /* the last reply, as it arrived. One spare byte so the scanner can
       be given a terminator without writing past the end. */
    uint8_t  reply[LD_BUF + 1];
    int      reply_len;
    int      have_reply;
    int      packets;        /* how many TCP packets it took */

    /* what the scanner found in it */
    char     type[64];
    char     error[64];

    /* stage 2: the decoded <data>, with a spare byte for a
       terminator. val_len is the truth -- a DER key has zero bytes in
       it and stage 3 will want the bytes, not the string. */
    uint8_t  val[LD_VAL + 1];
    int      val_len;

    uint32_t sent, recvd;

    /* ── v0.8.9: the session ──────────────────────────────────────────
       StartSession hands back a SessionID and, on every iOS this will
       meet, asks for the rest of the conversation to be encrypted. The
       two are separate facts: a session can exist without SSL (older
       iOS, and the StopSession request has to be sendable either way),
       and ssl_on is set only once the handshake has actually
       succeeded -- never from what the device ASKED for, because a
       flag set on intent is a flag that routes plaintext through the
       encrypted path. */
    char     session_id[96];
    int      session;        /* a SessionID came back            */
    int      ssl_wanted;     /* the device asked for SSL         */
    int      ssl_on;         /* the handshake succeeded          */
    int      ssl_broken;     /* a record went out half way       */
    uint32_t tls_tx, tls_rx;

    /* ── v0.8.9.5 ────────────────────────────────────────────────────
       The reason the last request failed, in words. Set at the point
       the decision is taken; read by whatever is going to put it on a
       screen. See usbmux_why() for why a number is not enough. */
    const char *why;

    /* How long the NEXT reply may take, in 10 ms ticks, or 0 for the
       ordinary budget. One request in this protocol waits for a human
       to tap a dialog, and a budget sized for a machine is the wrong
       budget for it. Cleared after it is used, so it cannot leak into
       the request after the one it was set for. */
    uint32_t next_budget;
} ld_t;

static ld_t g_ld;

static uint32_t ld_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static void ld_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

/* ── a bounded scanner, not a parser ─────────────────────────────────
   strstr() would need the buffer NUL-terminated and would stop at the
   first NUL inside it. A plist is text today, but "the reply is text"
   is an assumption about somebody else's service and this file's whole
   job is to stop making those. Length-bounded, so a reply full of
   zeroes is searched correctly and never read past. */
static int ld_find(const uint8_t *hay, int haylen, const char *needle)
{
    int n = (int)strlen(needle);
    int i, j;

    if (n == 0 || n > haylen) return -1;
    for (i = 0; i + n <= haylen; i++) {
        for (j = 0; j < n; j++)
            if (hay[i + j] != (uint8_t)needle[j]) break;
        if (j == n) return i;
    }
    return -1;
}

/* Find <key>NAME</key> and copy the <string> that follows it into
   `out`. Returns 1 if it found one.

   Deliberately dumb: it takes the FIRST <string> after the key and
   does not care about the structure around it. For a flat dict --
   which every lockdownd reply at stages 1 and 2 is -- that is right,
   and where it would be wrong it is wrong VISIBLY, because
   lockdown_dump() prints the bytes it was looking at. */
static int ld_key_string(const char *key, char *out, int outsz)
{
    char pat[48];
    int  at, s, e, n;

    out[0] = 0;
    if (!g_ld.have_reply) return 0;

    /* "<key>" + key + "</key>" */
    if ((int)strlen(key) + 12 >= (int)sizeof pat) return 0;
    strcpy(pat, "<key>");
    strcat(pat, key);
    strcat(pat, "</key>");

    at = ld_find(g_ld.reply, g_ld.reply_len, pat);
    if (at < 0) return 0;

    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<string>");
    if (s < 0) return 0;
    s += at + 8;

    e = ld_find(g_ld.reply + s, g_ld.reply_len - s, "</string>");
    if (e < 0) return 0;

    n = e;
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, g_ld.reply + s, (uint32_t)n);
    out[n] = 0;
    return 1;
}

/* ── the connection ────────────────────────────────────────────────── */

int lockdown_connect(void)
{
    int r;

    if (g_ld.connected && usbmux_connected()) return USB_OK;

    memset(&g_ld, 0, sizeof g_ld);

    if (!usbmux_present()) {
        r = usbmux_open();
        if (r < 0) {
            klog(KLOG_WARN, "lockdown: no Apple Mobile Device interface "
                 "to talk over (%d)", (uint32_t)r);
            return r;
        }
    }

    r = usbmux_connect(MUX_PORT_LOCKDOWN);
    if (r < 0) {
        /* Three distinguishable outcomes, named individually. 0.8.6.5's
           lesson was that "it only found the ESP" turned out to be four
           reasons wearing one face. */
        if (usbmux_refused())
            klog(KLOG_WARN, "lockdown: port %u is CLOSED on the device -- "
                 "the channel works, lockdownd is not listening",
                 (uint32_t)MUX_PORT_LOCKDOWN);
        else if (r == USB_ETIMEOUT)
            klog(KLOG_WARN, "lockdown: nothing answered the SYN to port "
                 "%u", (uint32_t)MUX_PORT_LOCKDOWN);
        else
            klog(KLOG_WARN, "lockdown: the connection to port %u failed "
                 "(%d)", (uint32_t)MUX_PORT_LOCKDOWN, (uint32_t)r);
        return r;
    }

    g_ld.connected = 1;
    klog(KLOG_OK, "lockdown: connected to port %u",
         (uint32_t)MUX_PORT_LOCKDOWN);
    return USB_OK;
}

void lockdown_close(void)
{
    /* v0.8.9: the TLS session goes first, so its close_notify travels
       down a channel that is still open. The other order sends an
       encrypted alert into a disconnected mux and logs a transfer
       failure for a connection nobody wanted any more. */
    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;

    /* Polite: tell the device the connection is going. Only if the
       transport is still up -- an RST down a channel that has already
       broken is a transfer that times out for no benefit. */
    if (g_ld.connected && usbmux_connected()) usbmux_disconnect();
    g_ld.connected = 0;
}

void lockdown_forget(void)
{
    /* NOT polite, and that is the point. `usb rescan` calls this
       before usb_enumerate() rewalks the bus, and the device this
       connection belongs to may be the one that was just unplugged --
       which is why the rescan is happening. A farewell packet to it
       is a transfer that burns the whole poll budget waiting for a
       device that is not there.

       usbmux_close() says the same thing about the same moment. The
       two names exist so the choice is visible at the call site
       instead of depending on what somebody assumed close() does. */

    /* v0.8.9: the TLS session is dropped WITHOUT a close_notify, for
       the same reason. tls_close() would encrypt an alert and hand it
       to the transport, which would try to send it to a device that is
       not there. The slot has to be freed either way or the third
       handshake on this handle finds no slot -- so the session is
       released by handing tls_close() a channel it will refuse to
       write to: ld_tr_send() checks g_ld.connected, which is already
       zero by the time it is asked. */
    g_ld.connected = 0;
    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    g_ld.ssl_broken = 0;
}

/* ── how big a REQUEST can be ─────────────────────────────────────────

   Separate from LD_BUF, which bounds a REPLY, because the two are
   bounded by different things and the one time they were the same
   number it was by accident.

   The largest request stage 3 sends is Pair: a dict holding a device
   certificate, a host certificate, a root certificate and a host
   private key, each a few hundred DER bytes expanded by base64 to
   four thirds of that and then wrapped. tests/test_plist.c builds
   that exact message at full size and it comes to a little over five
   kilobytes. Twelve leaves room for a device whose certificates are
   larger than the fixture's without pretending this holds a request
   of any size: a bigger one is refused by name.

   This is BSS in a freestanding kernel, not a malloc. It is one
   buffer because there is one connection -- the same reason
   usb/usbmux.c keeps one. */
#define LD_REQ   12288

/* ── sending a plist ─────────────────────────────────────────────────

   The length prefix and the body are ONE call rather than the two
   libimobiledevice uses. Nothing requires two: TCP is a byte stream
   and lockdownd reads four bytes and then that many.

   ── v0.8.9.0: IT NO LONGER HAS TO FIT A PACKET ──────────────────────

   This used to refuse anything over usbmux_max_payload() -- 2012
   bytes -- by name, and the refusal said "not written yet" about
   exactly this. usbmux_send_stream() is now what splits it, inside
   the transport where the sequence numbers and the device's window
   live. The bound that remains is LD_REQ, which is about the buffer
   and not about the wire.

   The failure that matters is a PARTIAL send: once some segments are
   out, lockdownd is mid-length-prefix and no later request can be
   parsed on that connection. There is no recovery short of tearing it
   down, so that is what happens, and the byte count says where it
   stopped. Retrying into a stream at an unknown offset is how a
   transport starts answering the wrong question. */
/* ════════════════════════════════════════════════════════════════════
   v0.8.9: TLS OVER THE MUX CHANNEL

   lockdownd answers StartSession with EnableSessionSSL, and everything
   after that point is a TLS session in which THE DEVICE IS THE SERVER
   and this host is a client presenting a client certificate -- the
   host certificate out of the pair record. That is the whole reason
   net/tls.c grew a client-certificate path in this release.

   net/tls.c used to call net/tcp.c by name. It takes a transport now,
   and this is the other one. Four functions, each a forwarding, and
   the three things worth knowing about them:

     - THE HANDLE IS NOT AN FD. net/tls.c uses it only to pick a
       session slot, so it must simply not collide with a TCP fd.
       0x4C44 is 'L','D' and is nowhere near the small integers
       net/tcp.c hands out.

     - RECV HAS NO BLOCKING CALL UNDERNEATH IT. usbmux_recv() returns
       immediately with 0 when nothing has arrived, and this file may
       not sleep -- include/rsa.h's rule, enforced by
       tools/check_kpoll_sites.py, keeps kpoll_sleep_ms() out of usb/
       entirely. So a timeout becomes a TRY BUDGET, the same shape
       ld_recv_plist() has always used, and the comment says so rather
       than letting a reader believe milliseconds are being honoured.

     - A PARTIAL SEND IS FATAL AND IS LATCHED. Half a TLS record on
       the wire leaves the peer reading the next record at the wrong
       offset, and nothing reports an error -- it decrypts to rubbish
       and fails a MAC check several messages later. ld_send_plist()
       has the same rule one layer down for the same reason; here the
       flag means no later call can quietly carry on.
   ════════════════════════════════════════════════════════════════════ */
/* usbmux_recv() costs one poll of the host controller. 40 tries per
   advertised millisecond is what ld_recv_plist()'s LD_POLL_TRIES works
   out to for its own ten-second budget, and keeping the same ratio
   means the two paths wait for about the same length of time on the
   same machine -- which matters when the thing being diagnosed is a
   device that stopped answering. */
#define LD_TLS_TRIES_PER_MS 40

static int ld_tr_send(int h, const uint8_t *d, uint16_t len)
{
    int sent = 0, r;
    (void)h;
    if (!g_ld.connected || !usbmux_connected()) return -1;
    if (g_ld.ssl_broken) return -1;

    r = usbmux_send_stream(d, (int)len, &sent);
    if (r < 0) {
        if (sent > 0) {
            klog(KLOG_WARN, "lockdown: %d of %d TLS bytes went out -- "
                 "the record stream is misaligned and cannot be reused",
                 (uint32_t)sent, (uint32_t)len);
            g_ld.ssl_broken = 1;
            g_ld.ssl_on     = 0;
            g_ld.connected  = 0;
            usbmux_disconnect();
        }
        return -1;
    }
    g_ld.tls_tx++;
    /* 0, not len: see include/tls.h on why the count is not
       meaningful. net/tcp.c returns 0 for a complete write and both
       transports have to mean the same thing by their return. */
    return 0;
}

static int ld_tr_recv(int h, uint8_t *buf, uint16_t max, uint32_t timeout_ms)
{
    uint32_t budget = timeout_ms * LD_TLS_TRIES_PER_MS;
    uint32_t i;
    int r;
    (void)h;
    if (!g_ld.connected || !usbmux_connected()) return -1;
    if (!budget) budget = 1;
    for (i = 0; i < budget; i++) {
        r = usbmux_recv(buf, (int)max);
        if (r < 0) return -1;
        if (r > 0) { g_ld.tls_rx++; return r; }
    }
    /* Nothing yet. NOT an error: net/tls.c's read loop asks again and
       counts the time itself, and a transport that reported a timeout
       as a dead connection would turn a slow device into a broken
       one. */
    return 0;
}

static int ld_tr_eof(int h)
{
    (void)h;
    return (!g_ld.connected || !usbmux_connected()) ? 1 : 0;
}

/* No close. The mux channel belongs to this file and is taken down by
   lockdown_close(); a transport that closed it would mean tls_close()
   dropping the connection out from under lockdown_stop_session(). See
   tls_transport_t::close. */
static const tls_transport_t LD_TRANSPORT = {
    "usbmux", ld_tr_send, ld_tr_recv, ld_tr_eof, 0
};

static int ld_send_plist(const char *xml)
{
    static uint8_t out[LD_REQ];
    int len = (int)strlen(xml);
    int sent = 0, r;

    if (!g_ld.connected || !usbmux_connected()) return USB_ENODEV;
    if (len + 4 > (int)sizeof out) {
        klog(KLOG_WARN, "lockdown: a %d-byte request is more than this "
             "can stage (%d)", (uint32_t)(len + 4),
             (uint32_t)sizeof out);
        return USB_EPARAM;
    }

    /* THE LENGTH DOES NOT COUNT ITSELF. The mux header four bytes
       below in the same stream does. */
    ld_wr32(out, (uint32_t)len);
    memcpy(out + 4, xml, (uint32_t)len);

    /* v0.8.9: once the session is encrypted the framing goes inside
       the TLS record layer, not beside it. One branch, here, rather
       than at every call site: a request that took the plaintext path
       by accident would reach lockdownd as rubbish and be answered
       with silence. */
    if (g_ld.ssl_on) {
        r = tls_send(LD_TLS_HANDLE, out, (uint16_t)(len + 4));
        if (r < 0) {
            g_ld.why = "the encrypted request would not go out";
            klog(KLOG_WARN, "lockdown: the encrypted request would not "
                 "send (%d)", (uint32_t)r);
            return USB_EXFER;
        }
        g_ld.sent++;
        return USB_OK;
    }

    r = usbmux_send_stream(out, len + 4, &sent);
    if (r < 0) {
        g_ld.why = usbmux_why();
        klog(KLOG_WARN, "lockdown: the request would not send (%d)",
             (uint32_t)r);
        if (sent > 0) {
            /* Half a request is worse than none: the connection is
               now at an offset lockdownd cannot parse from. Drop it
               so the next command reconnects rather than talking into
               a stream that is permanently misaligned. */
            klog(KLOG_WARN, "lockdown: %d of %d bytes went out -- "
                 "dropping the connection, it cannot be reused",
                 (uint32_t)sent, (uint32_t)(len + 4));
            g_ld.connected = 0;
            usbmux_disconnect();
        }
        return r;
    }
    g_ld.sent++;
    return USB_OK;
}

/* ── what the send path was proved against ───────────────────────────
   Eight defects injected into ld_send_plist(), each with a prediction
   written first. All eight are caught, but two of them only after the
   suite was given the checks they needed, and both are worth naming:

     - A stage bound of `len > sizeof out` instead of `len + 4 >
       sizeof out` accepts exactly four body lengths and copies up to
       four bytes past the end of a static array. Every oversize
       document the suite had was oversize by more than four, so both
       bounds refused it and the injection survived. It is now checked
       AT THE BYTE: LD_REQ-4 accepted, LD_REQ-3 refused.

     - Tearing the connection down on `sent >= 0` rather than
       `sent > 0` survived because every failure the suite could
       produce had already put bytes on the wire. A request whose
       FIRST segment never leaves is a request lockdownd has not seen
       a byte of, so the stream is still aligned and the channel is
       fine; throwing it away there costs a working connection for
       nothing. The suite now stalls the first segment and the fifth
       separately and requires the opposite outcome from each.

   Both were the same shape of gap: a condition whose two halves the
   tests could not tell apart, because every input happened to
   satisfy both. ── v0.8.9.0 */

/* ── reading a plist ─────────────────────────────────────────────────

   The reply arrives in at least two packets, because the device writes
   the length and the body separately. This accumulates until it has
   the whole thing, and KEEPS WHAT IT COLLECTED whatever happens --
   including a reply that never completed, which is the one most worth
   looking at. */
/* ── A READ THAT GAVE UP POISONS THE CHANNEL ─────────────────────────
   v0.8.9.3, and it is the other half of the timeout fix.

   lockdownd answers every request. If this side stops waiting, the
   answer still arrives -- and it arrives into a byte stream whose next
   reader is the NEXT request, which then gets the previous request's
   reply. Every reply after that is one behind, each one perfectly
   framed and perfectly wrong, and nothing reports an error. That is
   exactly what the HP did for an hour.

   Lengthening the budget makes it rare. It cannot make it impossible:
   a device that is busy enough will always beat any budget. So the
   channel is torn down instead. The next request reconnects, costs one
   SYN, and starts from a stream whose position both ends agree on.

   ld_send_plist() has had this rule for a partial send since 0.8.8.10,
   for the identical reason stated the identical way. The read side
   simply never got it. */
static void ld_desync(const char *why)
{
    klog(KLOG_WARN, "lockdown: %s -- dropping the channel rather than "
         "reading the answer as the next request's", why);
    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    if (usbmux_connected()) usbmux_disconnect();
    g_ld.connected = 0;
}

static int ld_recv_plist(void)
{
    int have = 0, need = -1, r;
    uint32_t t0, budget;

    if (!g_ld.connected || !usbmux_connected()) return USB_ENODEV;

    budget = g_ld.recvd ? LD_REPLY_TICKS : LD_FIRST_TICKS;
    if (g_ld.next_budget) {
        budget = g_ld.next_budget;
        g_ld.next_budget = 0;   /* this request only */
    }
    t0 = get_ticks();

    g_ld.reply_len  = 0;
    g_ld.have_reply = 0;
    g_ld.packets    = 0;
    g_ld.type[0]    = 0;
    g_ld.error[0]   = 0;
    g_ld.val_len    = 0;
    g_ld.val[0]     = 0;

    for (;;) {
        if (get_ticks() - t0 > budget) break;   /* 10 ms units */
        /* Read straight into the accumulation buffer at the offset
           already filled. A separate packet buffer would need to be
           the size of a packet AND be copied, and the only thing the
           copy could add is a second bound to get wrong. */
        r = g_ld.ssl_on
            ? tls_recv(LD_TLS_HANDLE, g_ld.reply + have,
                       (uint16_t)(LD_BUF - have), 500)
            : usbmux_recv(g_ld.reply + have, LD_BUF - have);
        if (r < 0) {
            /* Keep what arrived. A reply that broke half way through
               is evidence; discarding it on the way out is how the
               only informative screen in a release gets thrown away. */
            g_ld.reply_len  = have;
            g_ld.have_reply = have > 0;
            /* v0.8.9.5: the transport knows WHY and this does not.
               Asking it beats inventing a sentence from a code. */
            g_ld.why = g_ld.ssl_on ? "the encrypted channel broke while "
                                     "the reply was arriving"
                                   : usbmux_why();
            return r;
        }
        if (r == 0) continue;

        have += r;
        g_ld.packets++;
        t0 = get_ticks();       /* progress resets the budget */

        if (need < 0 && have >= 4) {
            need = 4 + (int)ld_be32(g_ld.reply);
            if (need < 4 || need > LD_BUF) {
                klog(KLOG_WARN, "lockdown: the reply says %d bytes, "
                     "which is more than this can hold (%d)",
                     (uint32_t)(need - 4), (uint32_t)(LD_BUF - 4));
                g_ld.reply_len  = have;
                g_ld.have_reply = 1;
                return USB_EPARAM;
            }
        }
        if (need >= 0 && have >= need) break;
    }

    g_ld.reply_len  = have;
    g_ld.have_reply = have > 0;

    if (have == 0) {
        g_ld.why = "the device said nothing at all before the budget "
                   "ran out";
        klog(KLOG_WARN, "lockdown: no reply at all");
        ld_desync("a request went unanswered");
        return USB_ETIMEOUT;
    }
    if (need < 0) {
        g_ld.why = "fewer bytes arrived than the four-byte length "
                   "prefix a reply begins with";
        klog(KLOG_WARN, "lockdown: %d bytes arrived, fewer than the "
             "four-byte length prefix", (uint32_t)have);
        ld_desync("a reply shorter than its own length prefix");
        return USB_EXFER;
    }
    if (have < need) {
        g_ld.why = "the reply stopped part way through";
        klog(KLOG_WARN, "lockdown: %d of %d bytes arrived -- the reply "
             "stopped part way", (uint32_t)have, (uint32_t)need);
        ld_desync("a reply that stopped part way");
        return USB_EXFER;
    }
    if (have > need)
        klog(KLOG_INFO, "lockdown: %d bytes past the end of the reply",
             (uint32_t)(have - need));

    g_ld.recvd++;
    g_ld.why = 0;
    /* The scanner is given a terminator, inside the spare byte the
       buffer carries for it. Nothing below relies on it -- ld_find()
       is length-bounded -- but a buffer handed to anything that does
       not know its length should not be the thing that bites. */
    g_ld.reply[have] = 0;
    return need - 4;
}

/* ── stage 1 ─────────────────────────────────────────────────────── */

int lockdown_query_type(void)
{
    int r;

    if (!g_ld.connected || !usbmux_connected()) {
        klog(KLOG_WARN, "lockdown: not connected");
        return USB_ENODEV;
    }

    r = ld_send_plist(LD_QUERYTYPE);
    if (r < 0) return r;
    klog(KLOG_INFO, "lockdown: QueryType sent, %d bytes",
         (uint32_t)(strlen(LD_QUERYTYPE) + 4));

    r = ld_recv_plist();
    if (r < 0) return r;

    ld_key_string("Type",  g_ld.type,  (int)sizeof g_ld.type);
    ld_key_string("Error", g_ld.error, (int)sizeof g_ld.error);

    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: the device answered with an Error");
        return 0;
    }
    if (!g_ld.type[0]) {
        klog(KLOG_WARN, "lockdown: a %d-byte reply with no Type in it",
             (uint32_t)r);
        return 0;
    }
    if (!strcmp(g_ld.type, LOCKDOWN_TYPE)) {
        klog(KLOG_OK, "lockdown: LOCKDOWND ANSWERED -- %s", g_ld.type);
        return 1;
    }
    klog(KLOG_WARN, "lockdown: answered type %s, expected %s",
         g_ld.type, LOCKDOWN_TYPE);
    return 0;
}

const char *lockdown_type(void)  { return g_ld.type; }
const char *lockdown_error(void) { return g_ld.error; }

/* ── STAGE 2: base64, because <data> is not <string> ──────────────────

   WHY THIS IS NOT shell/shell_v073.c's b64_decode_buf(). There is
   already a base64 decoder in this tree and it cannot be used here,
   which is worth saying out loud because "there is already one" is the
   first thing a reader will think:

     - it is `static` inside a three-thousand-line shell file, so
       nothing outside that file can reach it. That is the same shape
       as the disk table v0.8.6 had to move OUT of shell/iso_installer.c
       before tests/test_diskname.c could link it.
     - it walks a NUL-TERMINATED string and stops at the first
       character not in the alphabet. A plist's <data> element is
       wrapped across lines with tabs and newlines INSIDE it, so that
       decoder would stop at the end of the first line and return a
       sixth of the key.

   So this one is length-bounded and skips whitespace, which is what
   the job needs. Two base64 decoders in one tree is a smell and it is
   recorded in HANDOFF.md rather than left to be discovered; merging
   them means moving the shell's out of a file full of VGA drawing,
   which is a refactor and not this release.

   It REFUSES rather than guessing: a character that is neither
   whitespace nor base64 means the region is not what this thinks it
   is, and silently skipping it would turn a wrong guess about the
   framing into a shorter key that still looks like a key. */
/* ── v0.8.8.11: ONE base64 DECODER IN THE TREE, AND IT IS net/b64.c ──
   This used to be the second one, and the comment here said so:
   "two base64 decoders in one tree is a smell". Stage 3 needs the
   decoder from net/ as well -- the device's public key is base64
   inside a plist value and PEM inside that -- so a third copy was the
   alternative to consolidating. net/b64.c's decoder is this one's
   rules plus two it did not enforce: a trailing quad must be
   complete, and the leftover bits must be zero. */
static int ld_b64(const uint8_t *in, int inlen, uint8_t *out, int outsz)
{
    return b64_decode(in, inlen, out, outsz);
}

/* Find <key>NAME</key>, then the <data>...</data> after it, and decode
   it into g_ld.val. Returns the byte count, or -1.

   Same deliberate dumbness as ld_key_string(): the FIRST <data> after
   the key, with no regard for the structure around it. For the flat
   dict every lockdownd reply at this stage is, that is right -- and
   where it is wrong it is wrong visibly, because lockdown_dump()
   prints the bytes it was looking at. */
static int ld_key_data(const char *key)
{
    char pat[48];
    int  at, s, e, n;

    g_ld.val_len = 0;
    g_ld.val[0]  = 0;
    if (!g_ld.have_reply) return -1;

    if ((int)strlen(key) + 12 >= (int)sizeof pat) return -1;
    strcpy(pat, "<key>");
    strcat(pat, key);
    strcat(pat, "</key>");

    at = ld_find(g_ld.reply, g_ld.reply_len, pat);
    if (at < 0) return -1;

    s = ld_find(g_ld.reply + at, g_ld.reply_len - at, "<data>");
    if (s < 0) return -1;
    s += at + 6;

    e = ld_find(g_ld.reply + s, g_ld.reply_len - s, "</data>");
    if (e < 0) return -1;

    n = ld_b64(g_ld.reply + s, e, g_ld.val, LD_VAL - 1);
    if (n < 0) {
        klog(KLOG_WARN, "lockdown: the %s value is %d bytes of base64 "
             "this could not decode", key, (uint32_t)e);
        return -1;
    }
    g_ld.val_len = n;
    g_ld.val[n]  = 0;
    return n;
}

/* ── stage 2 ─────────────────────────────────────────────────────── */

int lockdown_get_value(const char *key)
{
    /* The request, built rather than fixed, because the key is the
       argument. Still no plist WRITER -- one dict with three string
       pairs does not need one, and stage 3's pair request, which
       carries certificates, is where that has to arrive. */
    static char req[512];
    int r;

    if (!g_ld.connected || !usbmux_connected()) {
        klog(KLOG_WARN, "lockdown: not connected");
        return USB_ENODEV;
    }
    if (!key || !*key) return USB_EPARAM;
    if (strlen(key) > 64) return USB_EPARAM;

    strcpy(req,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>HavenDOS</string>\n"
        "\t<key>Request</key>\n"
        "\t<string>GetValue</string>\n"
        "\t<key>Key</key>\n"
        "\t<string>");
    strcat(req, key);
    strcat(req, "</string>\n"
        "</dict>\n"
        "</plist>\n");

    r = ld_send_plist(req);
    if (r < 0) return r;
    klog(KLOG_INFO, "lockdown: GetValue %s sent, %d bytes", key,
         (uint32_t)(strlen(req) + 4));

    r = ld_recv_plist();
    if (r < 0) return r;

    ld_key_string("Error", g_ld.error, (int)sizeof g_ld.error);
    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: GetValue %s refused -- %s", key,
             g_ld.error);
        return 0;
    }

    if (ld_key_data("Value") < 0) {
        /* A value that is a <string> rather than <data> is a perfectly
           good answer to some keys -- ProductVersion, for one -- and
           it is not what DevicePublicKey returns. Try it rather than
           reporting nothing, and say which shape arrived. */
        if (ld_key_string("Value", (char *)g_ld.val, LD_VAL)) {
            g_ld.val_len = (int)strlen((const char *)g_ld.val);
            klog(KLOG_OK, "lockdown: %s = %s", key,
                 (const char *)g_ld.val);
            return 1;
        }
        klog(KLOG_WARN, "lockdown: a reply to GetValue %s with no "
             "Value in it", key);
        return 0;
    }

    klog(KLOG_OK, "lockdown: GOT %s -- %d bytes", key,
         (uint32_t)g_ld.val_len);
    return 1;
}

/* ── v0.8.8.11: SEND A REQUEST WE BUILT ELSEWHERE, AND HAND BACK THE
   REPLY ───────────────────────────────────────────────────────────────

   usb/pair.c builds the Pair request, which is several kilobytes of
   certificates and no business of this file's. What this file owns is
   the framing: the four-byte length, the split across mux packets,
   the reply's own length prefix.

   So the one thing missing was a way in. QueryType and GetValue each
   compose their own request from a string constant, which is right
   for a message that never changes and useless for one that does.

   The reply is NOT parsed here either. usb/pair.c reads it, because
   what a pairing reply means is a pairing question. This returns the
   bytes and their length and stops. */
int lockdown_request(const char *xml)
{
    int r;
    if (!xml || !xml[0]) return USB_EPARAM;
    if (!g_ld.connected) return USB_ENODEV;
    r = ld_send_plist(xml);
    if (r < 0) return r;
    r = ld_recv_plist();
    if (r < 0) return r;
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
   v0.8.9 -- STAGE 3's SECOND HALF: StartSession, then TLS

   Pairing gets the device to remember a host. A SESSION is what the
   host does with that afterwards: lockdownd refuses nearly every
   interesting request -- including the one that turns tethering on --
   outside a session, and it will not open one for a host it does not
   already trust.

     host -> { Request: StartSession, HostID, SystemBUID }
     dev  -> { Request: StartSession, SessionID: "...",
               EnableSessionSSL: true }

   InvalidHostID here means the pair record is not the one the device
   remembers, which is a DIFFERENT failure from a pairing that was
   refused and needs saying differently -- it is what a lost pair
   record looks like from the outside, and before this release losing
   the record was the only possibility.
   ════════════════════════════════════════════════════════════════════ */

/* `<key>NAME</key>` followed by `<true/>` or `<false/>`.
   ld_key_string() cannot answer this: a boolean in a plist is an empty
   element and has no <string> anywhere near it, so the existing
   scanner would run on to the NEXT key's value and report that. Two
   scanners rather than one general reader, for the reason
   include/lockdown.h gives: a scanner that knows what shape it is
   looking for cannot misread a different one. */
static int ld_key_bool(const char *key, int *out)
{
    char pat[48];
    int at, i;

    *out = 0;
    if (!g_ld.have_reply) return 0;
    if ((int)strlen(key) + 12 >= (int)sizeof pat) return 0;
    strcpy(pat, "<key>");
    strcat(pat, key);
    strcat(pat, "</key>");

    at = ld_find(g_ld.reply, g_ld.reply_len, pat);
    if (at < 0) return 0;
    i = at + (int)strlen(pat);

    while (i < g_ld.reply_len &&
           (g_ld.reply[i] == ' '  || g_ld.reply[i] == '\t' ||
            g_ld.reply[i] == '\n' || g_ld.reply[i] == '\r')) i++;

    if (i + 7 <= g_ld.reply_len &&
        memcmp(g_ld.reply + i, "<true/>", 7) == 0) { *out = 1; return 1; }
    if (i + 8 <= g_ld.reply_len &&
        memcmp(g_ld.reply + i, "<false/>", 8) == 0) { *out = 0; return 1; }
    return 0;
}

int lockdown_start_session(const char *host_id, const char *buid)
{
    static char req[640];
    int r;

    if (!g_ld.connected || !usbmux_connected()) {
        klog(KLOG_WARN, "lockdown: not connected");
        return USB_ENODEV;
    }
    if (!host_id || !*host_id || !buid || !*buid) return USB_EPARAM;
    /* Both are UUIDs, 36 characters. The bound is checked rather than
       assumed because these come off a file now, and a long value
       here would run off the end of req[] -- the shape of defect
       v0.8.8.11 found in two base64 helpers that could not refuse. */
    if (strlen(host_id) > 64 || strlen(buid) > 64) return USB_EPARAM;

    g_ld.session_id[0] = 0;
    g_ld.session = 0;
    g_ld.ssl_wanted = 0;

    strcpy(req,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>HavenDOS</string>\n"
        "\t<key>Request</key>\n"
        "\t<string>StartSession</string>\n"
        "\t<key>HostID</key>\n"
        "\t<string>");
    strcat(req, host_id);
    strcat(req, "</string>\n"
        "\t<key>SystemBUID</key>\n"
        "\t<string>");
    strcat(req, buid);
    strcat(req, "</string>\n"
        "</dict>\n"
        "</plist>\n");

    r = ld_send_plist(req);
    if (r < 0) return r;
    r = ld_recv_plist();
    if (r < 0) return r;

    ld_key_string("Error", g_ld.error, (int)sizeof g_ld.error);
    if (g_ld.error[0]) {
        klog(KLOG_WARN, "lockdown: StartSession refused -- %s",
             g_ld.error);
        return 0;
    }

    if (!ld_key_string("SessionID", g_ld.session_id,
                       (int)sizeof g_ld.session_id)) {
        klog(KLOG_WARN, "lockdown: a StartSession reply with no "
             "SessionID in it");
        return 0;
    }
    g_ld.session = 1;
    ld_key_bool("EnableSessionSSL", &g_ld.ssl_wanted);
    klog(KLOG_OK, "lockdown: session %s, SSL %s", g_ld.session_id,
         g_ld.ssl_wanted ? "requested" : "not requested");
    return 1;
}

/* ── the handshake ───────────────────────────────────────────────────
   Called after StartSession says EnableSessionSSL. Everything the
   client certificate needs is in `cc`, which shell/paircmd.c builds
   from the stored pair record -- this file never sees a key.

   The seed is the caller's: net/tls.c's PRNG is seeded per handshake
   and usb/ has no clock worth the name. shell/paircmd.c passes
   entropy, which is what kernel/entropy.c is for. */
int lockdown_enable_ssl(const void *cc, uint32_t seed)
{
    int r;

    if (!g_ld.connected || !usbmux_connected()) return USB_ENODEV;
    if (g_ld.ssl_on) return 1;                  /* already */
    if (!g_ld.session) {
        klog(KLOG_WARN, "lockdown: SSL without a session -- "
             "StartSession first");
        return USB_EPARAM;
    }
    g_ld.ssl_broken = 0;

    /* NO HOSTNAME. There is no name here to put in an SNI extension
       and an empty one is malformed rather than absent -- see
       send_client_hello() in net/tls.c. */
    r = tls_connect_ex(LD_TLS_HANDLE, &LD_TRANSPORT, 0, seed,
                       (const tls_client_cert_t *)cc);
    if (r < 0) {
        klog(KLOG_WARN, "lockdown: the TLS handshake failed (%d) at "
             "stage %d -- %s", (uint32_t)r, (uint32_t)g_tls_last_stage,
             tls_stage_name(g_tls_last_stage));
        return r;
    }

    /* Set LAST, and only here. Everything above this line travels in
       the clear and everything below it does not; a flag set from the
       device's REQUEST rather than from the handshake's success would
       put the next plaintext request inside a session that does not
       exist. */
    g_ld.ssl_on = 1;
    klog(KLOG_OK, "lockdown: the session is encrypted -- %d client "
         "certificate(s) sent", (uint32_t)g_tls_cert_sent);
    return 1;
}

int lockdown_stop_session(void)
{
    static char req[512];
    int r;

    if (!g_ld.session) return 0;
    if (!g_ld.connected || !usbmux_connected()) {
        /* Nothing to tell: forget the session rather than leaving a
           SessionID behind that no device has. */
        g_ld.session = 0; g_ld.ssl_on = 0; g_ld.session_id[0] = 0;
        return USB_ENODEV;
    }

    strcpy(req,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "\t<key>Label</key>\n"
        "\t<string>HavenDOS</string>\n"
        "\t<key>Request</key>\n"
        "\t<string>StopSession</string>\n"
        "\t<key>SessionID</key>\n"
        "\t<string>");
    strcat(req, g_ld.session_id);
    strcat(req, "</string>\n"
        "</dict>\n"
        "</plist>\n");

    /* StopSession travels INSIDE the session, encrypted, which is why
       the TLS teardown is below it and not above. */
    r = ld_send_plist(req);
    if (r >= 0) r = ld_recv_plist();

    if (g_ld.ssl_on) { tls_close(LD_TLS_HANDLE); g_ld.ssl_on = 0; }
    g_ld.session = 0;
    g_ld.session_id[0] = 0;
    return r < 0 ? r : 1;
}

const char *lockdown_session_id(void) { return g_ld.session_id; }
/* ── v0.8.9.5 ────────────────────────────────────────────────────────
   Why the last request failed, as a sentence. Never null. */
const char *lockdown_why(void)
{
    return g_ld.why ? g_ld.why
                    : "lockdown has not reported a reason";
}

/* Give the NEXT request a longer reply budget, in 10 ms ticks. One
   request in this protocol -- Pair -- can be waiting on a person
   reaching for the phone, and LD_REPLY_TICKS is five seconds because
   that is generous for a machine. Making the ordinary budget large
   enough for a human would make every real timeout take half a
   minute to report, so the exception is asked for rather than
   assumed. */
void lockdown_reply_budget(uint32_t ticks)
{
    g_ld.next_budget = ticks;
}

int lockdown_session(void)            { return g_ld.session; }
int lockdown_ssl_wanted(void)         { return g_ld.ssl_wanted; }
int lockdown_ssl(void)                { return g_ld.ssl_on; }

/* The last reply, whole, as it came off the wire. Stage 3 needs the
   document rather than one value out of it. */
const char *lockdown_reply(void)     { return (const char *)g_ld.reply; }
int         lockdown_reply_len(void) { return g_ld.reply_len; }

const char *lockdown_value(void)  { return (const char *)g_ld.val; }
int lockdown_value_len(void)      { return g_ld.val_len; }

/* ── what `usb lockdown` prints ──────────────────────────────────── */

int lockdown_dump(void)
{
    uint16_t sport = 0, dport = 0;
    uint32_t seq = 0, ack = 0, win = 0;

    /* ── v0.8.9.3: THE EVIDENCE OUTLIVES THE CONNECTION ──────────────
       This used to return immediately with "not connected", which was
       fine while the only way to be disconnected was never to have
       connected. ld_desync() changed that: a request that times out
       now DROPS the channel on purpose, and the reply fragment it was
       holding is the single most useful thing on the screen at that
       moment.

       Returning early would have thrown away exactly what the
       teardown exists to let somebody look at -- the same mistake
       ld_recv_plist() has a comment against for the bytes themselves.
       So say the connection is gone, and then go on and dump what
       arrived. */
    if (!g_ld.connected) {
        klog(KLOG_INFO, "lockdown: not connected%s",
             g_ld.have_reply ? " -- the last reply is below" : "");
        if (!g_ld.have_reply) return 0;
    } else {
        usbmux_conn_state(&sport, &dport, &seq, &ack, &win);
    klog(KLOG_INFO, "lockdown  %u->%u  seq %u ack %u  their window %u",
         (uint32_t)sport, (uint32_t)dport, seq, ack, win);
    }
    klog(KLOG_INFO, "        requests %u  replies %u",
         g_ld.sent, g_ld.recvd);

    /* ── v0.8.9: the session, and whether it is encrypted ────────────
       Three separate facts on purpose. "no session" and "a session in
       the clear" and "an encrypted session" need different next steps,
       and ssl_wanted beside ssl_on is what tells a device that asked
       for SSL and did not get it apart from one that never asked. */
    if (!g_ld.session) {
        klog(KLOG_INFO, "        no session (StartSession not done)");
    } else {
        klog(KLOG_INFO, "        session %s", g_ld.session_id);
        klog(KLOG_INFO, "        SSL: asked %s, handshake %s"
                        "%s  records tx %u rx %u",
             g_ld.ssl_wanted ? "yes" : "NO",
             g_ld.ssl_on ? "DONE" : "not done",
             g_ld.ssl_broken ? "  [a record went out half way]" : "",
             g_ld.tls_tx, g_ld.tls_rx);
    }

    if (!g_ld.have_reply) {
        klog(KLOG_INFO, "        no reply received yet");
        return 1;
    }

    /* ── THE REPLY AS IT ARRIVED ─────────────────────────────────────
       The length prefix in hex, then the first line of the body as
       text. The prefix is printed separately because getting it wrong
       by four is the single most likely fault in this file and it is
       invisible in the body.

       `packets` is here because "the reply came in two packets" is the
       thing that confirms the accumulation was needed at all. A reader
       written for one packet would have reported four bytes and an
       empty plist, and nothing on this screen would have said why. */
    klog(KLOG_INFO, "        reply: %d bytes in %u packets",
         (uint32_t)g_ld.reply_len, (uint32_t)g_ld.packets);
    if (g_ld.reply_len >= 4) {
        klog(KLOG_INFO, "        length prefix: %2x %2x %2x %2x = %u",
             (uint32_t)g_ld.reply[0], (uint32_t)g_ld.reply[1],
             (uint32_t)g_ld.reply[2], (uint32_t)g_ld.reply[3],
             ld_be32(g_ld.reply));
    } else {
        int i;
        char line[32];
        int  p = 0;
        for (i = 0; i < g_ld.reply_len && p < (int)sizeof line - 4; i++) {
            const char *hx = "0123456789ABCDEF";
            line[p++] = hx[(g_ld.reply[i] >> 4) & 0xF];
            line[p++] = hx[g_ld.reply[i] & 0xF];
            line[p++] = ' ';
        }
        line[p] = 0;
        klog(KLOG_INFO, "        too short for a prefix: %s", line);
    }

    /* The first stretch of printable text in the body, which for a
       plist is the XML declaration and tells you at a glance whether
       this is a plist at all. Bounded to one screen line. */
    if (g_ld.reply_len > 4) {
        char line[60];
        int  i, p = 0;
        for (i = 4; i < g_ld.reply_len && p < (int)sizeof line - 1; i++) {
            uint8_t c = g_ld.reply[i];
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            line[p++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[p] = 0;
        klog(KLOG_INFO, "        %s", line);
    }

    if (g_ld.type[0])
        klog(KLOG_OK,   "        Type:  %s", g_ld.type);
    if (g_ld.error[0])
        klog(KLOG_WARN, "        Error: %s", g_ld.error);

    /* ── STAGE 2's VALUE ─────────────────────────────────────────────
       The FIRST LINE and the length, not the key. A PEM key is four
       hundred bytes of base64 and putting it on an 80-column screen
       would push everything else off it -- and the first line is the
       part that says whether this is a key at all.

       The length is here because a value that decoded to something
       far shorter than a key is the symptom of base64 that stopped
       early, which is exactly what the shell's own decoder would do
       to a <data> element wrapped across lines. */
    if (g_ld.val_len) {
        char line[60];
        int  i, p = 0;
        for (i = 0; i < g_ld.val_len && p < (int)sizeof line - 1; i++) {
            uint8_t c = g_ld.val[i];
            if (c == '\n' || c == '\r') break;      /* first line only */
            line[p++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[p] = 0;
        klog(KLOG_OK, "        value: %d bytes", (uint32_t)g_ld.val_len);
        klog(KLOG_OK, "        %s", line);
    }
    return 1;
}
