/* ════════════════════════════════════════════════════════════════════
   usb/hub.c -- the USB hub class, so a device behind one can be found.

   ── WHY THIS IS IN 0.8.6 AND NOT 0.8.5 ───────────────────────────────

   0.8.5 shipped HID and judged root ports to be enough, and said so:
   "a laptop's internal keyboard and trackpad are usually on root ports,
   which is why this was judged enough for now -- but it is an assumption
   until somebody boots it." The HP's own listing bore that out: 16 root
   ports with devices on 2 and 9, both internal.

   A USB STICK IS NOT AN INTERNAL DEVICE. It goes in the socket on the
   side of the laptop, and on most laptops that socket is behind an
   internal hub. The 20 October handoff named this as the first suspect
   if a stick did not appear, and a release whose whole subject is
   removable storage cannot ship with "it may need a hub" as its
   acceptance risk.

   ── WHAT A HUB ACTUALLY COSTS ────────────────────────────────────────

   The class itself is small: a descriptor saying how many ports, a
   SET_FEATURE per port to turn its power on, a GET_STATUS per port to
   see what is there, a SET_FEATURE(PORT_RESET) and a wait. Four class
   requests and a loop.

   What it costs is in the CONTROLLER, and the two disagree completely:

     UHCI  nothing at all. A hub is transparent: the device behind it is
           addressed by its address like anything else, and the only
           thing missing from 0.8.5 was somebody to reset the port and
           hand out the address.

     xHCI  the controller owns the device, so it has to be TOLD the
           path. The slot context carries a 20-bit route string and the
           root port the path hangs off, and a low or full speed device
           behind a high speed hub needs the hub's slot and port in the
           transaction-translator fields as well or it never answers.

   That asymmetry is why usb_hc_ops_t::attach_path() exists and why the
   route arithmetic is in usb/core.c rather than here: the core owns how
   a device is addressed, and a class driver doing that arithmetic itself
   would be the second place that knows it.

   ── WHAT IS NOT HERE ─────────────────────────────────────────────────

   No hot-plug interrupt. A hub reports port changes on an interrupt IN
   endpoint, and THAT is still not polled -- v0.8.8 added a periodic
   check of the ROOT ports only (usb/hotplug.c), because a hub's change
   endpoint is a control transfer per hub per interval and the one
   laptop this tree is tested on appears to have no hub: two boots on
   25 October had the stick on root port 0 directly and `usb -vv`
   showed no hub anywhere.

   So a device plugged into a root port after boot is noticed within a
   second; one plugged in BEHIND A HUB is not, and `usb rescan` is the
   answer for it. Stated plainly rather than left for somebody to
   discover.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/klog.h"

extern void sleep_ms(uint32_t ms);

/* ── the hub class ───────────────────────────────────────────────────
   Numbers from the USB 2.0 specification, chapter 11. They are the
   specification's and not ours, which is why they are here rather than
   in include/usb.h: nothing above this file has any use for them. */
#define HUB_CLASS            0x09

#define HUB_DESC_TYPE        0x29   /* hub descriptor, USB 2.0          */
#define HUB_DESC_TYPE_SS     0x2A   /* and SuperSpeed's, which differs  */

/* wPortStatus */
#define PORT_CONNECTION      0x0001
#define PORT_ENABLE          0x0002
#define PORT_SUSPEND         0x0004
#define PORT_OVER_CURRENT    0x0008
#define PORT_RESET           0x0010
#define PORT_POWER           0x0100
#define PORT_LOW_SPEED       0x0200
#define PORT_HIGH_SPEED      0x0400

/* feature selectors */
#define FEAT_PORT_ENABLE      1
#define FEAT_PORT_RESET       4
#define FEAT_PORT_POWER       8
#define FEAT_C_PORT_CONNECTION 16
#define FEAT_C_PORT_RESET     20

#define HUB_MAX_PORTS        15      /* what one hub's loop will walk   */

static int g_hubs_seen;              /* for the one-line summary        */
static int g_found;
static int g_refused_uhci;

/* A hub's own class request: SET_FEATURE and CLEAR_FEATURE with the port
   in wIndex, recipient OTHER (3), which is the specification's word for
   "a port of this hub" and is not USB_RECIP_* in include/usb.h because
   nothing but a hub has ports. */
#define RECIP_OTHER  0x03

static int port_set(const usb_dev_t *h, int port, uint16_t feat)
{
    return usb_control(h, USB_DIR_OUT | USB_TYPE_CLASS | RECIP_OTHER,
                       USB_REQ_SET_FEATURE, feat, (uint16_t)(port + 1), 0, 0);
}

static int port_clear(const usb_dev_t *h, int port, uint16_t feat)
{
    return usb_control(h, USB_DIR_OUT | USB_TYPE_CLASS | RECIP_OTHER,
                       USB_REQ_CLEAR_FEATURE, feat, (uint16_t)(port + 1),
                       0, 0);
}

/* Port status is four bytes: wPortStatus then wPortChange, little-endian.
   Returned through two out parameters rather than one uint32_t, because a
   caller that has to remember which half is which is a caller that will
   get it wrong once. */
static int port_status(const usb_dev_t *h, int port,
                       uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    int r;
    memset(buf, 0, sizeof buf);
    r = usb_control(h, USB_DIR_IN | USB_TYPE_CLASS | RECIP_OTHER,
                    USB_REQ_GET_STATUS, 0, (uint16_t)(port + 1), buf, 4);
    if (r < 4) return (r < 0) ? r : USB_EXFER;
    if (status) *status = (uint16_t)(buf[0] | (buf[1] << 8));
    if (change) *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return USB_OK;
}

/* How many ports, from the hub descriptor.
 *
 * bNbrPorts is byte 2 of both the 2.0 and the SuperSpeed descriptor, so
 * one read serves both -- but the DESCRIPTOR TYPE differs (0x29 against
 * 0x2A) and a hub answers only to its own. A SuperSpeed hub asked for
 * 0x29 STALLs, which is a refusal and not a failure, so both are tried
 * and the stall is not logged as a fault. Getting this wrong would make
 * every USB 3 hub report zero ports, which looks exactly like a hub with
 * nothing plugged into it. */
static int hub_nports(const usb_dev_t *h)
{
    uint8_t d[8];
    int r, i;
    static const uint8_t types[2] = { HUB_DESC_TYPE, HUB_DESC_TYPE_SS };

    for (i = 0; i < 2; i++) {
        memset(d, 0, sizeof d);
        r = usb_control(h, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)(types[i] << 8), 0, d, 8);
        if (r >= 3 && d[2] > 0 && d[2] <= 127)
            return (int)d[2];
    }
    return 0;
}

/* The speed a device on this port is running at.
 *
 * Read off the HUB, not guessed from the hub's own speed. A high-speed
 * hub carries low, full and high speed devices on the same ports, and
 * which one it is decides both the packet size and whether the
 * controller needs a transaction translator. The two bits are mutually
 * exclusive and neither set means full speed. */
static uint8_t port_speed_of(uint16_t status)
{
    if (status & PORT_LOW_SPEED)  return USB_SPEED_LOW;
    if (status & PORT_HIGH_SPEED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

/* Reset one port and wait for the device to come out of it.
 *
 * The wait is bounded and the bound is checked, rather than a fixed
 * sleep: the specification gives a hub 20 ms of reset and the device 10
 * ms of recovery, but a cheap hub takes longer and a reset that has not
 * finished reads as a port with nothing on it. Polling the RESET bit is
 * the only way to know, and C_PORT_RESET must be cleared afterwards or
 * the next status read still reports the change.
 *
 * 500 ms total, in 10 ms steps. A hub that has not finished a port reset
 * in half a second is a hub whose port is broken, and the alternative --
 * an unbounded loop -- is the bug class kernel/machine.c was written to
 * get rid of: a wait with no exit is indistinguishable from a reset that
 * did not work. */
static int port_reset_wait(const usb_dev_t *h, int port, uint16_t *status)
{
    int ms;
    uint16_t st = 0, ch = 0;
    int r = port_set(h, port, FEAT_PORT_RESET);
    if (r < 0) return r;

    for (ms = 0; ms < 500; ms += 10) {
        sleep_ms(10);
        r = port_status(h, port, &st, &ch);
        if (r != USB_OK) return r;
        if (!(st & PORT_RESET)) break;
    }
    if (st & PORT_RESET) return USB_ETIMEOUT;

    port_clear(h, port, FEAT_C_PORT_RESET);
    /* The device's own recovery time. The specification says 10 ms and
       says a device need not answer before it has elapsed, so this is
       not optional padding. */
    sleep_ms(12);
    if (status) *status = st;
    return (st & PORT_ENABLE) ? USB_OK : USB_ENODEV;
}

/* ── one hub ──────────────────────────────────────────────────────────
   Walk its ports, bring up what is on them. Returns how many devices
   were attached. */
static int scan_one(int hubidx)
{
    const usb_dev_t *h = usb_device(hubidx);
    int nports, p, got = 0;

    if (!h) return 0;

    nports = hub_nports(h);
    if (nports <= 0) {
        klog(KLOG_WARN, "USB hub addr %u: no port count", (uint32_t)h->addr);
        return 0;
    }
    if (nports > HUB_MAX_PORTS) nports = HUB_MAX_PORTS;

    klog(KLOG_INFO, "USB hub addr %u tier %u: %u port(s)",
         (uint32_t)h->addr, (uint32_t)h->tier, (uint32_t)nports);

    for (p = 0; p < nports; p++) {
        /* Power the port before looking at it. A bus-powered hub brings
           its ports up unpowered, so a GET_STATUS first reports nothing
           connected and the loop walks past a device that is there.
           The 100 ms is bPwrOn2PwrGood's worst case, which the
           descriptor carries in 2 ms units -- not read here, because the
           maximum is 510 ms and one wait of 100 ms per hub is cheaper
           than being right about a number no hub in this decade sets
           above 50. */
        port_set(h, p, FEAT_PORT_POWER);
    }
    sleep_ms(100);

    for (p = 0; p < nports; p++) {
        uint16_t st = 0, ch = 0;
        uint8_t  speed;
        int      r, idx;

        if (port_status(h, p, &st, &ch) != USB_OK) continue;
        if (ch & PORT_CONNECTION) port_clear(h, p, FEAT_C_PORT_CONNECTION);
        if (!(st & PORT_CONNECTION)) continue;

        if (st & PORT_OVER_CURRENT) {
            /* Said out loud. An over-current port is a port the hub has
               switched off because something on it is drawing too much,
               and "nothing appeared" is a much worse answer than
               "the hub cut the power to that socket". */
            klog(KLOG_WARN, "USB hub addr %u port %u: over-current, "
                            "powered off by the hub",
                 (uint32_t)h->addr, (uint32_t)(p + 1));
            continue;
        }

        r = port_reset_wait(h, p, &st);
        if (r != USB_OK) {
            klog(KLOG_WARN, "USB hub addr %u port %u: reset failed (%d)",
                 (uint32_t)h->addr, (uint32_t)(p + 1), r);
            continue;
        }

        speed = port_speed_of(st);
        idx = usb_attach_behind(hubidx, p, speed);
        if (idx < 0) {
            if (idx == USB_EPARAM) {
                /* The back-end has no attach_path: see the header
                   comment. Counted and reported once rather than per
                   port, because sixteen identical warnings is not more
                   information than one. */
                g_refused_uhci++;
            } else {
                klog(KLOG_WARN, "USB hub addr %u port %u: did not "
                                "enumerate (%d)",
                     (uint32_t)h->addr, (uint32_t)(p + 1), idx);
            }
            continue;
        }
        got++;
    }
    return got;
}

/* ── the pass ─────────────────────────────────────────────────────────
 *
 * Breadth first, by tier, and NOT recursively.
 *
 * usb_attach_behind() appends to the core's device table, so a hub found
 * behind a hub appears in the table the outer loop is walking. Walking
 * it by index and re-checking the count each time round means a hub
 * found at tier 2 is scanned on the next pass with no recursion and no
 * second table -- and the tier bound in the core stops the one way this
 * could run away, which is a hub whose own descriptor reports it is
 * plugged into itself.
 *
 * Returns the number of devices added. */
int usb_hub_scan(void)
{
    int tier, added_total = 0;

    g_hubs_seen = g_found = g_refused_uhci = 0;

    for (tier = 0; tier < USB_MAX_TIER; tier++) {
        int i, n = usb_device_count(), added = 0;

        for (i = 0; i < n; i++) {
            const usb_dev_t *d = usb_device(i);
            if (!d || !d->valid) continue;
            if (d->tier != (uint8_t)tier) continue;
            /* A hub says so in its interface class, or -- if its
               configuration never parsed -- in its device class. Both,
               because 0.8.5 already found that a device can answer its
               device descriptor and not its configuration, and a hub
               that did that would otherwise hide every disk behind it. */
            if (d->if_class != HUB_CLASS && d->dev_class != HUB_CLASS)
                continue;
            g_hubs_seen++;
            added += scan_one(i);
        }

        added_total += added;
        /* Nothing new at this tier means nothing deeper can exist. */
        if (added == 0) break;
    }

    if (g_refused_uhci) {
        /* Once, with the reason, rather than per port. */
        klog(KLOG_WARN, "USB: %u device(s) behind a hub on a controller "
                        "this kernel cannot address through one "
                        "(UHCI); see usb/hub.c",
             (uint32_t)g_refused_uhci);
    }
    if (g_hubs_seen)
        klog(KLOG_OK, "USB: %u hub(s), %u device(s) behind them",
             (uint32_t)g_hubs_seen, (uint32_t)added_total);

    g_found = added_total;
    return added_total;
}

int usb_hub_count(void)    { return g_hubs_seen; }
int usb_hub_devices(void)  { return g_found; }
