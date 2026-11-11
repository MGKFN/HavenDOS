/* ════════════════════════════════════════════════════════════════════
   test_usbmux.c -- the Apple Mobile Device channel, against a device
   that speaks it.

   ── WHAT THIS SUITE CAN AND CANNOT DO ────────────────────────────────

   The same honesty usb/ethernet.c's suite needed, for the same reason:
   the protocol here is read off the published behaviour of usbmuxd and
   nobody in this tree has seen a byte of it on a wire.

   A suite cannot show that a guess about Apple's framing is right.
   What it can do is pin the guess down tightly enough that one boot on
   the HP refutes it:

     - the header really is sixteen big-endian bytes in that order
     - `length` really counts the header, which is the field most
       easily got wrong and the one a device refuses in silence
     - the magic this HOST writes really is 0xFEEDFACE -- and the one
       the DEVICE writes is 0xFACEFACE, which v0.8.7.7 refused packets
       over until the HP printed it. It is reported, never believed:
       usbmuxd validates the length and never reads the magic at all
     - the raw packet is kept BEFORE anything decides what it means

   ── AND THE BOUNDS, BECAUSE OF WHAT HAPPENED LAST TIME ───────────────

   v0.8.7's Ethernet driver passed 110 checks and could not move a byte
   on real hardware, because the simulated controller accepted a
   transfer of any length while the real back-end refused anything over
   64. So this controller carries the real bounds from the first line
   it is written, not after a laptop finds out.
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

/* ── the device ────────────────────────────────────────────────────
   v0.8.7.7: moved to tests/muxdev.inc, because tests/test_lockdown.c
   needs the same phone and two copies of a device model drift apart --
   which is what tools/check_diskname_sync.py exists to refuse. The
   fixtures, the state machine and the bounds all live there now. */
#include "muxdev.inc"


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

static void machine(void)
{
    phone_reset();
    usbmux_close();
    usb_enumerate();
}

int main(void)
{
    printf("usbmux: the Apple Mobile Device channel\n");
    usb_register_hc(&SIM_OPS, 0, 0, USB_HC_NONE);

    head("the fixtures describe their own length");
    {
        int i;
        for(i = 0; i < 2; i++){
            uint32_t tot = (uint32_t)g_cfgs[i][2] | ((uint32_t)g_cfgs[i][3] << 8);
            ck("a configuration fixture", tot == (uint32_t)g_cfglen[i],
               (long)tot);
        }
        ck("the Ethernet interface is first, and is also class 0xFF",
           CFG1[CFG1_IF_ETH + 2] == 1 && CFG1[CFG1_IF_ETH + 5] == 0xFF &&
           CFG1[CFG1_IF_ETH + 6] == 0xFD, CFG1[CFG1_IF_ETH + 6]);
        ck("and the mux interface is second, with the full triple",
           CFG1[CFG1_IF_MUX + 2] == 2 && CFG1[CFG1_IF_MUX + 5] == 0xFF &&
           CFG1[CFG1_IF_MUX + 6] == 0xFE &&
           CFG1[CFG1_IF_MUX + 7] == 0x02, CFG1[CFG1_IF_MUX + 5]);
    }

    /* ════════════════════════════════════════════════════════════════
       1. Claiming the interface
       ════════════════════════════════════════════════════════════════ */
    head("the phone boots into PTP and the channel is in another config");
    machine();
    {
        const usb_dev_t *d = usb_device(0);
        ck("it enumerated", d && d->valid, d ? 1 : 0);
        ck("into index 0", d && d->cfgidx == 0, d ? d->cfgidx : -1);
        ck("nothing is claimed yet", !usbmux_present(), usbmux_present());
    }

    head("usbmux_open surveys every configuration and finds FF/FE/02");
    {
        const usb_dev_t *d;
        ck("it opens", usbmux_open() == USB_OK, 0);
        ck("and reports itself open", usbmux_present(), 0);
        d = usb_device(0);
        ck("the device moved to index 1", d && d->cfgidx == 1,
           d ? d->cfgidx : -1);
        ck("SET_CONFIGURATION carried the VALUE 2, not the index",
           ph.set_cfg_value == 2, ph.set_cfg_value);
        ck("INTERFACE 2 was adopted -- the mux one, not the\n       Ethernet interface ahead of it",
           d && d->ifnum == 2, d ? d->ifnum : -1);
        ck("the Apple triple is on it",
           d && d->if_class == 0xFF && d->if_subclass == 0xFE &&
           d->if_protocol == 0x02, d ? d->if_class : -1);
        ck("with its bulk pair", d && d->nep == 2, d ? d->nep : -1);
    }

    head("opening twice does not re-select anything");
    {
        /* COUNT the calls. Re-selecting the SAME configuration leaves
           set_cfg_value identical, so the value cannot tell a second
           SET_CONFIGURATION from none -- which the harness caught. */
        int n = ph.set_cfgs;
        ck("it is already open", usbmux_open() == USB_OK, 0);
        ck("and no further SET_CONFIGURATION was sent",
           ph.set_cfgs == n, ph.set_cfgs - n);
    }

    head("a device with no such interface is not claimed");
    machine();
    ph.no_mux = 1;
    {
        /* Enumeration already sent one -- setup_config() selects index
           0, whose value is 1. The check is that the PROBE sent no
           further one, which is a different question. */
        int before = ph.set_cfg_value;
        ck("enumeration left it in its boot configuration", before == 1,
           before);
        ck("open refuses", usbmux_open() < 0, 0);
        ck("nothing is claimed", !usbmux_present(), usbmux_present());
        ck("and the probe sent no SET_CONFIGURATION of its own",
           ph.set_cfg_value == before, ph.set_cfg_value);
        ck("though every configuration WAS surveyed",
           ph.cfg_index_asked == 1, ph.cfg_index_asked);
    }

    head("an interface offering ONE bulk endpoint is not a channel");
    machine();
    ph.one_ep = 1;
    {
        ck("open refuses", usbmux_open() < 0, 0);
        ck("nothing is claimed", !usbmux_present(), usbmux_present());
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("a device that refuses SET_CONFIGURATION");
    machine();
    ph.refuse_setcfg = 1;
    {
        ck("open refuses", usbmux_open() < 0, 0);
        ck("nothing is claimed", !usbmux_present(), usbmux_present());
        ck("and it said so", g_logs > 0, g_logs);
    }

    /* ════════════════════════════════════════════════════════════════
       2. The header on the wire -- the part that is a guess
       ════════════════════════════════════════════════════════════════ */
    head("THE VERSION PACKET, byte for byte");
    machine();
    usbmux_open();
    {
        int r = usbmux_handshake();
        ck("the device answered 2", r == 2, r);
        ck("exactly two packets went out -- VERSION then SETUP",
           ph.out_calls == 2, ph.out_calls);

        /* ph.out holds the SETUP packet now; the VERSION was checked
           by the device answering at all. Re-run with a silent device
           below to inspect the VERSION itself. */
        /* SETUP goes out AFTER the version is agreed, so this one does
           carry the full header -- and it is the first packet in the
           exchange that does. */
        ck("the SETUP packet is 17 bytes -- full header plus one",
           ph.out_len == MUX_HDR_FULL + 1, ph.out_len);
        ck("its protocol field is 2", rd32(ph.out + 0) == 2,
           (long)rd32(ph.out + 0));
        ck("its LENGTH COUNTS THE HEADER",
           rd32(ph.out + 4) == (uint32_t)(MUX_HDR_FULL + 1),
           (long)rd32(ph.out + 4));
        ck("the magic is 0xFEEDFACE", rd32(ph.out + 8) == 0xFEEDFACEu,
           (long)rd32(ph.out + 8));
        ck("the setup byte is 0x07", ph.out[MUX_HDR_FULL] == 0x07,
           ph.out[MUX_HDR_FULL]);
        /* ── THE SEQUENCE PAIR, CORRECTED IN v0.8.7.7 ──────────────
           This suite used to assert tx_seq == 1 and rx_seq == 1 here,
           and the driver produced them, and both were wrong. usbmuxd's
           send_packet() RESETS the pair on a SETUP packet -- tx_seq to
           0, rx_seq to 0xFFFF -- and writes those values before
           advancing tx_seq. 0xFFFF is "nothing received from you yet",
           not a count of 65535.

           Two bugs hid behind the old expectation. tx_seq had been
           moved by the VERSION packet, which carries no such field;
           and rx_seq was a tally of arrivals. Neither could be seen,
           because this was the only sequenced packet the kernel ever
           sent and 1 happened to be what both wrong rules produced. */
        ck("tx_seq is 0 -- the SETUP packet RESETS it",
           rd16(ph.out + 12) == 0, rd16(ph.out + 12));
        ck("rx_seq is 0xFFFF -- 'nothing yet', not a count",
           rd16(ph.out + 14) == 0xFFFF, rd16(ph.out + 14));
        ck("...and the device read the same two values off the wire",
           ph.setup_tx == 0 && ph.setup_rx == 0xFFFF, ph.setup_tx);
        /* The VERSION packet went out on the short header and so must
           not have touched tx_seq. If it had, the SETUP would carry 1
           -- which is exactly what v0.8.7.6 sent. */
        ck("the SETUP is the first packet that carried the pair at all",
           ph.setup_seen == 1, ph.setup_seen);

        /* A SECOND HANDSHAKE SENDS NOTHING. Versioning happens once;
           repeating it would go out in the framing the first exchange
           agreed, which no device expects. */
        ck("handshaking again is a no-op that returns the known version",
           usbmux_handshake() == 2, 0);
        ck("and put no packet on the wire", ph.out_calls == 2,
           ph.out_calls);

        /* ── THE GAP THIS FILE RECORDED IS CLOSED ──────────────────
           It used to say: tx_seq and rx_seq are both 1 on this SETUP,
           so swapping the two writes is not visible here, and stage 1
           is real traffic so the check belongs there.

           It belongs here now, because the two values are no longer
           the same number. 0 and 0xFFFF cannot be swapped unnoticed,
           and the device above refuses the packet if they are. The
           stage-1 traffic gets its own checks further down, where the
           pair has to be mirrored rather than merely written. */
        ck("the two fields hold DIFFERENT values, so a swap shows",
           rd16(ph.out + 12) != rd16(ph.out + 14), 0);
    }

    head("and the VERSION packet itself, against a silent device");
    machine();
    ph.silent = 1;
    usbmux_open();
    {
        int r = usbmux_handshake();
        ck("it reports no answer rather than a version", r == 0, r);
        ck("one packet went out", ph.out_calls == 1, ph.out_calls);
        ck("TWENTY bytes -- the SHORT header plus three words",
           ph.out_len == MUX_HDR_SHORT + 12, ph.out_len);
        ck("protocol 0, a VERSION", rd32(ph.out + 0) == 0,
           (long)rd32(ph.out + 0));
        ck("length counts the header",
           rd32(ph.out + 4) == (uint32_t)(MUX_HDR_SHORT + 12),
           (long)rd32(ph.out + 4));
        ck("MAJOR 2, big-endian, immediately after the short header",
           rd32(ph.out + MUX_HDR_SHORT + 0) == 2,
           (long)rd32(ph.out + MUX_HDR_SHORT + 0));
        ck("minor 0", rd32(ph.out + MUX_HDR_SHORT + 4) == 0,
           (long)rd32(ph.out + MUX_HDR_SHORT + 4));
        ck("padding 0", rd32(ph.out + MUX_HDR_SHORT + 8) == 0,
           (long)rd32(ph.out + MUX_HDR_SHORT + 8));
        /* There is NO magic here. Byte 8 is the major version, and a
           driver that reads it as a magic reports "magic 2" -- which is
           exactly what the HP printed on 26 October. */
        ck("byte 8 is the major version, NOT a magic",
           rd32(ph.out + 8) == 2, (long)rd32(ph.out + 8));
        ck("and it really polled for a reply", ph.in_polls > 0,
           ph.in_polls);
        /* in_polls counts legitimate non-blocking polls and a silent
           device draws thousands of them, so the count cannot be the
           test -- the BLOCKING entry point has its own counter. */
        /* in_polls counts legitimate non-blocking polls and a silent
           device draws thousands of them, so the count cannot be the
           test -- the BLOCKING entry point has its own counter. The
           first version of this check compared in_polls against 1000
           and failed on a working driver. */
        ck("...through the NON-BLOCKING path, never the blocking one",
           ph.blocking_in == 0, ph.blocking_in);
    }

    /* ════════════════════════════════════════════════════════════════
       3. What it refuses to believe
       ════════════════════════════════════════════════════════════════ */
    head("THE DEVICE'S MAGIC IS 0xFACEFACE, and it is NOT the host's");
    machine();
    usbmux_open();
    {
        uint32_t proto = 0;
        int r;
        /* ── THE BYTES THAT COST v0.8.7.7 A BOOT ────────────────────
           The HP, 30 October. The phone's SYN/ACK to lockdownd:

               00 00 00 06   protocol 6, TCP
               00 00 00 24   length 36 = 16 header + 20 TCP
               FA CE FA CE   the magic
               00 00 00 01   tx_seq 0, rx_seq 1

           v0.8.7.7 REFUSED that packet, and said "the framing in this
           driver is wrong" -- true in a way it did not mean. Every
           other field was right: the device had parsed our SYN and
           answered it.

           The fixture used to send 0xFEEDFACE, the value the DRIVER
           writes, so the two agreed about a field they had both taken
           from the same place. */
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        queue_post(0);
        r = usbmux_recv_packet(&proto);
        ck("a packet carrying the DEVICE's magic is accepted", r >= 0, r);
        ck("...and the magic in it really is 0xFACEFACE",
           g_m.last[8] == 0xFA && g_m.last[9] == 0xCE &&
           g_m.last[10] == 0xFA && g_m.last[11] == 0xCE, g_m.last[8]);
        ck("which is NOT the value this host writes",
           MUX_MAGIC != MUX_MAGIC_DEVICE, 0);
    }

    head("A HEALTHY STAGE-1 FLOW COMPLAINS ABOUT NOTHING");
    machine();
    {
        /* ── A SPURIOUS WARNING IS A DEFECT ──────────────────────────
           Not a style point. `dmesg | grep usbmux` is how every
           hardware question in this release line has been answered,
           and a line printed on every healthy packet buries the one
           line that matters.

           This is also the only thing that can catch a magic check
           running during the VERSION exchange, now that the check
           reports rather than refuses: bytes 8..11 of a VERSION packet
           are the major version, so the warning would read "magic
           00000002" on every boot there has ever been. v0.8.7.3
           printed exactly that and it took three releases to place. */
        /* Measured from HERE, not from machine(): enumeration walks a
           simulated bus with one device on one port and warns about
           the empty ones, which is the core's business and not this
           exchange's. The subject is the stage-1 flow. */
        int w0 = g_warns, w;
        ck("the channel opens", usbmux_open() == USB_OK, 0);
        ck("the handshake succeeds", usbmux_handshake() == 2, 0);
        ck("the connection comes up",
           usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ck("eleven bytes go out", usbmux_send("hello there", 11) == USB_OK, 0);
        queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "ok", 2);
        ph.dev_seq += 2;
        {
            uint8_t buf[32];
            ck("and two come back", usbmux_recv(buf, (int)sizeof buf) == 2, 0);
        }
        w = g_warns - w0;
        ck("NOT ONE WARNING in the whole exchange", w == 0, w);
        ck("...though it did say things", g_logs > 0, g_logs);
    }

    head("the DEVICE's magic does not draw a warning on every packet");
    machine();
    usbmux_open();
    {
        uint32_t proto = 0;
        int logs;
        /* 0xFACEFACE is what a working phone sends, so a driver that
           treats it as unexpected logs a warning on every packet of a
           healthy connection -- which buries the one warning that
           matters under thousands that do not. */
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        queue_post(0);
        logs = g_warns;
        ck("the packet is accepted", usbmux_recv_packet(&proto) >= 0, 0);
        ck("and NOTHING was warned about it", g_warns == logs,
           g_warns - logs);
    }

    head("AND AN UNKNOWN MAGIC IS NOTED, NOT REFUSED");
    machine();
    usbmux_open();
    {
        uint32_t proto = 0;
        int logs, r;
        /* usbmuxd's device_data_input() does not read this field at
           all -- send_packet() writes it and nothing validates it.
           A receiver stricter than the only implementation known to
           work is a receiver that can refuse working devices, and
           v0.8.7.7 was one.

           So an unexpected value is EVIDENCE: logged, kept in the raw
           dump, and handed on. The length field is the real gate, and
           it is the one usbmuxd actually checks. */
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        ph.bad_magic = 1;
        queue_post(0);
        logs = g_logs;
        r = usbmux_recv_packet(&proto);
        ck("the packet is NOT refused", r >= 0, r);
        ck("but it was reported", g_logs > logs, g_logs - logs);
        ck("and the odd value is visible in the raw dump, not hidden",
           g_m.last[8] == 0xDE && g_m.last[9] == 0xAD, g_m.last[8]);
    }

    head("a reply that is not a VERSION is refused");
    machine();
    ph.wrong_proto = 1;
    usbmux_open();
    {
        int logs = g_logs;
        ck("the handshake fails", usbmux_handshake() < 0, 0);
        ck("and it said so", g_logs > logs, g_logs - logs);
        ck("no SETUP followed", ph.out_calls == 1, ph.out_calls);
    }

    head("A LENGTH FIELD UNDER THE HEADER is refused BY NAME");
    machine();
    ph.bad_len = 1;
    usbmux_open();
    {
        ck("the handshake fails", usbmux_handshake() < 0, 0);
        ck("and the receive sequence did NOT advance", g_m.rx_seq == 0,
           g_m.rx_seq);
    }
    machine();
    ph.bad_len = 1;
    usbmux_open();
    {
        /* ── A NEGATIVE NUMBER IS NOT A REFUSAL ────────────────────
           The first version of this checked only `< 0`, and
           tools/check_usbmux_suite.py reported the guard as MISSED:
           without it, mux_recv() returns `len - hdr`, which for a
           length of 4 and a header of 8 is -4. That IS negative, and
           every caller treats negative as a failure, so the suite
           passed.

           -4 is USB_ENODEV: "no controller, or nothing on the port".
           A length field four bytes long would have sent the next
           person to check whether the phone was plugged in. The error
           CODE is the test -- which is the same lesson as
           x_control() returning (3) for six different causes, in the
           one shape a return value can take it. */
        uint32_t proto = 0;
        uint8_t *p = ph.in;
        int r;
        /* A good handshake first, so the header is the full sixteen
           and the length field has somewhere to be wrong. Then a
           packet declaring EIGHT bytes -- less than its own header --
           with everything else about it correct. */
        ph.bad_len = 0;
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        wr32(p + 0, 6u);
        wr32(p + 4, 8u);                /* under the header */
        wr32(p + 8, 0xFACEFACEu);   /* the DEVICE's magic */
        wr16(p + 12, 0);
        wr16(p + 14, 0x1234);
        memset(p + MUX_HDR_FULL, 0, 20);
        ph.in_len = MUX_HDR_FULL + 20;
        r = usbmux_recv_packet(&proto);
        ck("the refusal is USB_EXFER, not arithmetic that went negative",
           r == USB_EXFER, r);
        /* Without the guard this returns 8 - 16 == -8, which is not
           any USB_E* at all: a caller switching on the error code
           falls off the end of every case it has. */
        ck("...and certainly not a number outside the USB_E* range",
           r >= USB_EPARAM, r);
    }

    head("a reply shorter than a header is refused");
    machine();
    usbmux_open();
    {
        /* A GOOD packet first, so g_buf holds a valid magic at bytes
           8..11. THEN a short one. Without the length check the driver
           reads the magic out of the PREVIOUS packet's bytes and
           believes a reply that never arrived -- which is why a fresh
           buffer could not show this, and the harness said so. */
        uint32_t proto = 0;
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        ph.short_reply = 1;
        queue_version(2);
        ck("the short reply is refused",
           usbmux_recv_packet(&proto) < 0, 0);
        ck("...and the stale magic in the buffer did not become one",
           g_m.version == 2, g_m.version);
    }

    head("a version-1 device gets no SETUP byte");
    machine();
    ph.dev_version = 1;
    usbmux_open();
    {
        int r = usbmux_handshake();
        ck("it answers 1", r == 1, r);
        ck("and only the VERSION went out", ph.out_calls == 1,
           ph.out_calls);
    }

    head("A SETUP THAT NEVER GOT OUT IS STILL OWED");
    machine();
    usbmux_open();
    {
        /* Found by tools/check_usbmux_suite.py, while working out why
           a different injection could not be caught.

           v0.8.7.6 returned the known version unconditionally on a
           second handshake. So if the VERSION was answered -- version
           2, sixteen-byte header from then on -- and the SETUP packet
           then failed to send, the host believed the version was
           agreed and the DEVICE never got its setup byte. Nothing
           retried, and every later call returned 2 without noticing:
           a channel permanently out of step, reporting itself fine. */
        int r;
        ph.stall_out_on = 2;           /* the VERSION goes, the SETUP stalls */
        r = usbmux_handshake();
        ck("the handshake reports the failure", r < 0, r);
        ck("and the device never saw a SETUP", ph.setup_seen == 0,
           ph.setup_seen);
        /* The version IS settled -- that part worked and must not be
           repeated. What is owed is the setup byte. */
        r = usbmux_handshake();
        ck("a second handshake returns the known version", r == 2, r);
        ck("THE SETUP WAS SENT, not skipped", ph.setup_seen == 1,
           ph.setup_seen);
        /* Two transfers REACHED the device: the VERSION and the SETUP
           that got out. The stalled one never did -- muxdev.inc counts
           out_calls after the stall check, which is the honest place
           for it: a transfer the controller refused did not happen. */
        ck("and the version was NOT exchanged a second time",
           ph.dev_hdr == MUX_HDR_FULL && ph.out_calls == 2,
           ph.out_calls);
        /* And it resets the pair, as a SETUP must, so the channel is
           in the state the device expects rather than merely open. */
        ck("the SETUP that did get out carried 0 and 0xFFFF",
           ph.setup_tx == 0 && ph.setup_rx == 0xFFFF, ph.setup_tx);
        ck("a third handshake sends nothing more", usbmux_handshake() == 2
           && ph.out_calls == 2, ph.out_calls);
    }

    head("a device that stalls the bulk OUT is cleared and reported");
    machine();
    usbmux_open();
    {
        ph.stall_out = 1;
        ck("the handshake reports the failure", usbmux_handshake() < 0, 0);
    }

    head("a handshake on a channel that is not open is refused");
    machine();
    {
        /* mux_send() refuses a closed channel too, so the return value
           is ENODEV either way. What differs is whether the handshake
           got as far as trying and logging -- the harness caught the
           first version comparing a value that could not move. */
        int logs = g_logs;
        ck("it is refused", usbmux_handshake() == USB_ENODEV, 0);
        ck("and nothing was sent", ph.out_calls == 0, ph.out_calls);
        ck("...and it did not even try", g_logs == logs, g_logs - logs);
    }

    /* ════════════════════════════════════════════════════════════════
       4. The evidence, and the bounds
       ════════════════════════════════════════════════════════════════ */
    head("THE RAW PACKET is kept before anything parses it");
    machine();
    usbmux_open();
    usbmux_handshake();
    {
        /* The framing in this driver is read off somebody else's
           implementation. `usb mux` prints these bytes so one boot can
           refute it. */
        ck("the length as it arrived",
           g_m.last_len == MUX_HDR_SHORT + 12, g_m.last_len);
        ck("byte 0 is the protocol field's top byte, not a payload byte",
           g_m.last[0] == 0, g_m.last[0]);
        /* Byte 8 is where a magic would be in the full framing, and in
           a VERSION reply it is the MAJOR VERSION. These are the bytes
           the HP printed on 26 October. */
        ck("bytes 8..11 are major version 2, not a magic",
           g_m.last[8] == 0 && g_m.last[9] == 0 &&
           g_m.last[10] == 0 && g_m.last[11] == 2, g_m.last[11]);
        ck("the dump reports the channel", usbmux_dump() == 1, 0);
    }

    head("...AND is kept for a packet that fails validation");
    machine();
    usbmux_open();
    {
        uint32_t proto = 0;
        uint8_t *p = ph.in;
        /* The packet most worth looking at is the one that did not
           parse. Recording after validation would throw exactly that
           one away, and `usb mux` could not show what arrived.

           v0.8.7.8: this used to fail the packet with a wrong magic,
           which is no longer a refusal -- usbmuxd never reads that
           field and the device's value is not the host's. THE LENGTH
           is the gate, and it is the one usbmuxd actually checks:
           device_data_input() compares it against the transfer length
           and does nothing else with the header. */
        ck("a good handshake first", usbmux_handshake() == 2, 0);
        wr32(p + 0, 0u);
        wr32(p + 4, 4u);                  /* a length under the header */
        wr32(p + 8, 0xFACEFACEu);
        wr16(p + 12, 0);
        wr16(p + 14, 0x1234);
        ph.in_len = MUX_HDR_FULL + 4;
        ck("the packet is refused", usbmux_recv_packet(&proto) < 0, 0);
        ck("but the bytes were kept anyway", g_m.have_last, 0);
        ck("all of them", g_m.last_len == MUX_HDR_FULL + 4, g_m.last_len);
        ck("with the bad LENGTH visible, not hidden",
           g_m.last[4] == 0 && g_m.last[7] == 4, g_m.last[7]);
    }

    head("THE PHONE'S OWN BYTES, as the HP printed them on 26 October");
    machine();
    usbmux_open();
    {
        /* Not a fixture somebody reasoned their way to -- the twenty
           bytes an iPhone 05AC:12A8 actually sent. The suite passed 79
           of 79 against a sixteen-byte header the phone has never used,
           because the device model and the driver shared one author's
           misunderstanding. These bytes cannot be wrong. */
        static const uint8_t WIRE[20] = {
            0x00,0x00,0x00,0x00,   /* protocol 0, VERSION */
            0x00,0x00,0x00,0x14,   /* length 20           */
            0x00,0x00,0x00,0x02,   /* major 2             */
            0x00,0x00,0x00,0x00,   /* minor 0             */
            0x00,0x00,0x00,0x00    /* padding             */
        };
        uint32_t proto = 0;
        int r;
        memcpy(ph.in, WIRE, sizeof WIRE);
        ph.in_len = (int)sizeof WIRE;
        r = usbmux_recv_packet(&proto);
        ck("it parses", r == 12, r);
        ck("as a VERSION", proto == 0, (long)proto);
        ck("and the major version reads 2",
           rd32((const uint8_t *)usbmux_payload()) == 2,
           (long)rd32((const uint8_t *)usbmux_payload()));
    }

    head("the configuration carrying BOTH interfaces is the one taken");
    phone_reset();
    usbmux_close();
    ph.nconfigs = 3;        /* before enumeration: attach reports it */
    ph.phone_shape = 1;
    usb_enumerate();
    {
        /* The phone offers the mux interface in two configurations and
           Ethernet in only one of them. Taking the first match claims
           the mux and switches away the configuration tethering needs. */
        ck("the channel opens", usbmux_open() == USB_OK, 0);
        ck("and it took configuration 2, not the first match at 1",
           ph.cfg_index_asked >= 2, ph.cfg_index_asked);
        /* In that configuration the mux is interface 2 -- Ethernet
           took interface 1. Picking the config is not picking the
           interface number with it. */
        ck("and the mux interface there is interface 2", g_m.ifnum == 2,
           g_m.ifnum);
    }

    head("every transfer fits the back-end's buffers");
    ck("nothing was refused as too long for a bulk write",
       ph.sync_refused == 0, ph.sync_refused);
    ck("nor for a poll", ph.async_refused == 0, ph.async_refused);
    ck("MUX_BUF is within both", MUX_BUF <= SIM_SYNC_MAX &&
       MUX_BUF <= SIM_ASYNC_MAX, MUX_BUF);

    /* ════════════════════════════════════════════════════════════════
       5. STAGE 1's TRANSPORT -- the TCP connection
       ════════════════════════════════════════════════════════════════

       The device above is not a parrot: it refuses a wrong length, a
       missing magic, a header that is not five words, ports that are
       not this connection's, and -- the reason it exists -- a sequence
       number it was not expecting. So "the handshake completed" is a
       claim about arithmetic, not about politeness. */

    head("THE SYN, byte for byte");
    machine();
    usbmux_open();
    {
        int r = usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("the connection comes up", r == USB_OK, r);
        ck("and reports itself up", usbmux_connected(), 0);
        ck("the device reached its established state",
           ph.tcp_state == 2, ph.tcp_state);
        /* ph.out now holds the completing ACK, so the SYN itself is
           checked through what the DEVICE recorded -- which is the
           stricter reading anyway: it is what crossed the wire. */
        ck("...and refused nothing on the way", ph.reject_len == 0 &&
           ph.reject_magic == 0 && ph.reject_off == 0 &&
           ph.reject_ports == 0 && ph.reject_seq == 0 &&
           ph.reject_flags == 0, ph.reject_seq);
    }

    head("...and the SYN's own twenty bytes, against a device that\n       answers nothing");
    machine();
    ph.silent = 1;
    usbmux_open();
    {
        /* A silent device never answers the VERSION, so the connect
           stops at the handshake -- which is itself the check that
           usbmux_connect() runs it first rather than assuming it. */
        int r = usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("connect refuses when the channel never versioned",
           r == USB_ETIMEOUT, r);
        ck("nothing is connected", !usbmux_connected(), 0);
        ck("and that is NOT recorded as the port refusing",
           !usbmux_refused(), usbmux_refused());
        ck("only the VERSION went out -- no SYN into the dark",
           ph.out_calls == 1, ph.out_calls);
    }

    head("the SYN as the device read it");
    machine();
    usbmux_open();
    usbmux_handshake();
    {
        /* Connect from a settled channel, so the SYN is the first
           packet the device sees after the SETUP and ph.saw_* holds
           it before the ACK overwrites them. The device's state
           machine stops at state 1 if syn_flags makes the answer
           unusable, which is how the SYN's own fields are inspected
           without the ACK racing them. */
        ph.syn_flags = F_SYN;          /* a SYN with no ACK: not a handshake */
        ck("the connect is refused", usbmux_connect(MUX_PORT_LOCKDOWN) < 0, 0);
        ck("the SYN had flags 0x02", ph.saw_flags == F_SYN, ph.saw_flags);
        ck("sequence 0", ph.saw_seq == 0, (long)ph.saw_seq);
        ck("acknowledgement 0", ph.saw_ack == 0, (long)ph.saw_ack);
        ck("a five-word header", ph.saw_off == 5, ph.saw_off);
        ck("no checksum, which is what the mux expects",
           ph.saw_checksum == 0, ph.saw_checksum);
        /* Both ports as NUMBERS, written out here rather than taken
           from include/usbmux.h: a port written little-endian would
           reach the device as 0x5448 and 0x7EF2, and a fixture reading
           the constant out of the header under test would agree with
           either. 0x4854 is 'HT'; the source port is the host's to
           choose and the device only echoes it back. */
        ck("destination port 62078 == 0xF27E, big-endian",
           ph.reply_port == 0xF27E, ph.reply_port);
        /* v0.8.9.9: a fresh port per connection, so the exact value
           depends on how many connects came before. The big-endian
           question is still answered: the high byte is 'H' (0x48),
           and a little-endian write would put the low byte there. */
        ck("source port is the one the driver chose, big-endian",
           ph.exp_sport == usbmux_last_sport(), ph.exp_sport);
        ck("...from the 'H' range", (ph.exp_sport >> 8) == 0x48 ||
           (ph.exp_sport >> 8) == 0x49, ph.exp_sport);
        /* ── THE WINDOW ────────────────────────────────────────────
           In units of 256 bytes, and DERIVED from MUX_BUF rather than
           copied from usbmuxd's 512. Re-derived here from the three
           numbers it depends on: a window written as a literal in two
           places is 0.8.7's transfer-size bug again. */
        ck("the window is MUX_BUF's worth, in 256-byte units",
           ph.saw_win == (uint16_t)((MUX_BUF - MUX_HDR_FULL - 20) / 256),
           ph.saw_win);
        ck("...which is smaller than usbmuxd's 512, deliberately",
           ph.saw_win < 512, ph.saw_win);
        ck("and it fits in what this kernel can actually receive",
           (int)ph.saw_win * 256 <= MUX_BUF - MUX_HDR_FULL - 20,
           ph.saw_win * 256);
        ck("the device did not reach established", ph.tcp_state == 1,
           ph.tcp_state);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("THE COMPLETING ACK, which is where the arithmetic shows");
    machine();
    usbmux_open();
    {
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* Our SYN consumed one sequence number and so did theirs. The
           device refuses anything else, so reaching state 2 is the
           arithmetic being right -- but assert the numbers too, so a
           failure says WHICH one moved. */
        ck("our sequence advanced to 1", ph.saw_seq == 1, (long)ph.saw_seq);
        ck("and we acknowledged their SYN -- ISN plus one",
           ph.saw_ack == ph.dev_isn + 1, (long)ph.saw_ack);
        ck("it was a bare ACK, not a SYN again", ph.saw_flags == F_ACK,
           ph.saw_flags);
        ck("the device accepted both numbers", ph.reject_seq == 0,
           ph.reject_seq);
    }

    head("THE MUX PAIR IS MIRRORED on the packets stage 1 sends");
    machine();
    usbmux_open();
    {
        /* The point the SETUP alone could not make. The device writes
           0x1234 in its own rx_seq field; usbmuxd's device_data_input()
           takes that value and sends it straight back. v0.8.7.6
           counted arrivals instead and would have produced 1 or 2
           here -- a plausible number, which is why nothing caught it. */
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ck("the ACK carried the device's OWN rx_seq value back",
           ph.saw_mux_rx == 0x1234, ph.saw_mux_rx);
        ck("...which is not a count of anything", ph.saw_mux_rx != 1 &&
           ph.saw_mux_rx != 2 && ph.saw_mux_rx != 3, ph.saw_mux_rx);
        /* tx_seq, by contrast, really does advance per packet: the
           SETUP reset it to 0 and sent it, the SYN carried 1, the ACK
           carried 2. */
        ck("tx_seq advanced over SETUP, SYN and ACK", ph.saw_mux_tx == 2,
           ph.saw_mux_tx);
    }

    head("A PORT THAT REFUSES is a different answer from silence");
    machine();
    ph.refuse_port = 1;
    usbmux_open();
    {
        int r = usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("the connect fails", r < 0, r);
        ck("nothing is connected", !usbmux_connected(), 0);
        /* The distinction the whole command depends on. "lockdownd is
           not listening" and "the channel broke" send the next person
           to different places, and one error number for both is the
           x_control() mistake one layer up. */
        ck("and it is recorded AS A REFUSAL", usbmux_refused(), 0);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("...and the refusal flag does not survive the next attempt");
    machine();
    usbmux_open();
    {
        ph.refuse_port = 1;
        ck("refused once", usbmux_connect(MUX_PORT_LOCKDOWN) < 0, 0);
        ck("the flag is set", usbmux_refused(), 0);
        ph.refuse_port = 0;
        ph.tcp_state = 0;
        ck("the second attempt connects",
           usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* A stale refusal would make `usb lockdown` print "the device
           refused the port" over a working connection. */
        ck("and the flag is cleared", !usbmux_refused(), 1);
    }

    head("a SYN/ACK with anything extra on it is not a handshake");
    machine();
    usbmux_open();
    {
        /* usbmuxd compares the whole flags byte rather than masking,
           and that strictness is the behaviour being copied: a FIN
           riding along means the device is closing as it opens. */
        ph.syn_flags = F_SYN | F_ACK | F_FIN;
        ck("the connect is refused", usbmux_connect(MUX_PORT_LOCKDOWN) < 0, 0);
        ck("nothing is connected", !usbmux_connected(), 0);
        ck("and it is NOT reported as the port refusing",
           !usbmux_refused(), 1);
        ck("and it said so", g_logs > 0, g_logs);
    }

    head("a connection to a port nothing listens on gets an RST");
    machine();
    usbmux_open();
    {
        int r;
        ph.listen_port = 62079;        /* lockdownd is next door */
        r = usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("the connect fails", r < 0, r);
        ck("as a refusal", usbmux_refused(), 0);
        ck("and the device recorded the wrong port",
           ph.reject_ports == 1, ph.reject_ports);
    }

    head("A PACKET FOR SOMEBODY ELSE'S CONNECTION IS NOT THIS REPLY");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        int r;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* A previous host's connection on this device would carry a
           different port pair. Taking its payload as this
           conversation's reply is how a plist from somebody else's
           session gets read as lockdownd's answer to us. */
        ph.exp_sport = 0x7777;         /* a port we never used */
        queue_tcp(F_ACK, ph.dev_seq, 1, "xx", 2);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the packet is refused", r < 0, r);
        ck("and not delivered as two bytes", r != 2, r);
    }

    head("a TCP packet shorter than its own header is refused");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        uint8_t *p = ph.in;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* Eighteen bytes of TCP header. Without the length check the
           driver reads bytes 18 and 19 out of whatever is after it in
           g_buf, which on this call is the PREVIOUS packet's data and
           looks entirely plausible -- the stale-buffer shape this file
           already carries a check for one layer up.

           THE PORTS ARE VALID ON PURPOSE. The first version of this
           left the header zeroed, so the port check refused the packet
           before the length check was reached and the harness reported
           the guard as MISSED. A test whose subject is masked by a
           guard somewhere else is v0.8.6.7's two bugs again, and the
           answer is the same: make everything else about the packet
           correct, so only the thing under test is wrong. */
        wr32(p + 0, 6u);
        wr32(p + 4, (uint32_t)(MUX_HDR_FULL + 18));
        wr32(p + 8, 0xFACEFACEu);   /* the DEVICE's magic */
        wr16(p + 12, 0);
        wr16(p + 14, 0x1234);
        memset(p + MUX_HDR_FULL, 0, 18);
        wr16(p + MUX_HDR_FULL + 0, 62078);     /* their source port */
        wr16(p + MUX_HDR_FULL + 2, usbmux_last_sport()); /* ...and ours */
        p[MUX_HDR_FULL + 12] = 5 << 4;
        p[MUX_HDR_FULL + 13] = F_ACK;
        ph.in_len = MUX_HDR_FULL + 18;
        {
            int r = usbmux_recv(buf, (int)sizeof buf);
            ck("it is refused", r < 0, r);
            /* r - 20 for an 18-byte header is -2, which is USB_ESTALL:
               "the device returned STALL". It did no such thing. */
            ck("as USB_EXFER, not as a STALL the device never sent",
               r == USB_EXFER, r);
        }
    }

    head("A PACKET BIGGER THAN THE TRANSFER IS NAMED, not stitched");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        uint8_t *p = ph.in;
        int logs;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* There is no reassembly here and there is not going to be:
           the xHCI back-end bounces every transfer through 2048 bytes,
           so a buffer above that would move the truncation rather than
           remove it. The case has to be REFUSED AND SAID, because "the
           reply was too big to read" and "the framing is wrong" have
           different fixes. */
        wr32(p + 0, 6u);
        wr32(p + 4, 9000u);            /* claims nine thousand bytes */
        wr32(p + 8, 0xFACEFACEu);   /* the DEVICE's magic */
        wr16(p + 12, 0);
        wr16(p + 14, 0x1234);
        memset(p + MUX_HDR_FULL, 0, 20);
        ph.in_len = MUX_HDR_FULL + 20; /* ...and carries forty */
        logs = g_logs;
        /* ── AT THE RAW LAYER, and with the NAMED code ──────────────
           The first version of this called usbmux_recv() and asserted
           only `< 0`, and the harness reported the guard as MISSED
           twice over: the port check refused the packet first, and
           the return value `len - hdr` was negative anyway.

           usbmux_recv_packet() is the layer the guard is in, and
           without it the call returns 8984 -- a PAYLOAD LENGTH, in a
           2048-byte buffer, handed to a caller that will read it. */
        {
            uint32_t proto = 0;
            int rr = usbmux_recv_packet(&proto);
            ck("it is refused", rr < 0, rr);
            ck("as USB_EXFER", rr == USB_EXFER, rr);
            ck("and NOT as a payload length bigger than the buffer",
               rr < MUX_BUF, rr);
        }
        ck("and it said so, with both numbers", g_logs > logs,
           g_logs - logs);
        ck("and the connection survives a packet it refused",
           usbmux_connected(), 0);
        (void)buf;
    }

    head("SENDING DATA advances the sequence by the BYTES sent");
    machine();
    usbmux_open();
    {
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ck("eleven bytes go out", usbmux_send("hello there", 11) == USB_OK, 0);
        /* The device refuses a seq it is not expecting, so this is a
           check on the wire and not on a variable: it accepted the
           bytes, which means the seq was 1 and the ack was 1. */
        ck("the device accepted them", ph.host_bytes == 11,
           (long)ph.host_bytes);
        ck("...and they were the right eleven",
           ph.host_data_len == 11 &&
           memcmp(ph.host_data, "hello there", 11) == 0, ph.host_data_len);
        ck("it rode on an ACK, with no PSH", ph.saw_flags == F_ACK,
           ph.saw_flags);
        ck("the device refused no sequence number", ph.reject_seq == 0,
           ph.reject_seq);
        /* And the NEXT send has to be at 12, which only a driver that
           advanced by the byte count can produce. A driver advancing
           per packet would send 2 and the device would refuse it. */
        ck("a second send continues the byte stream",
           usbmux_send("!", 1) == USB_OK, 0);
        ck("...which the device also accepted", ph.host_bytes == 12,
           (long)ph.host_bytes);
        ck("and still refused nothing", ph.reject_seq == 0, ph.reject_seq);
    }

    head("RECEIVING DATA acknowledges it, or the device stops sending");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        int r;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        queue_tcp(F_ACK, ph.dev_seq, 1, "abcdef", 6);
        ph.dev_seq += 6;
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("six bytes arrive", r == 6, r);
        ck("...and they are the right six", memcmp(buf, "abcdef", 6) == 0, 0);
        /* usbmuxd's own comment: "Device likes it best when we are
           prompty ACKing data". A receiver that does not ack stops
           receiving once its window is nominally full, and the symptom
           is a reply that lands and then silence forever. */
        ck("THE SIX WERE ACKNOWLEDGED, immediately",
           ph.saw_ack == ph.dev_isn + 1 + 6, (long)ph.saw_ack);
        ck("by a bare ACK, carrying no data", ph.saw_flags == F_ACK &&
           ph.host_bytes == 0, (long)ph.host_bytes);
        ck("which the device accepted", ph.reject_seq == 0, ph.reject_seq);
    }

    head("a bare ACK is not data, and is not an error either");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        queue_tcp(F_ACK, ph.dev_seq, 1, 0, 0);
        /* A caller polling for a reply must not read a window update
           as a zero-length reply, nor as a failure. 0.8.6's lesson:
           count the wire, because a two-byte receive yields a
           zero-length frame whether it was refused or counted. */
        ck("it reads as nothing yet", usbmux_recv(buf, (int)sizeof buf) == 0, 0);
        ck("and the connection is still up", usbmux_connected(), 0);
    }

    head("nothing waiting reads as nothing, not as a failure");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ph.in_len = 0;
        ck("zero, not an error", usbmux_recv(buf, (int)sizeof buf) == 0, 0);
        ck("and still up", usbmux_connected(), 0);
    }

    head("AN RST ON AN OPEN CONNECTION tears it down");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        queue_tcp(F_RST, ph.dev_seq, 1, 0, 0);
        ck("the read fails", usbmux_recv(buf, (int)sizeof buf) < 0, 0);
        /* A connection reported up after a reset is one a caller keeps
           writing into, and every write succeeds at the USB layer. */
        ck("and the connection is DOWN", !usbmux_connected(), 1);
        ck("so a send is refused", usbmux_send("x", 1) == USB_ENODEV, 0);
    }

    head("anything but a bare ACK on an open connection is refused");
    machine();
    usbmux_open();
    {
        uint8_t buf[64];
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        queue_tcp((uint8_t)(F_ACK | F_SYN), ph.dev_seq, 1, "zz", 2);
        ck("refused", usbmux_recv(buf, (int)sizeof buf) < 0, 0);
        ck("and the connection dropped", !usbmux_connected(), 1);
    }

    /* v0.8.9.9: this was "A PAYLOAD BIGGER THAN THE CALLER'S BUFFER IS
       REFUSED", and that contract is what broke TLS on the HP -- see
       usbmux_recv(). The worry it answered was a short read that left
       the rest unacknowledged. The rest is now stashed and the WHOLE
       packet acked, and these checks hold the driver to both halves. */
    /* v0.8.9.9: the HP's "REFUSED the port" after a broken connection
       was the phone still holding the old port pair. */
    head("each connection comes from a fresh source port");
    machine();
    usbmux_open();
    {
        uint16_t a, b;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        a = ph.exp_sport;
        usbmux_disconnect();
        ck("and again", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        b = ph.exp_sport;
        ck("the device saw two different ports", a != b, b);
        ck("both from our range", (a >> 8) >= 0x48 && (b >> 8) <= 0x49, a);
    }

    head("A PAYLOAD BIGGER THAN THE CALLER'S BUFFER is served in pieces");
    machine();
    usbmux_open();
    {
        uint8_t buf[8];
        static char big[64];
        int r, i;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        for (i = 0; i < 40; i++) big[i] = (char)('a' + (i % 26));
        queue_tcp(F_ACK, ph.dev_seq, 1, big, 40);
        ph.dev_seq += 40;
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the caller gets what fits", r == 8, r);
        ck("the first eight bytes", memcmp(buf, big, 8) == 0, buf[0]);
        ck("all forty were acknowledged, not eight",
           ph.saw_ack == ph.dev_isn + 1 + 40, (long)ph.saw_ack);
        ck("thirty-two are waiting", usbmux_pending(0, 0) == 32,
           usbmux_pending(0, 0));
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the next read continues where it stopped",
           r == 8 && memcmp(buf, big + 8, 8) == 0, r);
        ck("still up", usbmux_connected(), 0);
    }

    head("a send longer than one packet is refused by name");
    machine();
    usbmux_open();
    {
        static char big[4096];
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        memset(big, 'B', sizeof big);
        /* usbmux_max_payload() is the bound, and it is ASKED for
           rather than restated -- stage 3's pair request will trip
           this and needs to know what it tripped. */
        ck("the bound is MUX_BUF's, minus both headers",
           usbmux_max_payload() == MUX_BUF - MUX_HDR_FULL - 20,
           usbmux_max_payload());
        {
            /* The RETURN VALUE cannot be the whole test: mux_tcp_send()
               bounds the same thing one layer down and returns the same
               USB_EPARAM, so removing the named refusal above it would
               leave this passing. The log is what distinguishes them --
               v0.8.7's two bugs that only failed together, in
               miniature. */
            int logs = g_logs;
            ck("one byte over is refused",
               usbmux_send(big, usbmux_max_payload() + 1) == USB_EPARAM, 0);
            ck("...and refused BY NAME, not silently by the layer below",
               g_logs > logs, g_logs - logs);
        }
        ck("nothing went to the device", ph.host_bytes == 0,
           (long)ph.host_bytes);
        ck("exactly the bound is accepted",
           usbmux_send(big, usbmux_max_payload()) == USB_OK, 0);
        ck("...and the back-end did not refuse it as too long",
           ph.sync_refused == 0, ph.sync_refused);
    }

    head("send and receive are refused when there is no connection");
    machine();
    usbmux_open();
    {
        uint8_t buf[16];
        ck("the channel is open but no connection is",
           usbmux_present() && !usbmux_connected(), 0);
        ck("send is refused", usbmux_send("x", 1) == USB_ENODEV, 0);
        ck("receive is refused", usbmux_recv(buf, 16) == USB_ENODEV, 0);
    }

    head("disconnecting resets the device, and the channel survives");
    machine();
    usbmux_open();
    {
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        usbmux_disconnect();
        ck("the connection is down", !usbmux_connected(), 1);
        /* usbmuxd closes with an RST; there is no graceful shutdown in
           this protocol. The device has to see it, or it keeps the
           connection and the next SYN on the same ports is a duplicate. */
        ck("the device saw the reset", ph.tcp_state == 0, ph.tcp_state);
        ck("but the interface is still claimed", usbmux_present(), 0);
        /* And a second connection costs no handshake: the version
           exchange happens once. */
        ck("a second connection opens",
           usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ck("without a second VERSION exchange", ph.setup_seen == 1,
           ph.setup_seen);
    }

    head("usb rescan's close sends nothing -- the bus is about to move");
    machine();
    usbmux_open();
    {
        int calls;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        calls = ph.out_calls;
        /* usbmux_close() runs BEFORE usb_enumerate() rewalks the bus,
           and the device it would say goodbye to may be the one that
           was unplugged -- which is usually why the rescan is
           happening. A farewell packet there is a transfer that waits
           for a device that is not there. usbmux_disconnect() is the
           polite path and is a different function on purpose. */
        usbmux_close();
        ck("not one packet went out", ph.out_calls == calls,
           ph.out_calls - calls);
        ck("and nothing is claimed", !usbmux_present(), 1);
    }

    head("THE WINDOW IS DERIVED, and cannot drift from MUX_BUF");
    {
        /* Re-derived here from the same three numbers, independently,
           because a bound written as a literal in two places is how
           0.8.7 shipped a driver that refused every Ethernet frame
           before the controller was touched. */
        ck("MUX_WINDOW is the units times 256",
           MUX_WINDOW == MUX_WINDOW_UNITS * 256, MUX_WINDOW);
        ck("and fits inside one packet's payload",
           MUX_WINDOW <= MUX_BUF - MUX_HDR_FULL - 20, MUX_WINDOW);
        ck("the units fit the sixteen-bit field",
           MUX_WINDOW_UNITS > 0 && MUX_WINDOW_UNITS < 65536,
           MUX_WINDOW_UNITS);
        ck("and the payload bound agrees with the accessor",
           usbmux_max_payload() == MUX_MAX_PAYLOAD, MUX_MAX_PAYLOAD);
    }

    head("the device's window is read in BYTES, not in its own units");
    machine();
    usbmux_open();
    {
        uint32_t win = 0;
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        usbmux_conn_state(0, 0, 0, 0, &win);
        /* The fixture advertises 512, which is usbmuxd's 131072. A
           driver that forgot the shift would report 512 and `usb
           lockdown` would print a device offering half a kilobyte. */
        ck("512 units read back as 131072 bytes", win == 131072u,
           (long)win);
    }

    head("A NONZERO ISN is acknowledged correctly, and REPORTED");
    machine();
    ph.dev_isn = 0x40000000u;
    usbmux_open();
    {
        int logs = g_logs;
        ck("it still connects",
           usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        /* usbmuxd does `tx_ack++` from zero, which is right only
           because the device's ISN has always been 0. That is an
           assumption, and as written it is unfalsifiable: a different
           ISN would make usbmuxd ack the wrong byte and the symptom
           would be a conversation that stalls for no visible reason.

           This acks the SYN the device actually sent. For an ISN of 0
           that is byte-identical to usbmuxd, so nothing is risked --
           and for anything else it is correct where usbmuxd is wrong.
           Because nobody has ever seen that case, it is LOGGED: a
           deviation nobody is told about is one nobody can check. */
        ck("the acknowledgement is the ISN plus one",
           ph.saw_ack == 0x40000001u, (long)ph.saw_ack);
        ck("the device accepted it", ph.tcp_state == 2, ph.tcp_state);
        ck("and the deviation was said out loud", g_logs > logs,
           g_logs - logs);
    }

    /* ════════════════════════════════════════════════════════════════
       v0.8.9.0: A REQUEST LONGER THAN ONE PACKET

       usbmux_send() refuses more than MUX_MAX_PAYLOAD by name.
       usbmux_send_stream() splits instead, and the defect shape it
       brings with it is one a single-packet sender cannot have: a
       segment sent twice, a segment skipped, or sequence numbers that
       advance per PACKET instead of per BYTE. None of those changes
       the number of bytes the device counts, so none of them can be
       caught by a count. The fixture accumulates the host's whole
       byte stream and these checks compare it, byte for byte, with
       what went in.
       ════════════════════════════════════════════════════════════════ */
    {
        static uint8_t big[12288];
        int i, sent = 0;
        for (i = 0; i < (int)sizeof big; i++)
            big[i] = (uint8_t)((i * 37 + 11) & 0xFF);

        head("a 5000-byte request is split, and arrives whole");
        machine();
        ph.auto_ack = 1;
        usbmux_open();
        ck("it connects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        {
            int expect_segs = (5000 + MUX_MAX_PAYLOAD - 1) / MUX_MAX_PAYLOAD;
            uint32_t tx = 0;
            int bad = -1;

            ck("send_stream accepts it",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            ck("it reports all 5000 bytes sent", sent == 5000, sent);
            ck("the device counted 5000 bytes",
               ph.host_bytes == 5000u, (long)ph.host_bytes);
            ck("it took more than one packet", ph.host_segs > 1,
               ph.host_segs);
            ck("exactly as many packets as the bound requires",
               ph.host_segs == expect_segs, ph.host_segs);
            ck("the fixture did not overflow", !ph.host_stream_over, 0);
            ck("the stream is 5000 bytes long",
               ph.host_stream_len == 5000u, (long)ph.host_stream_len);
            for (i = 0; i < 5000; i++)
                if (ph.host_stream[i] != big[i]) { bad = i; break; }
            /* THE CHECK THIS WHOLE BLOCK EXISTS FOR. A duplicated or
               dropped segment lands here and nowhere else. */
            ck("every byte arrived, in order, exactly once", bad < 0, bad);

            usbmux_conn_state(0, 0, &tx, 0, 0);
            /* Our SYN consumed sequence number 0, so after 5000 bytes
               of payload the stream position is 5001. A sender that
               advanced per packet would read 4 here. */
            ck("the sequence number advanced by BYTES, not packets",
               tx == 5001u, (long)tx);
            ck("the device rejected no sequence number",
               ph.reject_seq == 0, ph.reject_seq);
            ck("and no flags", ph.reject_flags == 0, ph.reject_flags);
            ck("it acked every segment",
               ph.acks_sent == expect_segs, ph.acks_sent);
        }

        head("the boundaries: exactly one packet, and one byte more");
        machine();
        ph.auto_ack = 1;
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("exactly MUX_MAX_PAYLOAD goes in one packet",
           usbmux_send_stream(big, MUX_MAX_PAYLOAD, &sent) == USB_OK, 0);
        ck("...and it is one packet", ph.host_segs == 1, ph.host_segs);
        ck("...all of it", ph.host_bytes == (uint32_t)MUX_MAX_PAYLOAD,
           (long)ph.host_bytes);

        machine();
        ph.auto_ack = 1;
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("one byte more takes two packets",
           usbmux_send_stream(big, MUX_MAX_PAYLOAD + 1, &sent) == USB_OK, 0);
        ck("...exactly two", ph.host_segs == 2, ph.host_segs);
        ck("...and the second carries the single byte",
           ph.host_data_len == 1, ph.host_data_len);
        ck("...with the right byte in it",
           ph.host_data[0] == big[MUX_MAX_PAYLOAD], ph.host_data[0]);
        ck("...and the whole thing is intact",
           ph.host_stream_len == (uint32_t)MUX_MAX_PAYLOAD + 1
           && memcmp(ph.host_stream, big,
                     (size_t)MUX_MAX_PAYLOAD + 1) == 0, 0);

        head("the largest request lockdown will stage (LD_REQ, 12288)");
        machine();
        ph.auto_ack = 1;
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("it goes out",
           usbmux_send_stream(big, 12288, &sent) == USB_OK, 0);
        ck("all of it", sent == 12288, sent);
        ck("in seven packets",
           ph.host_segs == (12288 + MUX_MAX_PAYLOAD - 1) / MUX_MAX_PAYLOAD,
           ph.host_segs);
        ck("byte for byte",
           ph.host_stream_len == 12288u
           && memcmp(ph.host_stream, big, 12288) == 0, 0);

        head("a zero-length send is a no-op, not a packet");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("it succeeds", usbmux_send_stream(big, 0, &sent) == USB_OK, 0);
        ck("nothing was sent", ph.host_segs == 0, ph.host_segs);
        ck("and it says so", sent == 0, sent);

        head("what send_stream refuses");
        ck("a negative length",
           usbmux_send_stream(big, -1, &sent) == USB_EPARAM, 0);
        ck("a NULL buffer",
           usbmux_send_stream(0, 100, &sent) == USB_EPARAM, 0);
        ck("...and sent_out is zeroed on a refusal", sent == 0, sent);
        usbmux_disconnect();
        ck("a closed connection",
           usbmux_send_stream(big, 100, &sent) == USB_ENODEV, 0);
        ck("a NULL sent_out is allowed",
           usbmux_send_stream(big, 100, 0) == USB_ENODEV, 0);

        /* ── the stash ───────────────────────────────────────────────
           The harvest between segments can turn up DATA rather than a
           bare ACK. Those bytes have been acked by the time they are
           in our hands, so the device will never send them again:
           dropping them loses the first packet of a reply and the
           symptom is a plist that parses as truncated. */
        head("data that arrives mid-send is kept, and comes back first");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            uint8_t rx[64];
            uint32_t hits = 0, ws = 0;
            /* auto_ack OFF: the fixture holds one packet, so the
               queued DATA is what the harvest will find. */
            queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "EARLY", 5);
            ph.dev_seq += 5;

            ck("a three-packet request still succeeds",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            ck("all of it arrived",
               ph.host_stream_len == 5000u
               && memcmp(ph.host_stream, big, 5000) == 0, 0);
            ck("five bytes are waiting in the stash",
               usbmux_pending(&hits, &ws) == 5,
               usbmux_pending(0, 0));
            ck("and the stash says it was used", hits == 1u, (long)hits);

            memset(rx, 0, sizeof rx);
            ck("the next recv returns them", usbmux_recv(rx, sizeof rx) == 5, 0);
            ck("...and they are the right bytes",
               memcmp(rx, "EARLY", 5) == 0, 0);
            ck("the stash is empty afterwards",
               usbmux_pending(0, 0) == 0, usbmux_pending(0, 0));
        }

        head("the stash is not overwritten by a SECOND mid-send packet");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            uint8_t rx[64];
            uint32_t hits = 0;
            /* Two packets waiting. The harvest runs between each pair
               of segments, so without the "stash is occupied" guard
               the second would land on top of the first and five
               bytes the device considers delivered would be gone. */
            queue_tcp (F_ACK, ph.dev_seq,     ph.exp_host_seq, "FIRST",  5);
            queue_tcp2(F_ACK, ph.dev_seq + 5, ph.exp_host_seq, "SECOND", 6);
            ph.dev_seq += 11;
            ck("the fixture is holding two", ph.in_len > 0 && ph.in2_len > 0, 0);
            ck("a three-packet request succeeds",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            ck("the request still arrived whole",
               ph.host_stream_len == 5000u
               && memcmp(ph.host_stream, big, 5000) == 0, 0);
            ck("the stash harvested exactly once",
               usbmux_pending(&hits, 0) == 5 && hits == 1u, (long)hits);
            memset(rx, 0, sizeof rx);
            ck("and it is the FIRST packet, not the second",
               usbmux_recv(rx, sizeof rx) == 5
               && memcmp(rx, "FIRST", 5) == 0, 0);
            /* The second is still on the device's side of the wire,
               where the protocol already knows how to keep it. */
            memset(rx, 0, sizeof rx);
            ck("the second is still there to be read",
               usbmux_recv(rx, sizeof rx) == 6
               && memcmp(rx, "SECOND", 6) == 0, 0);
            ck("the fixture dropped nothing", ph.in_dropped == 0,
               ph.in_dropped);
        }

        head("bytes taken mid-send are ACKNOWLEDGED, not just kept");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            /* A receiver that stashes without acking stops receiving
               once the device's window is nominally full, and the
               symptom is a reply that arrives once and then silence.
               usbmux_recv() acks for that reason and so must the
               harvest -- which is invisible unless the ack VALUE is
               checked, because the fixture rejects an ack that is too
               high and accepts one that is too low. */
            queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "EARLY", 5);
            ph.dev_seq += 5;
            ck("the request goes out",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            ck("five bytes stashed", usbmux_pending(0, 0) == 5, 0);
            /* The device's ISN is 0, its SYN consumed sequence number
               0, and it has now sent five bytes -- so the host owes
               an ack of 6. */
            ck("the host acknowledged the five bytes it took",
               ph.saw_ack == ph.dev_isn + 1 + 5,
               (long)ph.saw_ack);
            ck("the device rejected no sequence number",
               ph.reject_seq == 0, ph.reject_seq);
        }

        head("a device that RESETS mid-send stops the request there");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            int r;
            /* The RST is found by the harvest after the first
               segment, which is a different path from a transfer that
               fails: the send SUCCEEDED and the connection died
               anyway. */
            queue_tcp(F_RST, ph.dev_seq, ph.exp_host_seq, 0, 0);
            sent = -1;
            r = usbmux_send_stream(big, 5000, &sent);
            ck("it stops rather than finishing into a dead socket",
               r < 0, r);
            ck("one segment went out", sent == MUX_MAX_PAYLOAD, sent);
            ck("the device got one segment and no more",
               ph.host_bytes == (uint32_t)MUX_MAX_PAYLOAD,
               (long)ph.host_bytes);
            ck("the connection is down",
               !usbmux_connected(), usbmux_connected());
            ck("and a further send is refused",
               usbmux_send_stream(big, 10, 0) == USB_ENODEV, 0);
        }

        head("a caller with a small buffer gets the stash in pieces, in order");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            uint8_t rx[2];
            queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "EARLY", 5);
            ph.dev_seq += 5;
            usbmux_send_stream(big, 5000, &sent);
            ck("five bytes stashed", usbmux_pending(0, 0) == 5, 0);
            /* v0.8.9.9: served, not refused. The first two bytes now,
               the rest kept for the next call -- a byte stream, which
               is what net/tls.c's five-byte header read needs. */
            ck("a two-byte buffer gets two bytes",
               usbmux_recv(rx, 2) == 2, 0);
            ck("the FIRST two", rx[0] == 'E' && rx[1] == 'A', rx[0]);
            ck("three are still there",
               usbmux_pending(0, 0) == 3, usbmux_pending(0, 0));
            ck("the next two follow", usbmux_recv(rx, 2) == 2 &&
               rx[0] == 'R' && rx[1] == 'L', rx[0]);
            ck("then the last one", usbmux_recv(rx, 2) == 1 &&
               rx[0] == 'Y', rx[0]);
            ck("and the stash is empty", usbmux_pending(0, 0) == 0,
               usbmux_pending(0, 0));
            /* Put five back for the disconnect check below. */
            queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "EARLY", 5);
            ph.dev_seq += 5;
            usbmux_send_stream(big, 5000, &sent);
        }

        /* ── THE 0.8.9.8 TLS HANDSHAKE, AT THE LAYER IT BROKE ────────
           net/tls.c reads a record as five bytes of header and then
           the body. The phone answers a ClientHello with its whole
           flight in one mux packet. This driver refused that packet
           to a five-byte reader with USB_EPARAM, and the handshake
           died at "ClientHello sent" with no alert, on the HP. */
        head("one big packet read five bytes at a time, the way TLS reads");
        machine();
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            static uint8_t flight[1200], got[1200];
            uint32_t acked_before;
            int i, have = 0, calls = 0, bad = 0;
            for (i = 0; i < (int)sizeof flight; i++)
                flight[i] = (uint8_t)(i * 7 + 3);
            flight[0] = 0x16; flight[1] = 3; flight[2] = 3;   /* a record */
            queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq,
                      (const char *)flight, (int)sizeof flight);
            ph.dev_seq += sizeof flight;
            acked_before = ph.last_host_ack;
            {
                int r = usbmux_recv(got, 5);
                ck("five bytes, not a refusal", r == 5, r);
                have = r > 0 ? r : 0;
            }
            ck("the record header, intact",
               got[0] == 0x16 && got[1] == 3 && got[2] == 3, got[0]);
            ck("the WHOLE packet was acked at once, not five bytes of it",
               ph.last_host_ack == acked_before + sizeof flight,
               (long)(ph.last_host_ack - acked_before));
            ck("the rest is waiting", usbmux_pending(0, 0) ==
               (int)sizeof flight - 5, usbmux_pending(0, 0));
            while (have < (int)sizeof flight && calls < 1000) {
                int want = (int)sizeof flight - have;
                int r;
                if (want > 300) want = 300;
                r = usbmux_recv(got + have, want);
                calls++;
                if (r < 0) { bad = 1; break; }
                have += r;
            }
            ck("no read in between failed", !bad, bad);
            ck("every byte arrived", have == (int)sizeof flight, have);
            ck("in order", memcmp(got, flight, sizeof flight) == 0, 0);
            ck("and it was counted as split", usbmux_split_reads() >= 1,
               (long)usbmux_split_reads());
            ck("the connection is still up", usbmux_connected(), 0);
        }
        /* The disconnect check below wants five bytes stashed. */
        queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "EARLY", 5);
        ph.dev_seq += 5;
        usbmux_send_stream(big, 5000, &sent);

        head("disconnecting drops the stash");
        ck("there are bytes to drop", usbmux_pending(0, 0) == 5, 0);
        usbmux_disconnect();
        ck("and they are gone", usbmux_pending(0, 0) == 0,
           usbmux_pending(0, 0));

        /* ── the window ──────────────────────────────────────────────
           usbmuxd blocks on the device's advertised window; this warns
           and sends. usb/usbmux.c's comment says why. What is tested
           is that the warning actually fires -- an unexercised
           diagnostic is worth nothing, and the fixture advertising
           usbmuxd's 131072 means the real device would never trip it. */
        head("sending past the device's advertised window is counted");
        machine();
        ph.auto_ack      = 1;
        ph.dev_win_units = 8;           /* 8 * 256 == 2048 bytes */
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            uint32_t win = 0, hits = 0, ws = 0;
            int logs = g_logs;
            usbmux_conn_state(0, 0, 0, 0, &win);
            ck("the device offered 2048 bytes", win == 2048u, (long)win);
            ck("a 5000-byte request still goes out",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            ck("...whole", ph.host_stream_len == 5000u, (long)ph.host_stream_len);
            usbmux_pending(&hits, &ws);
            ck("the overrun was counted", ws > 0u, (long)ws);
            ck("and said out loud", g_logs > logs, g_logs - logs);
        }

        head("inside the window, nothing is counted and nothing is said");
        machine();
        ph.auto_ack      = 1;
        ph.dev_win_units = 512;         /* usbmuxd's 131072 */
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            uint32_t hits = 0, ws = 0;
            ck("a 5000-byte request goes out",
               usbmux_send_stream(big, 5000, &sent) == USB_OK, 0);
            usbmux_pending(&hits, &ws);
            ck("no window overrun", ws == 0u, (long)ws);
            ck("no stash was needed", hits == 0u, (long)hits);
        }

        /* ── a partial send ──────────────────────────────────────────
           The failure that matters. Once some segments are out,
           lockdownd is part way through a length prefix and the
           connection cannot be reused -- so what this has to do is
           report how far it got, and NOT claim success. */
        head("a transfer that breaks part way through");
        machine();
        ph.auto_ack = 1;
        usbmux_open();
        usbmux_connect(MUX_PORT_LOCKDOWN);
        {
            int r;
            /* The connection took four bulk OUTs (version, setup,
               SYN, the completing ACK), so the request's first
               segment is the fifth. It is a full MUX_BUF -- 2048, a
               multiple of 512 -- so the sixth is its zero-length
               packet (v0.8.9.7) and the second segment is the
               seventh. */
            ph.stall_out_on = 7;
            sent = -1;
            r = usbmux_send_stream(big, 5000, &sent);
            ck("it fails rather than claiming success", r < 0, r);
            ck("it says how many bytes went out",
               sent == MUX_MAX_PAYLOAD, sent);
            ck("the device got exactly those", 
               ph.host_bytes == (uint32_t)MUX_MAX_PAYLOAD,
               (long)ph.host_bytes);
            ck("...and they were the FIRST bytes, not a later segment",
               memcmp(ph.host_stream, big, 64) == 0, 0);
        }
        ph.stall_out_on = 0;
    }

    head("closing lets the interface go");
    usbmux_close();
    ck("nothing is claimed", !usbmux_present(), usbmux_present());
    ck("and a handshake is refused", usbmux_handshake() == USB_ENODEV, 0);
    /* ════════════════════════════════════════════════════════════════
       v0.8.9.2 -- A PACKET THAT ARRIVES TWICE

       THE DEFECT THIS SUITE SHIPPED. The receive path never looked at
       a packet's sequence number: it added the payload length to the
       acknowledgement and handed the bytes up. So a retransmission --
       what TCP does whenever an ACK is late or lost -- was delivered
       as new data.

       On Kyan's HP that put every lockdown reply one request behind:
       QueryType returned the GetValue answer and GetValue returned the
       QueryType answer, each perfectly framed and perfectly wrong.

       This suite had 150-odd checks over this transport and could not
       have found it, because nothing here had ever sent the same
       packet twice. A property that holds only while something never
       happens needs a test in which it happens -- tests/test_xhci.c
       records the same lesson about slots.
       ════════════════════════════════════════════════════════════════ */
    head("a retransmitted packet is not delivered twice");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        uint32_t seq0 = ph.dev_seq;
        uint32_t drops0 = g_m.dup_drops;
        int r;

        queue_tcp(F_ACK, seq0, ph.exp_host_seq, "abcdef", 6);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("six bytes arrive", r == 6, r);
        ck("and they are the right six", memcmp(buf, "abcdef", 6) == 0, 0);

        /* THE SAME SEQUENCE NUMBER AND THE SAME BYTES. That is what
           makes it a retransmission, and the sequence number is the
           only thing a receiver can tell them apart by. */
        memset(buf, 0, sizeof buf);
        queue_tcp(F_ACK, seq0, ph.exp_host_seq, "abcdef", 6);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the duplicate is NOT delivered", r == 0, r);
        ck("and the caller's buffer was not touched", buf[0] == 0, buf[0]);
        ck("it was counted", g_m.dup_drops == drops0 + 1,
           (long)(g_m.dup_drops - drops0));

        /* ── AND IT WAS RE-ACKNOWLEDGED ──────────────────────────────
           A silent drop leaves the device repeating the same packet
           forever: it retransmitted because it believes we never got
           the bytes, and dropping without telling it where we are
           confirms that belief. The injection harness needed this --
           "dropped" and "dropped and re-acked" are indistinguishable
           from the caller's side, and the difference is a connection
           that recovers versus one that wedges. Counted at the bus,
           which is the only place it shows. */
        {
            uint32_t outs = ph.out_calls;
            queue_tcp(F_ACK, seq0, ph.exp_host_seq, "abcdef", 6);
            usbmux_recv(buf, (int)sizeof buf);
            ck("a dropped duplicate still sends an ACK back",
               ph.out_calls > outs, (long)(ph.out_calls - outs));
        }

        /* And the real next bytes still arrive, which is the half that
           makes a drop a drop rather than a wedge. */
        queue_tcp(F_ACK, seq0 + 6, ph.exp_host_seq, "ghij", 4);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the NEXT four bytes still arrive", r == 4, r);
        ck("and they are the right four", memcmp(buf, "ghij", 4) == 0, 0);
    }

    head("the acknowledgement is a position, not a running total");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        uint32_t seq0 = ph.dev_seq, ack;
        queue_tcp(F_ACK, seq0, ph.exp_host_seq, "abcdef", 6);
        usbmux_recv(buf, (int)sizeof buf);
        usbmux_conn_state(0, 0, 0, &ack, 0);
        ck("the ack is the byte after the ones that arrived",
           ack == seq0 + 6, (long)ack);

        /* A duplicate must not move it. Adding the payload length --
           which is what shipped -- walks the acknowledgement past the
           end of the device's stream, and every number after that is
           wrong. */
        queue_tcp(F_ACK, seq0, ph.exp_host_seq, "abcdef", 6);
        usbmux_recv(buf, (int)sizeof buf);
        usbmux_conn_state(0, 0, 0, &ack, 0);
        ck("a duplicate does not advance it", ack == seq0 + 6, (long)ack);
    }

    head("a packet from the FUTURE is refused too");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        uint32_t seq0 = ph.dev_seq;
        int r;
        /* A gap means a packet was lost. Accepting the later one
           delivers the stream out of order with no way to tell, which
           is the same class of wrong as accepting a duplicate and
           rather harder to notice. */
        queue_tcp(F_ACK, seq0 + 100, ph.exp_host_seq, "late", 4);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("it is not delivered", r == 0, r);
        /* The one in the gap still works when it turns up. */
        queue_tcp(F_ACK, seq0, ph.exp_host_seq, "ontime", 6);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("and the expected bytes still arrive", r == 6, r);
        ck("intact", memcmp(buf, "ontime", 6) == 0, 0);
    }


    /* ══════════════════════════════════════════════════════════════════
       v0.8.9.5 -- A CLOSE IS NOT A FAULT

       Found on Kyan's HP, and the reason this suite did not find it
       first is the same shape as v0.8.9.2's: muxdev.inc could send a
       FIN and no test had ever asked it to. Every established-state
       check in the 150 above this one is driven by a device that
       answers and keeps the connection, because that is the device a
       protocol test naturally models.

       The real one does not. lockdownd closes a connection left idle,
       `pair` left one idle for seventy-one seconds while it generated
       two RSA keys, and usbmux_recv() reported the resulting FIN
       through the catch-all for malformed packets -- USB_EXFER, the
       code for CRC, babble and buffer faults. The screen said the
       request "did not get a reply (-3)" and -3 sent the diagnosis at
       the USB layer, where nothing had gone wrong at all.

       So: three flag outcomes, three distinguishable answers, and a
       sentence for each. The sentences are checked because an error
       code that cannot say which of four things happened is four bugs
       wearing one number -- v0.8.7.4, and it cost that release.
       ══════════════════════════════════════════════════════════════════ */

    head("a FIN is a close, not a transfer error");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        int r;
        /* FIN|ACK is 0x11. A bare ACK is 0x10, so a test written
           against equality with F_ACK -- which is what shipped --
           funnels this into the malformed-packet branch. */
        queue_tcp((uint8_t)(F_FIN | F_ACK), ph.dev_seq,
                  ph.exp_host_seq, 0, 0);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("it is USB_ENODEV, the connection is gone", r == USB_ENODEV, r);
        ck("NOT USB_EXFER -- that is the bus, and the bus was fine",
           r != USB_EXFER, r);
        ck("usbmux_connected() agrees", usbmux_connected() == 0,
           usbmux_connected());
        ck("and the reason says it was closed",
           strstr(usbmux_why(), "closed") != 0, 0);
        ck("naming the FIN, so the next reader does not have to guess",
           strstr(usbmux_why(), "FIN") != 0, 0);
    }

    head("a bare FIN, with no ACK bit, closes the same way");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        int r;
        queue_tcp(F_FIN, ph.dev_seq, ph.exp_host_seq, 0, 0);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("still a close", r == USB_ENODEV, r);
        ck("still says so", strstr(usbmux_why(), "closed") != 0, 0);
    }

    head("an RST is still an RST, and says a different thing");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        int r;
        queue_tcp(F_RST, ph.dev_seq, ph.exp_host_seq, 0, 0);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("USB_ENODEV, as before", r == USB_ENODEV, r);
        ck("reported as a reset, not a close",
           strstr(usbmux_why(), "reset") != 0, 0);
        ck("and the two reasons are different sentences",
           strstr(usbmux_why(), "FIN") == 0, 0);
    }

    head("flags that are none of the three are still a transfer error");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        int r;
        /* URG. Nothing in this protocol sends it; a packet carrying it
           is a packet this side does not understand, which is what
           USB_EXFER is for and what the FIN was wrongly folded into. */
        queue_tcp((uint8_t)0x20, ph.dev_seq, ph.exp_host_seq, 0, 0);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("USB_EXFER, kept for the case it belongs to", r == USB_EXFER, r);
        ck("and named as neither of the three",
           strstr(usbmux_why(), "neither") != 0, 0);
    }

    head("usbmux_why() is never null, even before anything has failed");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        const char *w = usbmux_why();
        ck("a sentence, not a null pointer", w != 0, 0);
        ck("and not an empty one", w && w[0] != 0, 0);
    }

    head("and a new connection does not inherit the last one's reason");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        /* The reason has to be SET before the clearing can be tested.
           Checking it on a connection that never failed proves only
           that the fixture was reset, which is what the first version
           of this check did -- and tools/check_usbmux_suite.py caught
           it: the injection that kept the stale reason was reported
           MISSED, because there was no stale reason to keep. */
        queue_tcp((uint8_t)(F_FIN | F_ACK), ph.dev_seq,
                  ph.exp_host_seq, 0, 0);
        usbmux_recv(buf, (int)sizeof buf);
        ck("the close is on record", strstr(usbmux_why(), "closed") != 0, 0);

        /* The device accepts a fresh connection after a close, which
           is what `pair` now does across the keygen. The fixture is
           put back in the closed state to model that; everything else
           about it, counters included, is left exactly as the failed
           conversation left it. */
        ph.tcp_state = 0;
        ck("it reconnects", usbmux_connect(MUX_PORT_LOCKDOWN) == USB_OK, 0);
        ck("and the reason no longer names the old conversation",
           strstr(usbmux_why(), "closed") == 0, 0);
    }

    /* ══════════════════════════════════════════════════════════════════
       v0.8.9.7 -- A CONTROL PACKET IS THE PHONE TALKING

       Kyan's HP, 10 November, straight after the pair request:
       "the request failed (-3): a packet that was not TCP on an open
       connection". Protocol 1 is CONTROL; usbmuxd logs its text and
       keeps the connection. This driver dropped both.
       ══════════════════════════════════════════════════════════════════ */
    head("a CONTROL message does not end the connection");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[64];
        int r, type = -1;
        uint32_t count = 0;
        const char *said;
        queue_control(3, "something the phone objected to");
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("it is 'nothing yet', not a failure", r == 0, r);
        ck("the connection is still up", usbmux_connected() == 1,
           usbmux_connected());
        said = usbmux_device_said(&type, &count);
        ck("what the phone said is kept",
           said && !strcmp(said, "something the phone objected to"), 0);
        ck("  ... with its type", type == 3, type);
        ck("  ... and counted", count == 1, (int)count);

        /* And the conversation carries on: data after the message is
           still delivered. */
        queue_tcp(F_ACK, ph.dev_seq, ph.exp_host_seq, "hello", 5);
        r = usbmux_recv(buf, (int)sizeof buf);
        ck("the next TCP packet is still delivered",
           r == 5 && !memcmp(buf, "hello", 5), r);
    }

    head("a CONTROL message cannot put control characters on the screen");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[16];
        int type = 0;
        const char *said;
        queue_control(7, "line one\nline\x1b[2Jtwo\r\n");
        usbmux_recv(buf, (int)sizeof buf);
        said = usbmux_device_said(&type, 0);
        ck("newlines become spaces and the escape a dot",
           said && !strcmp(said, "line one line.[2Jtwo"), 0);
        ck("type 7 is kept as 7", type == 7, type);
    }

    head("an empty CONTROL message is still a message");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[16];
        uint32_t before = 0;
        usbmux_device_said(0, &before);
        queue_control(3, "");
        ck("nothing yet", usbmux_recv(buf, (int)sizeof buf) == 0, 0);
        {
            uint32_t after = 0;
            const char *said = usbmux_device_said(0, &after);
            ck("counted, with empty text",
               after == before + 1 && said && said[0] == 0, (int)after);
        }
    }

    head("what the phone said survives the connection it was said on");
    {
        uint8_t buf[16];
        machine();
        usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
        queue_control(3, "the reason");
        usbmux_recv(buf, (int)sizeof buf);
        ph.tcp_state = 0;
        usbmux_connect(MUX_PORT_LOCKDOWN);
        ck("a reconnect does not erase it",
           usbmux_device_said(0, 0) &&
           !strcmp(usbmux_device_said(0, 0), "the reason"), 0);
    }

    /* ══════════════════════════════════════════════════════════════════
       v0.8.9.7 -- THE ZERO-LENGTH PACKET
       muxdev.inc now delivers a bulk transfer only when it ends: on a
       USB packet shorter than 512, or on a zero-length one. The pair
       request's first mux packet is 2048 bytes, and without a ZLP the
       phone glued the second packet onto it.
       ══════════════════════════════════════════════════════════════════ */
    head("a full-size mux packet is followed by a zero-length packet");
    machine();
    ph.auto_ack = 1;
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        static uint8_t req[3728];
        int sent = -1, z0 = ph.zlps;
        for (int i = 0; i < (int)sizeof req; i++) req[i] = (uint8_t)(i * 7 + 3);
        ck("the pair-sized request goes out",
           usbmux_send_stream(req, (int)sizeof req, &sent) == USB_OK
           && sent == (int)sizeof req, sent);
        ck("exactly one ZLP: after the 2048-byte packet, not the short one",
           ph.zlps - z0 == 1, ph.zlps - z0);
        ck("the driver counted it", usbmux_zlps() >= 1u, (long)usbmux_zlps());
        ck("no transfer held two mux packets", ph.glued == 0, ph.glued);
        ck("and the device has every byte, in order",
           ph.host_stream_len == sizeof req &&
           memcmp(ph.host_stream, req, sizeof req) == 0,
           (long)ph.host_stream_len);
    }

    head("a ZLP that fails is a send that fails");
    machine();
    ph.auto_ack = 1;
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        static uint8_t req[3728];
        int sent = -1, r;
        memset(req, 'z', sizeof req);
        /* +1 is the full 2048-byte packet, +2 its ZLP. */
        ph.stall_out_on = ph.out_calls + 2;
        r = usbmux_send_stream(req, (int)sizeof req, &sent);
        ph.stall_out_on = 0;
        ck("it is reported as a failure, not a send",
           r < 0, r);
        ck("  ... and the device was never handed half a message",
           ph.host_stream_len == 0, (long)ph.host_stream_len);
    }

    head("a packet that is not a multiple of 512 gets no ZLP");
    machine();
    ph.auto_ack = 1;
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        static uint8_t small[300];
        int sent = -1, z0 = ph.zlps;
        memset(small, 'x', sizeof small);
        usbmux_send_stream(small, (int)sizeof small, &sent);
        ck("336 bytes on the wire, no ZLP", ph.zlps == z0, ph.zlps - z0);
    }

    head("a packet of exactly 512 bytes gets one too");
    machine();
    ph.auto_ack = 1;
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        static uint8_t five12[512 - MUX_HDR_FULL - MUX_TCP_HDR];
        int sent = -1, z0 = ph.zlps;
        memset(five12, 'y', sizeof five12);
        usbmux_send_stream(five12, (int)sizeof five12, &sent);
        ck("a 512-byte packet is terminated", ph.zlps - z0 == 1,
           ph.zlps - z0);
        ck("  ... and delivered", ph.host_stream_len == sizeof five12,
           (long)ph.host_stream_len);
    }

    head("other protocols on an open connection are still refused");
    machine();
    usbmux_open(); usbmux_handshake(); usbmux_connect(MUX_PORT_LOCKDOWN);
    {
        uint8_t buf[16];
        queue_post(2);                    /* SETUP, mid-connection */
        ck("SETUP mid-connection is still a transfer error",
           usbmux_recv(buf, (int)sizeof buf) == USB_EXFER, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
