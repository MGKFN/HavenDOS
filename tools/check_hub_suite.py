#!/usr/bin/env python3
"""check_hub_suite.py -- prove tests/test_hub.c can fail.

Hub support is in 0.8.6 because a USB stick goes in the socket on the
side of the laptop and on most laptops that socket is behind an internal
hub. The 20 October handoff named it as the first suspect if a stick did
not appear.

── WHY THIS HARNESS IS NOT OPTIONAL ──────────────────────────────────

Getting the route string wrong does not look like a bug. xHCI wants the
path to a device as four bits per tier, port numbers ONE-based, tier one
in bits 0..3. An off-by-one in the shift, or zero-based ports, produces a
PERFECTLY VALID route string pointing down a different socket -- so the
device does not answer, which is indistinguishable from an empty port.
There is no error to read, and QEMU is as happy with a wrong route as
with a right one.

The transaction translator is the same. A low or full speed device
behind a high speed hub needs the hub's slot and port in the slot
context or the controller issues full-speed traffic at high-speed timing
and the device never answers. Silence again.

So the defects below are planted in the two files that own this -- the
route arithmetic in usb/core.c and the port walk in usb/hub.c -- and the
suite has to notice. Half of them produce a route string that is simply
WRONG rather than absent, which is the kind of defect no amount of
booting it finds quickly.

Run:  python3 tools/check_hub_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

CORE = "usb/core.c"
HUB  = "usb/hub.c"
TEST = "tests/test_hub.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

# ── the route arithmetic, in usb/core.c ──────────────────────────────
CORE_BUGS = [
    ("the port is used zero-based in the route string",
     "a route of 0 means 'on a root port', so a device on port 1 of a "
     "hub becomes indistinguishable from one plugged straight into the "
     "machine -- and every other port is addressed one socket along",
     """    d.route       = parent.route |
                    (((uint32_t)(port + 1) & 0xFu) << (4u * parent.tier));""",
     """    d.route       = parent.route |
                    (((uint32_t)port & 0xFu) << (4u * parent.tier));"""),

    ("the route is shifted by the CHILD's tier, not the parent's",
     "off by one nibble: a tier-1 device lands in bits 4..7 and the "
     "controller looks for it down a path that does not exist. The "
     "result is a valid route string to nowhere",
     """                    (((uint32_t)(port + 1) & 0xFu) << (4u * parent.tier));""",
     """                    (((uint32_t)(port + 1) & 0xFu) << (4u * d.tier));"""),

    ("the parent's route is not carried",
     "every device below the first tier is addressed as though its hub "
     "were plugged into the root port. Tier 1 happens to still work, "
     "which is how this would ship",
     """    d.route       = parent.route |""",
     """    d.route       = 0 |"""),

    ("the route is shifted by 1 bit per tier instead of 4",
     "the route string is four bits per tier. Anything else overlaps "
     "the tiers into each other",
     """<< (4u * parent.tier));""",
     """<< (1u * parent.tier));"""),

    ("the root port comes from the device rather than the path",
     "the root port is the port the WHOLE PATH hangs off. Taking the "
     "hub port instead sends the controller down a different socket "
     "entirely",
     """    d.rootport    = parent.rootport;
    d.port        = parent.rootport;   /* what 0.8.5's screens print */""",
     """    d.rootport    = (uint8_t)port;
    d.port        = (uint8_t)port;"""),

    ("the tier is not incremented",
     "every device behind a hub claims tier 0, so the breadth loop in "
     "usb/hub.c rescans them for ever at tier 0 and the route for "
     "anything deeper is built from the wrong shift",
     """    d.route       = route;
    d.tier        = (uint8_t)(parent.tier + 1);""",
     """    d.route       = route;
    d.tier        = parent.tier;"""),

    ("the tier bound is gone",
     "a hub that reports itself as its own downstream device walks for "
     "ever, and the route string has only five nibbles to express it in",
     """    if (parent.tier + 1 >= USB_MAX_TIER) return USB_EPARAM;""",
     """    if (0) return USB_EPARAM;"""),

    ("a back-end with no attach_path is called anyway",
     "a null function pointer. The honest answer is to say so -- a "
     "controller that cannot address through a hub makes every device "
     "behind one invisible, and 'nothing appeared' is the least useful "
     "possible answer",
     """    if (!h->ops->attach_path) {""",
     """    if (0) {"""),

    ("the transaction translator is never set",
     "a low or full speed device behind a high speed hub gets "
     "full-speed traffic at high-speed timing and never answers, which "
     "looks exactly like an empty port",
     """    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {""",
     """    if (0) {"""),

    ("the translator is given to high-speed devices too",
     "a high-speed device needs no conversion, and a slot context with "
     "a translator on it is a device the controller addresses through "
     "one that is not there",
     """    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        if (parent.tt_slot) {""",
     """    if (1) {
        if (parent.tt_slot) {"""),

    ("the translator is not inherited from the parent",
     "it belongs to the highest-speed hub in the path, not the nearest "
     "one. A full-speed device two hubs down would point at the "
     "full-speed hub above it rather than at the hub actually "
     "translating",
     """        if (parent.tt_slot) {
            d.tt_slot = parent.tt_slot;
            d.tt_port = parent.tt_port;
        } else if (parent.speed == USB_SPEED_HIGH ||""",
     """        if (0) {
            d.tt_slot = parent.tt_slot;
            d.tt_port = parent.tt_port;
        } else if (parent.speed == USB_SPEED_HIGH ||"""),

    ("the translator port is zero-based",
     "the same one-based mistake as the route string, in the field that "
     "tells the controller which of the hub's ports is doing the "
     "conversion",
     """            d.tt_port = (uint8_t)(port + 1);""",
     """            d.tt_port = (uint8_t)port;"""),

    ("a hub index outside the table is accepted",
     "usb/hub.c passes an index it got from the table, so this is a "
     "bound on somebody else's arithmetic -- the kind that holds until "
     "the caller changes",
     """    if (hub < 0 || hub >= g_ndev) return USB_EPARAM;""",
     """    if (hub < 0) return USB_EPARAM;"""),

    ("the path is not restored after attach_path",
     "a back-end filling in its own numbers puts the device somewhere "
     "it is not, which is the same reason usb_enumerate() sets the "
     "controller index itself",
     """    d.hc          = parent.hc;
    d.rootport    = parent.rootport;
    d.port        = parent.rootport;
    d.route       = route;
    d.tier        = (uint8_t)(parent.tier + 1);
    d.parent      = (int8_t)hub;
    d.parent_port = (uint8_t)port;
    d.valid       = 1;""",
     """    d.valid       = 1;"""),

    ("the hub pass is not run after the root-port walk",
     "0.8.5 exactly: root ports only, and a stick in a side socket is "
     "invisible with nothing on screen to say why",
     """    usb_hub_scan();
""",
     """"""),

    ("a root-port device's path fields are left to memset",
     "usb_attach_behind() reads these off the PARENT to build a child's, "
     "so a field that is right only because memset ran is a field "
     "somebody will move",
     """            d.route       = 0;
            d.rootport    = (uint8_t)p;
            d.tier        = 0;
            d.parent      = -1;""",
     """            d.rootport    = (uint8_t)p;
            d.tier        = 0;"""),
]

# ── the port walk, in usb/hub.c ──────────────────────────────────────
HUB_BUGS = [
    ("ports are not powered before being looked at",
     "a bus-powered hub brings its ports up unpowered, so GET_STATUS "
     "reports nothing connected and the loop walks past a device that "
     "is there",
     """        port_set(h, p, FEAT_PORT_POWER);""",
     """        (void)p;"""),

    ("only the 2.0 hub descriptor is asked for",
     "a SuperSpeed hub answers 0x2A and STALLS 0x29, so every USB 3 hub "
     "reports zero ports -- which is indistinguishable from a hub with "
     "nothing plugged into it",
     """    static const uint8_t types[2] = { HUB_DESC_TYPE, HUB_DESC_TYPE_SS };""",
     """    static const uint8_t types[1] = { HUB_DESC_TYPE };"""),

    ("the port is not reset before the device is addressed",
     "a device coming out of a port that has not been reset is a device "
     "that has not been told to listen at address 0",
     """        r = port_reset_wait(h, p, &st);
        if (r != USB_OK) {""",
     """        r = USB_OK;
        if (0) {"""),

    ("the reset wait has no bound",
     "a hub whose status register never clears spins the kernel, and "
     "from the outside that is indistinguishable from a reset that did "
     "not work. kernel/machine.c exists because of this bug class",
     """    for (ms = 0; ms < 500; ms += 10) {""",
     """    for (ms = 0; ; ms += 10) {"""),

    ("a port that never enables is attached anyway",
     "the reset did not take, so the device is not listening and the "
     "attach talks to nothing",
     """    return (st & PORT_ENABLE) ? USB_OK : USB_ENODEV;""",
     """    return USB_OK;"""),

    ("the reset change bit is never cleared",
     "C_PORT_RESET stays set, so the next status read still reports the "
     "change and a second pass resets a working port",
     """    port_clear(h, port, FEAT_C_PORT_RESET);""",
     """    (void)port;"""),

    ("the device's recovery time is not waited out",
     "the specification gives a device 10 ms after a reset and says it "
     "need not answer before then. A device asked too early answers at "
     "neither address",
     """    sleep_ms(12);
    if (status) *status = st;""",
     """    if (status) *status = st;"""),

    ("an empty port is attached",
     "every port of every hub gets an attach, which on xHCI means an "
     "Enable Slot and an Address Device for a socket with nothing in it",
     """        if (!(st & PORT_CONNECTION)) continue;""",
     """        if (0) continue;"""),

    ("an over-current port is reset instead of reported",
     "the hub has switched that socket off because something on it is "
     "drawing too much. 'Nothing appeared' is a much worse answer than "
     "'the hub cut the power to that port'",
     """        if (st & PORT_OVER_CURRENT) {""",
     """        if (0) {"""),

    ("the speed is taken from the hub rather than its port",
     "a high-speed hub carries low, full and high speed devices on the "
     "same ports, and which one it is decides both the packet size and "
     "whether a transaction translator is needed",
     """        speed = port_speed_of(st);""",
     """        speed = h->speed;"""),

    ("low and full speed are swapped",
     "the two bits are the hub's own report. Addressing a low-speed "
     "device as full-speed gets the packet size wrong on the default "
     "pipe",
     """    if (status & PORT_LOW_SPEED)  return USB_SPEED_LOW;
    if (status & PORT_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;""",
     """    if (status & PORT_LOW_SPEED)  return USB_SPEED_FULL;
    if (status & PORT_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_LOW;"""),

    ("a hub is only recognised by its interface class",
     "0.8.5 found that a device can answer its device descriptor and "
     "not its configuration. A hub that did that would hide every disk "
     "behind it",
     """            if (d->if_class != HUB_CLASS && d->dev_class != HUB_CLASS)
                continue;""",
     """            if (d->if_class != HUB_CLASS)
                continue;"""),

    ("the walk is one tier deep",
     "a hub behind a hub is found and never scanned, so the stick on it "
     "is invisible. A monitor with a built-in hub plugged into a laptop "
     "with an internal one is two tiers",
     """    for (tier = 0; tier < USB_MAX_TIER; tier++) {""",
     """    for (tier = 0; tier < 1; tier++) {"""),

    ("the power loop and the status loop use different bounds",
     "only the first port gets powered, so a device on any other port "
     "of a bus-powered hub reports nothing connected and the walk goes "
     "straight past it. Two loops over the same ports is two chances "
     "for the bound to be wrong",
     """    for (p = 0; p < nports; p++) {
        /* Power the port before looking at it.""",
     """    for (p = 0; p < 1; p++) {
        /* Power the port before looking at it."""),

    ("the port count is not clamped",
     "bNbrPorts comes off the wire from somebody else's firmware. A hub "
     "claiming 127 ports is a loop 127 long over a 15-port array",
     """    if (nports > HUB_MAX_PORTS) nports = HUB_MAX_PORTS;""",
     """    (void)0;"""),

    ("the connection change bit is never cleared",
     "it stays set, so every later pass sees a change that has already "
     "been dealt with",
     """        if (ch & PORT_CONNECTION) port_clear(h, p, FEAT_C_PORT_CONNECTION);""",
     """        (void)ch;"""),

    # ── v0.9.2.2 ─────────────────────────────────────────────────────
    ("the controller is never told a hub is a hub (0.9.2.1 and before)",
     "Kyan's HP, 11 November: a keyboard behind a hub lit up and typed "
     "nothing. A low/full speed device behind a high speed hub needs "
     "split transactions, which xHCI runs only for a slot marked Hub",
     """    r0 = usb_hub_info(h, nports, ttt, h->dev_protocol == 2);""",
     """    r0 = USB_OK; (void)ttt;"""),

    ("the think time is read from the wrong bits",
     "wHubCharacteristics 6:5; bits 1:0 are power switching, and taking "
     "them gives every self-powered hub a think time it does not have",
     """            if (i == 0 && r >= 5) *think_time = (d[3] >> 5) & 3;""",
     """            if (i == 0 && r >= 5) *think_time = d[3] & 3;"""),
]

HARMLESS_CORE = (
    "a comment is added to usb/core.c (must not be caught)",
    "nothing about the code changed, so a catch here means the loop is "
    "restoring badly or the suite is checking text",
    """int usb_attach_behind(int hub, int port, uint8_t speed)""",
    """/* See include/usb.h for the contract. */
int usb_attach_behind(int hub, int port, uint8_t speed)""")

HARMLESS_HUB = (
    "a comment is added to usb/hub.c (must not be caught)",
    "nothing about the code changed, so a catch here means the loop is "
    "restoring badly or the suite is checking text",
    """int usb_hub_count(void)    { return g_hubs_seen; }""",
    """/* hubs seen by the last scan */
int usb_hub_count(void)    { return g_hubs_seen; }""")

if __name__ == "__main__":
    rc = inject.run(
        "hubcore", CORE, TEST, CC, CORE_BUGS, HARMLESS_CORE, timeout=60,
        files=(CORE, HUB, TEST),
        preamble="Part 1 of 2: the route arithmetic, in usb/core.c.\n"
                 "A wrong route string is a VALID address to the wrong "
                 "socket, so the symptom is silence.")
    print()
    rc2 = inject.run(
        "hubwalk", HUB, TEST, CC, HUB_BUGS, HARMLESS_HUB, timeout=60,
        files=(CORE, HUB, TEST),
        preamble="Part 2 of 2: the port walk, in usb/hub.c.")
    sys.exit(rc or rc2)
