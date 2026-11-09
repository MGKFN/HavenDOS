#!/usr/bin/env python3
"""check_netdev_suite.py -- prove tests/test_netdev.c can fail.

net/netdev.c is an array and some function pointers, and
tests/test_netdev.c passed 97 of 97 on its first run. By this tree's own
rule that is the least trustworthy result available, and 0.8.6's three
new suites all did the same before their harnesses found TEN holes in
them -- all one shape: the suite checked the OUTCOME where it had to
check the WIRE.

Most of the defects below are not hypothetical. Each is a mistake the
three switches in net/net.c actually made, or one the shape of the code
invites:

  - "the MAC is read on every ask" is the reason it is cached at all. On
    a PCI NIC it costs a register read; on a USB NIC it is a CONTROL
    TRANSFER, and net/arp.c asks twice per reply.
  - "an unknown kind reports a zero MAC" is net_get_mac()'s old `else
    for(i..) mac[i]=0` reached by forgetting one of four edits: the
    frames go out, the counters go up, nothing comes back, and nothing
    on any screen can say why.
  - "the bus word comes from index 0" is the v0.8.3.1 bug verbatim, one
    layer along: the picker said NVMe and the confirmation screen said
    VirtIO.
  - "unplugging does not move the pin" is the one failure in this file
    that would be silent. A person who chose NIC 1 goes on sending
    through what is now a different NIC.
  - "the preference puts USB first" means a machine with a real NIC
    changes its mind about which one it is when somebody plugs a phone
    in to charge.

Run:  python3 tools/check_netdev_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "net/netdev.c"
TEST = "tests/test_netdev.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-Iinclude", "-DNET_NO_HW",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("the MAC is read on every ask instead of once",
     "the whole reason it is cached. On a USB NIC get_mac() is a CONTROL "
     "TRANSFER and net/arp.c asks twice per reply, so this is a control "
     "transfer per ARP reply and an address that can arrive mid-transfer",
     """void netdev_mac(int i, uint8_t mac[6])
{
    int k;
    if (!mac) return;
    if (i < 0 || i >= g_nnic) { for (k = 0; k < 6; k++) mac[k] = 0; return; }
    for (k = 0; k < 6; k++) mac[k] = g_nic[i].mac[k];
}""",
     """void netdev_mac(int i, uint8_t mac[6])
{
    int k;
    if (!mac) return;
    if (i < 0 || i >= g_nnic) { for (k = 0; k < 6; k++) mac[k] = 0; return; }
    if (g_nic[i].ops->get_mac) g_nic[i].ops->get_mac(g_nic[i].unit, mac);
    else for (k = 0; k < 6; k++) mac[k] = g_nic[i].mac[k];
}"""),

    ("an index that is not a NIC leaves the caller's buffer alone",
     "net_get_mac()'s contract is that it always fills six bytes. A "
     "caller that passes a stale buffer and a bad index then puts "
     "whatever was in it into an ARP reply",
     """    if (i < 0 || i >= g_nnic) { for (k = 0; k < 6; k++) mac[k] = 0; return; }""",
     """    if (i < 0 || i >= g_nnic) return;"""),

    ("a NIC with no get_mac is left with uninitialised bytes",
     "the explicit zeroing before the optional call is what makes a NIC "
     "that cannot answer report zeroes rather than whatever was in the "
     "table slot the last NIC vacated",
     """    memset(g_nic[i].mac, 0, 6);
    if (ops->get_mac) ops->get_mac(unit, g_nic[i].mac);""",
     """    if (ops->get_mac) ops->get_mac(unit, g_nic[i].mac);"""),

    ("the bus word comes from index 0 rather than the NIC",
     "the v0.8.3.1 bug verbatim, one layer along: every NIC reports the "
     "first one's name, on the screen whose only job is to say which "
     "adapter the stack is using",
     """    return g_nic[i].ops->name;
}

/* ════""",
     """    return g_nic[0].ops->name;
}

/* ════"""),

    ("an out-of-range index invents a NIC",
     "fs/blkdev.c's own note records what this looked like for disks: a "
     "welcome screen printing 'No disks found' directly above '1 VirtIO "
     "disk found'",
     """    if (i < 0 || i >= g_nnic) return "none";""",
     """    if (i < 0 || i >= g_nnic) return "e1000";"""),

    ("a send to a NIC with no send function is dispatched anyway",
     "a receive-only NIC's ops->send is a null pointer, and the check "
     "above it is the only thing between this layer and a call through "
     "zero",
     """    if (!g_nic[i].ops->send)           return -1;   /* receive-only */
""",
     """"""),

    ("THE WIRE: a zero-length send reaches the driver",
     "returns -1 either way, because the hardware refuses an empty frame "
     "too -- so the return value cannot tell this layer's refusal from "
     "the device's. This is the exact shape of the ten holes 0.8.6's "
     "harnesses found",
     """    if (len == 0)                      return -1;
""",
     """"""),

    ("a null frame reaches the driver",
     "the same shape again: a driver asked to send from address zero",
     """    if (i < 0 || i >= g_nnic || !data) return -1;""",
     """    if (i < 0 || i >= g_nnic) return -1;"""),

    ("a null receive buffer reaches the driver",
     "the driver memcpys into it",
     """    if (i < 0 || i >= g_nnic || !buf || maxlen == 0) return 0;""",
     """    if (i < 0 || i >= g_nnic) return 0;"""),

    ("the unit handed to a driver is the table index",
     "the driver's own index and this layer's are different numbers. "
     "This is the renumbering that made the installer's picker and its "
     "confirmation screen name different disks",
     """    return g_nic[i].ops->send(g_nic[i].unit, data, len);""",
     """    return g_nic[i].ops->send(i, data, len);"""),

    ("netdev_unit reports the index rather than the driver's unit",
     "`net nic` then shows a column that happens to be right only while "
     "nothing has been unplugged",
     """    return (i >= 0 && i < g_nnic) ? g_nic[i].unit : -1;""",
     """    return (i >= 0 && i < g_nnic) ? i : -1;"""),

    ("a NIC that can neither send nor receive is registered",
     "an entry in the table that every later loop has to skip, and that "
     "netdev_preferred() will hand to the stack",
     """    if (!ops->send && !ops->recv) return -1;   /* neither way: not a NIC */
""",
     """"""),

    ("a NIC with no bus word is registered",
     "the word lives in the ops table, which is what lets "
     "check_nic_dispatch.py guard the dispatch rather than a comment. A "
     "null name is a null pointer every screen prints",
     """    if (!ops->name)            return -1;      /* the bus word lives here */
""",
     """"""),

    ("the table bound is off by one",
     "a four-entry table written at index 4: the next object in the file "
     "is whatever the linker put there",
     """    if (g_nnic >= NETDEV_MAX)  return -1;""",
     """    if (g_nnic > NETDEV_MAX)   return -1;"""),

    ("a NIC that cannot report link state is reported DOWN",
     "the safe default inverted. Every NIC in this tree before 0.8.7 "
     "lacks a link_up, so this takes the network away from every machine "
     "that works today -- a regression dressed as rigour",
     """    if (!g_nic[i].ops->link_up) return 1;""",
     """    if (!g_nic[i].ops->link_up) return 0;"""),

    ("an index that is not a NIC reports link UP",
     "a caller that polls link state to decide whether to send then "
     "sends through a NIC that is not there",
     """    if (i < 0 || i >= g_nnic)   return 0;
    if (!g_nic[i].ops->link_up) return 1;""",
     """    if (i < 0 || i >= g_nnic)   return 1;
    if (!g_nic[i].ops->link_up) return 1;"""),

    ("the preference puts the USB NIC first",
     "a machine with a real NIC changes its mind about which one it is "
     "when somebody plugs a phone in to charge",
     """        NET_BUS_E1000, NET_BUS_RTL8139, NET_BUS_USBNET""",
     """        NET_BUS_USBNET, NET_BUS_E1000, NET_BUS_RTL8139"""),

    ("the preference drops the e1000",
     "the shape of 'the boot preference drops NVMe' in the block layer: "
     "the NIC most machines actually have falls through to whatever "
     "registered first",
     """        NET_BUS_E1000, NET_BUS_RTL8139, NET_BUS_USBNET""",
     """        NET_BUS_RTL8139, NET_BUS_USBNET"""),

    ("an empty table reports NIC 0 as the preferred one",
     "-1 means there is no NIC; 0 means there is one, and every caller "
     "goes on to send through it",
     """    return g_nnic > 0 ? 0 : -1;""",
     """    return 0;"""),

    ("a kind the preference has never heard of is not a NIC at all",
     "a driver added without touching the preference array then works on "
     "every machine except the one where it is the only NIC",
     """    return g_nnic > 0 ? 0 : -1;
}""",
     """    return -1;
}"""),

    ("netdev_active trusts a stale pin",
     "the range check is what stops an index that was valid before "
     "something was unplugged from being used after",
     """    if (g_active < 0 || g_active >= g_nnic) return netdev_preferred();""",
     """    if (g_active < 0) return netdev_preferred();"""),

    ("a NIC arriving steals a pin a person set",
     "plugging in a second adapter silently moves the stack onto it",
     """    if (!g_pinned) g_active = netdev_preferred();
    return i;""",
     """    g_active = netdev_preferred();
    return i;"""),

    ("THE SILENT ONE: unplugging does not move the pin",
     "netdev_unregister() COMPACTS the table, so every index above the "
     "one removed moves down by one. A person who pinned NIC 2 goes on "
     "sending through index 2, which is now a different adapter -- and "
     "nothing on any screen can say so",
     """    if (g_pinned) {
        if (g_active == i)     g_pinned = 0;       /* theirs is gone */
        else if (g_active > i) g_active--;         /* theirs moved down */
    }""",
     """    if (g_pinned) {
        if (g_active == i)     g_pinned = 0;       /* theirs is gone */
    }"""),

    ("unplugging the pinned NIC keeps the pin",
     "the pin now names whichever NIC slid into that index, or an index "
     "past the end of the table",
     """        if (g_active == i)     g_pinned = 0;       /* theirs is gone */
        else if (g_active > i) g_active--;         /* theirs moved down */""",
     """        if (g_active > i) g_active--;             /* theirs moved down */"""),

    ("unplugging does not compact the table",
     "the removed NIC's ops pointer stays in the slot, so the index goes "
     "on dispatching to a driver whose hardware is not in the machine",
     """    for (k = i; k < g_nnic - 1; k++) g_nic[k] = g_nic[k + 1];
    memset(&g_nic[g_nnic - 1], 0, sizeof g_nic[0]);
    g_nnic--;""",
     """    g_nnic--;"""),

    ("the vacated slot is not cleared",
     "nothing reads past g_nnic today, which is what 'unreachable by "
     "construction' said about the three bus-dispatch switches 0.8.6 "
     "deleted",
     """    memset(&g_nic[g_nnic - 1], 0, sizeof g_nic[0]);
""",
     """"""),

    ("unplugging accepts an out-of-range index",
     "a memmove from past the end of the table, over the table",
     """    if (i < 0 || i >= g_nnic) return -1;

    for (k = i;""",
     """    if (i < 0) return -1;

    for (k = i;"""),

    ("netdev_reset leaves the active index behind",
     "the table is empty and the stack is pointed at NIC 0 of nothing",
     """    g_nnic   = 0;
    g_active = -1;
    g_pinned = 0;""",
     """    g_nnic   = 0;
    g_pinned = 0;"""),

    ("netdev_scan clears the table it is adding to",
     "a USB NIC registered when the bus was walked is thrown away by "
     "net_init() calling the PCI scan afterwards -- the tethered phone "
     "disappears at the moment the network comes up",
     """int netdev_scan(void) { return g_nnic; }""",
     """int netdev_scan(void) { netdev_reset(); return g_nnic; }"""),

    ("the USB helper registers on the wrong bus",
     "the preference and `net nic` both read the kind, so a USB adapter "
     "claiming to be an e1000 outranks the machine's own NIC",
     """    return netdev_register(ops, NET_BUS_USBNET, unit);""",
     """    return netdev_register(ops, NET_BUS_E1000, unit);"""),
]

# Must NOT be caught. A comment reworded: no behaviour changes, so a
# catch here means the loop is restoring badly or the suite is asserting
# something that is not behaviour. Both make every result above
# meaningless -- see tools/inject.py.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """int             netdev_count(void) { return g_nnic; }""",
    """/* how many NICs are registered right now */
int             netdev_count(void) { return g_nnic; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "netdev", SRC, TEST, CC, BUGS, HARMLESS, timeout=30,
        preamble="Proving tests/test_netdev.c can fail.\n"
                 "net/netdev.c decides which adapter every frame leaves "
                 "by, and the suite passed 97 of 97 first time."))
