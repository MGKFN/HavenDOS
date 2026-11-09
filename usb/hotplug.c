/* ════════════════════════════════════════════════════════════════════
   usb/hotplug.c -- a device plugged in after boot, noticed, v0.8.8

   "No USB hot-plug" has been an open item since 0.8.5 and the reason
   given was always the same one: nothing in this kernel ran
   periodically, so a port change was never looked at.  `usb rescan`
   exists because of it, and usb/hub.c says so in its own header.

   0.8.8 gives the kernel a tick (include/kpoll.h), so this is the
   client that uses it.  Two things live here.

   ── ONE COPY OF THE RESCAN SEQUENCE ─────────────────────────────────

   usb_rescan_all() is the body that was inside `usb rescan` in
   shell/usb.c.  It moved because this file needs it too, and two
   copies of a four-call sequence whose ORDER is the whole point would
   drift -- which is precisely what tools/check_diskname_sync.py was
   written to refuse one layer down, and what v0.8.6 did when it moved
   the disk table out of shell/iso_installer.c so a suite could LINK it
   rather than copy it.

   The order is the order the layers stack, and each step is there
   because of a bug:

     usbnet_release()   a NIC holds an INDEX into the core's device
                        table, and usb_enumerate() rebuilds that table.
                        v0.8.7.
     lockdown_forget()  the connection's transport is about to be torn
                        down; one that was not told would report itself
                        up.  v0.8.7.7.
     usbmux_close()     likewise, one layer below it.
     usb_enumerate()    the bus itself.  Issues Disable Slot for every
                        device first -- v0.8.6.1, found on the HP after
                        the third rescan lost a device.
     msc_rescan()       each mass-storage device holds a COPY of its
                        usb_dev_t, so a stale one addresses whatever
                        moved into that slot.
     blk_scan()         a disk that has gone away must not stay in the
                        installer's picker.
     usbnet_probe()     and the NIC comes back, with a fresh index.

   ── HOW A CHANGE IS DETECTED, AND WHAT IS NOT DETECTED ──────────────

   By polling the ROOT PORTS, through the port_connected() op both
   back-ends already supply.  On xHCI that is one MMIO read of PORTSC
   and a test of CCS; on UHCI one PORTSC read and a test of the same
   bit.  Pure reads, no write-1-to-clear, nothing new in either driver
   and nothing new in usb_hc_ops_t.

   THAT WAS A DELIBERATE CHOICE OVER THE CHANGE BITS.  xHCI's PORTSC
   has CSC, which latches a connect change and is cleared by writing
   1 -- it would catch an unplug-and-replug inside one interval, which
   a connected-bitmap comparison cannot.  It is not used, for the
   reason v0.8.7.8 cost a release: a guard that has never run against
   a real packet is not a guard.  A write-1-to-clear bit touched from
   a periodic poll is a write to a live controller register on a
   timer, on hardware nobody here has read the specification of, with
   the existing enumeration path also acking CSC during a reset --
   two writers of one latch.  A bitmap of connected ports is a read.
   When somebody has a port-change trace off real silicon, the bits
   are the better answer; until then they are a guess that writes.

   NOT DETECTED, and recorded rather than claimed:

     - A DEVICE BEHIND A HUB.  A hub reports port changes on its
       interrupt IN endpoint, which means a control transfer or an
       interrupt poll per hub per interval, and the HP cannot test any
       of it: two boots on 25 October had the stick on root port 0
       directly and `usb -vv` showed no hub anywhere -- this laptop's
       side sockets appear to be wired straight to the xHCI root
       ports.  0.8.7 shipped a NIC layer nothing could exercise and
       the next release was spent finding out what was wrong with it.
       Not again, in the same release, on a path with a control
       transfer in it.
     - AN UNPLUG AND REPLUG INSIDE ONE INTERVAL.  The bitmap is the
       same before and after.  `usb rescan` is still there.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/kpoll.h"
#include "../include/usb.h"
#include "../include/klog.h"
#include "../include/types.h"

/* The layers above the core.  Declared rather than included: this file
   is the one place the rescan ORDER lives, and pulling in four headers
   to say so would invite somebody to call them in a different order
   somewhere else. */
extern int  msc_rescan(void);
extern int  blk_scan(void);
extern void usbnet_release(void);
extern int  usbnet_probe(void);
extern void lockdown_forget(void);
extern void usbmux_close(void);
extern void net_dhcp_retry_reset(void);

/* What `usb rescan` and the poll client both report. */
static usb_rescan_t g_last;

const usb_rescan_t *usb_rescan_result(void){ return &g_last; }

void usb_rescan_all(void)
{
    usbnet_release();
    lockdown_forget();
    usbmux_close();
    g_last.devices = usb_enumerate();
    g_last.disks   = msc_rescan();
    g_last.blkdevs = blk_scan();
    g_last.nics    = usbnet_probe();
    g_last.hubs    = usb_hub_count();
    g_last.behind  = usb_hub_devices();
}

/* ── the port bitmap ─────────────────────────────────────────────────
 * One word per controller, one bit per root port.  USB_MAX_HC
 * controllers and 32 ports each; a controller reporting more than 32
 * root ports has its extra ports ignored, which is said out loud
 * because silently reading 32 of 40 ports is the kind of truncation
 * that looks like a dead socket. */
#define HP_MAX_PORTS 32
static uint32_t g_seen[USB_MAX_HC];
static uint8_t  g_have[USB_MAX_HC];
static uint32_t g_changes;          /* port changes observed            */
static uint32_t g_rescans;          /* rescans the poll client ran      */
static uint32_t g_truncated;        /* controllers with >32 root ports  */

uint32_t usb_hotplug_changes(void){ return g_changes; }
uint32_t usb_hotplug_rescans(void){ return g_rescans; }

static uint32_t ports_of(const usb_hc_t *h, int *n_out)
{
    uint32_t bits = 0;
    int n = h->ops->port_count ? h->ops->port_count((usb_hc_t *)h) : 0;
    if(n > HP_MAX_PORTS){ g_truncated++; n = HP_MAX_PORTS; }
    if(n_out) *n_out = n;
    if(!h->ops->port_connected) return 0;
    for(int p = 0; p < n; p++)
        if(h->ops->port_connected((usb_hc_t *)h, p))
            bits |= (uint32_t)1u << p;
    return bits;
}

/* Take a reading WITHOUT acting on it.  Called once at registration so
   the first poll does not read every occupied port as a brand-new
   arrival and rescan a bus that has just been enumerated. */
void usb_hotplug_baseline(void)
{
    int nhc = usb_hc_count();
    for(int c = 0; c < nhc && c < USB_MAX_HC; c++){
        const usb_hc_t *h = usb_hc(c);
        if(!h || !h->ops) continue;
        g_seen[c] = ports_of(h, 0);
        g_have[c] = 1;
    }
}

static void usb_hotplug_work(void)
{
    int nhc = usb_hc_count();
    int changed = 0;

    for(int c = 0; c < nhc && c < USB_MAX_HC; c++){
        const usb_hc_t *h = usb_hc(c);
        if(!h || !h->ops) continue;
        uint32_t now = ports_of(h, 0);
        if(!g_have[c]){ g_seen[c] = now; g_have[c] = 1; continue; }
        if(now == g_seen[c]) continue;

        /* Report the direction, per port, before rescanning -- a bus
           walk prints a dozen lines and the one fact worth having is
           which socket moved. */
        uint32_t diff = now ^ g_seen[c];
        for(int p = 0; p < HP_MAX_PORTS; p++){
            if(!(diff & ((uint32_t)1u << p))) continue;
            g_changes++;
            klog(KLOG_INFO, "usb: %s port %u: device %s",
                 usb_hc_name(h), (uint32_t)p,
                 (now & ((uint32_t)1u << p)) ? "arrived" : "removed");
        }
        /* ── g_seen IS NOT UPDATED HERE, ON PURPOSE ────────────────
           It was, and the redundancy made both writes untestable.
           usb_hotplug_baseline() runs after the rescan and records the
           settled reading, so this assignment and that call each
           masked the other: delete either one alone and the behaviour
           is identical, which is why tools/check_hotplug_suite.py's
           injections into BOTH were MISSED.  That is v0.8.6.7's
           failing-only-in-pairs shape, and four of test_netdev's seven
           holes were the same thing.

           The post-rescan reading is the one that has to win anyway:
           enumeration resets ports, so a value read here -- in the
           middle of a change -- can disagree with the settled one, and
           recording the mid-change value is what would make one plug
           event rescan twice. */
        changed = 1;
    }

    if(!changed) return;

    g_rescans++;
    usb_rescan_all();
    klog(KLOG_OK, "usb: %u device%s, %u disk%s, %u NIC%s after hot-plug",
         (uint32_t)g_last.devices, g_last.devices == 1 ? "" : "s",
         (uint32_t)g_last.disks,   g_last.disks   == 1 ? "" : "s",
         (uint32_t)g_last.nics,    g_last.nics    == 1 ? "" : "s");

    /* The bitmap is re-read after the rescan, not trusted from before
       it: enumeration resets ports and the reading taken mid-change
       can disagree with the settled one.  Without this, one plug event
       could rescan twice. */
    usb_hotplug_baseline();

    /* A NIC that has just arrived needs an address, and the compile-
       time fallback is QEMU's.  v0.8.7.2 added this to `usb rescan`;
       here it is the same reasoning with nobody having typed
       anything. */
    if(g_last.nics > 0) net_dhcp_retry_reset();
}

/* ── registration ────────────────────────────────────────────────────
 * One second.  The cost per poll is one MMIO read per root port --
 * the HP's xHCI has twelve -- and a second is far below the time it
 * takes a person to plug something in and look at the screen. */
void usb_kpoll_register(void)
{
    usb_hotplug_baseline();
    kpoll_register("usb ports", 1000, usb_hotplug_work);
}

uint32_t usb_hotplug_truncated(void){ return g_truncated; }
