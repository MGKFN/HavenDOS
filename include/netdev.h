#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/netdev.h -- the one place that knows a NIC is on a bus.

   ── WHY THIS EXISTS, AND WHY IT EXISTS BEFORE THE DRIVER ────────────

   0.8.6 "Argo" closed with one instruction carried forward in its own
   words:

     > Carry this into 0.8.7. USB Ethernet is the next bus, and the
     > lesson of this release is that guarding the LABEL was never
     > enough -- the thing that chooses has to be one place too, and a
     > grep has to be able to see it. 0.8.7 adds no block device, but it
     > does add a NIC, and net/ethernet.c has not been grepped for the
     > same shape.

   It was grepped. net/ethernet.c is clean -- it calls nic_send() and
   nic_recv() and names no driver. The shape is one layer below it, in
   net/net.c, and it is the same shape exactly:

       net/net.c   net_get_mac()   if(nic_type==1) rtl8139 else e1000
       net/net.c   nic_send()      if(nic_type==1) rtl8139 else e1000
       net/net.c   nic_recv()      if(nic_type==1) rtl8139 else e1000

   THREE switches on one `static int nic_type`, plus a fourth place that
   decides the probe order inside net_init(), plus the numbering itself
   -- 0=none 1=rtl8139 2=e1000 -- written as a comment above the
   variable rather than as anything a compiler or a grep can check.

   That is three of the four copies fs/blkdev.c was created to delete,
   and the fourth -- an order decided in one function and depended on by
   another -- is the one that made the installer call a disk NVMe while
   the filesystem bound itself to the ATA one.

   Adding a USB NIC to that costs four edits, any one of which can be
   forgotten, and forgetting the one in net_get_mac() produces a machine
   that sends ARP replies claiming a MAC address of 00:00:00:00:00:00
   and cannot be told why. So the table comes first and the driver comes
   second, which is the order 0.8.6 wishes it had used.

   ── WHAT IS HERE AND WHAT IS NOT ────────────────────────────────────

   Here: the table, the dispatch, the per-NIC refusal, the bus word, the
   registration order, the active-NIC preference, and the cached MAC.

   Not here: frames. net/ethernet.c owns the Ethernet header and
   net/net.c owns the IP configuration; neither names a driver and
   neither should start.

   ── THE CACHED MAC, WHICH IS NOT AN OPTIMISATION ────────────────────

   netdev_register() reads the MAC ONCE and keeps it. On a PCI NIC that
   is a register read and asking twice costs nothing. On a USB NIC it is
   a CONTROL TRANSFER, and net/arp.c asks for the MAC twice per reply.
   A control transfer per ARP reply on a tethered phone is a packet rate
   limited by the USB bus, and the failure mode is not slowness but a
   get_mac() that returns garbage mid-transfer and an ARP reply that
   lies about who we are.

   A NIC whose address changes while it is registered does not exist.
   One that is unplugged is deregistered, which is a different thing.
   ════════════════════════════════════════════════════════════════════ */

#define NETDEV_MAX   4

#define NET_BUS_NONE    0
#define NET_BUS_RTL8139 1
#define NET_BUS_E1000   2
#define NET_BUS_USBNET  3   /* any USB NIC: the driver supplies the word */

/* One ops table per NIC kind. The bus word lives IN IT, which is what
   makes tools/check_nic_dispatch.py able to guard the dispatch itself
   rather than guard a comment claiming everything asks in one place --
   a comment that did not survive contact with a new file, twice.

   `unit` is the driver's own index, so a machine with two of the same
   kind of adapter is not a special case waiting to be discovered.

   link_up is OPTIONAL and a 0 means "this NIC cannot say", which is
   reported as up: a NIC that does not report link state has always
   been treated as connected and pretending otherwise would take the
   network away from every machine that works today. A USB NIC CAN say,
   over an interrupt IN endpoint, and that is the first thing in this
   tree that will ever return 0 from it. */
typedef struct {
    const char *name;
    int      (*send)(int unit, const void *data, uint16_t len);
    uint16_t (*recv)(int unit, void *buf, uint16_t maxlen);
    void     (*get_mac)(int unit, uint8_t mac[6]);
    int      (*link_up)(int unit);
} netdev_ops_t;

typedef struct {
    const netdev_ops_t *ops;
    uint8_t kind;        /* NET_BUS_*                                   */
    int     unit;        /* the driver's own index                      */
    uint8_t mac[6];      /* read once at registration -- see above      */
} netdev_t;

/* ── the table ───────────────────────────────────────────────────── */

void netdev_reset(void);

/* Returns the new NIC's index, or -1. Refuses a NIC with no send and no
   recv: a NIC that can do neither is not a NIC, and registering one
   would put an entry in the table that every later loop has to skip. */
int  netdev_register(const netdev_ops_t *ops, uint8_t kind, int unit);

/* Give a NIC back -- a USB adapter that was unplugged. Compacts the
   table, so an index is only valid until this is called; the active NIC
   is re-chosen. There is no counterpart in fs/blkdev.c because a disk
   that vanishes mid-write is a different problem and 0.8.6 left it
   alone deliberately. */
int  netdev_unregister(int i);

int                 netdev_count(void);
const netdev_t     *netdev_dev(int i);
uint8_t             netdev_kind(int i);
int                 netdev_unit(int i);

/* ── the bus word ────────────────────────────────────────────────────
   Out of range returns "none" rather than inventing a NIC that is not
   there. fs/blkdev.c's own comment records what the alternative looked
   like: a welcome screen printing "No disks found" directly above
   "1 VirtIO disk found". */
const char *netdev_bus_name(int i);

/* ── transfers ───────────────────────────────────────────────────────
   These take an index, not a bus. Everything above this line names no
   driver at all, which is the whole point and the thing three switches
   in net/net.c got wrong.

   send returns <0 on failure; recv returns 0 for "nothing waiting",
   which is not an error and is the usual answer. */
int      netdev_send(int i, const void *data, uint16_t len);
uint16_t netdev_recv(int i, void *buf, uint16_t maxlen);
void     netdev_mac(int i, uint8_t mac[6]);
int      netdev_link_up(int i);

/* ── which NIC the stack uses ────────────────────────────────────────

   Two questions, two functions, each with its argument beside it --
   which is the shape fs/blkdev.c arrived at after the two orders in the
   block layer were found to disagree.

   netdev_active() is the CHOICE: the NIC net/net.c sends through.
   netdev_preferred() is the PREFERENCE that chose it when nobody said.

   The preference is e1000, RTL8139, then USB, and removable-last is for
   predictability rather than for safety -- a USB stick becoming the
   system volume loses data and picking the wrong NIC loses a ping. It
   is last because a machine with a real NIC should not change its mind
   about which one it is when somebody plugs a phone in to charge.

   `net nic <n>` overrides it, which is why this is settable at all: on
   the HP, whose only built-in NIC is a MediaTek Wi-Fi part with no
   driver here, the USB adapter is the only entry in the table and the
   preference never comes up. On a machine with both, the person
   plugging in the phone is the one who knows. */
int  netdev_active(void);
int  netdev_set_active(int i);
int  netdev_preferred(void);

/* Register every NIC the back-ends can find, in preference order, and
   choose the active one. Safe on a machine with no NIC at all.
   Returns the number registered.

   NET_NO_HW keeps the PCI drivers out for tests/test_netdev.c, which
   links this file for the table, the dispatch and the words and has no
   hardware to call -- exactly as BLK_NO_HW does for fs/blkdev.c. */
int  netdev_scan(void);

/* Register a USB NIC found by the USB core. Called from usb/ rather
   than from netdev_scan(), because a USB NIC appears when the bus is
   walked and not when the PCI bus is. */
int  netdev_register_usb(const netdev_ops_t *ops, int unit);
