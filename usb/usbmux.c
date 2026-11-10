/* ════════════════════════════════════════════════════════════════════
   usb/usbmux.c -- the Apple Mobile Device channel: stage 0, and the
   connection layer stage 1 runs over.

   include/usbmux.h carries the argument: iOS will not turn tethering
   on for a host it has no pairing record for, pairing runs over this
   interface, and the road there is staged so every step is a yes or no
   the phone itself gives.

   This file is the transport. It claims the interface, exchanges
   version packets, and as of v0.8.7.7 carries a single TCP connection
   to a port on the device. It does not pair and it does not speak
   lockdownd -- usb/lockdown.c does, over this.

   ── WHERE THE FRAMING COMES FROM, v0.8.7.7 ───────────────────────────

   Not a guess any more. Every field is usbmuxd's own src/device.c:
   send_packet() for the mux header, send_tcp() for the TCP one,
   device_tcp_input() for the handshake, device_data_input() for what a
   receiver may believe. That is the only implementation known to work
   against a real device, so where this file differs from it the
   difference is marked and argued rather than assumed to be an
   improvement. 0.8.7.3 shipped a sixteen-byte header because it
   reasoned about the protocol instead of reading it.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/usbmux.h"
#include "../include/usbnet.h"   /* only for the Ethernet interface triple */
#include "../include/klog.h"

/* Big-endian helpers. include/net.h has these, but pulling the whole
   network stack into a USB driver to byte-swap four integers is how a
   header ends up included for one inline function and a layering
   argument starts. */
static uint32_t be32(uint32_t v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}
static uint16_t be16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ── how long is the header? ──────────────────────────────────────
   Eight bytes until both ends have agreed on version 2, sixteen after.

   The magic, tx_seq and rx_seq are a version-2 addition, so they cannot
   be present on the packet that ESTABLISHES the version -- neither end
   knows yet. v0.8.7.3 hardcoded sixteen and read byte 8 as the magic;
   on the HP that byte was 2, and the driver reported "magic 2, expected
   FEEDFACE". It was right that the framing was wrong, and the 2 it
   complained about was the phone saying it speaks usbmux 2.0.

   The real packet, off the wire on 26 October:

       00 00 00 00   protocol = 0   VERSION
       00 00 00 14   length   = 20
       00 00 00 02   major    = 2
       00 00 00 00   minor    = 0
       00 00 00 00   padding

   8 + 12 = 20, which is what the device sent, to the byte. */
#define MUX_HDR_SHORT  8
#define MUX_HDR_FULL  16
#define MUX_BUF       2048      /* fits the xHCI bound; see check_usb_buffers */

/* ── the window, and why it is not usbmuxd's ──────────────────────────
   The TCP window field is in units of 256 bytes, and usbmuxd writes
   131072 >> 8 == 512 because it has a 262144-byte buffer behind it.
   MUX_BUF is 2048 and one packet has to fit it whole, so the most this
   kernel can ever accept in a single mux packet is

       MUX_BUF - MUX_HDR_FULL - MUX_TCP_HDR  ==  2012

   rounded DOWN to the 256-byte unit the field can express: 1792.

   Advertising more than that is advertising an overflow. Advertising
   it is the one deliberate departure from the implementation that
   works, and it is derived here rather than written as a number so it
   cannot drift away from MUX_BUF -- the shape of 0.8.7's transfer-size
   bug, where the driver and its back-end each held their own idea of
   the bound. tests/test_usbmux.c re-derives it and compares. */
#define MUX_MAX_PAYLOAD (MUX_BUF - MUX_HDR_FULL - MUX_TCP_HDR)
#define MUX_WINDOW_UNITS (MUX_MAX_PAYLOAD / 256)
#define MUX_WINDOW       (MUX_WINDOW_UNITS * 256)

/* The source port. usbmuxd hands out 1, 2, 3... from find_sport(); the
   value is the host's to choose and the device only echoes it back as
   the destination port. One connection means one port, so it is a
   constant -- but NOT 1, deliberately: a port the device may have seen
   from a previous host's connection is the one case where an echoed
   port cannot be told from a stale packet. */
#define MUX_SPORT      0x4854   /* 'HT' -- arbitrary, ours, and unmistakable */

/* usbmux_handshake() and the connect handshake both poll rather than
   wait: a driver must not depend on the bottom half v0.8.8 added,
   which runs only at idle points. One constant
   so the two cannot drift. */
#define MUX_POLL_TRIES 20000

typedef struct {
    int      open;
    int      dev;            /* index into the USB core's device table */
    uint8_t  ifnum;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;

    /* ── the mux-level pair. NOT counters. See include/usbmux.h ──────
       tx_seq moves only on a packet that carries it, the SETUP packet
       resets both, and rx_seq is the last value the DEVICE put in its
       own rx_seq field, mirrored back untouched. v0.8.7.6 treated
       both as tallies, which was invisible because the only sequenced
       packet it ever sent was one SETUP on which the wrong answers
       were 1 and 1. */
    uint16_t tx_seq, rx_seq;
    int      version;        /* what the device said, 0 if it has not */
    int      setup_done;     /* the version-2 setup byte got out       */

    /* ── the one connection ─────────────────────────────────────────
       usbmuxd keeps a collection; this keeps one, and says so in the
       header rather than growing a table with one row in it. */
    int      conn;           /* 0 none, 1 connecting, 2 up */
    int      refused;        /* the last connect got an RST, not silence */
    uint16_t sport, dport;
    uint32_t tx_tcp_seq;     /* our byte stream position */
    uint32_t tx_tcp_ack;     /* what we have acknowledged of theirs */
    uint32_t rx_isn;         /* the device's initial sequence number */
    uint32_t dup_drops;      /* packets refused by mux_rx_accept()    */
    uint32_t peer_win;       /* theirs, in bytes, for the dump */

    /* ── data that arrived while we were SENDING ─────────────────────
       usbmux_send_stream() splits a request across packets and reads
       between them, to collect the device's ACKs and the window they
       carry. A read can return DATA instead, and there is exactly one
       correct thing to do with it: keep it, and give it to the next
       usbmux_recv() before anything newer.

       Dropping it is not an option -- it has already been ACKed by
       the time it is in our hands, so the device will never send it
       again, and the symptom is a reply that is missing its first
       packet and parses as a truncated plist. One packet of stash is
       enough because the harvest does not run when it is occupied:
       un-read bytes stay on the device's side of the wire, where the
       protocol already knows how to keep them. */
    uint8_t  pend[MUX_MAX_PAYLOAD];
    int      pend_len;
    uint32_t pend_hits;     /* times the stash saved a packet         */
    uint32_t win_short;     /* times we sent past the advertised window */

    /* the last packet as it arrived, kept for `usbmux` to print */
    uint8_t  last[64];
    int      last_len;
    int      have_last;

    uint32_t sent, received, errs;

    /* ── v0.8.9.5: WHY, IN WORDS ─────────────────────────────────────
       A negative return says a layer refused; it does not say what the
       wire did. usbmux_recv() alone returns USB_EXFER for a protocol
       mismatch, a short header, a foreign port pair AND -- until this
       release -- a device that simply closed the connection, which is
       not an error at all. The caller prints a number and the reason
       goes to a log whose echo kernel/main.c turns off before the
       shell starts, which is the 0.8.9.1 failure repeating itself one
       layer down.

       This is a pointer to a string literal, not a buffer: it costs
       four bytes, it cannot be overrun, and it is set at the point the
       decision is taken rather than reconstructed from a code after
       the fact. */
    const char *why;

    uint32_t fins;          /* times the device closed on us          */
    uint32_t zlps;          /* v0.8.9.7: zero-length packets sent     */

    /* ── v0.8.9.7: WHAT THE DEVICE SAID ──────────────────────────────
       The last CONTROL message the device sent, as printable text, its
       type, and how many there have been. See mux_tcp_recv(). */
    char     dev_msg[160];
    uint8_t  dev_msg_type;
    uint32_t dev_msgs;
} mux_t;

static mux_t g_m;
static uint8_t g_buf[MUX_BUF];

/* The header this connection is currently using. Version 0 means the
   exchange that decides it has not happened yet, so the short one. */
static int mux_hdr_len(void)
{
    return (g_m.version >= 2) ? MUX_HDR_FULL : MUX_HDR_SHORT;
}

/* ── finding it ──────────────────────────────────────────────────── */

typedef struct {
    int     found;      /* this configuration carries the mux interface */
    uint8_t ifnum;
    int     with_eth;   /* ...and the Apple Ethernet interface as well  */
} survey_t;

static void survey_cb(void *ctx, uint8_t num, uint8_t alt, uint8_t nep,
                      uint8_t cls, uint8_t sub, uint8_t proto)
{
    survey_t *s = (survey_t *)ctx;
    (void)nep;
    /* The mux interface is matched FIRST, and on the whole triple.

       0xFF is "vendor specific" and says nothing on its own: the Apple
       ETHERNET interface in this same configuration is 0xFF too, and it
       comes first. So a match on the class alone lands on Ethernet. The
       Ethernet test below must NOT be allowed to shield this one by
       consuming that interface before it is reached -- an ordering that
       makes the bug untestable is not a fix for it.

       Alternate settings of the mux interface are not what we want: its
       endpoints are on alt 0, unlike the Ethernet interface's. */
    if (!s->found && alt == 0 &&
        cls == MUX_IF_CLASS && sub == MUX_IF_SUBCLASS &&
        proto == MUX_IF_PROTOCOL) {
        s->found = 1;
        s->ifnum = num;
    }
    /* Only COUNTED, to tell which configuration carries both. Its alt
       does not matter for that. */
    if (cls == IPHETH_IF_CLASS && sub == IPHETH_IF_SUBCLASS &&
        proto == IPHETH_IF_PROTOCOL)
        s->with_eth = 1;
}

int usbmux_present(void) { return g_m.open; }

int usbmux_open(void)
{
    int i;

    if (g_m.open) return USB_OK;

    for (i = 0; i < usb_device_count(); i++) {
        usb_dev_t      *d = (usb_dev_t *)usb_device(i);
        const usb_ep_t *in, *out;
        survey_t        s;
        int             ci, r, cfg = -1;

        if (!d || !d->valid) continue;

        /* Survey every configuration. This changes nothing on the
           device -- see usb_survey_config() -- which matters because it
           runs against a phone somebody is using.

           v0.8.7.6: take the configuration that carries the Apple USB
           ETHERNET interface as well, when there is one. Both live in
           the same configuration on an iPhone -- index 3 on 05AC:12A8 --
           and 0.8.7.3 took the first that had the mux, which is index 2
           and has no Ethernet in it. Selecting that one threw away the
           configuration tethering needs, every time `usb mux` ran. The
           handoff recorded this as "the USB core claims one interface
           per device, so usbmux and Ethernet cannot be held at once";
           the core's limit is real, but the configurations were never
           the thing forcing the choice. */
        {
            int first = -1;
            uint8_t first_if = 0;
            for (ci = 0; ci < (int)d->nconfigs; ci++) {
                s.found = 0; s.ifnum = 0; s.with_eth = 0;
                if (usb_survey_config(d, (uint8_t)ci, survey_cb, &s) < 0)
                    continue;
                if (!s.found) continue;
                if (first < 0) { first = ci; first_if = s.ifnum; }
                if (s.with_eth) { cfg = ci; break; }
            }
            if (cfg < 0 && first >= 0) {   /* no config has both */
                cfg = first;
                s.ifnum = first_if;
            }
        }
        if (cfg < 0) continue;              /* not an Apple device */

        klog(KLOG_INFO, "usbmux: %4x:%4x cfg %d if %u is Apple Mobile "
             "Device%s", (uint32_t)d->vendor, (uint32_t)d->product,
             (uint32_t)cfg, (uint32_t)s.ifnum,
             s.with_eth ? ", with Ethernet" : "");

        r = usb_select_config(d, (uint8_t)cfg, (int)s.ifnum);
        if (r < 0) {
            klog(KLOG_WARN, "usbmux: SET_CONFIGURATION %d refused (%d)",
                 (uint32_t)cfg, (uint32_t)r);
            return r;
        }

        in  = usb_find_ep(d, USB_EP_BULK, 1);
        out = usb_find_ep(d, USB_EP_BULK, 0);
        if (!in || !out) {
            klog(KLOG_WARN, "usbmux: no bulk pair on interface %u "
                 "(in %d out %d)", (uint32_t)s.ifnum, in ? 1 : 0,
                 out ? 1 : 0);
            return USB_ENODEV;
        }

        memset(&g_m, 0, sizeof g_m);
        g_m.open    = 1;
        g_m.dev     = i;
        g_m.ifnum   = s.ifnum;
        g_m.ep_in   = in->addr;
        g_m.ep_out  = out->addr;
        g_m.mps_in  = in->mps;
        g_m.mps_out = out->mps;

        klog(KLOG_OK, "usbmux: open, ep %2x in / %2x out, %u/%u bytes",
             (uint32_t)g_m.ep_in, (uint32_t)g_m.ep_out,
             (uint32_t)g_m.mps_in, (uint32_t)g_m.mps_out);
        return USB_OK;
    }
    return USB_ENODEV;
}

void usbmux_close(void)
{
    /* No RST on the way out, deliberately. This is called from `usb
       rescan` BEFORE usb_enumerate() rebuilds the device table, which
       is the one moment when g_m.dev may already address a different
       device -- so a packet sent here would go to whatever moved into
       that slot. usbmux_disconnect() is the polite path and exists
       separately for callers that still have a live bus. */
    memset(&g_m, 0, sizeof g_m);
}

/* ── packets ─────────────────────────────────────────────────────── */

/* Build a header into g_buf and send header+payload as one transfer.
   The length field counts the header, which is the detail most easily
   got wrong and the one a device refuses silently.

   `hdr2` is an optional SECOND header placed between the mux header and
   the payload -- the TCP header, for protocol 6. usbmuxd's
   send_packet() takes exactly this shape and for the same reason: the
   mux length field has to count all three pieces, and a caller that
   assembles the TCP header into its own buffer first has to know the
   mux header's size to place it, which is the knowledge this function
   exists to keep. */
static int mux_send(uint32_t proto, const void *hdr2, int h2len,
                    const void *payload, int plen)
{
    const usb_dev_t *d = usb_device(g_m.dev);
    int hdr   = mux_hdr_len();
    int total = hdr + h2len + plen;
    int r;

    if (!g_m.open)                 return USB_ENODEV;
    if (!d || !d->valid)           return USB_ENODEV;
    if (plen < 0 || h2len < 0)     return USB_EPARAM;
    if (total > MUX_BUF)           return USB_EPARAM;

    *(uint32_t *)(g_buf +  0) = be32(proto);
    *(uint32_t *)(g_buf +  4) = be32((uint32_t)total);
    if (hdr == MUX_HDR_FULL) {
        /* ── THE SETUP PACKET RESETS THE PAIR ────────────────────────
           usbmuxd's send_packet() does this and nothing else does it:
           on MUX_PROTO_SETUP, tx_seq becomes 0 and rx_seq becomes
           0xFFFF, and THOSE are the values that go on the wire. 0xFFFF
           is "I have received nothing from you", not 65535 packets.

           v0.8.7.6 wrote 1 and 1 here, because tx_seq had been moved
           by the VERSION packet (which carries no such field) and
           rx_seq was counting arrivals. Both wrong, and both invisible
           until something sent a SECOND sequenced packet. */
        if (proto == MUX_PROTO_SETUP) {
            g_m.tx_seq = 0;
            g_m.rx_seq = MUX_RX_SEQ_NONE;
        }
        *(uint32_t *)(g_buf +  8) = be32(MUX_MAGIC);
        *(uint16_t *)(g_buf + 12) = be16(g_m.tx_seq);
        *(uint16_t *)(g_buf + 14) = be16(g_m.rx_seq);
    }
    if (h2len && hdr2) memcpy(g_buf + hdr, hdr2, (uint32_t)h2len);
    if (plen && payload)
        memcpy(g_buf + hdr + h2len, payload, (uint32_t)plen);

    r = usb_bulk(d, g_m.ep_out, 0, g_buf, total, (int)g_m.mps_out);
    if (r < 0) {
        g_m.errs++;
        if (r == USB_ESTALL) usb_clear_halt(d, g_m.ep_out, 0);
        return r;
    }

    /* ── v0.8.9.7: A PACKET THAT FILLS ITS LAST USB PACKET NEEDS A ZLP ──
     *
     * A bulk transfer ends at the first USB packet SHORTER than the
     * endpoint's maximum. A mux packet whose length is an exact
     * multiple of it -- 512 on a high-speed iPhone -- ends on a full
     * one, and the device cannot tell it is over: it goes on reading,
     * and the NEXT mux packet lands in the same transfer, glued to the
     * end of this one. Its header then describes a packet of one
     * length inside a transfer of another, and the phone's usbmux
     * refuses it -- in a CONTROL message, which until this release was
     * itself dropped unread.
     *
     * usbmuxd sends a zero-length packet after exactly these
     * (usb.c, usb_send: "Send ZLP"). This did not, and nothing ever
     * noticed, because nothing this side sent was ever that size: the
     * QueryType and GetValue requests are a few hundred bytes. The
     * pair request is 3728, sent as two packets, and the first is the
     * full MUX_BUF -- 16 + 20 + 2012 = 2048 = 4 x 512. The very first
     * full-size packet this driver has ever sent was a multiple of 512.
     *
     * On Kyan's HP, 10 November: QueryType worked, GetValue worked,
     * the pair request went out, and the phone answered with a packet
     * that was not TCP. */
    if (g_m.mps_out && (total % (int)g_m.mps_out) == 0) {
        r = usb_bulk(d, g_m.ep_out, 0, g_buf, 0, (int)g_m.mps_out);
        if (r < 0) {
            g_m.errs++;
            g_m.why = "the zero-length packet that ends a full-size "
                      "transfer would not go out";
            klog(KLOG_WARN, "usbmux: ZLP after %d bytes failed (%d)",
                 (uint32_t)total, (uint32_t)r);
            if (r == USB_ESTALL) usb_clear_halt(d, g_m.ep_out, 0);
            return r;
        }
        g_m.zlps++;
    }
    /* Only a packet that CARRIED the field advances it. The VERSION
       packet goes out on the short header, so it must not -- and
       v0.8.7.6 advanced it unconditionally, which is why its SETUP
       carried 1 where usbmuxd carries 0. `sent` is the statistic;
       tx_seq is the protocol, and they are not the same number. */
    if (hdr == MUX_HDR_FULL) g_m.tx_seq++;
    g_m.sent++;
    return USB_OK;
}

/* Read one packet. Returns the PAYLOAD length, 0 if nothing came, or a
   negative USB_E*. The raw bytes are kept for `usbmux` to print before
   anything here has decided what they mean. */
static int mux_recv(uint32_t *proto_out)
{
    const usb_dev_t *d = usb_device(g_m.dev);
    uint32_t magic, len;
    int hdr = mux_hdr_len();
    int r;

    if (!g_m.open)       return USB_ENODEV;
    if (!d || !d->valid) return USB_ENODEV;

    r = usb_bulk_poll(d, g_m.ep_in, 1, g_buf, MUX_BUF, (int)g_m.mps_in);
    if (r == 0) return 0;                     /* nothing yet */
    if (r < 0) {
        g_m.errs++;
        if (r == USB_ESTALL) usb_clear_halt(d, g_m.ep_in, 1);
        return r;
    }

    /* Kept BEFORE it is parsed. The whole reason this driver has a dump
       command is that the protocol here is read off somebody else's
       implementation, and a transport that silently parses cannot be
       asked whether it parsed the right thing. */
    {
        int k = r < (int)sizeof g_m.last ? r : (int)sizeof g_m.last;
        memcpy(g_m.last, g_buf, (uint32_t)k);
        g_m.last_len  = r;
        g_m.have_last = 1;
    }
    g_m.received++;

    if (r < hdr) return USB_EXFER;

    /* ── THE MAGIC IS EVIDENCE, NOT A GATE. v0.8.7.8 ────────────────
       The HP settled this on 30 October, and it cost a boot.

       v0.8.7.7 REFUSED a packet whose magic was not 0xFEEDFACE. The
       phone's first TCP packet -- the SYN/ACK to lockdownd, parsed and
       answered correctly in every other respect -- carries

           00 00 00 06   protocol 6, TCP
           00 00 00 24   length 36 = 16 header + 20 TCP
           FA CE FA CE   <- the magic
           00 00 00 01   tx_seq 0, rx_seq 1

       0xFACEFACE. Not 0xFEEDFACE. The driver threw away a perfectly
       good SYN/ACK and reported "the framing in this driver is wrong",
       which was true in a way it did not mean.

       AND USBMUXD NEVER CHECKS IT. src/device.c writes the magic in
       send_packet() and reads it NOWHERE: device_data_input() checks
       that the length field equals the transfer length, takes rx_seq,
       and switches on the protocol. There is no magic validation in
       the only implementation known to work against this device.

       So the check was strictness the working implementation does not
       have, on a field whose device-side value nobody wrote down --
       and the first real packet to carry one was refused by it. That
       is "more correct than the thing that works", which is the
       position include/usbmux.h already warns about twice, arrived at
       by carrying a guard forward rather than by adding one.

       It is still READ and still REPORTED, because a value nobody
       expected is worth seeing. It is no longer believed. */
    if (hdr == MUX_HDR_FULL) {
        magic = be32(*(uint32_t *)(g_buf + 8));
        if (magic != MUX_MAGIC && magic != MUX_MAGIC_DEVICE)
            klog(KLOG_WARN, "usbmux: magic %x, neither %x nor %x -- "
                 "noted, not refused", magic, (uint32_t)MUX_MAGIC,
                 (uint32_t)MUX_MAGIC_DEVICE);
    }

    len = be32(*(uint32_t *)(g_buf + 4));
    if ((int)len < hdr) return USB_EXFER;

    /* ── A PACKET LONGER THAN THE TRANSFER IS NAMED, NOT STITCHED ────
       usbmuxd reassembles into a 64 KB buffer because a mux packet may
       span several USB reads. This kernel cannot: the xHCI back-end
       bounces every transfer through 2048 bytes, so a reassembly
       buffer above that would not raise the ceiling, it would only
       move where the truncation happens.

       So the case is REFUSED and said out loud, with the two numbers
       that distinguish it. "The reply was bigger than this kernel can
       read" and "the framing is wrong" are different facts with
       different fixes, and a shared error number would send the next
       person to the wrong one -- which is the whole lesson of
       x_control() returning (3) for six different causes. */
    if ((int)len > r) {
        klog(KLOG_WARN, "usbmux: packet says %u bytes, the transfer "
             "carried %d -- too big to read in one piece", len,
             (uint32_t)r);
        return USB_EXFER;
    }

    if (proto_out) *proto_out = be32(*(uint32_t *)(g_buf + 0));

    /* ── rx_seq IS MIRRORED, NOT COUNTED ────────────────────────────
       usbmuxd's device_data_input() does exactly this one assignment:
       dev->rx_seq = ntohs(mhdr->rx_seq). It takes the value out of the
       arriving header's rx_seq field and sends it straight back on the
       next packet. It is not a tally of what has been received, which
       is what v0.8.7.6 made it.

       It looks like a strange thing for a protocol to do, and it may
       well be that the device never checks it. That is not a reason to
       do something else: usbmuxd is the only implementation known to
       work against this device, and a field nobody has proved is
       ignored is a field to copy. */
    if (hdr == MUX_HDR_FULL)
        g_m.rx_seq = be16(*(uint16_t *)(g_buf + 14));

    return (int)len - hdr;
}

/* ── stage 0: the version exchange ───────────────────────────────── */

/* The setup byte a version-2 device wants before anything else.
   Separate because it can be OWED -- see usbmux_handshake(). */
static int mux_send_setup(void)
{
    uint8_t setup = 0x07;
    int r = mux_send(MUX_PROTO_SETUP, 0, 0, &setup, 1);
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: SETUP refused (%d)", (uint32_t)r);
        return r;
    }
    g_m.setup_done = 1;
    klog(KLOG_INFO, "usbmux: SETUP sent");
    return USB_OK;
}

int usbmux_handshake(void)
{
    uint32_t proto = 0;
    uint32_t vh[3];
    int      r, tries;

    if (!g_m.open) return USB_ENODEV;

    /* ── VERSIONING HAPPENS ONCE. THE SETUP MAY STILL BE OWED ────────
       A second version exchange would go out in the framing the first
       one agreed, which no device expects -- so the early return is
       right. v0.8.7.6 returned here unconditionally, and that left one
       state the channel could not get out of:

         the VERSION is answered, g_m.version becomes 2, and then the
         SETUP packet fails to send.

       The host now believes the version is agreed, so it uses the
       sixteen-byte header; the DEVICE never got its setup byte. Every
       later call returned 2 without noticing, and nothing retried --
       a channel permanently out of step, reporting itself healthy.

       Found by tools/check_usbmux_suite.py, while establishing why a
       different injection could not be caught. The setup is owed until
       it gets out. */
    if (g_m.version >= 1) {
        if (g_m.version >= 2 && !g_m.setup_done) {
            klog(KLOG_WARN, "usbmux: the SETUP byte never got out -- "
                 "sending it again");
            r = mux_send_setup();
            if (r < 0) return r;
        }
        return g_m.version;
    }

    vh[0] = be32(2);        /* major */
    vh[1] = be32(0);        /* minor */
    vh[2] = 0;              /* padding */

    r = mux_send(MUX_PROTO_VERSION, 0, 0, vh, (int)sizeof vh);
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: the VERSION packet would not send (%d)",
             (uint32_t)r);
        return r;
    }

    /* A driver cannot depend on the bottom half, so this is a bounded poll rather
       than a wait. 0.8.8's tick is what turns this into something that
       does not spin. */
    for (tries = 0; tries < MUX_POLL_TRIES; tries++) {
        r = mux_recv(&proto);
        if (r > 0 || r < 0) break;
    }
    if (r == 0) {
        klog(KLOG_WARN, "usbmux: no answer to VERSION after %d polls",
             (uint32_t)tries);
        return 0;
    }
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: VERSION reply unreadable (%d)",
             (uint32_t)r);
        return r;
    }
    if (proto != MUX_PROTO_VERSION) {
        klog(KLOG_WARN, "usbmux: answered protocol %u, expected a "
             "VERSION", proto);
        return USB_EXFER;
    }
    if (r < 4) return USB_EXFER;

    /* The payload begins after the header THIS exchange used -- eight
       bytes, not sixteen. Reading it at sixteen lands on the padding,
       which is how a phone that answered "2" could have been recorded
       as version 0 even once the magic check was out of the way. */
    g_m.version = (int)be32(*(uint32_t *)(g_buf + MUX_HDR_SHORT));
    klog(KLOG_OK, "usbmux: THE DEVICE ANSWERED -- version %d",
         (uint32_t)g_m.version);

    /* A version-2 device wants a setup byte before anything else. */
    if (g_m.version >= 2) {
        r = mux_send_setup();
        if (r < 0) return r;
    }
    return g_m.version;
}

/* Read one packet in whatever framing this connection has settled on.
   Returns the payload length, 0 if nothing arrived, or a negative
   USB_E*. The payload itself is at usbmux_payload().

   Stage 1 needs this -- lockdownd is a conversation, not a handshake --
   and it is also the only way to exercise the magic check, which by
   definition cannot fire during the version exchange. */
int usbmux_recv_packet(uint32_t *proto_out)
{
    return mux_recv(proto_out);
}

const void *usbmux_payload(void)
{
    return g_buf + mux_hdr_len();
}

/* ════════════════════════════════════════════════════════════════════
   The connection layer, v0.8.7.7 -- stage 1's transport.

   Protocol 6 carries a standard twenty-byte BSD tcphdr. The mux is the
   reliable layer underneath, so there is NO CHECKSUM: usbmuxd memsets
   the header and never fills th_sum, and the device accepts it. A
   checksum computed here would be the only one in the conversation and
   nothing would check it.

   The one field that reads wrong is the window: it is in units of 256
   bytes. usbmuxd writes `htons(tx_win >> 8)` and reads `ntohs(th_win)
   << 8`, so a window of 1792 bytes appears on the wire as 7.
   ════════════════════════════════════════════════════════════════════ */

/* Build a TCP header. Every field is written, including the two that
   are always zero: a memset and three assignments leaves a reader
   wondering whether th_sum was forgotten or decided, and this protocol
   has already cost one release over a field somebody assumed. */
static void mux_tcp_hdr(uint8_t *th, uint8_t flags)
{
    *(uint16_t *)(th + MUX_TH_SPORT) = be16(g_m.sport);
    *(uint16_t *)(th + MUX_TH_DPORT) = be16(g_m.dport);
    *(uint32_t *)(th + MUX_TH_SEQ)   = be32(g_m.tx_tcp_seq);
    *(uint32_t *)(th + MUX_TH_ACK)   = be32(g_m.tx_tcp_ack);
    th[MUX_TH_OFF]                   = (uint8_t)((MUX_TCP_HDR / 4) << 4);
    th[MUX_TH_FLAGS]                 = flags;
    /* The window this kernel can actually honour, not the one usbmuxd
       advertises -- see MUX_WINDOW above. */
    *(uint16_t *)(th + MUX_TH_WIN)   = be16((uint16_t)MUX_WINDOW_UNITS);
    *(uint16_t *)(th + MUX_TH_SUM)   = 0;   /* the mux is reliable; see above */
    *(uint16_t *)(th + MUX_TH_URP)   = 0;
}

/* Send one TCP packet on the connection. `data` may be null. */
static int mux_tcp_send(uint8_t flags, const void *data, int len)
{
    uint8_t th[MUX_TCP_HDR];

    if (!g_m.conn) return USB_ENODEV;
    if (len < 0 || len > MUX_MAX_PAYLOAD) return USB_EPARAM;

    mux_tcp_hdr(th, flags);
    return mux_send(MUX_PROTO_TCP, th, MUX_TCP_HDR, data, len);
}

/* Read one TCP packet for THIS connection.

   Returns the payload length (0 is legitimate -- a bare ACK), or
   USB_ETIMEOUT when nothing arrived at all, or another negative
   USB_E*. The flags and the sequence numbers come back through the
   out-parameters because the handshake and the data path judge them
   differently and neither should be re-reading g_buf to find out. */
static int mux_tcp_recv(uint8_t *flags_out, uint32_t *seq_out,
                        uint32_t *ack_out)
{
    const uint8_t *th;
    uint32_t proto = 0;
    uint16_t sport, dport;
    int      r;

    r = mux_recv(&proto);
    if (r == 0)  return USB_ETIMEOUT;   /* nothing yet, not a failure */
    if (r < 0)   return r;

    /* ── v0.8.9.7: A CONTROL PACKET IS THE DEVICE TALKING ────────────
     *
     * Before this, every protocol but TCP on an open connection was a
     * fault, and the connection was dropped on the spot. On Kyan's HP,
     * 10 November, right after the 3728-byte pair request went out:
     *
     *     the request failed (-3):
     *     a packet that was not TCP on an open connection
     *
     * Protocol 1 is CONTROL, and it is how an iPhone's usbmux sends the
     * host a message in words. usbmuxd (device.c, device_control_input)
     * takes the first payload byte as a type -- 3 is an error, 7 is
     * information -- and the rest as text, LOGS IT, and carries on with
     * the connection exactly as it was. So the phone had explained what
     * it objected to, in a sentence, and this driver threw the sentence
     * away unread and hung up.
     *
     * Now it is kept -- printable bytes only, so a phone's message
     * cannot put control characters on the console -- logged, counted,
     * and skipped: USB_ETIMEOUT is what every caller already reads as
     * "nothing for you yet, keep polling". usbmux_device_said() hands
     * it to whoever is about to explain a failure on screen.
     *
     * The type values are usbmuxd's, not off the wire: no CONTROL
     * packet has been captured from a real phone yet. The next photo
     * of `pair` will be the first. */
    if (proto == MUX_PROTO_CONTROL) {
        const uint8_t *p = (const uint8_t *)usbmux_payload();
        int i, k = 0;
        g_m.dev_msgs++;
        g_m.dev_msg_type = r > 0 ? p[0] : 0;
        for (i = 1; i < r && k < (int)sizeof g_m.dev_msg - 1; i++) {
            uint8_t c = p[i];
            if (c == 0) break;
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            g_m.dev_msg[k++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        while (k > 0 && g_m.dev_msg[k - 1] == ' ') k--;
        g_m.dev_msg[k] = 0;
        klog(g_m.dev_msg_type == 3 ? KLOG_WARN : KLOG_INFO,
             "usbmux: the device says (type %u): %s",
             (uint32_t)g_m.dev_msg_type, k ? g_m.dev_msg : "(no text)");
        return USB_ETIMEOUT;
    }

    if (proto != MUX_PROTO_TCP) {
        g_m.why = "a packet that was not TCP on an open connection";
        klog(KLOG_WARN, "usbmux: protocol %u on an open connection, "
             "expected TCP", proto);
        return USB_EXFER;
    }
    /* usbmuxd refuses a TCP packet shorter than its header before
       looking at any field in it, and so does this: a twenty-byte
       header read out of an eighteen-byte packet reads whatever is
       after it in the buffer, which on the second packet is the first
       packet's fields and looks entirely plausible. That is the stale-
       buffer shape tests/test_usbmux.c already carries a check for one
       layer up. */
    if (r < MUX_TCP_HDR) {
        g_m.why = "a TCP packet shorter than its own header";
        klog(KLOG_WARN, "usbmux: TCP packet of %d bytes, shorter than "
             "its own header", (uint32_t)r);
        return USB_EXFER;
    }

    th = (const uint8_t *)usbmux_payload();

    /* ── THE PORTS ARE CHECKED, which usbmuxd does by looking the
       connection up by them. One connection here means one pair, and
       a packet for a different pair is a packet for a connection some
       previous host opened. Taking its payload as this conversation's
       reply is how a plist from somebody else's session gets parsed as
       lockdownd's answer. Note the swap: the device's SOURCE port is
       our destination. */
    dport = be16(*(const uint16_t *)(th + MUX_TH_SPORT));
    sport = be16(*(const uint16_t *)(th + MUX_TH_DPORT));
    if (sport != g_m.sport || dport != g_m.dport) {
        g_m.why = "a packet for a different port pair -- another "
                  "connection's traffic on the same bulk pipe";
        klog(KLOG_WARN, "usbmux: TCP %u->%u is not this connection's "
             "%u->%u", (uint32_t)dport, (uint32_t)sport,
             (uint32_t)g_m.dport, (uint32_t)g_m.sport);
        return USB_EXFER;
    }

    if (flags_out) *flags_out = th[MUX_TH_FLAGS];
    if (seq_out)   *seq_out   = be32(*(const uint32_t *)(th + MUX_TH_SEQ));
    if (ack_out)   *ack_out   = be32(*(const uint32_t *)(th + MUX_TH_ACK));
    g_m.peer_win = (uint32_t)be16(*(const uint16_t *)(th + MUX_TH_WIN)) << 8;

    return r - MUX_TCP_HDR;
}

/* Where this packet's TCP payload starts. */
static const uint8_t *mux_tcp_payload(void)
{
    return (const uint8_t *)usbmux_payload() + MUX_TCP_HDR;
}

int usbmux_connected(void)   { return g_m.conn == 2; }
int usbmux_refused(void)     { return g_m.refused; }
int usbmux_max_payload(void) { return MUX_MAX_PAYLOAD; }

/* The last thing this layer refused, in words, or a string saying it
   has not refused anything. Never null: a caller that has to test for
   one will one day not, and "(null)" on a diagnostic screen is worse
   than a sentence. */
/* v0.8.9.7: the last thing the device said in a CONTROL packet, or 0
   when it has said nothing. *type_out gets usbmuxd's type: 3 is an
   error, 7 is information. */
uint32_t usbmux_zlps(void) { return g_m.zlps; }

const char *usbmux_device_said(int *type_out, uint32_t *count_out)
{
    if (type_out)  *type_out  = g_m.dev_msg_type;
    if (count_out) *count_out = g_m.dev_msgs;
    return g_m.dev_msgs ? g_m.dev_msg : 0;
}

const char *usbmux_why(void)
{
    return g_m.why ? g_m.why : "the mux layer has not reported a reason";
}

int usbmux_connect(uint16_t dport)
{
    uint8_t  flags = 0;
    uint32_t seq = 0, ack = 0;
    int      r, tries;

    g_m.refused = 0;
    g_m.why     = 0;        /* a new connection owes nothing to the last */
    /* The device's messages are NOT cleared: one sent while the last
       connection died is exactly the one worth reading after it. */

    if (!g_m.open) return USB_ENODEV;

    /* Idempotent, so a caller does not have to remember the order --
       and free when it has already run. */
    r = usbmux_handshake();
    if (r < 0)  return r;
    if (r == 0) return USB_ETIMEOUT;   /* the device never answered VERSION */

    if (g_m.conn) usbmux_disconnect();

    /* ── 1, NOT 2, AND WHAT THAT IS AND IS NOT WORTH ─────────────────
       mux_tcp_send() refuses a closed connection, so the SYN cannot go
       out before this is set to something. It is set to 1 so that
       usbmux_send() and usbmux_recv(), which both require 2, refuse
       while the handshake is in flight, and so usbmux_connected()
       does not claim a connection that is half open.

       AN EARLIER COMMENT HERE CLAIMED MORE THAN THE CODE DOES: that a
       SYN/ACK for a connection already up would be refused by a state
       machine below. There is no such check -- usbmux_connect() tears
       down an existing connection and starts over. The claim was
       wrong, and this tree's own rule is that when two comments
       disagree one of them is a bug; a comment disagreeing with the
       code it sits on is the same thing.

       tools/check_usbmux_suite.py reports the 1-versus-2 distinction
       as unreachable, and correctly: nothing can run between the SYN
       and the ACK in a kernel where the handshake is synchronous. It
       becomes reachable at 0.8.8, where kernel_poll() runs real work
       during exactly this kind of wait. The injection is recorded
       there, not deleted. */
    g_m.conn       = 1;
    g_m.sport      = MUX_SPORT;
    g_m.dport      = dport;
    g_m.tx_tcp_seq = 0;
    g_m.tx_tcp_ack = 0;
    g_m.rx_isn     = 0;
    g_m.peer_win   = 0;
    g_m.pend_len   = 0;

    r = mux_tcp_send(MUX_TH_SYN, 0, 0);
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: the SYN to port %u would not send (%d)",
             (uint32_t)dport, (uint32_t)r);
        g_m.conn = 0;
        return r;
    }
    klog(KLOG_INFO, "usbmux: SYN -> port %u, from %u",
         (uint32_t)dport, (uint32_t)g_m.sport);

    for (tries = 0; tries < MUX_POLL_TRIES; tries++) {
        r = mux_tcp_recv(&flags, &seq, &ack);
        if (r != USB_ETIMEOUT) break;
    }
    if (r == USB_ETIMEOUT) {
        klog(KLOG_WARN, "usbmux: no answer to the SYN after %d polls",
             (uint32_t)tries);
        g_m.conn = 0;
        return USB_ETIMEOUT;
    }
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: the SYN/ACK was unreadable (%d)",
             (uint32_t)r);
        g_m.conn = 0;
        return r;
    }

    /* ── AN RST IS AN ANSWER, and a different one from silence ───────
       The device saying "that port is closed" has told you something.
       Reporting it as the same failure as a channel that broke sends
       the next person to re-check the framing when the framing worked
       perfectly. usbmux_refused() carries the distinction out. */
    if (flags & MUX_TH_RST) {
        klog(KLOG_WARN, "usbmux: port %u REFUSED the connection (RST)",
             (uint32_t)dport);
        g_m.conn    = 0;
        g_m.refused = 1;
        return USB_ESTALL;
    }
    /* usbmuxd compares the whole flags byte against SYN|ACK rather
       than masking, and that strictness is the point: a SYN with no
       ACK, or a FIN riding along, is not a completed handshake and
       continuing into one is how a connection that was never
       established starts carrying a conversation. */
    if (flags != (MUX_TH_SYN | MUX_TH_ACK_F)) {
        klog(KLOG_WARN, "usbmux: flags %2x answering the SYN, expected "
             "%2x", (uint32_t)flags,
             (uint32_t)(MUX_TH_SYN | MUX_TH_ACK_F));
        g_m.conn = 0;
        return USB_EXFER;
    }

    /* ── THE ACK ARITHMETIC, and the one place this is NOT usbmuxd ───
       usbmuxd does `conn->tx_seq++; conn->tx_ack++;` from zero, which
       produces the right ack only because the device's initial
       sequence number is 0. It has always been 0, which is why that
       has never failed -- but it is an assumption, not a derivation,
       and it is unfalsifiable as written: if the device ever picked a
       different ISN, usbmuxd would ack the wrong byte and the symptom
       would be a conversation that stalls for no visible reason.

       So: ack the SYN the device actually sent, isn + 1. For isn == 0
       that is byte-identical to usbmuxd, so nothing is risked. For any
       other isn it is correct where usbmuxd is wrong -- and because
       that case has never been seen by anyone, it is LOGGED rather
       than quietly handled. A deviation nobody is told about is a
       deviation nobody can check. */
    g_m.rx_isn = seq;
    if (seq != 0)
        klog(KLOG_WARN, "usbmux: the device's ISN is %u, not 0 -- first "
             "time anyone has seen that", seq);

    g_m.tx_tcp_seq = 1;             /* our SYN consumed one sequence number */
    g_m.tx_tcp_ack = seq + 1;       /* ...and so did theirs */

    r = mux_tcp_send(MUX_TH_ACK_F, 0, 0);
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: the final ACK would not send (%d)",
             (uint32_t)r);
        g_m.conn = 0;
        return r;
    }

    g_m.conn = 2;
    klog(KLOG_OK, "usbmux: connected to port %u, seq %u ack %u",
         (uint32_t)dport, g_m.tx_tcp_seq, g_m.tx_tcp_ack);
    return USB_OK;
}

int usbmux_send(const void *data, int len)
{
    int r;

    if (g_m.conn != 2) return USB_ENODEV;
    if (len < 0)       return USB_EPARAM;
    /* The caller is told the bound rather than having its data
       silently cut in half. usb/lockdown.c splits above this. */
    if (len > MUX_MAX_PAYLOAD) {
        klog(KLOG_WARN, "usbmux: %d bytes is more than one packet can "
             "carry (%d)", (uint32_t)len, (uint32_t)MUX_MAX_PAYLOAD);
        return USB_EPARAM;
    }

    /* Data rides on an ACK. usbmuxd sends TH_ACK with a payload and
       never sets PSH, so neither does this. */
    r = mux_tcp_send(MUX_TH_ACK_F, data, len);
    if (r < 0) return r;

    /* The sequence number advances by the BYTES sent, which is the one
       place in this protocol where a counter really is a counter. */
    g_m.tx_tcp_seq += (uint32_t)len;
    return USB_OK;
}

/* ── one non-blocking look at the wire, mid-send ─────────────────────

   Called between the segments of a long request. Three outcomes:

     a bare ACK   the window it carries is already recorded by
                  mux_tcp_recv()'s caller path; nothing else to do
     data         stashed, and ACKED, exactly as usbmux_recv() would
     nothing      the common case, and not a failure

   It does NOT run when the stash is occupied, which is what keeps one
   packet of stash sufficient. It returns a negative code only for a
   connection that has actually broken, because a broken connection
   half way through an eight-kilobyte request should stop the request
   rather than finish sending into a closed socket. */
/* ════════════════════════════════════════════════════════════════════
   THE SEQUENCE NUMBER -- v0.8.9.2, and the bug it fixes was on the HP

   Until this release NOTHING in the receive path looked at the
   sequence number of an arriving packet. `rx_isn` was recorded during
   the handshake and then never read again, and both readers advanced
   the acknowledgement by ADDING the payload length:

       g_m.tx_tcp_ack += (uint32_t)r;

   which is a count of bytes this side has seen, not a position in the
   device's stream. Those are the same number only while every packet
   arrives exactly once.

   WHAT IT LOOKED LIKE. A reply delivered twice. On Kyan's HP, a
   lockdown QueryType returned the right answer, and the GetValue that
   followed returned THE SAME QueryType REPLY again -- byte for byte,
   301 bytes, correctly framed. From then on every request got the
   previous request's answer:

       QueryType  -> the GetValue reply   (918 bytes)
       GetValue   -> the QueryType reply  (301 bytes)

   Every reply parses perfectly and contains the wrong thing, so the
   error surfaces as "no Type in it" -- an entire stage of pairing
   reported as a protocol failure when the transport had simply gone
   one message out of step. It survived disconnect and reconnect,
   because a leftover packet carrying an old sequence number was
   indistinguishable from new data.

   A duplicate is not an exotic event: it is what TCP does whenever an
   ACK is late or lost, and v0.8.8.10 added a second reader
   (mux_harvest) that acks on its own. The transport is reliable in the
   sense that bytes are not corrupted; it was never reliable in the
   sense that a packet cannot arrive twice.

   THE RULE, which is simply TCP's. `tx_tcp_ack` IS the next byte this
   side expects, so a data packet is new if and only if its sequence
   number equals it. Anything else is dropped and re-acknowledged, so
   the device is told where we actually are rather than left to guess.
   And the acknowledgement is DERIVED from the packet -- seq + length,
   never a running total -- so it cannot drift away from the stream it
   describes.

   Both readers use it, so it is one function. Two copies of this would
   be two chances to get the comparison backwards, and it is the kind
   of wrong that still passes every test where nothing is ever
   duplicated.
   ════════════════════════════════════════════════════════════════════ */
static int mux_rx_accept(uint32_t seq, int r)
{
    if (seq == g_m.tx_tcp_ack) {
        /* Derived, not accumulated. */
        g_m.tx_tcp_ack = seq + (uint32_t)r;
        return 1;
    }

    g_m.dup_drops++;
    klog(KLOG_WARN, "usbmux: %d bytes at seq %u, expected %u -- dropped "
         "and re-acked (%u so far)", (uint32_t)r, seq, g_m.tx_tcp_ack,
         g_m.dup_drops);

    /* Re-ACK with the position we are actually at. A silent drop
       leaves the device repeating the same packet forever. */
    if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)
        klog(KLOG_WARN, "usbmux: could not re-ACK at %u", g_m.tx_tcp_ack);
    return 0;
}

static int mux_harvest(void)
{
    uint8_t  flags = 0;
    uint32_t seq = 0, ack = 0;
    int      r;

    if (g_m.pend_len > 0) return 0;

    r = mux_tcp_recv(&flags, &seq, &ack);
    if (r == USB_ETIMEOUT) return 0;            /* nothing waiting */
    if (r < 0)             return r;

    if (flags & MUX_TH_RST) {
        g_m.why = "the device reset the connection part way through a "
                  "request";
        klog(KLOG_WARN, "usbmux: the device reset the connection "
             "part way through a request");
        g_m.conn = 0;
        return USB_ENODEV;
    }
    /* v0.8.9.5: the same split as usbmux_recv(), for the same reason.
       Both readers see the same wire and a reason that depends on
       which of them happened to be looking is not a reason. */
    if (flags & MUX_TH_FIN) {
        g_m.fins++;
        g_m.why = "the device closed the connection (FIN) part way "
                  "through a request";
        klog(KLOG_WARN, "usbmux: the device sent FIN (flags %2x) "
             "mid-request", (uint32_t)flags);
        g_m.conn = 0;
        return USB_ENODEV;
    }
    if (flags != MUX_TH_ACK_F) {
        g_m.why = "a mid-request packet whose TCP flags are neither a "
                  "bare ACK, a FIN nor an RST";
        klog(KLOG_WARN, "usbmux: flags %2x mid-request, expected %2x",
             (uint32_t)flags, (uint32_t)MUX_TH_ACK_F);
        g_m.conn = 0;
        return USB_EXFER;
    }
    if (r == 0) return 0;                       /* the ACK we wanted */

    if (r > (int)sizeof g_m.pend) {
        /* Cannot happen -- mux_tcp_recv is bounded by MUX_MAX_PAYLOAD
           and so is the stash -- and is checked anyway, because the
           alternative to this branch is a memcpy past the end of a
           struct field. */
        klog(KLOG_WARN, "usbmux: %d bytes mid-request will not fit the "
             "stash (%d)", (uint32_t)r, (uint32_t)sizeof g_m.pend);
        return USB_EPARAM;
    }

    /* A duplicate must not be STASHED either -- a stale packet put
       aside here is delivered to the next reader as though it were
       what that reader asked for, which is precisely how a reply ends
       up answering the request after it. */
    if (!mux_rx_accept(seq, r)) return 0;

    memcpy(g_m.pend, mux_tcp_payload(), (uint32_t)r);
    g_m.pend_len = r;
    g_m.pend_hits++;

    /* Acked here and not later, for the reason usbmux_recv() gives:
       the device stops sending once its window is nominally full. */
    if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)
        klog(KLOG_WARN, "usbmux: could not ACK %d stashed bytes",
             (uint32_t)r);
    return 0;
}

/* ── a request longer than one packet ────────────────────────────────

   usbmux_send() carries at most MUX_MAX_PAYLOAD (2012) bytes, which
   was enough for every message up to stage 2. Stage 3's pair request
   carries three certificates and a public key and runs to seven or
   eight kilobytes, so it has to be split -- and TCP is a byte stream,
   so splitting it is legitimate: lockdownd reads four bytes of length
   and then that many, however they are packetised.

   ── THE WINDOW, AND WHY THIS WARNS INSTEAD OF BLOCKING ──────────────

   usbmuxd enforces the device's advertised window: it will not put
   more unacknowledged bytes on the wire than the window allows, and
   it resumes from an output buffer when an ACK widens it. This does
   not, and the reason is specific rather than lazy.

   The window arrives in units of 256 bytes and the device advertises
   131072 of them, so a ten-kilobyte request is nowhere near it and
   the enforcement path would never run -- which means it would never
   be exercised, on hardware or in a suite, and an unexercised blocking
   path in a transport is how a working transfer becomes a hang. What
   is here instead: the running total of unacknowledged bytes is
   compared against the window before every segment, and exceeding it
   is COUNTED AND LOGGED by name, then sent anyway.

   So if a device ever does advertise a window this fills, the next
   `usbmux` dump says so in one line, and that is the measurement that
   would justify writing the blocking path. Guessing at it now would
   not.

   Returns USB_OK, or the first failure, having sent the segments
   before it. A partial request is reported rather than retried: the
   stream is now at an offset lockdownd cannot parse from, so the
   caller's job is to tear the connection down, and tests assert that
   the byte count says where it stopped. */
int usbmux_send_stream(const void *data, int len, int *sent_out)
{
    const uint8_t *p = (const uint8_t *)data;
    int off = 0, segs = 0, r;

    if (sent_out) *sent_out = 0;
    if (g_m.conn != 2)      return USB_ENODEV;
    if (len < 0 || !data)   return USB_EPARAM;
    if (len == 0)           return USB_OK;

    while (off < len) {
        int chunk = len - off;
        if (chunk > MUX_MAX_PAYLOAD) chunk = MUX_MAX_PAYLOAD;

        /* Unacknowledged bytes if this segment goes out. Nothing
           here tracks which of OUR bytes the device has acked, so
           this is the conservative figure: every byte of this request
           so far. Conservative is the right direction for a counter
           whose only job is to tell us whether the window was ever
           close to full. */
        if (segs > 0) {
            uint32_t outstanding = (uint32_t)(off + chunk);
            if (g_m.peer_win && outstanding > g_m.peer_win) {
                g_m.win_short++;
                klog(KLOG_WARN, "usbmux: %u unacknowledged bytes past "
                     "the device's %u-byte window -- sending anyway, "
                     "see usbmux_send_stream", outstanding,
                     g_m.peer_win);
            }
        }

        r = usbmux_send(p + off, chunk);
        if (r < 0) {
            klog(KLOG_WARN, "usbmux: segment %d of a %d-byte request "
                 "would not send (%d) -- %d bytes are already on the "
                 "wire", (uint32_t)(segs + 1), (uint32_t)len,
                 (uint32_t)r, (uint32_t)off);
            if (sent_out) *sent_out = off;
            return r;
        }
        off  += chunk;
        segs++;
        if (sent_out) *sent_out = off;

        /* Between segments only. After the last one the caller is
           about to read the reply, and a harvest there would stash a
           packet the caller's own loop is better placed to handle. */
        if (off < len) {
            r = mux_harvest();
            if (r < 0) {
                klog(KLOG_WARN, "usbmux: the connection broke after "
                     "%d of %d bytes", (uint32_t)off, (uint32_t)len);
                return r;
            }
        }
    }

    if (segs > 1)
        klog(KLOG_INFO, "usbmux: sent %d bytes in %d packets",
             (uint32_t)len, (uint32_t)segs);
    return USB_OK;
}

/* ════════════════════════════════════════════════════════════════════
   WHAT usbmux_send_stream() WAS PROVED AGAINST

   Twenty defects were injected into this function, mux_harvest() and
   the stash branch of usbmux_recv(), one at a time, each with a
   prediction written down first. Seventeen were caught. The three
   that were not are all REDUNDANCY rather than gaps, and are recorded
   here so nobody mistakes them for untested code:

     - Removing `if (g_m.conn != 2) return USB_ENODEV;` from this
       function survives, because usbmux_send() checks the same thing
       one layer down and the loop fails on its first segment. The
       check stays: it returns before touching the caller's data.

     - Removing the RST branch from mux_harvest() survives, because
       the `flags != MUX_TH_ACK_F` test immediately below it also
       takes the connection down. The branch stays because the log
       line names what happened, and "flags 04 mid-request" is a
       worse thing to read on a screen than "the device reset the
       connection".

     - Removing the `return r` after a failed harvest survives,
       because the next usbmux_send() sees conn == 0 and fails
       identically. It stays because it stops one pointless bulk
       transfer.

   The seventeen caught include the ones that matter and that only a
   byte-for-byte comparison can see: sending the first chunk every
   time (right byte count, wrong bytes), advancing the offset by a
   whole payload instead of by the chunk, skipping a trailing partial
   segment, splitting one byte short, a stash that is dropped,
   overwritten by a second packet, served after newer data, truncated
   into a small caller buffer, not acked at all, or acked by the wrong
   amount -- and a stash left behind by usbmux_disconnect().

   Closing two of those required the fixture to be able to hold TWO
   packets in flight (tests/muxdev.inc, in2) and required asserting
   the ACK VALUE rather than its presence: the device model rejects an
   ack that is too high and accepts one that is too low, so a missing
   ack is invisible to every other check in the suite.
   ════════════════════════════════════════════════════════════════════ */

/* How many bytes are waiting in the mid-send stash, and how many
   times it has been used. For `usbmux` and for the suites. */
int usbmux_pending(uint32_t *hits, uint32_t *win_short)
{
    if (hits)      *hits      = g_m.pend_hits;
    if (win_short) *win_short = g_m.win_short;
    return g_m.pend_len;
}

int usbmux_recv(void *buf, int max)
{
    uint8_t  flags = 0;
    uint32_t seq = 0, ack = 0;
    int      r;

    if (g_m.conn != 2) return USB_ENODEV;
    if (max < 0)       return USB_EPARAM;

    /* ── the stash, first and in order ───────────────────────────────
       Bytes collected by usbmux_send_stream() are OLDER than anything
       still on the wire, and a byte stream that delivers them second
       is a byte stream that has reordered itself. They were acked
       when they were taken, so nothing further is owed to the device
       here. */
    if (g_m.pend_len > 0) {
        if (g_m.pend_len > max) {
            klog(KLOG_WARN, "usbmux: %d stashed bytes and the caller "
                 "has room for %d", (uint32_t)g_m.pend_len,
                 (uint32_t)max);
            return USB_EPARAM;
        }
        r = g_m.pend_len;
        memcpy(buf, g_m.pend, (uint32_t)r);
        g_m.pend_len = 0;
        return r;
    }

    r = mux_tcp_recv(&flags, &seq, &ack);
    if (r == USB_ETIMEOUT) return 0;        /* nothing yet */
    if (r < 0)             return r;

    if (flags & MUX_TH_RST) {
        g_m.why = "the device reset the connection";
        klog(KLOG_WARN, "usbmux: the device reset the connection");
        g_m.conn = 0;
        return USB_ENODEV;
    }

    /* ── v0.8.9.5: A FIN IS NOT A TRANSFER ERROR ─────────────────────
       usbmuxd has no half-close, so this still tears the connection
       down -- that part was right. What was wrong is that it did so
       through the catch-all below, which reports USB_EXFER, the code
       for CRC, babble and buffer faults. A device that closed a
       connection politely was indistinguishable from a bus that had
       corrupted a packet.

       On Kyan's HP that produced `the request did not get a reply
       (-3)` after a 71-second keygen, and -3 sent the diagnosis
       towards the USB layer, where nothing was wrong. lockdownd had
       closed an idle connection, which is the ordinary thing for it to
       do and the one outcome the error code could not say.

       FIN|ACK is 0x11 and a bare ACK is 0x10, which is why this has to
       be tested before equality with MUX_TH_ACK_F and not after. */
    if (flags & MUX_TH_FIN) {
        g_m.fins++;
        g_m.why = "the device closed the connection (FIN). lockdownd "
                  "does this to a connection left idle -- generate the "
                  "keys first, then connect";
        klog(KLOG_WARN, "usbmux: the device sent FIN (flags %2x) -- it "
             "closed the connection", (uint32_t)flags);
        g_m.conn = 0;
        return USB_ENODEV;
    }

    if (flags != MUX_TH_ACK_F) {
        g_m.why = "a packet whose TCP flags are neither a bare ACK, a "
                  "FIN nor an RST";
        klog(KLOG_WARN, "usbmux: flags %2x on an established "
             "connection, expected %2x", (uint32_t)flags,
             (uint32_t)MUX_TH_ACK_F);
        g_m.conn = 0;
        return USB_EXFER;
    }

    if (r == 0) return 0;                   /* a bare ACK is not data */

    if (r > max) {
        /* Refused, not truncated: a short read here would leave the
           rest of the device's byte stream unacknowledged and the
           connection would wedge with no error anywhere. */
        klog(KLOG_WARN, "usbmux: %d bytes arrived and the caller has "
             "room for %d", (uint32_t)r, (uint32_t)max);
        return USB_EPARAM;
    }

    /* Before the copy: a duplicate must not reach the caller at all.
       See mux_rx_accept(). */
    if (!mux_rx_accept(seq, r)) return 0;

    memcpy(buf, mux_tcp_payload(), (uint32_t)r);

    /* ── ACK IT NOW ──────────────────────────────────────────────────
       usbmuxd's own comment: "Device likes it best when we are prompty
       ACKing data". A receiver that does not ack stops receiving once
       its window is nominally full, and the symptom is a reply that
       arrives in one piece and then nothing ever again -- which looks
       exactly like a device that has lost interest. */
    if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)
        klog(KLOG_WARN, "usbmux: could not ACK %d bytes -- the device "
             "will stop sending", (uint32_t)r);

    return r;
}

void usbmux_disconnect(void)
{
    if (g_m.conn) {
        /* usbmuxd closes with an RST rather than a FIN; there is no
           graceful shutdown in this protocol. Best effort: a channel
           that has already broken cannot be told politely. */
        mux_tcp_send(MUX_TH_RST, 0, 0);
    }
    g_m.conn       = 0;
    g_m.sport      = 0;
    g_m.dport      = 0;
    g_m.tx_tcp_seq = 0;
    g_m.tx_tcp_ack = 0;
    g_m.rx_isn     = 0;
    g_m.peer_win   = 0;
    g_m.pend_len   = 0;
    /* dup_drops is NOT cleared: it counts the life of the bus, and a
       connection that has to be reopened is exactly when somebody
       wants to know how many duplicates got us here. `usb mux` shows
       it. */
}

void usbmux_conn_state(uint16_t *sport, uint16_t *dport,
                       uint32_t *tx_seq, uint32_t *tx_ack,
                       uint32_t *win)
{
    if (sport)  *sport  = g_m.sport;
    if (dport)  *dport  = g_m.dport;
    if (tx_seq) *tx_seq = g_m.tx_tcp_seq;
    if (tx_ack) *tx_ack = g_m.tx_tcp_ack;
    if (win)    *win    = g_m.peer_win;
}

/* ── what `usbmux` prints ────────────────────────────────────────── */

int usbmux_dump(void)
{
    const usb_dev_t *d;
    int i;

    if (!g_m.open) return 0;
    d = usb_device(g_m.dev);

    klog(KLOG_INFO, "usbmux  %4x:%4x  if %u  ep %2x/%2x",
         d ? (uint32_t)d->vendor : 0, d ? (uint32_t)d->product : 0,
         (uint32_t)g_m.ifnum, (uint32_t)g_m.ep_in, (uint32_t)g_m.ep_out);
    klog(KLOG_INFO, "        version %d  tx %u  rx %u  errs %u",
         (uint32_t)g_m.version, g_m.sent, g_m.received, g_m.errs);
    /* v0.8.9.2. Not zero means packets arrived that were not the next
       bytes of the stream -- duplicates, almost always. Before this
       release they were DELIVERED, and every reply after one was a
       reply to the previous request. Counted for the life of the bus,
       so a reconnect does not hide them. */
    klog(KLOG_INFO, "        out-of-sequence packets dropped: %u",
         g_m.dup_drops);
    /* The mux sequence pair, printed because v0.8.7.6 had all three
       of its rules wrong and nothing on screen would have shown it.
       After a SETUP and nothing else these read tx 1 rx FFFF. */
    klog(KLOG_INFO, "        mux seq: tx %u  rx %4x",
         (uint32_t)g_m.tx_seq, (uint32_t)g_m.rx_seq);
    if (g_m.conn) {
        klog(KLOG_INFO, "        conn %u->%u  %s  seq %u ack %u",
             (uint32_t)g_m.sport, (uint32_t)g_m.dport,
             g_m.conn == 2 ? "up" : "connecting",
             g_m.tx_tcp_seq, g_m.tx_tcp_ack);
        /* Their window in BYTES. The header carries it in 256-byte
           units and printing the raw field is how somebody concludes
           the device offered seven bytes. */
        klog(KLOG_INFO, "        their window %u bytes, ours %u",
             g_m.peer_win, (uint32_t)MUX_WINDOW);
    }

    if (!g_m.have_last) {
        klog(KLOG_INFO, "        last packet: nothing received yet");
    } else {
        char line[64];
        int  p = 0, lim = g_m.last_len;
        if (lim > 16) lim = 16;
        for (i = 0; i < lim && p < (int)sizeof line - 4; i++) {
            const char *hx = "0123456789ABCDEF";
            line[p++] = hx[(g_m.last[i] >> 4) & 0xF];
            line[p++] = hx[g_m.last[i] & 0xF];
            line[p++] = ' ';
        }
        line[p] = 0;
        klog(KLOG_INFO, "        last packet: %d bytes", g_m.last_len);
        klog(KLOG_INFO, "        %s", line);
    }
    return 1;
}
