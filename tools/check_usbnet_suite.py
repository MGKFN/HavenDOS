#!/usr/bin/env python3
"""check_usbnet_suite.py -- prove tests/test_usbnet.c can fail.

usb/ethernet.c is the first driver in this tree written against a
protocol NOBODY HERE HAS SEEN ON A WIRE. The descriptors are confirmed
-- `usb cfg` on the HP, 25 October, found class FF/FD/01 on interface 2
of configuration index 3, with its endpoints on alt 1 and alt 2 and none
on alt 0. The framing is not: the two bytes prepended to a received
frame, the fixed 1514-byte padded transmit and the two vendor control
requests are what the published Linux driver does.

That makes this suite's job unusual. It cannot show the guess is right.
What it has to do is pin the guess down tightly enough that the HP can
refute it in one screen -- so an injection that changes the framing in
any direction must be caught, because a suite that tolerates 1514
becoming 1516 is a suite that would not notice the day somebody
"corrected" it from a forum post.

Most of the defects below are things the shape of this driver invites:

  - "the frame length is sent instead of the buffer" is the obvious
    thing to write and is exactly what the device does not accept.
  - "the receive blocks" is v0.8.5's keyboard at one character per
    second, one endpoint type along: net_poll() runs from the shell's
    idle loop and nothing here runs on a timer.
  - "a short MAC is accepted" registers a NIC whose address is half the
    phone's and half zero, which net/arp.c then claims in a reply.
  - "the first transfer is recorded after stripping" destroys the one
    piece of evidence that can settle the framing question.
  - "the carrier check is not rate-limited" is a control transfer per
    screen redraw.

Run:  python3 tools/check_usbnet_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/ethernet.c"
TEST = "tests/test_usbnet.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-DNET_NO_HW", "-Iinclude",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ── the framing, which is the part that is a guess ────────────────
    ("the transmit sends the frame length instead of the buffer",
     "the obvious thing to write, and what the device does not accept. "
     "A 60-byte bulk OUT where a 1514-byte one was expected is a frame "
     "the phone drops without saying anything -- the counters go up and "
     "nothing comes back",
     """    r = usb_bulk(d, n->ep_out, 0, g_tx, IPHETH_TX_LEN, (int)n->mps_out);""",
     """    r = usb_bulk(d, n->ep_out, 0, g_tx, (int)len, (int)n->mps_out);"""),

    ("the transmit buffer is not zero-padded",
     "the tail of the previous frame is sent after this one. On a "
     "tethered link that is the previous packet's payload leaking into "
     "the padding of every short frame",
     """    if (len < IPHETH_TX_LEN) memset(g_tx + len, 0, IPHETH_TX_LEN - len);
""",
     """"""),

    ("the receive strips no bytes",
     "the two bytes the device prepends become the first two bytes of "
     "the destination MAC, so every frame is addressed to somebody "
     "else and the whole link looks dead",
     """    len = r - IPHETH_RX_SKIP;""",
     """    len = r;"""),

    ("the receive strips the wrong number of bytes",
     "off by one, which shifts every Ethernet header by a byte. The "
     "frames arrive, the counters rise, and nothing parses",
     """#define IPHETH_RX_SKIP  2""",
     """#define IPHETH_RX_SKIP  1"""),

    ("the receive buffer has no room for the prefix",
     "IPHETH_RX_LEN is the transmit length PLUS the prefix. Without "
     "the two extra bytes a full-length frame is read short and the "
     "last two bytes of every maximum-size packet are lost",
     """#define IPHETH_RX_LEN   (IPHETH_TX_LEN + IPHETH_RX_SKIP)""",
     """#define IPHETH_RX_LEN   IPHETH_TX_LEN"""),

    # ── blocking, which is the one that makes the machine unusable ────
    ("the receive BLOCKS",
     "net_poll() runs from the shell's idle loop and nothing in this "
     "kernel runs on a timer, so a bulk IN that waits out its timeout "
     "on every poll with no packet is a shell that stops answering. "
     "That is v0.8.5 shipping a keyboard at one character per second, "
     "one endpoint type along",
     """    r = usb_bulk_poll(d, n->ep_in, 1, g_rx, IPHETH_RX_LEN, (int)n->mps_in);""",
     """    r = usb_bulk(d, n->ep_in, 1, g_rx, IPHETH_RX_LEN, (int)n->mps_in);"""),

    # ── the evidence ──────────────────────────────────────────────────
    ("the first transfer is recorded AFTER the prefix is stripped",
     "and the one piece of evidence that can settle whether "
     "IPHETH_RX_SKIP is 2 is destroyed. `usb eth` would print a frame "
     "the driver had already decided about, which answers nothing",
     """    if (!n->have_first) {
        int k = r < (int)sizeof n->first ? r : (int)sizeof n->first;
        memcpy(n->first, g_rx, (uint32_t)k);
        n->first_len  = r;
        n->have_first = 1;
    }

    len = r - IPHETH_RX_SKIP;""",
     """    len = r - IPHETH_RX_SKIP;
    if (!n->have_first && len > 0) {
        int k = len < (int)sizeof n->first ? len : (int)sizeof n->first;
        memcpy(n->first, g_rx + IPHETH_RX_SKIP, (uint32_t)k);
        n->first_len  = len;
        n->have_first = 1;
    }"""),

    ("the recorded transfer is overwritten by every later one",
     "the FIRST transfer is the interesting one, because it is the one "
     "that arrives before anything in this kernel has had an opinion "
     "about the framing",
     """    if (!n->have_first) {
        int k = r < (int)sizeof n->first ? r : (int)sizeof n->first;""",
     """    if (1) {
        int k = r < (int)sizeof n->first ? r : (int)sizeof n->first;"""),

    # ── the MAC ───────────────────────────────────────────────────────
    ("a short MAC answer is accepted",
     "three bytes of address and three of whatever was in the buffer. "
     "net/netdev.c caches it at registration and net/arp.c puts it in "
     "a reply, so a NIC that lies about who it is poisons every ARP "
     "cache on the network it is attached to",
     """    if (r < 6) return r < 0 ? r : USB_EXFER;""",
     """    if (r < 0) return r;"""),

    ("a NIC with no address is registered anyway",
     "the exact failure net/netdev.c's cached MAC exists to stop, put "
     "back one layer up: an entry in the table whose address is all "
     "zeroes",
     """    r = get_mac(d, n);
    if (r < 0) {""",
     """    r = get_mac(d, n);
    if (0) {"""),

    ("the MAC request carries the wrong wIndex",
     "both vendor requests go to wIndex 2. A real phone stalls the "
     "wrong one, and a stall here is indistinguishable from a phone "
     "that has stopped answering",
     """#define IPHETH_CTRL_INDEX       2""",
     """#define IPHETH_CTRL_INDEX       0"""),

    ("the vendor request type is not an IN",
     "0xC0 is IN, vendor, device. Anything else sends the request in "
     "the wrong direction and the six bytes are never read",
     """#define IPHETH_CTRL_TYPE_IN     0xC0""",
     """#define IPHETH_CTRL_TYPE_IN     0x40"""),

    # ── carrier ───────────────────────────────────────────────────────
    ("the carrier check is not rate-limited",
     "a CONTROL TRANSFER per call, and net_ready() can be asked on any "
     "screen redraw. The failure is not slowness alone: a phone "
     "answering control requests as fast as the screen refreshes has "
     "less bus left for frames",
     """    if (n->carrier_asked && (now - n->carrier_ms) < IPHETH_CARRIER_MS)
        return n->carrier;
""",
     """"""),

    ("the first carrier check is skipped",
     "`carrier_asked` starting clear is what makes the first call ask. "
     "Without it a working link reports DOWN until a second has passed "
     "-- the worst possible first impression of a tethered phone",
     """    if (n->carrier_asked && (now - n->carrier_ms) < IPHETH_CARRIER_MS)""",
     """    if ((now - n->carrier_ms) < IPHETH_CARRIER_MS)"""),

    ("the carrier byte is treated as a flag",
     "IPHETH_CARRIER_ON is one VALUE, 0x04, not a bit to mask. A phone "
     "answering 0x05 is not a phone with a carrier",
     """    n->carrier = (g_ctl[0] == IPHETH_CARRIER_ON) ? 1 : 0;""",
     """    n->carrier = (g_ctl[0] & IPHETH_CARRIER_ON) ? 1 : 0;"""),

    ("a refused carrier check reports the link DOWN",
     "an invention. One failed control transfer would drop a working "
     "link, and a device that has really gone away is noticed by the "
     "NIC layer's own index check",
     """    if (r < 0) return n->carrier;
    n->carrier = (g_ctl[0] == IPHETH_CARRIER_ON) ? 1 : 0;""",
     """    if (r < 0) return (n->carrier = 0);
    n->carrier = (g_ctl[0] == IPHETH_CARRIER_ON) ? 1 : 0;"""),

    # ── bringing it up ────────────────────────────────────────────────
    # IPHETH_ALT is in include/usbnet.h, and inject.run() damages one
    # file. The USE SITE is here, which is the thing worth injecting
    # anyway: a constant that is right and a call that ignores it is the
    # defect, not the constant.
    ("the interface is claimed at alternate setting 0",
     "which HAS NO ENDPOINTS. That is the detail the whole release "
     "turns on, and this is what the driver would look like if "
     "alternate-setting support had not been written",
     # v0.8.9.9: re-anchored -- the driver claims the interface now,
     # beside usbmux, instead of selecting the configuration over it.
     """    r = usb_claim_interface(d, (uint8_t)cfg, s.ifnum, IPHETH_ALT);""",
     """    r = usb_claim_interface(d, (uint8_t)cfg, s.ifnum, 0);"""),

    ("v0.8.9.9: the driver finds the FIRST bulk pair, not its own",
     "with usbmux claimed beside it, the first bulk pair on the phone "
     "is the mux's -- frames would go down the pairing channel",
     """    in  = usb_find_ep_if(d, s.ifnum, USB_EP_BULK, 1);""",
     """    in  = usb_find_ep(d, USB_EP_BULK, 1);"""),

    ("only the first configuration is surveyed",
     "the phone boots into index 0, which is PTP. Tethering is in "
     "index 3 and nothing in index 0 says so",
     """    for (ci = 0; ci < (int)d->nconfigs; ci++) {""",
     """    for (ci = 0; ci < 1; ci++) {"""),

    ("the survey matches on the class alone",
     "0xFF is 'vendor specific' and says nothing. Interface 1 of the "
     "same configuration is ALSO 0xFF -- it is usbmux, the pairing "
     "channel -- so matching on the class alone claims the wrong "
     "interface and switches an alternate setting on it",
     """    if (cls == IPHETH_IF_CLASS && sub == IPHETH_IF_SUBCLASS &&
        proto == IPHETH_IF_PROTOCOL) {""",
     """    if (cls == IPHETH_IF_CLASS) {"""),

    ("the endpoint directions are swapped",
     "frames are written to the IN endpoint and read from the OUT one. "
     "Both transfers fail, and on xHCI they fail by being retried",
     """    n->ep_in   = in->addr;
    n->ep_out  = out->addr;""",
     """    n->ep_in   = out->addr;
    n->ep_out  = in->addr;"""),

    ("an interface with only one bulk endpoint is accepted",
     "a NIC that can send and not receive, or the reverse, registered "
     "as though it were whole",
     """    if (!in || !out) {""",
     """    if (!in && !out) {"""),

    ("a device already brought up is brought up again",
     "`usb rescan` calls the probe again, and switching an alternate "
     "setting on an interface that is working is a link that drops "
     "every time somebody looks at the bus",
     """        for (k = 0; k < USBNET_MAX; k++)
            if (g_n[k].used && g_n[k].dev == i) break;
        if (k < USBNET_MAX) continue;
""",
     """"""),

    ("a slot is left used when the NIC layer refuses it",
     "an entry in this driver's table that nothing dispatches to, and "
     "a count that disagrees with netdev_count(). Two tables "
     "disagreeing is the failure this release keeps running into",
     """            memset(&g_n[k], 0, sizeof g_n[k]);
            continue;""",
     """            continue;"""),

    ("release does not unregister from the NIC layer",
     "`usb rescan` releases before re-enumerating because each NIC "
     "holds an INDEX into the USB device table. A netdev entry left "
     "behind points at a slot this file has cleared, which is a "
     "dispatch through a zeroed ops table",
     """    for (k = netdev_count() - 1; k >= 0; k--)
        if (netdev_kind(k) == NET_BUS_USBNET) netdev_unregister(k);
""",
     """"""),

    # ── NOT INDEPENDENTLY REACHABLE, and recorded rather than deleted
    #
    # "usbnet_release() clears this driver's table before unregistering
    # from the NIC layer" was written as an injection and reported
    # MISSED. Read properly: netdev_unregister() does not call into the
    # driver's ops, and unit_of() refuses a slot whose `used` is clear,
    # so either guard alone is enough and swapping the order changes
    # nothing observable.
    #
    # The order is still the right way round -- it is what would matter
    # the day netdev_unregister() grows a callback, and the comment in
    # usbnet_release() says so. But it is defence in depth behind
    # unit_of(), which is the same "only fails together" shape as
    # v0.8.6.7's pair of bugs and the four properties
    # tests/test_netdev.c had to read the table directly to cover.
    #
    # Left as a note, following v0.8.6.3's precedent of recording the
    # two of its ten injections that were not defects: an injection
    # deleted without a word is one somebody writes again.

    # ── bounds ────────────────────────────────────────────────────────
    ("a frame longer than the transmit buffer is accepted",
     "a memcpy past the end of g_tx, which is the next static in the "
     "file. Truncating would be bad enough -- the far end would see a "
     "short frame with a valid header -- and this is worse",
     """    if (len == 0 || len > IPHETH_TX_LEN) { n->tx_errs++; return -1; }""",
     """    if (len == 0) { n->tx_errs++; return -1; }"""),

    ("a transfer of two bytes or fewer is treated as a frame",
     "r - 2 is zero or negative, and a negative length reaches memcpy "
     "as a very large unsigned one",
     """    if (len <= 0) { n->rx_runts++; return 0; }""",
     """    if (len < 0) { n->rx_runts++; return 0; }"""),

    ("the receive does not clamp to the caller's buffer",
     "the caller's maxlen is the only thing this driver knows about "
     "the buffer it was handed",
     """    if (len > (int)maxlen) len = (int)maxlen;""",
     """    ;"""),

    ("a stalled endpoint is never cleared",
     "usb/msc.c's recovery discipline, which the handoff names as the "
     "template for this driver: a stall that is not cleared makes every "
     "transfer after the first failure fail too",
     """        if (r == USB_ESTALL) usb_clear_halt(d, n->ep_out, 0);
""",
     """"""),
]

# Must NOT be caught. A comment reworded: no behaviour changes, so a
# catch here means the loop is restoring badly or the suite is checking
# text. See tools/inject.py.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """int usbnet_count(void) { return g_nn; }""",
    """/* how many adapters this driver currently holds */
int usbnet_count(void) { return g_nn; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "usbnet", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        preamble="Proving tests/test_usbnet.c can fail.\n"
                 "This is the first driver here written against a "
                 "protocol nobody in this tree\nhas seen on a wire, and "
                 "the suite passed 95 of 95 first time."))
