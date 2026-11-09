/* ════════════════════════════════════════════════════════════════════
   net/netdev.c -- the one place that knows a NIC is on a bus.

   include/netdev.h carries the argument for why this file exists: three
   switches on one `static int nic_type` in net/net.c, plus a probe
   order decided in a fourth place, found by applying 0.8.6's own
   closing instruction to net/.

   What is here: the table, the dispatch, the per-NIC refusal, the bus
   word, the registration order, the active-NIC preference and the
   cached MAC. Nothing above a frame: no Ethernet header, no IP, no ARP.

   This file is compiled natively by tests/test_netdev.c, which is why
   the PCI probes are the only part that touches hardware and they sit
   behind NET_NO_HW.
   ════════════════════════════════════════════════════════════════════ */

#include "netdev.h"
#include "string.h"

/* ── NOT g_dev AND NOT g_ndev, and this is not style, v0.8.7 ──────────
 *
 * usb/core.c has a `static usb_dev_t g_dev[]` and a `static int g_ndev`
 * of its own. In the kernel they are separate translation units and
 * there is no problem. In a suite that INCLUDES both -- which
 * tests/test_usbnet.c does, because the Apple Ethernet driver's job is
 * three requests through the REAL core against a simulated phone --
 * something much worse than a clash happens.
 *
 * The array clashes loudly: two incompatible types, a hard error.
 *
 * THE COUNTER DOES NOT. `static int g_ndev;` with no initialiser is a
 * TENTATIVE definition, and C permits any number of tentative
 * definitions of the same object with the same type in one translation
 * unit -- so the two MERGE INTO ONE VARIABLE, silently, with no
 * warning at any level. usb_enumerate() then set it to the number of
 * USB devices and netdev_count() read it as the number of NICs, so the
 * NIC layer dispatched through a table slot whose ops pointer was
 * null.
 *
 * It was found by a segmentation fault and it took a sanitiser to
 * place. A build that merges two modules' private state without a
 * diagnostic is not something to leave resting on both files happening
 * to choose different names, so tools/check_statics.py now refuses it.
 */
static netdev_t g_nic[NETDEV_MAX];
static int      g_nnic;
static int      g_active = -1;

/* Was the active NIC chosen by a person, or by the preference? A manual
   choice survives a NIC being added; it does not survive that NIC being
   unplugged, because then there is nothing to honour. */
static int      g_pinned;

/* ════════════════════════════════════════════════════════════════════
   The table
   ════════════════════════════════════════════════════════════════════ */

void netdev_reset(void)
{
    memset(g_nic, 0, sizeof g_nic);
    g_nnic   = 0;
    g_active = -1;
    g_pinned = 0;
}

int netdev_register(const netdev_ops_t *ops, uint8_t kind, int unit)
{
    int i;

    if (!ops)                  return -1;
    if (!ops->send && !ops->recv) return -1;   /* neither way: not a NIC */
    if (!ops->name)            return -1;      /* the bus word lives here */
    if (g_nnic >= NETDEV_MAX)  return -1;

    i = g_nnic;
    g_nic[i].ops  = ops;
    g_nic[i].kind = kind;
    g_nic[i].unit = unit;

    /* ONCE, here. See the header: on a USB NIC this is a control
       transfer and net/arp.c asks twice per reply. A NIC that cannot
       answer leaves the address zero, which is what net_get_mac()
       returned for an unknown nic_type and is at least the same lie in
       one place instead of three. */
    memset(g_nic[i].mac, 0, 6);
    if (ops->get_mac) ops->get_mac(unit, g_nic[i].mac);

    g_nnic++;

    /* A NIC arriving does not take the network off a NIC a person
       chose. It does fill an empty choice, so the first NIC registered
       on a machine with no others is active without anybody asking. */
    if (!g_pinned) g_active = netdev_preferred();
    return i;
}

int netdev_unregister(int i)
{
    int k;
    if (i < 0 || i >= g_nnic) return -1;

    for (k = i; k < g_nnic - 1; k++) g_nic[k] = g_nic[k + 1];
    memset(&g_nic[g_nnic - 1], 0, sizeof g_nic[0]);
    g_nnic--;

    /* The pin is an INDEX and the indices just moved. Honouring it
       after a compaction is how a person who chose NIC 1 ends up
       sending through NIC 2 without being told. */
    if (g_pinned) {
        if (g_active == i)     g_pinned = 0;       /* theirs is gone */
        else if (g_active > i) g_active--;         /* theirs moved down */
    }
    if (!g_pinned) g_active = netdev_preferred();
    else if (g_active >= g_nnic) { g_pinned = 0; g_active = netdev_preferred(); }
    return 0;
}

int             netdev_count(void) { return g_nnic; }
const netdev_t *netdev_dev(int i)
{
    return (i >= 0 && i < g_nnic) ? &g_nic[i] : 0;
}
uint8_t netdev_kind(int i)
{
    return (i >= 0 && i < g_nnic) ? g_nic[i].kind : (uint8_t)NET_BUS_NONE;
}
int netdev_unit(int i)
{
    return (i >= 0 && i < g_nnic) ? g_nic[i].unit : -1;
}

const char *netdev_bus_name(int i)
{
    if (i < 0 || i >= g_nnic) return "none";
    return g_nic[i].ops->name;
}

/* ════════════════════════════════════════════════════════════════════
   Dispatch
   ════════════════════════════════════════════════════════════════════ */

int netdev_send(int i, const void *data, uint16_t len)
{
    if (i < 0 || i >= g_nnic || !data) return -1;
    if (!g_nic[i].ops->send)           return -1;   /* receive-only */
    if (len == 0)                      return -1;
    return g_nic[i].ops->send(g_nic[i].unit, data, len);
}

uint16_t netdev_recv(int i, void *buf, uint16_t maxlen)
{
    if (i < 0 || i >= g_nnic || !buf || maxlen == 0) return 0;
    if (!g_nic[i].ops->recv)                         return 0;
    return g_nic[i].ops->recv(g_nic[i].unit, buf, maxlen);
}

void netdev_mac(int i, uint8_t mac[6])
{
    int k;
    if (!mac) return;
    if (i < 0 || i >= g_nnic) { for (k = 0; k < 6; k++) mac[k] = 0; return; }
    for (k = 0; k < 6; k++) mac[k] = g_nic[i].mac[k];
}

/* A NIC that cannot report link state is up. See the header: every NIC
   in this tree before 0.8.7 was treated that way and taking the network
   off them to be tidy would be a regression dressed as rigour. */
int netdev_link_up(int i)
{
    if (i < 0 || i >= g_nnic)   return 0;
    if (!g_nic[i].ops->link_up) return 1;
    return g_nic[i].ops->link_up(g_nic[i].unit) ? 1 : 0;
}

/* ════════════════════════════════════════════════════════════════════
   Which NIC the stack uses
   ════════════════════════════════════════════════════════════════════ */

int netdev_preferred(void)
{
    static const uint8_t pref[] = {
        NET_BUS_E1000, NET_BUS_RTL8139, NET_BUS_USBNET
    };
    unsigned p;
    int i;
    for (p = 0; p < sizeof pref / sizeof pref[0]; p++)
        for (i = 0; i < g_nnic; i++)
            if (g_nic[i].kind == pref[p]) return i;
    /* A kind not in the list is still a NIC. Falling through to the
       first registered one means a driver added without touching this
       array works, rather than working on every machine except the one
       where it is the only NIC. */
    return g_nnic > 0 ? 0 : -1;
}

int netdev_active(void)
{
    if (g_active < 0 || g_active >= g_nnic) return netdev_preferred();
    return g_active;
}

int netdev_set_active(int i)
{
    if (i < 0 || i >= g_nnic) return -1;
    g_active = i;
    g_pinned = 1;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
   The back-ends
   ════════════════════════════════════════════════════════════════════

   One ops table per NIC kind, with the bus word in it. NET_NO_HW is
   defined by the suites, which link this file for the table, the
   dispatch and the words and have no drivers to call.
   ════════════════════════════════════════════════════════════════════ */

/* The USB side is NOT behind NET_NO_HW: a USB NIC registers through the
   ops table its own driver supplies, so there is nothing to call here
   and nothing for a suite to stub. usb/ethernet.c owns that table. */
int netdev_register_usb(const netdev_ops_t *ops, int unit)
{
    return netdev_register(ops, NET_BUS_USBNET, unit);
}

#ifndef NET_NO_HW

#include "rtl8139.h"
#include "e1000.h"
#include "pci.h"

/* ── RTL8139 ──────────────────────────────────────────────────────── */
static int  rtl_send(int unit, const void *d, uint16_t n)
{ (void)unit; return rtl8139_send(d, n); }
static uint16_t rtl_recv(int unit, void *b, uint16_t n)
{ (void)unit; return rtl8139_recv(b, n); }
static void rtl_mac(int unit, uint8_t mac[6])
{ (void)unit; rtl8139_get_mac(mac); }

static const netdev_ops_t rtl_ops = {
    "RTL8139", rtl_send, rtl_recv, rtl_mac, 0
};

/* ── Intel e1000 ──────────────────────────────────────────────────── */
static int  e1k_send(int unit, const void *d, uint16_t n)
{ (void)unit; return e1000_send(d, n); }
static uint16_t e1k_recv(int unit, void *b, uint16_t n)
{ (void)unit; return e1000_recv(b, n); }
static void e1k_mac(int unit, uint8_t mac[6])
{ (void)unit; e1000_get_mac(mac); }

static const netdev_ops_t e1k_ops = {
    "e1000", e1k_send, e1k_recv, e1k_mac, 0
};

/* Probe order is REGISTRATION order and that is identity: it is what
   `net nic` prints and what `net nic <n>` takes. The preference above
   is a separate question with a separate function, which is the thing
   the block layer had to be taught twice. */
int netdev_scan(void)
{
    uint16_t rtl_io    = 0;
    uint32_t e1000_mem = 0;

    pci_scan_net2(&rtl_io, &e1000_mem);

    if (e1000_mem && e1000_init(e1000_mem))
        netdev_register(&e1k_ops, NET_BUS_E1000, 0);
    if (rtl_io && rtl8139_init(rtl_io))
        netdev_register(&rtl_ops, NET_BUS_RTL8139, 0);

    return g_nnic;
}

#else  /* NET_NO_HW */

int netdev_scan(void) { return g_nnic; }

#endif /* NET_NO_HW */
