/* ═══════════════════════════════════════════════════════════════════════
   muxdev.inc -- ONE simulated iPhone, for the two suites that need one.

   ── WHY THIS IS A FILE AND NOT A COPY ───────────────────────────

   v0.8.7.7 added usb/lockdown.c, which runs over usb/usbmux.c. Testing
   it against a stubbed transport would test the stub; testing it
   against a second copy of this device would be two devices drifting
   apart, which is exactly what tools/check_diskname_sync.py exists to
   refuse and what the tree has been bitten by twice.

   So both tests/test_usbmux.c and tests/test_lockdown.c include this,
   and the device below is the only model of a phone in the tree.
   tests/blkbus.inc is the same arrangement one layer down.

   ── WHAT IT IS ───────────────────────────────────────────────

   A simulated host controller registered through usb_register_hc(), so
   usb_select_config(), usb_set_interface() and the descriptor walk are
   the kernel's own -- and behind it a device that VALIDATES what
   arrives rather than answering it. Every refusal is counted with a
   reason, so a suite can assert on why instead of on silence.

   The bounds it enforces are the REAL back-end's. v0.8.7's Ethernet
   suite passed 110 checks against a simulated controller that accepted
   a transfer of any length while the real one refused anything over
   64, and the driver could not move a byte on hardware.

   ── WHAT A SUITE ADDS ────────────────────────────────────

   This models the MUX and its TCP. It does not model any service
   listening on a port -- tests/test_lockdown.c supplies lockdownd by
   defining MUXDEV_SERVICE and a dev_service() of its own, which
   dev_rx() calls with each payload that arrives on an established
   connection.

   Include it AFTER the suite's klog stub and g_ms, and BEFORE
   usb/core.c.
   ═════════════════════════════════════════════════════════════════════ */

/* The real back-end's bounds. See tools/check_usb_buffers.py. */
#define SIM_SYNC_MAX   2048
#define SIM_ASYNC_MAX  2048

#define EP_IN   0x85
#define EP_OUT  0x06
/* v0.8.7.6: the header is version-dependent, which is what the HP
   taught this file. Eight bytes until both ends have agreed on version
   2, sixteen after -- the magic and the sequence numbers are a
   version-2 addition and cannot be on the packet that establishes the
   version. The fixture below used to answer with sixteen always, which
   is why 79 of 79 checks passed against framing the phone rejected. */
#define MUX_HDR_SHORT 8
#define MUX_HDR_FULL  16
#define MUX_HDR MUX_HDR_FULL

/* ── the simulated phone ─────────────────────────────────────────────
   Configuration index 1 carries the Apple Mobile Device interface as
   interface 1, which is where `usb cfg` found it on the HP. */
static const uint8_t CFG0[] = {
    9, 2, 25,0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 6, 1, 1, 0,            /* still image (PTP)         */
    7, 5, 0x81, 2, 64,0, 0
};
/* ── TWO 0xFF INTERFACES, and that is the point ───────────────────────
   The real phone's configuration 3 carries BOTH: FF/FD/01 is Apple USB
   Ethernet and FF/FE/02 is the Apple Mobile Device channel. A survey
   that matched on the class alone would claim the Ethernet interface
   and switch the configuration out from under it.

   The first version of this fixture had only the mux interface, so
   "matches 0xFF" and "matches the whole triple" were the same test.
   The harness caught it. The Ethernet one is listed FIRST, so a
   class-only match picks the wrong one rather than the right one by
   luck. */
static const uint8_t CFG1[] = {
    9, 2, 50,0, 3, 2, 0, 0xA0, 50,
    9, 4, 0, 0, 0, 6, 1, 1, 0,            /* if 0: PTP                 */
    9, 4, 1, 0, 0, 0xFF, 0xFD, 0x01, 0,   /* if 1: APPLE ETHERNET      */
    9, 4, 2, 0, 2, 0xFF, 0xFE, 0x02, 0,   /* if 2: APPLE MOBILE DEVICE */
    7, 5, EP_IN,  2, 0x00,0x02, 0,
    7, 5, EP_OUT, 2, 0x00,0x02, 0
};
/* Byte offsets into CFG1, written out rather than counted in the head:
   the first version of the no_mux flag blanked a byte inside an
   endpoint descriptor. */
#define CFG1_IF_ETH   18
#define CFG1_IF_MUX   27
/* v0.8.7.6: a configuration with the mux interface and NO Ethernet,
   which is the real phone's index 2. Index 3 carries both. usbmux_open
   took the FIRST configuration with a mux interface, so it chose 2 and
   threw away the configuration tethering needs -- every time `usb mux`
   ran. The fixture had no such configuration, so the choice could not
   be wrong here. */
static const uint8_t CFG2[] = {
    9, 2, 32,0, 2, 3, 0, 0xA0, 50,
    9, 4, 0, 0, 0, 6, 1, 1, 0,            /* if 0: PTP                 */
    9, 4, 1, 0, 2, 0xFF, 0xFE, 0x02, 0,   /* if 1: APPLE MOBILE DEVICE */
    7, 5, EP_IN,  2, 0x00,0x02, 0,
    7, 5, EP_OUT, 2, 0x00,0x02, 0
};

static const uint8_t *g_cfgs[2]   = { CFG0, CFG1 };
static const uint16_t g_cfglen[2] = { (uint16_t)sizeof CFG0,
                                      (uint16_t)sizeof CFG1 };

static struct {
    int nconfigs;
    int phone_shape;       /* three configs, mux-only before mux+eth   */
    int no_mux;            /* the interface is not offered            */
    int refuse_setcfg;
    int silent;            /* accepts packets, never answers          */
    int bad_magic;         /* answers with the wrong magic            */
    int short_reply;       /* answers with fewer than 16 bytes        */
    int bad_len;           /* a valid magic, but length under 16      */
    int wrong_proto;       /* answers, but not with a VERSION         */
    int one_ep;            /* the interface offers one bulk endpoint  */
    /* v0.8.9.7: the transfer the device is still reading, because the
       last USB packet of it was full-sized. See s_bulk(). */
    uint8_t xfer[8192];
    int  xfer_len;
    int  xfer_overrun;
    int  xfer_calls;       /* non-empty OUTs in the transfer so far   */
    int  zlps;             /* zero-length packets received            */
    int  glued;            /* transfers that held more than one call  */
    int set_cfgs;          /* how many SET_CONFIGURATIONs were sent   */
    int dev_version;       /* what it says it is                      */

    int cfg_index_asked;
    int set_cfg_value;
    int sync_refused, async_refused;

    /* what the host sent, last packet */
    uint8_t out[2048];
    int     out_len, out_calls;
    /* ── THE DEVICE'S OWN TCP STATE MACHINE, v0.8.7.7 ─────────────────
       v0.8.7's Ethernet suite passed 110 checks against a simulated
       CONTROLLER that accepted a transfer of any length, and the driver
       could not move a byte on hardware. The lesson in include/usb.h
       is that a mock which cannot refuse proves nothing, and the one
       below it -- `test_usbmux` passing 79 of 79 against a header the
       phone has never used -- is that a fixture written from the
       driver's own guess agrees with it perfectly.

       So this is not a parrot. It is a device that validates what
       arrives the way usbmuxd's device_data_input() and
       device_tcp_input() validate it, and records WHY it refused, so a
       test can assert on the reason rather than on silence:

         - a mux length field that does not equal the transfer length
         - a missing magic once the version is settled
         - a TCP header that is not five words
         - ports that are not this connection's
         - A SEQUENCE NUMBER THAT IS NOT THE ONE IT IS EXPECTING

       That last one is the whole reason this exists. The sequence
       arithmetic is the part of stage 1 with no screen of its own, and
       a device that shrugs at a wrong seq cannot tell a correct driver
       from one that merely sends plausible-looking packets.

       EVERY OFFSET BELOW IS WRITTEN AS A NUMBER, not taken from
       include/usbmux.h. A fixture that reads the layout out of the
       header it is checking cannot disagree with it. */
    int      dev_hdr;          /* 8 until it has answered VERSION, then 16 */
    int      setup_seen;
    uint16_t setup_tx, setup_rx;   /* the mux pair on the SETUP packet   */
    uint16_t saw_mux_tx, saw_mux_rx;  /* ...and on the last packet of all */
    uint16_t dev_mux_rx;       /* what IT writes in its own rx_seq field */

    int      listen_port;      /* the only port it will accept          */
    int      refuse_port;      /* answer the SYN with RST               */
    int      syn_flags;        /* flags to answer the SYN with, 0 = SYN|ACK */
    uint32_t dev_isn;          /* its initial sequence number           */

    int      tcp_state;        /* 0 closed, 1 SYN seen, 2 established   */
    int      syns;             /* SYNs it has accepted, ever            */
    uint16_t exp_sport;        /* the source port it saw on the SYN     */
    uint16_t reply_port;       /* the port it answers FROM -- addressed */
    uint32_t exp_host_seq;     /* the seq it will accept next           */
    uint32_t dev_seq;          /* its own byte position                 */
    uint32_t last_host_ack;    /* the last ack it was given             */
    uint8_t  saw_flags;
    uint32_t saw_seq, saw_ack, saw_win;
    int      saw_off;
    int      saw_checksum;     /* counted, not refused -- see below     */

    int      reject_len, reject_magic, reject_off, reject_ports;
    int      reject_seq, reject_flags;

    uint32_t host_bytes;       /* payload bytes accepted from the host  */
    uint8_t  host_data[2048];
    int      host_data_len;

    /* ── v0.8.9.0: THE HOST'S BYTE STREAM, NOT ITS LAST PACKET ───────
       host_data above holds the most recent payload and nothing
       else, which was enough while every request fitted one packet.
       usbmux_send_stream() splits a request across several, and the
       question that then matters is not what the last packet held but
       whether the WHOLE request arrived, in order, exactly once.

       So this accumulates. A suite asserts on these bytes rather than
       on a count, because a count cannot tell a correct eight-
       kilobyte request from one with a segment duplicated and another
       dropped -- which is the defect shape a split sender has and a
       single-packet sender cannot. */
    uint8_t  host_stream[20480];
    uint32_t host_stream_len;
    int      host_stream_over;  /* it sent more than this can hold     */
    int      host_segs;         /* payload-bearing packets from the host */

    /* ── acking the host, which the real device does ─────────────────
       OFF by default, and that is a fixture limitation stated rather
       than hidden: `in` below holds ONE packet, so an unsolicited ACK
       would displace a reply a suite had queued, and every suite
       written before this field assumed it would not.

       A suite that is testing the mid-send harvest turns it on. */
    int      auto_ack;
    int      acks_sent;

    /* The window it advertises, in 256-byte units. 512 is usbmuxd's
       131072 bytes and the default; a suite that wants to see the
       host notice a full window sets it small. */
    int      dev_win_units;
    /* what is waiting to be read */
    uint8_t in[2048];
    int     in_len, in_polls;
    /* ── v0.8.9.0: A SECOND PACKET IN FLIGHT ─────────────────────────
       One slot was enough while nothing read the wire except a
       caller's own recv loop. usbmux_send_stream() reads BETWEEN the
       segments of a long request, and the guard worth testing there
       is the one that stops it harvesting a second packet while the
       first is still in its stash -- which a device that can only
       have one packet outstanding can never exercise.

       So there are two slots. queue_tcp() fills the second when the
       first is occupied, s_intr_poll() promotes it, and a third
       packet is counted as dropped rather than silently replacing
       either. The real device is not limited to two; two is what it
       takes to make the guard reachable, and the counter says when
       that stops being enough. */
    uint8_t in2[2048];
    int     in2_len;
    int     in_dropped;
    int     blocking_in;   /* the path the driver must NOT use */
    int     stall_in, stall_out;
    /* Stall the Nth bulk OUT rather than the first. The one-shot
       stall_out above can only hit the VERSION packet, so the SETUP --
       the one whose failure left v0.8.7.6's channel permanently out of
       step, claiming a version the device never agreed -- could not be
       reached at all. */
    int     stall_out_on;
} ph;

static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void wr16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }

#define TH_LEN   20
#define F_FIN    0x01
#define F_SYN    0x02
#define F_RST    0x04
#define F_PSH    0x08
#define F_ACK    0x10
#define LOCKDOWN_PORT 62078

static void phone_reset(void)
{
    memset(&ph, 0, sizeof ph);
    ph.nconfigs = 2;
    ph.dev_version = 2;
    ph.cfg_index_asked = -1;
    ph.dev_hdr = MUX_HDR_SHORT;      /* until it has answered VERSION */
    ph.listen_port = LOCKDOWN_PORT;
    ph.dev_isn = 0;                  /* what every iPhone has ever used */
    /* A value nothing could arrive at by counting. The host must
       MIRROR this in its own rx_seq field; v0.8.7.6 would have sent a
       tally of packets received and produced 1, 2, 3. */
    ph.dev_mux_rx = 0x1234;
    g_ms = 1000; g_logs = 0;
}

/* The device's answer to a VERSION packet, queued for the next read.

   These are the exact bytes an iPhone 05AC:12A8 put on the wire on
   26 October 2026, read off the HP's screen:

       00 00 00 00   protocol = 0   VERSION
       00 00 00 14   length   = 20
       00 00 00 02   major    = 2
       00 00 00 00   minor    = 0
       00 00 00 00   padding

   Twenty bytes: an EIGHT byte header and a twelve byte payload. There
   is no magic in it and no sequence numbers, because neither end knows
   the version yet. */
static void queue_version(int major)
{
    uint8_t *p = ph.in;
    wr32(p + 0, ph.wrong_proto ? 6u : 0u);   /* 6 is TCP, not VERSION */
    wr32(p + 4, ph.bad_len ? 4u : (uint32_t)(MUX_HDR_SHORT + 12));
    wr32(p + MUX_HDR_SHORT + 0, (uint32_t)major);
    wr32(p + MUX_HDR_SHORT + 4, 0);
    wr32(p + MUX_HDR_SHORT + 8, 0);
    ph.in_len = ph.short_reply ? 4 : MUX_HDR_SHORT + 12;
}

/* A packet the device sends AFTER the version is settled, which does
   carry a magic and sequence numbers. The magic is the DEVICE's --
   0xFACEFACE, off the HP on 30 October -- not the host's. */
static void queue_post(uint32_t proto)
{
    uint8_t *p = ph.in;
    wr32(p + 0, proto);
    wr32(p + 4, (uint32_t)(MUX_HDR_FULL + 4));
    wr32(p + 8, ph.bad_magic ? 0xDEADBEEFu : 0xFACEFACEu);
    wr16(p + 12, 0);
    wr16(p + 14, 0);
    wr32(p + MUX_HDR_FULL, 0);
    ph.in_len = MUX_HDR_FULL + 4;
}

/* ── the device, v0.8.7.7 ─────────────────────────────────────────────

   Everything below writes and reads the TCP header at LITERAL offsets.
   include/usbmux.h names them MUX_TH_SEQ and friends; using those here
   would mean the fixture and the driver agree about the layout by
   construction, and a suite that cannot disagree with the code is the
   mistake this file already carries two scars from.

       0   source port        2 bytes
       2   destination port   2
       4   sequence           4
       8   acknowledgement    4
      12   data offset        high nibble: words. 5.
      13   flags
      14   window             in units of 256 bytes
      16   checksum           always zero over the mux
      18   urgent pointer     always zero
*/

/* Queue a TCP packet from the device. `seq` and `ack` are the device's
   own numbers; the ports are swapped relative to what the host sent,
   which is the detail a driver that checks ports can get backwards. */
static void queue_tcp(uint8_t flags, uint32_t seq, uint32_t ack,
                      const void *data, int len)
{
    uint8_t *p  = ph.in;
    uint8_t *th = p + MUX_HDR_FULL;
    int total = MUX_HDR_FULL + TH_LEN + len;

    wr32(p + 0, 6u);                     /* MUX_PROTO_TCP */
    wr32(p + 4, (uint32_t)total);
    /* ── THE DEVICE'S OWN MAGIC, off the wire ────────────────────────
       0xFACEFACE, not the host's 0xFEEDFACE. These are the bytes an
       iPhone 05AC:12A8 put on the HP's screen on 30 October in the
       SYN/ACK to lockdownd:

           00 00 00 06   protocol 6, TCP
           00 00 00 24   length 36 = 16 header + 20 TCP
           FA CE FA CE   the magic
           00 00 00 01   tx_seq 0, rx_seq 1

       The fixture used to send 0xFEEDFACE -- the value the DRIVER
       writes -- so the driver and the device model agreed about a
       field they had both got from the same place, and the receive-
       side magic check looked correct in 79 checks and refused the
       first real packet. "Where a protocol comes from outside this
       tree, get bytes off the wire into the fixture." */
    wr32(p + 8, ph.bad_magic ? 0xDEADBEEFu : 0xFACEFACEu);
    wr16(p + 12, 0);                     /* its tx_seq; usbmuxd ignores it */
    wr16(p + 14, ph.dev_mux_rx);         /* the value the host must mirror */

    /* The source port is the port that was ADDRESSED, not the one
       this device happens to listen on. They differ in exactly one
       case -- a SYN to a closed port -- and that is the case where it
       matters: an RST arriving from a port the host never addressed is
       not the host's connection and a correct driver ignores it. The
       first version of this fixture answered from the listen port and
       made a correct refusal look like a failure. */
    wr16(th +  0, ph.reply_port ? (uint16_t)ph.reply_port
                                : (uint16_t)LOCKDOWN_PORT);
    wr16(th +  2, ph.exp_sport);
    wr32(th +  4, seq);
    wr32(th +  8, ack);
    th[12] = (uint8_t)((TH_LEN / 4) << 4);
    th[13] = flags;
    wr16(th + 14, (uint16_t)(ph.dev_win_units ? ph.dev_win_units : 512));
    wr16(th + 16, 0);
    wr16(th + 18, 0);
    if (len && data) memcpy(th + TH_LEN, data, (size_t)len);
    ph.in_len = total;
}

/* v0.8.9.7: a CONTROL packet -- protocol 1, the device talking.
   The layout (first payload byte a type, the rest text) is usbmuxd's
   reading of it in device_control_input(), NOT bytes off a phone: none
   has been captured yet. When one is, it replaces this. */
static void queue_control(uint8_t type, const char *text)
{
    uint8_t *p = ph.in;
    int tl = text ? (int)strlen(text) : 0;
    int total = MUX_HDR_FULL + 1 + tl;
    wr32(p + 0, 1u);                     /* MUX_PROTO_CONTROL */
    wr32(p + 4, (uint32_t)total);
    wr32(p + 8, 0xFACEFACEu);
    wr16(p + 12, 0);
    wr16(p + 14, ph.dev_mux_rx);
    p[MUX_HDR_FULL] = type;
    if (tl) memcpy(p + MUX_HDR_FULL + 1, text, (size_t)tl);
    ph.in_len = total;
}

/* Queue a packet BEHIND one already waiting, for the suites that need
   two in flight. Identical to queue_tcp() otherwise. */
static void queue_tcp2(uint8_t flags, uint32_t seq, uint32_t ack,
                       const void *data, int len)
{
    uint8_t save[2048];
    int     save_len = ph.in_len;

    if (save_len > 0) memcpy(save, ph.in, (size_t)save_len);
    queue_tcp(flags, seq, ack, data, len);
    if (save_len > 0) {
        if (ph.in2_len > 0) { ph.in_dropped++; }
        else {
            memcpy(ph.in2, ph.in, (size_t)ph.in_len);
            ph.in2_len = ph.in_len;
            memcpy(ph.in, save, (size_t)save_len);
            ph.in_len = save_len;
        }
    }
}

#ifdef MUXDEV_SERVICE
/* Supplied by the suite: what the service on the port does with the
   bytes it is given. It answers by calling queue_tcp(). */
static void dev_service(const uint8_t *data, int len);
#endif

/* What the device does with a packet the host sent. */
static void dev_rx(const uint8_t *p, int len)
{
    uint32_t proto, mlen;
    int      hdr = ph.dev_hdr ? ph.dev_hdr : MUX_HDR_SHORT;

    if (len < hdr) { ph.reject_len++; return; }

    proto = rd32(p + 0);
    mlen  = rd32(p + 4);
    /* usbmuxd's device_data_input() refuses on exactly this comparison
       and does nothing else with the packet. The length counting the
       header is the field the handoff calls "the one a device refuses
       in silence" -- so this one does not do it silently. */
    if ((int)mlen != len) { ph.reject_len++; return; }

    if (hdr == MUX_HDR_FULL) {
        /* The device checks what the HOST sends. There is no evidence
           either way about whether a real one does -- usbmuxd is the
           host side and cannot tell us -- but the host's value is not
           in doubt (send_packet() writes 0xFEEDFACE) so a driver that
           stopped writing it is a defect worth catching. Note this is
           the opposite direction from the magic the device SENDS,
           which is 0xFACEFACE and which nothing may refuse. */
        if (rd32(p + 8) != 0xFEEDFACEu) { ph.reject_magic++; return; }
        ph.saw_mux_tx = rd16(p + 12);
        ph.saw_mux_rx = rd16(p + 14);
    }

    if (proto == 0) {                    /* VERSION */
        if (!ph.silent) {
            queue_version(ph.dev_version);
            /* From here on it expects the full header, which is what
               makes the SETUP the first packet that carries the
               sequence pair. */
            if (ph.dev_version >= 2) ph.dev_hdr = MUX_HDR_FULL;
        }
        return;
    }
    if (proto == 2) {                    /* SETUP */
        ph.setup_seen++;
        ph.setup_tx = ph.saw_mux_tx;
        ph.setup_rx = ph.saw_mux_rx;
        return;
    }
    if (proto != 6) return;              /* not TCP; nothing models it */

    /* ── TCP ───────────────────────────────────────────────────────── */
    {
        const uint8_t *th = p + hdr;
        uint16_t sport, dport, win;
        uint32_t seq, ack;
        uint8_t  flags;
        int      plen, off;
        int      port = ph.listen_port ? ph.listen_port : LOCKDOWN_PORT;

        if (len < hdr + TH_LEN) { ph.reject_len++; return; }

        sport = rd16(th +  0);
        dport = rd16(th +  2);
        seq   = rd32(th +  4);
        ack   = rd32(th +  8);
        off   = th[12] >> 4;
        flags = th[13];
        win   = rd16(th + 14);
        plen  = len - hdr - TH_LEN;

        ph.saw_flags = flags;
        ph.saw_seq   = seq;
        ph.saw_ack   = ack;
        ph.saw_win   = win;
        ph.saw_off   = off;
        /* COUNTED, NOT REFUSED. usbmuxd never fills th_sum and the
           device accepts that, so a fixture that rejected a zero
           checksum would be stricter than the real thing, which is its
           own kind of lie -- it would fail a correct driver. The count
           is here so a driver that starts computing one is noticed. */
        if (rd16(th + 16) != 0) ph.saw_checksum++;

        if (off != TH_LEN / 4) { ph.reject_off++; return; }

        if (ph.tcp_state == 0) {
            /* An OPENING is a SYN and nothing else. Masking rather
               than comparing would let a SYN|ACK -- a host answering
               a handshake nobody started -- open a connection, and
               usbmuxd compares the whole byte for the same reason on
               its own side. */
            if (flags != F_SYN)        { ph.reject_flags++; return; }
            if (dport != (uint16_t)port) {
                /* A connection to a port nothing is listening on gets
                   an RST, which is what the device does and what makes
                   "closed" a distinguishable answer. It comes FROM the
                   port that was addressed. */
                ph.exp_sport  = sport;
                ph.reply_port = dport;
                queue_tcp(F_RST, 0, seq + 1, 0, 0);
                ph.reject_ports++;
                return;
            }
            ph.exp_sport  = sport;
            ph.reply_port = dport;
            if (ph.refuse_port) {
                queue_tcp(F_RST, 0, seq + 1, 0, 0);
                return;
            }
            /* COUNTED, because tcp_state cannot distinguish a
               second connection from the first: both end at 2. A
               fixture whose state cannot tell two things apart is the
               shape v0.8.7's NIC suite had four of. */
            ph.syns++;
            ph.tcp_state    = 1;
            ph.dev_seq      = ph.dev_isn + 1;
            ph.exp_host_seq = seq + 1;
            queue_tcp(ph.syn_flags ? (uint8_t)ph.syn_flags
                                   : (uint8_t)(F_SYN | F_ACK),
                      ph.dev_isn, seq + 1, 0, 0);
            return;
        }

        if (sport != ph.exp_sport || dport != (uint16_t)port) {
            ph.reject_ports++; return;
        }
        if (flags & F_RST) { ph.tcp_state = 0; return; }

        if (ph.tcp_state == 1) {
            if (flags != F_ACK)            { ph.reject_flags++; return; }
            /* THE CHECK THIS FIXTURE EXISTS FOR. Our SYN consumed one
               sequence number and so did theirs, so the completing ACK
               must carry seq isn_host+1 and ack dev_isn+1. A driver
               that incremented one of them and not the other, or that
               acked its own ISN, lands here. */
            if (seq != ph.exp_host_seq)    { ph.reject_seq++; return; }
            if (ack != ph.dev_isn + 1)     { ph.reject_seq++; return; }
            ph.tcp_state     = 2;
            ph.last_host_ack = ack;
            return;
        }

        /* established */
        if (flags != F_ACK)  { ph.reject_flags++; return; }
        if (seq != ph.exp_host_seq) { ph.reject_seq++; return; }
        /* An ack for bytes it has not sent is not a slow receiver, it
           is a wrong number. */
        if (ack > ph.dev_seq) { ph.reject_seq++; return; }
        ph.last_host_ack = ack;
        if (plen > 0) {
            ph.host_bytes   += (uint32_t)plen;
            ph.host_data_len = plen;
            ph.host_segs++;
            if (plen <= (int)sizeof ph.host_data)
                memcpy(ph.host_data, th + TH_LEN, (size_t)plen);
            /* The stream, in arrival order. Overflow is RECORDED and
               the bytes dropped, rather than written past the end of
               the array: a fixture that corrupts itself reports
               whatever is next in the struct as the host's data. */
            if (ph.host_stream_len + (uint32_t)plen
                    <= sizeof ph.host_stream) {
                memcpy(ph.host_stream + ph.host_stream_len,
                       th + TH_LEN, (size_t)plen);
                ph.host_stream_len += (uint32_t)plen;
            } else {
                ph.host_stream_over = 1;
            }
            ph.exp_host_seq += (uint32_t)plen;
            if (ph.auto_ack) {
                /* What a real device does with every payload it
                   accepts. Its own sequence number does not move --
                   a bare ACK carries no bytes. */
                queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, 0, 0);
                ph.acks_sent++;
            }
#ifdef MUXDEV_SERVICE
            /* A suite that models a SERVICE on the port gets the bytes
               here. The mux has already accepted them and advanced its
               sequence numbers, so a service that answers is answering
               a connection the transport considers healthy -- which is
               the only arrangement in which the plist framing above it
               is the thing under test. */
            dev_service(th + TH_LEN, plen);
#endif
        }
    }
}

static int s_port_count(struct usb_hc *h){ (void)h; return 1; }
static int s_port_connected(struct usb_hc *h, int p){ (void)h; return p == 0; }

static int s_attach(struct usb_hc *h, int port, usb_dev_t *out)
{
    (void)h;
    if(port != 0) return USB_ENODEV;
    out->addr = 2; out->speed = USB_SPEED_HIGH; out->maxpacket0 = 64;
    out->vendor = 0x05AC; out->product = 0x12A8;
    out->dev_class = 0x06; out->nconfigs = (uint8_t)ph.nconfigs;
    return USB_OK;
}

static int s_control(struct usb_hc *h, const usb_dev_t *d, uint8_t rtype,
                     uint8_t req, uint16_t value, uint16_t index,
                     void *data, uint16_t len)
{
    (void)h; (void)d; (void)index;
    if((rtype & 0x80u) && req == USB_REQ_GET_DESCRIPTOR &&
       (value >> 8) == USB_DESC_CONFIG){
        int idx = value & 0xFFu;
        uint8_t blob[128];
        int n;
        ph.cfg_index_asked = idx;
        if(idx < 0 || idx >= ph.nconfigs) return USB_ESTALL;
        /* The phone's shape: PTP, then mux-only, then mux AND Ethernet.
           The right answer is the LAST of the three, not the first that
           matches. */
        if(ph.phone_shape){
            const uint8_t *src = (idx == 0) ? CFG0
                               : (idx == 1) ? CFG2 : CFG1;
            n = (idx == 0) ? (int)sizeof CFG0
              : (idx == 1) ? (int)sizeof CFG2 : (int)sizeof CFG1;
            memcpy(blob, src, (size_t)n);
        } else {
            n = (int)g_cfglen[idx];
            memcpy(blob, g_cfgs[idx], (size_t)n);
        }
        /* The interface's class triple blanked: a device that offers
           no Apple Mobile Device interface at all. */
        if(idx == 1 && ph.no_mux) blob[CFG1_IF_MUX + 5] = 0x03;
        /* alt 0's second endpoint made an interrupt one, so the
           interface offers one bulk endpoint and not a pair. */
        if(idx == 1 && ph.one_ep) blob[43 + 3] = 3;
        if(n > (int)len) n = (int)len;
        if(data) memcpy(data, blob, (size_t)n);
        return n;
    }
    if(!(rtype & 0x80u) && req == USB_REQ_SET_CONFIGURATION){
        if(ph.refuse_setcfg) return USB_ESTALL;
        ph.set_cfg_value = (int)value;
        ph.set_cfgs++;
        return 0;
    }
    if(!(rtype & 0x80u) && (rtype & 0x1Fu) == USB_RECIP_ENDPOINT &&
       req == USB_REQ_CLEAR_FEATURE) return 0;
    return USB_ESTALL;
}

static int s_bulk(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{
    (void)h; (void)d; (void)mps;
    if(len < 0 || len > SIM_SYNC_MAX){ ph.sync_refused++; return USB_EPARAM; }
    if(in){ ph.blocking_in++; return USB_ETIMEOUT; }   /* the blocking path */
    if(ep != EP_OUT) return USB_ENODEV;
    if(ph.stall_out){ ph.stall_out = 0; return USB_ESTALL; }
    if(ph.stall_out_on && ph.out_calls + 1 == ph.stall_out_on){
        ph.stall_out_on = 0; return USB_ESTALL; }
    ph.out_calls++;
    ph.out_len = len;
    if(len > 0 && len <= (int)sizeof ph.out && data)
        memcpy(ph.out, data, (size_t)len);

    /* ── v0.8.9.7: A TRANSFER ENDS ON A SHORT PACKET ──────────────────
       USB has no length field for a bulk transfer. It ends at the first
       USB packet shorter than the endpoint's maximum -- here 512, the
       iPhone's -- and a transfer whose length is an exact multiple of
       that ends only when the host sends a zero-length packet. Until
       then the device is still reading, and whatever the host sends
       next is MORE OF THE SAME TRANSFER.

       This fixture used to hand every call to the device as a complete
       message, so a driver that never sent a ZLP was indistinguishable
       from one that did. The real phone glued the pair request's two
       mux packets into one transfer and refused it -- the first packet
       is 2048 bytes, 4 x 512 -- and the suite had never been able to
       see that happen. Now it accumulates, and delivers when the
       transfer actually ends. */
    {
        int m = mps > 0 ? mps : 512;
        if(len > 0 && data){
            if(ph.xfer_len + len > (int)sizeof ph.xfer){
                ph.xfer_overrun++;
                ph.xfer_len = 0;
                return USB_EXFER;
            }
            memcpy(ph.xfer + ph.xfer_len, data, (size_t)len);
            ph.xfer_len += len;
            ph.xfer_calls++;
        }
        if(len == 0) ph.zlps++;
        if(len == 0 || (len % m) != 0){
            /* v0.8.7.7: handed to a device that VALIDATES it, rather
               than a branch that answers a VERSION and ignores
               everything else. */
            if(ph.xfer_len > 0){
                if(ph.xfer_calls > 1) ph.glued++;   /* two messages, one transfer */
                dev_rx(ph.xfer, ph.xfer_len);
            }
            ph.xfer_len = 0;
            ph.xfer_calls = 0;
        }
    }
    return len;
}

static int s_intr_poll(struct usb_hc *h, const usb_dev_t *d, uint8_t ep,
                       int in, void *data, int len, int mps)
{
    int n;
    (void)h; (void)d; (void)mps;
    if(!in) return USB_EPARAM;
    if(len < 0 || len > SIM_ASYNC_MAX){ ph.async_refused++; return USB_EPARAM; }
    if(ep != EP_IN) return USB_ENODEV;
    ph.in_polls++;
    if(ph.stall_in){ ph.stall_in = 0; return USB_ESTALL; }
    if(ph.in_len == 0) return 0;
    n = ph.in_len; if(n > len) n = len;
    if(data) memcpy(data, ph.in, (size_t)n);
    ph.in_len = 0;
    /* Promote whatever was queued behind it. */
    if(ph.in2_len > 0){
        memcpy(ph.in, ph.in2, (size_t)ph.in2_len);
        ph.in_len  = ph.in2_len;
        ph.in2_len = 0;
    }
    return n;
}

static int  s_configure(struct usb_hc *h, usb_dev_t *d){ (void)h;(void)d; return USB_OK; }
static void s_reset_toggle(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in)
{ (void)h;(void)d;(void)ep;(void)in; }

static const usb_hc_ops_t SIM_OPS = {
    .name = "SIM", .port_count = s_port_count,
    .port_connected = s_port_connected, .attach = s_attach,
    .control = s_control, .bulk = s_bulk, .intr = s_bulk,
    .reset_toggle = s_reset_toggle, .intr_poll = s_intr_poll,
    .configure = s_configure,
};

int  usb_hub_scan(void)     { return 0; }
int  uhci_hc_register(void) { return 0; }
int  xhci_hc_register(void) { return 0; }
