#!/usr/bin/env python3
"""check_nettick_suite.py -- prove the network section of
tests/test_kpoll.c can fail.

net/net_tick.c is two small state machines and both are about not
doing something: not asking for an address that was already
negotiated, not asking through a NIC that has no carrier, not asking
while somebody is typing, and not saying the same thing on every poll.

A SUITE CAN PASS ALL OF THAT BY ACCIDENT, because "nothing happened"
is also what a client that never runs produces.  Half the injections
below therefore make the client do MORE rather than less, and the
suite has to notice a request that should not have been sent.

The costs here are specific:

  - dhcp_request() blocks for about four seconds and this runs from
    the keyboard's idle loop. A client that retries every two seconds
    instead of backing off leaves the shell unresponsive roughly 40%
    of the time on a machine with no DHCP server, for ever, and the
    user reads that as the OS being broken.
  - a warning printed on every interval buries the one line that
    matters. 0.8.7.8's subject: `dmesg | grep usbmux` is how every
    hardware question in this line has been answered.
  - the carrier client's remembered state is indexed by NIC number,
    and netdev_unregister() COMPACTS the table -- so index 1 after an
    unplug is a different NIC. That is v0.8.7's pinned-index bug in a
    new place, and the one failure in this file that would be silent.

Run:  python3 tools/check_nettick_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/net_tick.c"
TEST = "tests/test_kpoll.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("it asks again after the address was negotiated",
     "a DHCP request every two seconds for ever on a working machine, "
     "each one a four-second block in the keyboard's idle loop. The "
     "shell would be unusable and `ifconfig` would look fine",
     """    if(net_config_source() != NET_CFG_FALLBACK){ g_dhcp_done = 1; return; }""",
     """    if(net_config_source() == NET_CFG_DHCP) return;"""),

    ("it asks again over an address set by hand",
     "somebody typed `ifconfig set` for a reason. Overwriting a "
     "manual address from a timer is the one thing a network client "
     "must never do, and it would happen minutes after they set it",
     """    if(net_config_source() != NET_CFG_FALLBACK){ g_dhcp_done = 1; return; }""",
     """    if(net_config_source() == NET_CFG_DHCP){ g_dhcp_done = 1; return; }"""),

    ("there is no backoff -- it retries at the poll interval",
     "dhcp_request() blocks for about four seconds. At a two-second "
     "poll that is 40% of the time with the shell not answering, on "
     "any machine with no DHCP server, for as long as it is switched "
     "on",
     """    if((int32_t)(kpoll_ticks() - g_dhcp_next) < 0) return;""",
     """    /* no backoff */"""),

    ("the backoff interval never advances",
     "the same thing by the other route: the first wait is honoured "
     "and every wait after it is zero",
     """    g_dhcp_next = kpoll_ticks() + DHCP_BACKOFF_TICKS[g_dhcp_tries];""",
     """    g_dhcp_next = kpoll_ticks() + DHCP_BACKOFF_TICKS[0];"""),

    ("it never gives up",
     "five attempts is a judgement about a network that is not there. "
     "Without the cap, a machine on a cable with nothing on the other "
     "end blocks for four seconds every five minutes until it is "
     "switched off",
     """    if(g_dhcp_tries >= DHCP_MAX_TRIES){""",
     """    if(0){"""),

    ("the give-up warning is printed on every poll",
     "a line printed on every healthy pass buries the one that "
     "matters -- 0.8.7.8's lesson, and the reason both mux suites "
     "count KLOG_WARN separately now",
     """    if(g_dhcp_tries >= DHCP_MAX_TRIES){
        g_dhcp_done = 1;""",
     """    if(g_dhcp_tries >= DHCP_MAX_TRIES){"""),

    ("it asks through a NIC that reports no carrier",
     "four seconds of blocking to ask a network that is not there. "
     "v0.8.7.5 added this check to the BOOT path for the same reason "
     "and recorded why: 'no carrier' names the cause, 'no DHCP "
     "answer' only describes a symptom of it",
     """    if(!netdev_link_up(netdev_active())) return;""",
     """    /* carrier not checked */"""),

    ("it asks when there is no NIC at all",
     "netdev_active() is -1, and every send goes to index -1",
     """    if(netdev_active() < 0) return;          /* no NIC to ask through  */""",
     """    /* no NIC check */"""),

    ("a waiting keystroke does not defer the attempt",
     "it does not make the four-second freeze shorter; it makes it "
     "land when nobody is typing. Without it the freeze lands exactly "
     "when somebody has pressed a key, which is the only time they "
     "will notice",
     """    if(keyboard_haschar()){ g_dhcp_skipped_key++; return; }""",
     """    /* not deferred */"""),

    ("the first carrier reading is reported as a transition",
     "every NIC reports a carrier event at boot, and the one event "
     "that means something -- a hotspot being switched on -- is lost "
     "in them",
     """        if(!g_carrier_known[i]){
            g_carrier_known[i] = 1;
            g_carrier[i] = (uint8_t)up;
            continue;                 /* the first reading is not an event */
        }""",
     """        if(!g_carrier_known[i]) g_carrier_known[i] = 1;"""),

    ("the remembered carrier is not forgotten when the NIC count changes",
     "netdev_unregister() COMPACTS the table, so index 1 after an "
     "unplug is a DIFFERENT NIC. Remembering its predecessor's "
     "carrier reports a transition that never happened -- v0.8.7's "
     "pinned-index bug one layer along, and silent",
     """    if(n != g_carrier_ncount){
        for(int i = 0; i < NETDEV_MAX; i++) g_carrier_known[i] = 0;
        g_carrier_ncount = n;
    }""",
     """    g_carrier_ncount = n;"""),

    ("a link coming back does not restart the DHCP sequence",
     "an iPhone hotspot switched off and on hands out a different "
     "subnet, and the address held came from the old one. The symptom "
     "is `dns: resolution failed` over a link reporting itself up, "
     "which is 26 October on the HP exactly",
     """        if(up) net_dhcp_retry_reset();""",
     """        /* no reset */"""),

    ("the carrier state is written before the comparison",
     "the transition can never be seen: every reading equals the one "
     "just stored",
     """        if(g_carrier[i] == (uint8_t)up) continue;
        g_carrier[i] = (uint8_t)up;
        g_carrier_events++;""",
     """        g_carrier[i] = (uint8_t)up;
        if(g_carrier[i] == (uint8_t)up) continue;
        g_carrier_events++;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """uint32_t net_carrier_events(void){ return g_carrier_events; }""",
    """/* carrier transitions observed since boot */
uint32_t net_carrier_events(void){ return g_carrier_events; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "nettick", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        preamble="Proving the network section of tests/test_kpoll.c can "
                 "fail.\nBoth clients in net/net_tick.c are mostly about "
                 "NOT doing something, which a suite can pass by "
                 "accident."))
