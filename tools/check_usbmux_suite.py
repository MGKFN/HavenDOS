#!/usr/bin/env python3
"""check_usbmux_suite.py -- prove tests/test_usbmux.c can fail.

usb/usbmux.c speaks a protocol nobody in this tree has seen on a wire.
Its framing is read off the published behaviour of usbmuxd, exactly as
usb/ethernet.c's was -- and usb/ethernet.c passed 110 checks and could
not move a byte on real hardware.

So this suite's job is not to show the guess is right. It is to pin the
guess down tightly enough that one boot on the HP refutes it, and an
injection that moves any field of the header in any direction has to be
caught. A suite that tolerates the length field dropping the header is
a suite that would not notice the day somebody "corrects" it from a
forum post.

Run:  python3 tools/check_usbmux_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/usbmux.c"
TEST = "tests/test_usbmux.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-Iinclude",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ══════════════════════════════════════════════════════════════════
    # v0.8.9.2 -- THE SEQUENCE NUMBER
    #
    # Found on Kyan's HP, not by this harness, and the reason it was not
    # found here is worth more than the injection: tests/muxdev.inc had
    # never sent the same packet twice. The receive path added the
    # payload length to the acknowledgement and handed the bytes up, so
    # a retransmission -- what TCP does whenever an ACK is late -- was
    # delivered as new data, and from then on every lockdown request
    # got the PREVIOUS request's reply. Each one parsed perfectly and
    # contained the wrong thing.
    # ══════════════════════════════════════════════════════════════════
    ("a duplicate packet is delivered as new data",
     "the defect that shipped. tx_tcp_ack IS the next byte expected, so "
     "a data packet is new if and only if its sequence number equals "
     "it. Accepting anything walks the stream one message out of step "
     "and never recovers",
     """    if (seq == g_m.tx_tcp_ack) {
        /* Derived, not accumulated. */
        g_m.tx_tcp_ack = seq + (uint32_t)r;
        return 1;
    }""",
     """    if (1) {
        g_m.tx_tcp_ack += (uint32_t)r;
        return 1;
    }"""),

    # NOT REACHABLE, recorded rather than counted (v0.8.6.3's rule):
    #
    # "the acknowledgement is accumulated rather than derived" --
    #     g_m.tx_tcp_ack = seq + r  ->  g_m.tx_tcp_ack += r
    #
    # Inside that branch seq == g_m.tx_tcp_ack is the condition that
    # got us there, so the two expressions are the same number. The
    # derived form is written that way because it says what the value
    # MEANS -- a position in the device's stream -- and because a
    # running total is what the defect above actually was. It cannot
    # be distinguished by any test, and an injection that cannot
    # express a reachable defect reports a catch the suite did not
    # earn.

    ("an out-of-sequence packet is dropped SILENTLY",
     "without the re-ACK the device repeats the same packet forever, "
     "and a connection that would have recovered wedges instead",
     """    if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)
        klog(KLOG_WARN, "usbmux: could not re-ACK at %u", g_m.tx_tcp_ack);
    return 0;""",
     """    return 0;"""),

    ("a duplicate is STASHED instead of dropped",
     "mux_harvest() puts a packet aside for the next reader. A stale "
     "one put aside there is handed to that reader as though it were "
     "the answer to its own request -- which is exactly how a reply "
     "ends up answering the request after it",
     """    if (!mux_rx_accept(seq, r)) return 0;

    memcpy(g_m.pend, mux_tcp_payload(), (uint32_t)r);""",
     """    memcpy(g_m.pend, mux_tcp_payload(), (uint32_t)r);"""),

    ("a duplicate reaches the caller before being refused",
     "the copy happens first and the refusal second, so the caller's "
     "buffer holds the stale bytes whatever is returned",
     # v0.8.9.9: re-anchored -- the split-read block now sits between
     # the accept and the whole-packet copy.
     """    if (!mux_rx_accept(seq, r)) return 0;

    /* ── v0.8.9.9: A BYTE STREAM""",
     """    memcpy(buf, mux_tcp_payload(), (uint32_t)(r < max ? r : max));
    if (!mux_rx_accept(seq, r)) return 0;

    /* ── v0.8.9.9: A BYTE STREAM"""),


    # ── the header, which is the whole guess ──────────────────────────
    ("the length field does not count the header",
     "the field most easily got wrong and the one a device refuses in "
     "silence. 16 bytes short on every packet",
     """    *(uint32_t *)(g_buf +  4) = be32((uint32_t)total);""",
     """    *(uint32_t *)(g_buf +  4) = be32((uint32_t)plen);"""),

    ("the header is written little-endian",
     "every field of this protocol is network order. A little-endian "
     "magic is 0xCEFAEDFE, which is not a magic at all",
     """    *(uint32_t *)(g_buf +  8) = be32(MUX_MAGIC);""",
     """    *(uint32_t *)(g_buf +  8) = MUX_MAGIC;"""),

    # MUX_MAGIC is in include/usbmux.h and inject.run() damages one
    # file, so this hits the use site -- which is the thing worth
    # injecting anyway: a constant that is right and a packet that
    # carries something else is the defect.
    ("the magic sent is wrong",
     "the one field that says the framing is right",
     """    *(uint32_t *)(g_buf +  8) = be32(MUX_MAGIC);""",
     """    *(uint32_t *)(g_buf +  8) = be32(MUX_MAGIC + 1u);"""),

    # ── IT CAME BACK, as this block said it would ────────────────────
    # v0.8.7.6 recorded "tx_seq and rx_seq are swapped" as removed
    # because it was NOT REACHABLE: the only sequenced packet the driver
    # sent was one SETUP, on which both fields were 1, so swapping the
    # two writes changed nothing on the wire. The note said it becomes
    # reachable at stage 1 and the injection comes back then.
    #
    # It is reachable for a better reason than expected. The two values
    # on a SETUP are not 1 and 1 at all -- usbmuxd RESETS the pair to 0
    # and 0xFFFF -- so v0.8.7.6 was writing two wrong numbers that
    # happened to be equal, which is why nothing could tell them apart.
    # They differ now, and the swap is caught on the first packet.

    ("tx_seq and rx_seq are swapped",
     "0 and 0xFFFF, written into each other's field. The device reads "
     "a host that has received 65535 packets and sent none",
     """        *(uint16_t *)(g_buf + 12) = be16(g_m.tx_seq);
        *(uint16_t *)(g_buf + 14) = be16(g_m.rx_seq);""",
     """        *(uint16_t *)(g_buf + 12) = be16(g_m.rx_seq);
        *(uint16_t *)(g_buf + 14) = be16(g_m.tx_seq);"""),

    ("tx_seq never advances",
     "a sequence number that is always zero is a host that, as far as "
     "the device can tell, keeps resending its first packet",
     """    if (hdr == MUX_HDR_FULL) g_m.tx_seq++;
    g_m.sent++;""",
     """    g_m.sent++;"""),

    # ── MASKED BY THE RESET, and recorded rather than deleted ────────
    #
    # "tx_seq advances on the VERSION packet too" -- removing the
    # `hdr == MUX_HDR_FULL` guard on the increment. Reported MISSED,
    # and correctly.
    #
    # The VERSION is the only packet sent on the short header, and the
    # very next packet is the SETUP, which RESETS tx_seq to 0 before
    # writing it. So whatever the VERSION did to the counter is
    # overwritten before anything carries it. There is no sequence of
    # calls that can tell the two apart: a version-1 device never
    # reaches the full header at all, and a version-2 device always
    # sends a SETUP.
    #
    # The guard stays. It states the rule -- a field is advanced by the
    # packets that carry it -- and mux_send() is called by three paths
    # today and will be called by stage 3's.
    #
    # AND CHASING IT FOUND A REAL BUG. Establishing why it could not be
    # caught meant asking what happens if the SETUP does not send, and
    # the answer was that v0.8.7.6 never sent it again: the channel sat
    # claiming a version the device had never agreed. That injection is
    # above, and it is caught. Which is the argument for running the
    # harness even when every result in it is going to be a shrug.

    ("the SETUP packet does not reset the sequence pair",
     "usbmuxd's send_packet() sets tx_seq to 0 and rx_seq to 0xFFFF on "
     "a SETUP and writes those values. Without it the pair carries "
     "whatever the version exchange left",
     """        if (proto == MUX_PROTO_SETUP) {
            g_m.tx_seq = 0;
            g_m.rx_seq = MUX_RX_SEQ_NONE;
        }""",
     """        ;"""),

    ("rx_seq starts at zero rather than 'nothing yet'",
     "0xFFFF is the value that means no packet has been received. Zero "
     "is a count, and claiming to have received packet zero is a claim",
     """            g_m.rx_seq = MUX_RX_SEQ_NONE;""",
     """            g_m.rx_seq = 0;"""),

    ("rx_seq counts arrivals instead of mirroring the device's field",
     "v0.8.7.6's rule. usbmuxd's device_data_input() does one "
     "assignment -- it takes the arriving header's rx_seq and sends it "
     "straight back -- and a tally produces 1, 2, 3, which looks "
     "entirely plausible and is not what the device said",
     """    if (hdr == MUX_HDR_FULL)
        g_m.rx_seq = be16(*(uint16_t *)(g_buf + 14));""",
     """    if (hdr == MUX_HDR_FULL)
        g_m.rx_seq++;"""),

    ("rx_seq is mirrored from the device's TX field, not its RX one",
     "two bytes to the left: the device's own tx_seq, which usbmuxd "
     "ignores entirely",
     """        g_m.rx_seq = be16(*(uint16_t *)(g_buf + 14));""",
     """        g_m.rx_seq = be16(*(uint16_t *)(g_buf + 12));"""),

    # ── what it must refuse to believe ────────────────────────────────
    # ── GONE, AND WHY. The HP, 30 October ────────────────────────────
    #
    # "a wrong magic is parsed around" was an injection here until
    # v0.8.7.8, and it was CAUGHT, and the guard it protected was
    # WRONG.
    #
    # The phone's first TCP packet -- the SYN/ACK to lockdownd, correct
    # in every other field -- carries magic 0xFACEFACE. The driver
    # wrote 0xFEEDFACE, refused the packet, and reported "the framing
    # in this driver is wrong".
    #
    # usbmuxd NEVER READS THE FIELD. src/device.c writes it in
    # send_packet() and device_data_input() validates only the length.
    # The guard was strictness the working implementation does not
    # have, and the suite and the fixture agreed it was right because
    # both had taken the value from the same place.
    #
    # THE LESSON IS ABOUT THIS FILE, not about the driver: an injection
    # being caught says the suite notices a change. It says nothing
    # about whether the behaviour being protected is correct. A harness
    # can only ever ask "would we notice if this changed" -- never
    # "should it be this way".
    #
    # What replaces it is below: the magic must be REPORTED and the
    # packet must still be delivered.

    ("an unexpected magic is refused instead of reported",
     "v0.8.7.7's bug exactly. The device sends 0xFACEFACE where this "
     "host writes 0xFEEDFACE, usbmuxd never reads the field, and a "
     "receiver stricter than the implementation that works is one "
     "that refuses working devices",
     """        if (magic != MUX_MAGIC && magic != MUX_MAGIC_DEVICE)
            klog(KLOG_WARN, "usbmux: magic %x, neither %x nor %x -- "
                 "noted, not refused", magic, (uint32_t)MUX_MAGIC,
                 (uint32_t)MUX_MAGIC_DEVICE);""",
     """        if (magic != MUX_MAGIC && magic != MUX_MAGIC_DEVICE)
            return USB_EXFER;"""),

    ("the device's own magic is treated as unexpected",
     "0xFACEFACE is what the phone actually sends, so a driver that "
     "warns about it warns on every packet of a working connection",
     """        if (magic != MUX_MAGIC && magic != MUX_MAGIC_DEVICE)""",
     """        if (magic != MUX_MAGIC)"""),

    # ── REDUNDANT, BY DESIGN, and recorded rather than deleted ───────
    #
    # "a reply shorter than a header is accepted" was written as an
    # injection and reported MISSED. It is masked: a short reply leaves
    # the magic and length fields holding the PREVIOUS packet's bytes,
    # and the `len > r` check downstream refuses them.
    #
    # The guard stays. It is the cheapest and most direct of the three
    # and it is the only one that does not depend on what happens to be
    # left in the buffer -- which is exactly the kind of reasoning
    # v0.8.6.2 refused to accept about fat16_borrow(), and v0.8.7's
    # four "only fails together" pairs refused again.
    #
    # Not a defect this suite can reach. Recorded so nobody writes it
    # a second time.

    ("a length field smaller than the header is believed",
     "len - 16 is negative, and a negative length reaches the caller "
     "as a payload size",
     """    if ((int)len < hdr) return USB_EXFER;""",
     """    ;"""),

    ("a packet longer than the transfer is believed",
     "there is no reassembly here: the xHCI back-end bounces every "
     "transfer through 2048 bytes. A packet claiming more than arrived "
     "is a payload length read out of whatever follows it in the buffer",
     """    if ((int)len > r) {""",
     """    if (0) {"""),

    # ── the VERSION exchange ──────────────────────────────────────────
    ("the VERSION payload is sent little-endian",
     "the device reads major 2 as 33554432",
     """    vh[0] = be32(2);        /* major */""",
     """    vh[0] = 2;"""),

    ("the version packet asks for version 1",
     "a version-1 session never gets the setup byte, and the device "
     "never finishes opening the channel",
     """    vh[0] = be32(2);        /* major */
    vh[1] = be32(0);        /* minor */""",
     """    vh[0] = be32(1);        /* major */
    vh[1] = be32(0);        /* minor */"""),

    ("the setup byte is wrong",
     "0x07 is what a version-2 device expects before anything else",
     """    uint8_t setup = 0x07;""",
     """    uint8_t setup = 0x01;"""),

    ("SETUP is sent to a version-1 device",
     "a device that did not ask for it",
     """    if (g_m.version >= 2) {""",
     """    if (g_m.version >= 1) {"""),

    ("a reply that is not a VERSION is accepted as one",
     "the first four payload bytes of whatever arrived become the "
     "device's version number",
     """    if (proto != MUX_PROTO_VERSION) {""",
     """    if (0) {"""),

    ("the version is read from the header, not the payload",
     "off by the header length: the version comes back as part of the "
     "protocol field",
     """    g_m.version = (int)be32(*(uint32_t *)(g_buf + MUX_HDR_SHORT));""",
     """    g_m.version = (int)be32(*(uint32_t *)(g_buf));"""),

    # ── the evidence ──────────────────────────────────────────────────
    ("the raw packet is kept AFTER the header is validated",
     "and a packet with a wrong magic -- the one most worth looking at "
     "-- is never recorded, so `usb mux` cannot show what arrived",
     """    {
        int k = r < (int)sizeof g_m.last ? r : (int)sizeof g_m.last;
        memcpy(g_m.last, g_buf, (uint32_t)k);
        g_m.last_len  = r;
        g_m.have_last = 1;
    }
    g_m.received++;

    if (r < hdr) return USB_EXFER;""",
     """    g_m.received++;

    if (r < hdr) return USB_EXFER;
    if (hdr == MUX_HDR_FULL &&
        be32(*(uint32_t *)(g_buf + 8)) != MUX_MAGIC) return USB_EXFER;
    {
        int k = r < (int)sizeof g_m.last ? r : (int)sizeof g_m.last;
        memcpy(g_m.last, g_buf, (uint32_t)k);
        g_m.last_len  = r;
        g_m.have_last = 1;
    }"""),

    # ── v0.8.7.6: the header length is the version ───────────────────
    ("the header is sixteen bytes always",
     "the 0.8.7.3 bug exactly: the VERSION exchange is read with a "
     "header the version has not been agreed for, so byte 8 -- the "
     "major version -- is taken for a magic. On the HP this printed "
     "\"magic 2, expected FEEDFACE\"",
     """    return (g_m.version >= 2) ? MUX_HDR_FULL : MUX_HDR_SHORT;""",
     """    return MUX_HDR_FULL;"""),

    ("the header is eight bytes always",
     "the other direction: once the version is settled both ends add "
     "the magic and the sequence numbers, and a driver still reading "
     "eight takes the magic for its payload",
     """    return (g_m.version >= 2) ? MUX_HDR_FULL : MUX_HDR_SHORT;""",
     """    return MUX_HDR_SHORT;"""),

    ("the magic is checked during the version exchange",
     "there is no magic in a VERSION packet -- neither end knows the "
     "version yet -- so this checks the major version against "
     "0xFEEDFACE and refuses every device there is",
     """    if (hdr == MUX_HDR_FULL) {
        magic = be32(*(uint32_t *)(g_buf + 8));""",
     """    if (1) {
        magic = be32(*(uint32_t *)(g_buf + 8));"""),

    # ── claiming the interface ────────────────────────────────────────
    ("the survey matches on the class alone",
     "0xFF is 'vendor specific' and says nothing. The Apple Ethernet "
     "interface in the same configuration is also 0xFF, so matching on "
     "the class claims the wrong one and switches its configuration",
     """    if (!s->found && alt == 0 &&
        cls == MUX_IF_CLASS && sub == MUX_IF_SUBCLASS &&
        proto == MUX_IF_PROTOCOL) {""",
     """    if (!s->found && alt == 0 && cls == MUX_IF_CLASS) {"""),

    ("the first configuration with a mux interface is taken",
     "the phone offers the channel in two configurations and Ethernet "
     "in only one of them. Taking the first match claims the mux and "
     "switches away the configuration tethering needs -- which is what "
     "`usb mux` did to `usb eth` on the HP, every time it ran",
     """                if (s.with_eth) { cfg = ci; break; }""",
     """                { cfg = ci; break; }"""),

    ("only the first configuration is surveyed",
     "the phone boots into PTP and the channel is not in it",
     """        for (ci = 0; ci < (int)d->nconfigs; ci++) {""",
     """        for (ci = 0; ci < 1; ci++) {"""),

    ("an interface with one bulk endpoint is accepted",
     "a channel that can send and not receive",
     """        if (!in || !out) {""",
     """        if (!in && !out) {"""),

    ("the endpoint directions are swapped",
     "packets written to the IN endpoint and read from the OUT one",
     """        g_m.ep_in   = in->addr;
        g_m.ep_out  = out->addr;""",
     """        g_m.ep_in   = out->addr;
        g_m.ep_out  = in->addr;"""),

    ("open re-selects a configuration it has already claimed",
     "`usb mux` run twice would switch the configuration of a channel "
     "that is working",
     """    if (g_m.open) return USB_OK;
""",
     """"""),

    ("a handshake on a closed channel is attempted",
     "a transfer to endpoint zero of device zero",
     """    if (!g_m.open) return USB_ENODEV;

    /* ── VERSIONING HAPPENS ONCE. THE SETUP MAY STILL BE OWED ────""",
     """    /* ── VERSIONING HAPPENS ONCE. THE SETUP MAY STILL BE OWED ────"""),

    # ════════════════════════════════════════════════════════════════════
    # THE CONNECTION LAYER, v0.8.7.7
    #
    # Everything below is the transport stage 1 runs over. The simulated
    # device in tests/muxdev.inc refuses a sequence number it was not
    # expecting, which is what makes most of these catchable at all: a
    # fixture that shrugged at wrong arithmetic could only tell a
    # correct driver from one that crashed.
    # ════════════════════════════════════════════════════════════════════

    ("the TCP header says four words, not five",
     "th_off is the header length in 32-bit words and a BSD tcphdr is "
     "five of them. Four makes the last four bytes of the header read "
     "as the first four of the payload",
     """    th[MUX_TH_OFF]                   = (uint8_t)((MUX_TCP_HDR / 4) << 4);""",
     """    th[MUX_TH_OFF]                   = (uint8_t)(4 << 4);"""),

    ("the header offset is written in the low nibble",
     "th_off is the HIGH nibble; the low four bits are reserved and "
     "must be zero",
     """    th[MUX_TH_OFF]                   = (uint8_t)((MUX_TCP_HDR / 4) << 4);""",
     """    th[MUX_TH_OFF]                   = (uint8_t)(MUX_TCP_HDR / 4);"""),

    ("the ports are written little-endian",
     "62078 little-endian is 0x7EF2, which is port 32498",
     """    *(uint16_t *)(th + MUX_TH_DPORT) = be16(g_m.dport);""",
     """    *(uint16_t *)(th + MUX_TH_DPORT) = g_m.dport;"""),

    ("the sequence number is written little-endian",
     "every field of this protocol is network order",
     """    *(uint32_t *)(th + MUX_TH_SEQ)   = be32(g_m.tx_tcp_seq);""",
     """    *(uint32_t *)(th + MUX_TH_SEQ)   = g_m.tx_tcp_seq;"""),

    ("the source and destination ports are swapped",
     "the device answers to the port it was addressed on, so a swap "
     "sends the SYN to port 0x4854 and claims to come from lockdownd",
     """    *(uint16_t *)(th + MUX_TH_SPORT) = be16(g_m.sport);
    *(uint16_t *)(th + MUX_TH_DPORT) = be16(g_m.dport);""",
     """    *(uint16_t *)(th + MUX_TH_SPORT) = be16(g_m.dport);
    *(uint16_t *)(th + MUX_TH_DPORT) = be16(g_m.sport);"""),

    ("the window is advertised as usbmuxd's 512",
     "131072 bytes, over a 2048-byte buffer. The advertisement exists "
     "to prevent exactly the overflow it would then be inviting -- and "
     "this is the ONE place this driver deliberately differs from the "
     "implementation that works, so it is the one most likely to be "
     "'corrected' by somebody reading usbmuxd",
     """    *(uint16_t *)(th + MUX_TH_WIN)   = be16((uint16_t)MUX_WINDOW_UNITS);""",
     """    *(uint16_t *)(th + MUX_TH_WIN)   = be16(512);"""),

    ("the window is advertised in bytes, not 256-byte units",
     "the field is scaled by 256 in both directions. 1792 written raw "
     "claims 458752 bytes",
     """    *(uint16_t *)(th + MUX_TH_WIN)   = be16((uint16_t)MUX_WINDOW_UNITS);""",
     """    *(uint16_t *)(th + MUX_TH_WIN)   = be16((uint16_t)MUX_WINDOW);"""),

    ("a TCP checksum is computed",
     "there is no checksum over the mux -- usbmuxd memsets the header "
     "and never fills it, and a checksum here would be the only one in "
     "the conversation with nothing to check it",
     """    *(uint16_t *)(th + MUX_TH_SUM)   = 0;   /* the mux is reliable; see above */""",
     """    *(uint16_t *)(th + MUX_TH_SUM)   = be16(0x1234);"""),

    ("the peer's window is read without the shift",
     "a device offering 131072 bytes is reported as offering 512, and "
     "`usb lockdown` prints a device with half a kilobyte of window",
     """    g_m.peer_win = (uint32_t)be16(*(const uint16_t *)(th + MUX_TH_WIN)) << 8;""",
     """    g_m.peer_win = (uint32_t)be16(*(const uint16_t *)(th + MUX_TH_WIN));"""),

    # ── the handshake ─────────────────────────────────────────────────
    ("the SYN carries an ACK as well",
     "a host answering a handshake nobody started",
     """    r = mux_tcp_send(MUX_TH_SYN, 0, 0);""",
     """    r = mux_tcp_send(MUX_TH_SYN | MUX_TH_ACK_F, 0, 0);"""),

    ("our SYN does not consume a sequence number",
     "the completing ACK carries seq 0, which the device has already "
     "counted as the SYN",
     """    g_m.tx_tcp_seq = 1;             /* our SYN consumed one sequence number */""",
     """    g_m.tx_tcp_seq = 0;             /* our SYN consumed one sequence number */"""),

    ("the device's SYN is not acknowledged",
     "ack = their ISN rather than ISN + 1, so the device is told its "
     "own SYN never arrived",
     """    g_m.tx_tcp_ack = seq + 1;       /* ...and so did theirs */""",
     """    g_m.tx_tcp_ack = seq;           /* ...and so did theirs */"""),

    ("the acknowledgement is one rather than the ISN plus one",
     "usbmuxd's own arithmetic, which is right only because the "
     "device's ISN has always been 0. The injection is what usbmuxd "
     "does; the suite's nonzero-ISN case is what refutes it",
     """    g_m.tx_tcp_ack = seq + 1;       /* ...and so did theirs */""",
     """    g_m.tx_tcp_ack = 1;             /* ...and so did theirs */"""),

    ("the SYN/ACK flags are masked rather than compared",
     "usbmuxd compares the whole byte. A FIN riding along means the "
     "device is closing as it opens, and masking carries on into a "
     "connection that was never established",
     """    if (flags != (MUX_TH_SYN | MUX_TH_ACK_F)) {""",
     """    if (!(flags & MUX_TH_SYN)) {"""),

    ("the completing ACK is never sent",
     "the device stays half-open and never delivers anything",
     """    r = mux_tcp_send(MUX_TH_ACK_F, 0, 0);
    if (r < 0) {
        klog(KLOG_WARN, "usbmux: the final ACK would not send (%d)",
             (uint32_t)r);
        g_m.conn = 0;
        return r;
    }

    g_m.conn = 2;""",
     """    g_m.conn = 2;"""),

    ("an RST is not reported as a refusal",
     "'the port is closed' and 'the channel broke' send the next "
     "person to different places, and `usb lockdown` prints a "
     "different screen for each",
     """        g_m.conn    = 0;
        g_m.refused = 1;
        return USB_ESTALL;""",
     """        g_m.conn    = 0;
        return USB_ESTALL;"""),

    ("the refusal flag is not cleared on a new attempt",
     "a stale refusal makes `usb lockdown` report a closed port over a "
     "working connection",
     """    g_m.refused = 0;
    g_m.why     = 0;        /* a new connection owes nothing to the last */""",
     """    g_m.why     = 0;        /* a new connection owes nothing to the last */"""),

    ("connect does not run the version exchange first",
     "the SYN goes out on an eight-byte header, because the version "
     "that decides the header length has not been agreed",
     """    r = usbmux_handshake();
    if (r < 0)  return r;
    if (r == 0) return USB_ETIMEOUT;   /* the device never answered VERSION */""",
     """    ;"""),

    # ── NOT REACHABLE UNTIL 0.8.8, and recorded rather than deleted ──
    #
    # "the connection is marked up before the handshake completes" --
    # setting g_m.conn to 2 instead of 1 at the top of
    # usbmux_connect(). Reported MISSED.
    #
    # usbmux_send() and usbmux_recv() both require 2, so the 1 is what
    # makes them refuse while the handshake is in flight. But the
    # handshake is SYNCHRONOUS: nothing in this kernel can run between
    # the SYN and the ACK, so nothing can call them at that moment and
    # no suite can observe the difference.
    #
    # It becomes reachable at 0.8.8 "Helios", whose whole subject is
    # kernel_poll() running real work during exactly this kind of wait.
    # Add the injection back then, with a test that polls mid-handshake.
    #
    # The guard stays, and so does usbmux_connected() returning false
    # for it -- a connection reported up while it is half open is one
    # a caller writes into, and every write succeeds at the USB layer.
    #
    # Chasing this one found a comment that claimed more than the code
    # did: it said a SYN/ACK for an already-open connection would be
    # refused by a state machine below, and there is no such check.
    # Corrected in usb/usbmux.c. When two comments in one file
    # disagree, one of them is a bug -- and a comment disagreeing with
    # the code under it is the same thing.

    # ── the data path ─────────────────────────────────────────────────
    ("sending data does not advance the sequence number",
     "the device sees every packet claiming the same stream position "
     "and refuses the second one",
     """    g_m.tx_tcp_seq += (uint32_t)len;
    return USB_OK;""",
     """    return USB_OK;"""),

    ("the sequence advances per packet instead of per byte",
     "TCP numbers bytes. A packet count gets the first packet right "
     "and nothing after it",
     """    g_m.tx_tcp_seq += (uint32_t)len;""",
     """    g_m.tx_tcp_seq += 1;"""),

    ("data rides on a PSH as well as an ACK",
     "usbmuxd sends TH_ACK with a payload and never sets PSH, and the "
     "device compares the whole flags byte",
     """    r = mux_tcp_send(MUX_TH_ACK_F, data, len);
    if (r < 0) return r;""",
     """    r = mux_tcp_send(MUX_TH_ACK_F | MUX_TH_PSH, data, len);
    if (r < 0) return r;"""),

    ("received data is not acknowledged",
     "usbmuxd's own comment says the device likes it best when data is "
     "promptly ACKed. A receiver that does not stops receiving once "
     "its window is nominally full, and the symptom is a reply that "
     "lands once and then silence for ever. v0.8.9.2: this had gone "
     "STALE when the ack moved into mux_rx_accept(), and is anchored "
     "on the comment above it now so it cannot drift again",
     """       exactly like a device that has lost interest. */
    if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)""",
     """       exactly like a device that has lost interest. */
    if (0)"""),

    ("the acknowledgement does not count the bytes received",
     "the device is told its data never arrived and sends it again. "
     "v0.8.9.2: this injection had gone STALE -- it still named the "
     "`+=` line, which moved into mux_rx_accept() and became an "
     "assignment",
     """        g_m.tx_tcp_ack = seq + (uint32_t)r;""",
     """        g_m.tx_tcp_ack = seq;"""),

    ("v0.8.9.9: a packet bigger than the reader is refused again",
     "the 0.8.9.8 bug itself: net/tls.c reads a five-byte record "
     "header, the phone's whole handshake flight arrives in one packet, "
     "and the handshake dies at 'ClientHello sent' with no alert",
     """    if (r > max) {
        if (r - max > (int)sizeof g_m.pend) return USB_EPARAM; /* cannot */""",
     """    if (r > max) {
        return USB_EPARAM;"""),

    ("v0.8.9.9: the rest of a split packet is thrown away",
     "it was accepted and acked, so the device never sends it again "
     "and the TLS record it belonged to is short for ever",
     """        g_m.pend_len = r - max;""",
     """        g_m.pend_len = 0;"""),

    ("v0.8.9.9: a split packet is not acknowledged",
     "the device holds the whole packet as unacked, its window fills, "
     "and the conversation stops after one reply",
     """        g_m.split_reads++;
        if (mux_tcp_send(MUX_TH_ACK_F, 0, 0) < 0)""",
     """        g_m.split_reads++;
        if (0)"""),

    ("v0.8.9.9: a partly-served stash serves its first bytes again",
     "the stream repeats itself: a record header read twice and the "
     "TLS layer parses the middle of a certificate as a length",
     """        if (g_m.pend_len > 0)
            memmove(g_m.pend, g_m.pend + r, (uint32_t)g_m.pend_len);""",
     """        ;"""),

    ("v0.8.9.9: every connection uses the same source port",
     "the phone still holds the last connection on that port pair and "
     "answers the next SYN with RST -- 'The device REFUSED the port' on "
     "the HP, on the one port an iPhone always listens on",
     """(uint16_t)(MUX_SPORT + (g_sport_seq++ & 0xFFu));""",
     """(uint16_t)(MUX_SPORT);"""),

    ("a send longer than one packet is passed down unchecked",
     "mux_tcp_send() bounds the same thing and returns the same "
     "USB_EPARAM, so the return value cannot tell them apart -- the "
     "NAMED refusal is what stage 3's pair request will need to read",
     """    if (len > MUX_MAX_PAYLOAD) {
        klog(KLOG_WARN, "usbmux: %d bytes is more than one packet can "
             "carry (%d)", (uint32_t)len, (uint32_t)MUX_MAX_PAYLOAD);
        return USB_EPARAM;
    }""",
     """    ;"""),

    ("an RST on an open connection leaves it reported as up",
     "a caller keeps writing into it and every write succeeds at the "
     "USB layer",
     """        g_m.why = "the device reset the connection";
        klog(KLOG_WARN, "usbmux: the device reset the connection");
        g_m.conn = 0;""",
     """        g_m.why = "the device reset the connection";
        klog(KLOG_WARN, "usbmux: the device reset the connection");"""),

    ("flags other than a bare ACK are accepted on an open connection",
     "usbmuxd treats anything else as a reset; it has no half-close. "
     "v0.8.9.2: STALE since v0.8.8.10, which added the same check to "
     "mux_harvest() and gave this injection two matches -- anchored on "
     "the comment that belongs to usbmux_recv() now. v0.8.9.5: STALE "
     "AGAIN, because that comment was wrong -- a FIN is not a reset -- "
     "and splitting the FIN out took the comment with it. Anchored on "
     "the reason string instead, which names the branch it belongs to "
     "and is the thing a reader would have to change to move it",
     """        g_m.why = "a packet whose TCP flags are neither a bare ACK, a "
                  "FIN nor an RST";
        klog(KLOG_WARN, "usbmux: flags %2x on an established "
             "connection, expected %2x", (uint32_t)flags,
             (uint32_t)MUX_TH_ACK_F);
        g_m.conn = 0;
        return USB_EXFER;""",
     """        g_m.why = "a packet whose TCP flags are neither a bare ACK, a "
                  "FIN nor an RST";"""),

    ("a TCP packet shorter than its own header is parsed",
     "a twenty-byte header read out of an eighteen-byte packet reads "
     "the PREVIOUS packet's fields, which look entirely plausible",
     """    if (r < MUX_TCP_HDR) {""",
     """    if (0) {"""),

    ("the ports on an incoming packet are not checked",
     "a previous host's connection on the same device carries a "
     "different port pair, and its payload would be read as this "
     "conversation's reply",
     """    if (sport != g_m.sport || dport != g_m.dport) {""",
     """    if (0) {"""),

    ("the incoming ports are read the wrong way round",
     "the device's SOURCE port is our destination. Reading them "
     "straight refuses every packet of a working connection",
     """    dport = be16(*(const uint16_t *)(th + MUX_TH_SPORT));
    sport = be16(*(const uint16_t *)(th + MUX_TH_DPORT));""",
     """    sport = be16(*(const uint16_t *)(th + MUX_TH_SPORT));
    dport = be16(*(const uint16_t *)(th + MUX_TH_DPORT));"""),

    ("a bare ACK is reported as a failure",
     "a window update is not data and is not an error, and a caller "
     "polling for a reply must not read it as either",
     """    if (r == 0) return 0;                   /* a bare ACK is not data */""",
     """    if (r == 0) return USB_EXFER;"""),

    ("disconnect does not tell the device",
     "the device keeps the connection, and the next SYN on the same "
     "ports is a duplicate of one it thinks is open",
     """    if (g_m.conn) {
        /* usbmuxd closes with an RST rather than a FIN; there is no
           graceful shutdown in this protocol. Best effort: a channel
           that has already broken cannot be told politely. */
        mux_tcp_send(MUX_TH_RST, 0, 0);
    }""",
     """    ;"""),

    ("usbmux_close() says goodbye on a bus that is about to be rewalked",
     "`usb rescan` calls it before usb_enumerate(), and the device it "
     "would address may be the one that was just unplugged -- which is "
     "usually why the rescan is happening. Written through the PUBLIC "
     "entry point: the first version called mux_tcp_send() directly, "
     "which is declared further down the file, so it was 'caught' by "
     "the compiler and proved nothing about the suite",
     """void usbmux_close(void)
{""",
     """void usbmux_close(void)
{
    usbmux_disconnect();"""),

    # ── the owed SETUP, which this harness is the reason for ──────────
    ("a SETUP that never got out is never sent again",
     "v0.8.7.6's behaviour, found while working out why the VERSION "
     "tx_seq injection below could not be caught. The VERSION is "
     "answered, the version becomes 2 and the header becomes sixteen "
     "bytes -- and then the SETUP fails to send. The host believes the "
     "version is agreed; the device never got its setup byte. Nothing "
     "retried, and every later call returned 2 without noticing",
     """        if (g_m.version >= 2 && !g_m.setup_done) {
            klog(KLOG_WARN, "usbmux: the SETUP byte never got out -- "
                 "sending it again");
            r = mux_send_setup();
            if (r < 0) return r;
        }
        return g_m.version;""",
     """        return g_m.version;"""),

    ("the setup is marked done even when it did not send",
     "the other half: the flag set before the transfer is checked, so "
     "the retry above never fires",
     """    g_m.setup_done = 1;
    klog(KLOG_INFO, "usbmux: SETUP sent");""",
     """    klog(KLOG_INFO, "usbmux: SETUP sent");"""),

    ("the version exchange is repeated when the setup is owed",
     "a second VERSION packet would go out in the framing the FIRST "
     "exchange agreed, which no device expects -- the retry must send "
     "the setup byte only",
     """    if (g_m.version >= 1) {
        if (g_m.version >= 2 && !g_m.setup_done) {""",
     """    if (0) {
        if (g_m.version >= 2 && !g_m.setup_done) {"""),

    # ── STILL NOT REACHABLE, and for a DIFFERENT reason now ──────────
    #
    # "a packet longer than the buffer is built anyway" -- mux_send()'s
    # `total > MUX_BUF` check. v0.8.7.6 recorded it as unreachable
    # because nothing this file sent came within two thousand bytes of
    # the bound, and said it becomes reachable once lockdownd messages
    # exist.
    #
    # Lockdownd messages exist now, and it is STILL not reachable --
    # but the old reason is gone and a new one has taken its place,
    # which is worth writing down rather than leaving the note to look
    # as though nothing was checked.
    #
    # usbmux_send() refuses more than MUX_MAX_PAYLOAD, which is
    # MUX_BUF minus both headers, so by the time mux_send() runs the
    # total cannot exceed MUX_BUF. The guard is MASKED by the layer
    # above it. The suite sends exactly the bound -- total == MUX_BUF,
    # which `>` lets through correctly -- so no call can make the two
    # disagree.
    #
    # The guard stays. mux_send() is called by three paths and will be
    # called by stage 3's, and a bound enforced only by the caller is
    # one the next caller does not have. v0.8.6.2 refused this exact
    # reasoning about fat16_borrow() and was right to.
    #
    # Recorded, not deleted, following v0.8.6.3's precedent: an
    # injection deleted without a word is one somebody writes again.

    # ══════════════════════════════════════════════════════════════════
    # v0.8.9.5 -- A CLOSE REPORTED AS A FAULT
    #
    # Found on Kyan's HP. `pair` held a lockdownd connection open for
    # seventy-one seconds while it generated two RSA keys, lockdownd
    # closed it -- which is the ordinary thing for it to do -- and the
    # FIN came back to the caller as USB_EXFER, the code for CRC,
    # babble and buffer faults. The screen said the request "did not
    # get a reply (-3)" and the -3 pointed at the USB layer, which had
    # done nothing wrong.
    #
    # The suite missed it for the same reason it missed v0.8.9.2's
    # duplicate: tests/muxdev.inc COULD send a FIN -- queue_tcp() takes
    # any flags -- and no check had ever asked it to. The fixture was
    # not short of a feature. It was short of a question.
    # ══════════════════════════════════════════════════════════════════
    ("a FIN is reported as a transfer error",
     "the defect that shipped. FIN|ACK is 0x11, a bare ACK is 0x10, so "
     "equality with MUX_TH_ACK_F sends every close down the malformed-"
     "packet path. USB_EXFER means the bus corrupted something; a "
     "device hanging up politely is not that, and a caller told it is "
     "goes looking at the wrong layer",
     """    if (flags & MUX_TH_FIN) {
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
                  "FIN nor an RST";""",
     """    if (flags != MUX_TH_ACK_F) {
        g_m.why = "a packet whose TCP flags are neither a bare ACK, a "
                  "FIN nor an RST";"""),

    ("a FIN closes, but is reported with the wrong code",
     "the branch exists and sets the right sentence, and still returns "
     "the code that sends the reader to the bus. The sentence and the "
     "code have to agree or the one nobody reads is the one that is "
     "right",
     """             "closed the connection", (uint32_t)flags);
        g_m.conn = 0;
        return USB_ENODEV;
    }

    if (flags != MUX_TH_ACK_F) {""",
     """             "closed the connection", (uint32_t)flags);
        g_m.conn = 0;
        return USB_EXFER;
    }

    if (flags != MUX_TH_ACK_F) {"""),

    ("a FIN is named but the connection is left up",
     "usbmuxd has no half-close: a FIN ends the connection in both "
     "directions. Leaving conn == 2 means the next send goes out on a "
     "connection the device has finished with, which is exactly the "
     "3728-byte request that started this",
     """        klog(KLOG_WARN, "usbmux: the device sent FIN (flags %2x) -- it "
             "closed the connection", (uint32_t)flags);
        g_m.conn = 0;""",
     """        klog(KLOG_WARN, "usbmux: the device sent FIN (flags %2x) -- it "
             "closed the connection", (uint32_t)flags);"""),

    ("an RST and a FIN are given the same sentence",
     "two different things the device can do, one explanation. That is "
     "v0.8.7.4's defect -- six faults wearing one number -- in words "
     "rather than in a code, and it is no better for being prose",
     """        g_m.why = "the device reset the connection";
        klog(KLOG_WARN, "usbmux: the device reset the connection");""",
     """        g_m.why = "the device closed the connection (FIN)";
        klog(KLOG_WARN, "usbmux: the device reset the connection");"""),

    ("the reason survives into the next connection",
     "a fresh connect that keeps the last one's reason hands the next "
     "reader a sentence about a conversation that is over. A stale "
     "diagnostic is worse than none: it is confidently wrong",
     """    g_m.refused = 0;
    g_m.why     = 0;        /* a new connection owes nothing to the last */""",
     """    g_m.refused = 0;"""),

    ("usbmux_why() hands back a null pointer",
     "every caller of this is about to put it on a screen or into a "
     "format string. One that has to remember to test for null is one "
     "that will one day not, and the result is a fault inside the "
     "diagnostic that was meant to explain a fault",
     """    return g_m.why ? g_m.why : "the mux layer has not reported a reason";""",
     """    return g_m.why;"""),

    # ── v0.8.9.7: the phone's CONTROL messages ──────────────────────
    ("a CONTROL packet drops the connection again",
     "the defect that shipped: the phone explains in a sentence what it "
     "objected to, and the driver hangs up without reading it. On the HP "
     "this was 'a packet that was not TCP on an open connection' right "
     "after the pair request",
     """    if (proto == MUX_PROTO_CONTROL) {""",
     """    if (0) {"""),

    ("a phone's message reaches the console raw",
     "an escape sequence or a newline in the text goes straight onto the "
     "screen it is meant to explain",
     """        g_m.dev_msg[k++] = (c >= 32 && c < 127) ? (char)c : '.';""",
     """        g_m.dev_msg[k++] = (char)c;"""),

    ("a reconnect erases what the phone said",
     "the message sent as a connection died is the one worth reading "
     "after it, and `pair` reconnects before it reports",
     """    g_m.why     = 0;        /* a new connection owes nothing to the last */""",
     """    g_m.why     = 0;        /* a new connection owes nothing to the last */
    g_m.dev_msgs = 0;"""),

    ("no zero-length packet after a full-size transfer",
     "THE defect of 0.8.9.7: the pair request's first mux packet is 2048 "
     "bytes, 4 x 512, the phone keeps reading, glues the second packet "
     "onto it and refuses the result",
     """    if (g_m.mps_out && (total % (int)g_m.mps_out) == 0) {""",
     """    if (0) {"""),

    ("a zero-length packet after every transfer",
     "harmless-looking and wrong: a ZLP after a SHORT packet is an empty "
     "transfer of its own, which a device can take as a zero-length "
     "message",
     """    if (g_m.mps_out && (total % (int)g_m.mps_out) == 0) {""",
     """    if (g_m.mps_out) {"""),

    ("a failed ZLP is ignored",
     "the transfer it was meant to end is still open on the device, so "
     "the next packet is glued to it -- reporting success here is "
     "reporting the bug as a send",
     """            if (r == USB_ESTALL) usb_clear_halt(d, g_m.ep_out, 0);
            return r;
        }
        g_m.zlps++;""",
     """        }
        g_m.zlps++;"""),

    # ── AND ONE THAT CANNOT BE REACHED FROM A SUITE AT ALL ───────────
    #
    # "a non-TCP packet on an open connection is read as data" --
    # mux_tcp_recv()'s `proto != MUX_PROTO_TCP` check. The device in
    # tests/muxdev.inc only ever sends protocol 6 on an established
    # connection, because that is all a real one sends; teaching it to
    # send a stray VERSION would be modelling a device nobody has seen
    # in order to reach a guard.
    #
    # The guard stays, for the same reason as the one above: it is the
    # only thing standing between a control packet and a plist parser.
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """int usbmux_present(void) { return g_m.open; }""",
    """/* is a channel claimed right now */
int usbmux_present(void) { return g_m.open; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "usbmux", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        preamble="Proving tests/test_usbmux.c can fail.\n"
                 "The framing here is read off somebody else's "
                 "implementation, and the last\ntime that was true the "
                 "suite passed 110 checks over a driver that could not\n"
                 "move a byte."))
