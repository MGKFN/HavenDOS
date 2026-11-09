/* ════════════════════════════════════════════════════════════════════
   test_netdev.c -- one NIC layer, and the two orders it holds.

   net/netdev.c was written in v0.8.7 for the reason fs/blkdev.c was
   written in v0.8.6, found the same way: 0.8.6 closed by saying that
   the next bus would arrive in net/, that guarding the bus LABEL had
   not been enough, and that the thing which CHOOSES has to be one place
   a grep can see. net/ethernet.c turned out to be clean. net/net.c held
   three switches on one `static int nic_type` -- net_get_mac(),
   nic_send(), nic_recv() -- plus a probe order decided in a fourth
   place and the numbering itself written as a comment.

   What this suite is for is not the dispatch, which is four lines. It
   is the two ORDERS, the refusals, and one thing the block layer never
   had to think about:

     REGISTRATION ORDER is identity. It is what `net nic` prints and
     what `net nic <n>` takes.

     PREFERENCE is "which NIC should the stack use when nobody said",
     and it is a separate question with a separate function, because
     the block layer had to be taught that twice.

     A NIC CAN BE UNPLUGGED. A disk that vanishes mid-write is a
     problem 0.8.6 deliberately left alone; a USB Ethernet adapter
     being pulled out is the ordinary case 0.8.7 exists for, and
     netdev_unregister() COMPACTS the table. So an index a person
     pinned can come to mean a different NIC, and that is the one
     failure in here that would be silent.

   ── WHAT IS COUNTED, AND WHY IT IS NOT THE OUTCOME ──────────────────

   0.8.6's three new suites all passed first time and their injection
   harnesses found ten holes, with one consistent shape: the suite
   checked the OUTCOME where it had to check the WIRE. A write to a
   read-only stick fails either way, because the device refuses it too,
   so counting completed writes cannot tell the driver's refusal from
   the device's.

   The same trap is here exactly. netdev_send() to a receive-only NIC
   returns -1, and so does a send to a NIC whose hardware refuses. Only
   one of those must reach the driver. So every simulated NIC counts the
   CALLS IT RECEIVES, and the refusal tests assert the count did not
   move -- not that the return value was negative.
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

/* NET_NO_HW keeps the PCI drivers out: this suite links net/netdev.c
   for the table, the dispatch, the words and both orders, and has no
   hardware to call. Exactly what BLK_NO_HW does for fs/blkdev.c. */
#define NET_NO_HW 1
#include "../net/netdev.c"

/* ── the simulated NICs ───────────────────────────────────────────────
   Three units per kind, each counting what reaches it. A NIC that can
   REFUSE is the point: include/usb.h's note about a mock that returns
   success applies here unchanged, and a NIC that always accepts a frame
   cannot tell this layer's refusal from the hardware's. */
#define SIMS 6
static struct {
    int      sends, recvs, macs, links;
    int      send_ret;        /* what the hardware answers              */
    uint16_t recv_len;        /* bytes it has waiting                   */
    int      link;            /* what link_up() reports                 */
    uint8_t  mac[6];
    uint16_t last_len;        /* the length that actually reached it    */
    uint8_t  last[8];
} sim[SIMS];

static void sim_reset(void){
    memset(sim, 0, sizeof sim);
    for(int i = 0; i < SIMS; i++){
        sim[i].send_ret = 0;
        sim[i].link     = 1;
        for(int k = 0; k < 6; k++) sim[i].mac[k] = (uint8_t)(0x10 * (i + 1) + k);
    }
}

static int sim_send(int unit, const void *d, uint16_t n){
    sim[unit].sends++;
    sim[unit].last_len = n;
    for(int k = 0; k < 8 && k < n; k++) sim[unit].last[k] = ((const uint8_t*)d)[k];
    return sim[unit].send_ret;
}
static uint16_t sim_recv(int unit, void *b, uint16_t n){
    sim[unit].recvs++;
    uint16_t len = sim[unit].recv_len;
    if(len > n) len = n;
    memset(b, 0xA5, len);
    return len;
}
static void sim_mac(int unit, uint8_t mac[6]){
    sim[unit].macs++;
    for(int k = 0; k < 6; k++) mac[k] = sim[unit].mac[k];
}
static int sim_link(int unit){ sim[unit].links++; return sim[unit].link; }

/* Four ops tables, differing only in what they leave out -- because
   what netdev.c does with a MISSING function is most of its refusal
   logic, and a table with every slot filled exercises none of it. */
static const netdev_ops_t ops_full = {
    "SIM-full", sim_send, sim_recv, sim_mac, sim_link
};
static const netdev_ops_t ops_nolink = {       /* cannot report link    */
    "SIM-nolink", sim_send, sim_recv, sim_mac, 0
};
static const netdev_ops_t ops_rxonly = {       /* receive but not send  */
    "SIM-rxonly", 0, sim_recv, sim_mac, 0
};
static const netdev_ops_t ops_nomac = {        /* no address to give    */
    "SIM-nomac", sim_send, sim_recv, 0, 0
};
static const netdev_ops_t ops_dead = {         /* neither way: not a NIC */
    "SIM-dead", 0, 0, sim_mac, 0
};
static const netdev_ops_t ops_noname = {       /* no bus word            */
    0, sim_send, sim_recv, sim_mac, 0
};

int main(void)
{
    uint8_t mac[6], buf[64];

    printf("netdev: the NIC table, its two orders, and what it refuses\n");

    /* ════════════════════════════════════════════════════════════════
       1. Registration, and what is not a NIC
       ════════════════════════════════════════════════════════════════ */
    head("registration refuses what cannot carry a frame");
    sim_reset(); netdev_reset();
    ck("an empty table has no NICs", netdev_count() == 0, netdev_count());
    ck("and no active NIC", netdev_active() == -1, netdev_active());
    ck("a null ops table is refused",
       netdev_register(0, NET_BUS_E1000, 0) == -1, 0);
    ck("a NIC that can neither send nor receive is refused",
       netdev_register(&ops_dead, NET_BUS_E1000, 0) == -1, 0);
    ck("a NIC with no bus word is refused -- the word lives in the ops",
       netdev_register(&ops_noname, NET_BUS_E1000, 0) == -1, 0);
    ck("none of the three reached the table", netdev_count() == 0,
       netdev_count());

    head("a receive-only NIC is a NIC");
    sim_reset(); netdev_reset();
    ck("it registers", netdev_register(&ops_rxonly, NET_BUS_USBNET, 0) == 0, 0);
    ck("and is the active one, there being no other",
       netdev_active() == 0, netdev_active());

    head("the table fills and then refuses");
    sim_reset(); netdev_reset();
    for(int i = 0; i < NETDEV_MAX; i++)
        netdev_register(&ops_full, NET_BUS_E1000, i);
    ck("NETDEV_MAX NICs registered", netdev_count() == NETDEV_MAX,
       netdev_count());
    ck("the next is refused rather than overwriting one",
       netdev_register(&ops_full, NET_BUS_E1000, 0) == -1, 0);
    ck("and the table is unchanged", netdev_count() == NETDEV_MAX,
       netdev_count());

    /* ════════════════════════════════════════════════════════════════
       2. The cached MAC -- read once, at registration
       ════════════════════════════════════════════════════════════════

       Not an optimisation. On a USB NIC get_mac() is a CONTROL
       TRANSFER, and net/arp.c asks for the MAC twice per reply; the
       failure mode is not slowness but an answer that arrives
       mid-transfer and an ARP reply that lies about who we are. */
    head("the MAC is read once, at registration, and kept");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000, 2);
    ck("get_mac reached the driver exactly once", sim[2].macs == 1,
       sim[2].macs);
    netdev_mac(0, mac);
    netdev_mac(0, mac);
    netdev_mac(0, mac);
    ck("three later asks reach it not at all", sim[2].macs == 1,
       sim[2].macs);
    ck("and all three answer the address it gave",
       mac[0] == 0x30 && mac[5] == 0x35, mac[0]);

    head("a NIC with no address reports zeroes, not stale memory");
    sim_reset(); netdev_reset();
    memset(mac, 0xEE, 6);
    netdev_register(&ops_nomac, NET_BUS_E1000, 0);
    netdev_mac(0, mac);
    ck("six zero bytes", mac[0] == 0 && mac[3] == 0 && mac[5] == 0, mac[0]);

    head("an index that is not a NIC reports zeroes too");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000, 0);
    memset(mac, 0xEE, 6);
    netdev_mac(7, mac);
    ck("not the first NIC's address, and not 0xEE",
       mac[0] == 0 && mac[5] == 0, mac[0]);

    /* ════════════════════════════════════════════════════════════════
       3. Dispatch -- COUNTING THE WIRE, not the outcome
       ════════════════════════════════════════════════════════════════ */
    head("a frame reaches the NIC it was addressed to, and only it");
    sim_reset(); netdev_reset();
    /* The UNITS are deliberately not the indices. A NIC registered at
       index 1 with unit 1 cannot tell `ops->send(g_nic[i].unit, ...)`
       from `ops->send(i, ...)`, and the harness caught this suite
       unable to: the driver's own index and this layer's ARE different
       numbers, which is the renumbering that made the installer's
       picker and its confirmation screen name different disks. */
    netdev_register(&ops_full, NET_BUS_E1000,   4);   /* index 0, unit 4 */
    netdev_register(&ops_full, NET_BUS_RTL8139, 5);   /* index 1, unit 5 */
    netdev_register(&ops_full, NET_BUS_USBNET,  3);   /* index 2, unit 3 */
    {
        uint8_t frame[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        ck("send to NIC 1 succeeds", netdev_send(1, frame, 4) == 0, 0);
        ck("the driver was called with UNIT 5, not index 1",
           sim[5].sends == 1, sim[5].sends);
        ck("  ...and unit 1 was not called at all", sim[1].sends == 0,
           sim[1].sends);
        ck("NIC 0's driver saw none", sim[4].sends == 0, sim[4].sends);
        ck("NIC 2's driver saw none", sim[3].sends == 0, sim[3].sends);
        ck("the bytes arrived unaltered",
           sim[5].last[0] == 0xDE && sim[5].last[3] == 0xEF, sim[5].last[0]);
        ck("and the length did too", sim[5].last_len == 4, sim[5].last_len);

        ck("a receive is dispatched by unit too",
           (sim[3].recv_len = 20, netdev_recv(2, buf, sizeof buf) == 20), 0);
        ck("  ...reaching unit 3", sim[3].recvs == 1, sim[3].recvs);
        ck("  ...and not unit 2", sim[2].recvs == 0, sim[2].recvs);

        ck("and so is a MAC read, at registration",
           sim[4].macs == 1 && sim[5].macs == 1 && sim[3].macs == 1,
           sim[3].macs);
        netdev_mac(1, mac);
        ck("  ...giving unit 5's address, not index 1's",
           mac[0] == 0x60 && mac[5] == 0x65, mac[0]);
    }

    head("THE WIRE: a refusal by this layer must not reach a driver");
    sim_reset(); netdev_reset();
    netdev_register(&ops_rxonly, NET_BUS_USBNET, 0);   /* no send()      */
    netdev_register(&ops_full,   NET_BUS_E1000,  1);
    sim[1].send_ret = -1;                             /* hardware refuses */
    {
        uint8_t frame[4] = { 1, 2, 3, 4 };
        /* These two return the same value. Only one of them is allowed
           to have touched the hardware, and the return value cannot
           tell them apart -- which is the whole lesson of 0.8.6's ten
           suite holes. */
        ck("a send to a receive-only NIC returns -1",
           netdev_send(0, frame, 4) == -1, 0);
        ck("  ...and reached no driver at all", sim[0].sends == 0,
           sim[0].sends);
        ck("a send the HARDWARE refuses also returns -1",
           netdev_send(1, frame, 4) == -1, 0);
        ck("  ...and did reach the driver, which is the difference",
           sim[1].sends == 1, sim[1].sends);

        ck("a send with no buffer returns -1",
           netdev_send(1, 0, 4) == -1, 0);
        ck("  ...and did not reach the driver", sim[1].sends == 1,
           sim[1].sends);
        ck("a zero-length send returns -1",
           netdev_send(1, frame, 0) == -1, 0);
        ck("  ...and did not reach the driver", sim[1].sends == 1,
           sim[1].sends);
        ck("a send to an index that is not a NIC returns -1",
           netdev_send(9, frame, 4) == -1, 0);
        ck("a send to a negative index returns -1",
           netdev_send(-1, frame, 4) == -1, 0);
        ck("  ...and neither reached any driver",
           sim[0].sends == 0 && sim[1].sends == 1, sim[1].sends);
    }

    head("receive: nothing waiting is not an error");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000, 0);
    sim[0].recv_len = 0;
    ck("an empty NIC answers 0", netdev_recv(0, buf, sizeof buf) == 0, 0);
    ck("and the driver was asked", sim[0].recvs == 1, sim[0].recvs);
    sim[0].recv_len = 40;
    ck("a NIC with a frame answers its length",
       netdev_recv(0, buf, sizeof buf) == 40, 0);
    ck("a short buffer truncates rather than overruns",
       netdev_recv(0, buf, 8) == 8, 0);
    ck("a null buffer answers 0", netdev_recv(0, 0, 64) == 0, 0);
    ck("  ...without asking the driver", sim[0].recvs == 3, sim[0].recvs);
    ck("a zero-length buffer answers 0", netdev_recv(0, buf, 0) == 0, 0);
    ck("  ...without asking the driver", sim[0].recvs == 3, sim[0].recvs);
    ck("an index that is not a NIC answers 0",
       netdev_recv(5, buf, sizeof buf) == 0, 0);

    head("a receive from a send-only NIC is refused at this layer");
    sim_reset(); netdev_reset();
    netdev_register(&ops_dead, NET_BUS_E1000, 0);   /* refused outright */
    ck("it never registered, so there is nothing to ask",
       netdev_count() == 0, netdev_count());

    /* ════════════════════════════════════════════════════════════════
       4. Link state
       ════════════════════════════════════════════════════════════════ */
    head("a NIC that cannot report link state is up");
    sim_reset(); netdev_reset();
    netdev_register(&ops_nolink, NET_BUS_RTL8139, 0);
    ck("up, without a link_up function to ask", netdev_link_up(0) == 1, 0);
    ck("and nothing was called", sim[0].links == 0, sim[0].links);

    head("a NIC that CAN report it is believed, both ways");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_USBNET, 0);
    sim[0].link = 1;
    ck("link up is up", netdev_link_up(0) == 1, 0);
    sim[0].link = 0;
    ck("link down is down -- the first 0 this tree can produce",
       netdev_link_up(0) == 0, 0);
    ck("a non-zero that is not 1 is still up", (sim[0].link = 7,
       netdev_link_up(0) == 1), 0);
    ck("an index that is not a NIC is down, not up",
       netdev_link_up(4) == 0, 0);

    /* ════════════════════════════════════════════════════════════════
       5. The two orders
       ════════════════════════════════════════════════════════════════ */
    head("registration order is identity: index, unit and word");
    sim_reset(); netdev_reset();
    /* DIFFERENT ops tables, so the two bus words differ. Registering two
       NICs that share one name is how a `return g_nic[0].ops->name`
       passes for `g_nic[i]` -- the v0.8.3.1 bug verbatim, and the
       harness caught this suite doing exactly that. */
    netdev_register(&ops_full,   NET_BUS_USBNET, 3);
    netdev_register(&ops_nolink, NET_BUS_E1000,  1);
    ck("the first registered is NIC 0", netdev_kind(0) == NET_BUS_USBNET,
       netdev_kind(0));
    ck("the second is NIC 1", netdev_kind(1) == NET_BUS_E1000,
       netdev_kind(1));
    ck("each carries its own driver unit, not its index",
       netdev_unit(0) == 3 && netdev_unit(1) == 1, netdev_unit(0));
    ck("NIC 0 reports its OWN bus word",
       strcmp(netdev_bus_name(0), "SIM-full") == 0, 0);
    ck("and NIC 1 reports a DIFFERENT one, not the first NIC's",
       strcmp(netdev_bus_name(1), "SIM-nolink") == 0, 0);
    ck("an index that is not a NIC has no word, rather than a borrowed one",
       strcmp(netdev_bus_name(6), "none") == 0, 0);
    ck("and no kind", netdev_kind(6) == NET_BUS_NONE, netdev_kind(6));
    ck("and no unit", netdev_unit(6) == -1, netdev_unit(6));

    head("PREFERENCE is a different question: e1000, RTL8139, then USB");
    sim_reset(); netdev_reset();
    /* Registered in the OPPOSITE order to the preference, deliberately:
       a preference that happens to agree with registration order proves
       nothing, and the block layer's two orders agreeing by accident on
       the test machine is how they came to disagree in the field. */
    netdev_register(&ops_full, NET_BUS_USBNET,  0);
    netdev_register(&ops_full, NET_BUS_RTL8139, 1);
    netdev_register(&ops_full, NET_BUS_E1000,   2);
    ck("the e1000 is preferred though it registered last",
       netdev_preferred() == 2, netdev_preferred());
    ck("and is therefore active", netdev_active() == 2, netdev_active());
    netdev_reset(); sim_reset();
    netdev_register(&ops_full, NET_BUS_USBNET,  0);
    netdev_register(&ops_full, NET_BUS_RTL8139, 1);
    ck("with no e1000, the RTL8139 is preferred over the USB NIC",
       netdev_preferred() == 1, netdev_preferred());
    netdev_reset(); sim_reset();
    netdev_register(&ops_full, NET_BUS_USBNET, 0);
    ck("a USB NIC alone is preferred -- which is the HP",
       netdev_preferred() == 0, netdev_preferred());

    head("a kind the preference list has never heard of is still a NIC");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, 99, 0);
    ck("it is preferred, there being nothing else",
       netdev_preferred() == 0, netdev_preferred());
    ck("and active", netdev_active() == 0, netdev_active());
    netdev_register(&ops_full, NET_BUS_E1000, 1);
    ck("a known kind outranks it once one appears",
       netdev_preferred() == 1, netdev_preferred());

    /* ════════════════════════════════════════════════════════════════
       6. The pin, and what unplugging does to it
       ════════════════════════════════════════════════════════════════ */
    head("a person's choice outranks the preference");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,  0);
    netdev_register(&ops_full, NET_BUS_USBNET, 1);
    ck("the e1000 is active by preference", netdev_active() == 0,
       netdev_active());
    ck("pinning the USB NIC succeeds", netdev_set_active(1) == 0, 0);
    ck("and it is active", netdev_active() == 1, netdev_active());
    ck("pinning an index that is not a NIC is refused",
       netdev_set_active(4) == -1, 0);
    ck("and leaves the pin alone", netdev_active() == 1, netdev_active());
    ck("a NIC arriving does not steal the pin",
       (netdev_register(&ops_full, NET_BUS_E1000, 2),
        netdev_active() == 1), netdev_active());

    head("THE SILENT ONE: unplugging compacts the table, so a pin moves");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,   0);   /* index 0         */
    netdev_register(&ops_full, NET_BUS_RTL8139, 1);   /* index 1         */
    netdev_register(&ops_full, NET_BUS_USBNET,  2);   /* index 2, pinned */
    netdev_set_active(2);
    ck("the USB NIC is pinned at index 2", netdev_active() == 2,
       netdev_active());
    ck("unplugging NIC 0 succeeds", netdev_unregister(0) == 0, 0);
    ck("two NICs are left", netdev_count() == 2, netdev_count());
    ck("the pinned NIC FOLLOWED its unit down to index 1",
       netdev_active() == 1, netdev_active());
    ck("  ...and it is still the USB NIC, by unit",
       netdev_unit(netdev_active()) == 2, netdev_unit(netdev_active()));
    ck("  ...and by kind",
       netdev_kind(netdev_active()) == NET_BUS_USBNET,
       netdev_kind(netdev_active()));

    head("unplugging the pinned NIC falls back to the preference");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,   0);
    netdev_register(&ops_full, NET_BUS_USBNET,  1);
    netdev_set_active(1);
    ck("the USB NIC is pinned", netdev_active() == 1, netdev_active());
    netdev_unregister(1);
    ck("with it gone, the e1000 is active again", netdev_active() == 0,
       netdev_active());
    ck("and the pin is released, so a new NIC can be preferred",
       (netdev_register(&ops_full, NET_BUS_E1000, 5),
        netdev_active() == 0), netdev_active());

    /* ── the case above cannot distinguish releasing the pin from the
       belt-and-braces `g_active >= g_nnic` repair below it, because
       removing the LAST index leaves the pin out of range either way.
       The harness caught that: dropping `if (g_active == i) g_pinned =
       0;` was MISSED. So here the pinned NIC is removed from the
       BOTTOM, where a kept pin stays in range and names whichever NIC
       slid down into its index -- and the NIC that slides in is
       deliberately NOT the one the preference would choose. */
    head("  ...even when a kept pin would stay in range");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_USBNET,  0);   /* index 0, pinned */
    netdev_register(&ops_full, NET_BUS_USBNET,  1);   /* index 1          */
    netdev_register(&ops_full, NET_BUS_E1000,   2);   /* index 2, preferred */
    netdev_set_active(0);
    ck("the first USB NIC is pinned at index 0", netdev_active() == 0,
       netdev_active());
    netdev_unregister(0);
    ck("unplugging it releases the pin and the e1000 wins",
       netdev_kind(netdev_active()) == NET_BUS_E1000,
       netdev_kind(netdev_active()));
    ck("  ...which is index 1 after the compaction", netdev_active() == 1,
       netdev_active());
    ck("  ...and not the USB NIC that slid into index 0",
       netdev_unit(netdev_active()) == 2, netdev_unit(netdev_active()));

    head("unplugging a NIC ABOVE the pin leaves the pin where it is");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,   0);
    netdev_register(&ops_full, NET_BUS_RTL8139, 1);
    netdev_register(&ops_full, NET_BUS_USBNET,  2);
    netdev_set_active(0);
    netdev_unregister(2);
    ck("still index 0", netdev_active() == 0, netdev_active());
    ck("still the e1000", netdev_kind(0) == NET_BUS_E1000, netdev_kind(0));

    head("unplugging the last NIC leaves no network at all");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_USBNET, 0);
    netdev_unregister(0);
    ck("no NICs", netdev_count() == 0, netdev_count());
    ck("no active NIC", netdev_active() == -1, netdev_active());
    ck("a send goes nowhere", netdev_send(0, "x", 1) == -1, 0);
    ck("and reached no driver", sim[0].sends == 0, sim[0].sends);

    head("unplugging refuses an index that is not a NIC");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000, 0);
    ck("out of range is refused", netdev_unregister(3) == -1, 0);
    ck("negative is refused", netdev_unregister(-1) == -1, 0);
    ck("and the table is untouched", netdev_count() == 1, netdev_count());

    head("the slot a NIC left is cleared, not left pointing at its ops");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,   7);
    netdev_register(&ops_full, NET_BUS_RTL8139, 8);
    netdev_unregister(0);
    ck("one NIC left, and it is the second one", netdev_unit(0) == 8,
       netdev_unit(0));
    ck("the vacated index reports no unit", netdev_unit(1) == -1,
       netdev_unit(1));
    ck("and no bus word", strcmp(netdev_bus_name(1), "none") == 0, 0);
    ck("and a send to it reaches nothing",
       (netdev_send(1, "x", 1) == -1) && sim[7].sends == 0, sim[7].sends);

    /* ════════════════════════════════════════════════════════════════
       7. netdev_scan under NET_NO_HW, and netdev_register_usb
       ════════════════════════════════════════════════════════════════ */
    head("the USB registration helper puts a NIC on the right bus");
    sim_reset(); netdev_reset();
    ck("it registers", netdev_register_usb(&ops_full, 4) == 0, 0);
    ck("as a USB NIC", netdev_kind(0) == NET_BUS_USBNET, netdev_kind(0));
    ck("carrying its own unit", netdev_unit(0) == 4, netdev_unit(0));
    ck("and it refuses what netdev_register refuses",
       netdev_register_usb(&ops_dead, 5) == -1, 0);

    head("netdev_scan finds nothing when there is no hardware to find");
    sim_reset(); netdev_reset();
    ck("an empty machine scans to zero", netdev_scan() == 0, netdev_scan());
    netdev_register_usb(&ops_full, 0);
    ck("a scan does not clear what is already registered",
       netdev_scan() == 1, netdev_scan());

    /* ════════════════════════════════════════════════════════════════
       8. The table itself, read directly
       ════════════════════════════════════════════════════════════════

       Everything above goes through the public functions, which is how a
       suite should normally work. Four properties of this layer are
       deliberately NOT reachable that way, and tools/check_netdev_suite.py
       caught the suite missing all four:

         - netdev_reset() leaving g_active behind
         - the slot a NIC vacated keeping its ops pointer and its MAC
         - netdev_register() not zeroing a MAC before the optional read
         - netdev_active() trusting an index past the end of the table

       Each is masked by a guard somewhere else, so each injection was
       MISSED while the code was right -- the same shape as v0.8.6.7's
       pair of bugs that only failed together, and the same shape as the
       `fat16_borrow()` path v0.8.6.2 fixed with "no path in 0.8.6.1
       could reach it". "Unreachable by construction" is what the three
       bus-dispatch switches 0.8.6 deleted were also true by.

       This suite compiles net/netdev.c in, so it can read the table.
       These four checks do, and they say so. */
    head("the table, read directly: reset leaves NOTHING behind");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_USBNET, 0);
    netdev_set_active(0);
    netdev_reset();
    ck("g_nnic is zero", g_nnic == 0, g_nnic);
    ck("g_active is -1, not a stale index", g_active == -1, g_active);
    ck("g_pinned is clear", g_pinned == 0, g_pinned);
    ck("and slot 0 holds no ops pointer", g_nic[0].ops == 0, 0);

    head("a stale active index is not trusted, whatever set it");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000, 0);
    g_active = 3;                  /* forced: no API path produces this */
    g_pinned = 1;
    ck("netdev_active falls back to the preference",
       netdev_active() == 0, netdev_active());
    ck("and a send through it reaches the real NIC",
       (netdev_send(netdev_active(), "x", 1) == 0) && sim[0].sends == 1,
       sim[0].sends);

    head("the slot a NIC vacated keeps nothing of it");
    sim_reset(); netdev_reset();
    netdev_register(&ops_full, NET_BUS_E1000,   1);
    netdev_register(&ops_full, NET_BUS_RTL8139, 2);
    ck("slot 1 holds the second NIC's address before the unplug",
       g_nic[1].mac[0] == 0x30, g_nic[1].mac[0]);
    netdev_unregister(0);
    ck("the vacated slot's ops pointer is cleared",
       g_nic[1].ops == 0, 0);
    ck("its cached MAC is cleared",
       g_nic[1].mac[0] == 0 && g_nic[1].mac[5] == 0, g_nic[1].mac[0]);
    ck("its unit is cleared", g_nic[1].unit == 0, g_nic[1].unit);
    ck("its kind is cleared", g_nic[1].kind == NET_BUS_NONE,
       g_nic[1].kind);

    head("registration zeroes the MAC before asking for one");
    sim_reset(); netdev_reset();
    /* A slot dirtied by hand, because the clearing in
       netdev_unregister() means no API sequence can hand registration a
       dirty one. That is exactly why this is worth a check: the two
       guards cover each other, and either alone leaves a NIC with no
       address reporting somebody else's. */
    for (int k = 0; k < 6; k++) g_nic[0].mac[k] = 0xC3;
    netdev_register(&ops_nomac, NET_BUS_USBNET, 0);
    ck("a NIC that cannot answer reports zeroes, not what was there",
       g_nic[0].mac[0] == 0 && g_nic[0].mac[5] == 0, g_nic[0].mac[0]);
    netdev_mac(0, mac);
    ck("and the public accessor agrees",
       mac[0] == 0 && mac[5] == 0, mac[0]);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
