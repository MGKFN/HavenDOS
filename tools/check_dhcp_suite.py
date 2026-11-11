#!/usr/bin/env python3
"""check_dhcp_suite.py -- prove tests/test_dhcp.c can fail.

DHCP had never worked in this tree. Three defects, in three files, each
enough on its own: the broadcast ARPed for, the 512-byte UDP limit, and
the 548-byte minimum reply. None was caught because there was no suite,
and the fallback address is QEMU's own, so a machine that never got a
lease behaved exactly like one that did. These injections put each of
them back, plus the ones that would bring the iPhone link's failure back
by a different road.

Run:  python3 tools/check_dhcp_suite.py

NEVER in a background process. This patches the tree and restores it
in a `finally`.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/dhcp.c"
TEST = "tests/test_dhcp.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    # ── the three that kept DHCP from ever working ──────────────────
    ("the broadcast address is ARPed for again",
     "THE first defect: nobody owns 255.255.255.255, nobody answers, "
     "and the DISCOVER never reaches the wire",
     """    if(dst_ip == 0xFFFFFFFFu ||""",
     """    if(0 ||""", "net/ipv4.c"),

    ("udp_send() refuses more than 512 bytes again",
     "a DHCP message is 548; the DISCOVER is built and silently not sent",
     """    if(len > UDP_MAX_PAYLOAD) return -1;""",
     """    if(len > 512) return -1;""", "net/udp.c"),

    ("a reply must be the size of the buffer this file builds",
     "548 bytes, which no real server sends: BOOTP's minimum is 300 "
     "and that is what an answer looks like",
     """    if(len < DHCP_MIN_REPLY) return 0;""",
     """    if(len < sizeof(dhcp_pkt_t)) return 0;"""),

    # ── and the roads back to the iPhone failure ────────────────────
    ("any datagram may overwrite the one being waited for",
     "an OFFER followed by a broadcast in the same poll is gone before "
     "it is read",
     """    if(udp_want && ntohs(hdr->dst_port) != udp_want){""",
     """    if(0){""", "net/udp.c"),

    ("DHCP stops holding the slot for port 68",
     "the filter exists and nobody asks for it",
     """    udp_want_port(DHCP_CLIENT_PORT);

    for(int attempt""",
     """    for(int attempt"""),

    ("a unicast reply before there is an address is not ours",
     "a server may ignore the broadcast flag and send the OFFER to the "
     "address it is offering, which this machine does not have yet",
     """    if(hdr->dst != my_ip && hdr->dst != 0xFFFFFFFFu && !dhcp_reply){""",
     """    if(hdr->dst != my_ip && hdr->dst != 0xFFFFFFFFu){""", "net/ipv4.c"),

    # NOT here: "the message type is not checked". It is checked twice --
    # wait_reply() returns only the type asked for, and dhcp_request()
    # checks `t != DHCPACK` again -- so taking out either one alone
    # changes nothing a suite can see. An injection that cannot express
    # a reachable defect reports a catch nobody earned (0.8.6's rule).
    # tests/test_dhcp.c's "an OFFER sent again is not an ACK" is the
    # check that stands behind both.

    ("a NAK is ignored",
     "the server refuses the address and the machine waits out the "
     "timeout instead of starting again",
     """        if(t == DHCPNAK){ g_dhcp_naks++; continue; }""",
     """        if(t == DHCPNAK){ continue; }"""),

    ("the hardware address is not checked",
     "on a link with two clients a reply for the other one is taken",
     """    if(memcmp(p->chaddr, mac, 6) != 0) return 0;""",
     """    (void)mac;"""),

    ("an ACK without a router falls back to the OLD gateway",
     "the old gateway belongs to the old network -- 10.0.2.2 on an "
     "iPhone's 172.20.10.x",
     """        if(!g2) g2 = gw ? gw : save_gw;""",
     """        if(!g2) g2 = save_gw;"""),

    ("the subnet's own broadcast is ARPed for",
     "same mistake as the limited broadcast, one address over",
     """        (dst_ip & mask) == (my_ip & mask) && (dst_ip | mask) == 0xFFFFFFFFu)){""",
     """        0)){""", "net/ipv4.c"),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """/* Wait up to `ms` for a reply of type `want` (or a NAK). Returns the
   type that arrived, or 0. */""",
    """/* Wait at most `ms` for a reply of type `want`, or a NAK. Returns
   the type that arrived, or 0. */""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "dhcp", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_dhcp.c can fail.\n"
                 "DHCP never worked before 0.8.9.8; the first three "
                 "injections are why."))
