/* ════════════════════════════════════════════════════════════════════
   usb/ethernet.c -- Apple USB Ethernet: iPhone tethering.

   include/usbnet.h carries the argument, the confirmed descriptor
   layout, and -- more importantly -- the list of what in here is NOT
   confirmed. Read it before trusting a byte of the framing below.

   This file knows about USB and about Ethernet frames. It does not
   know about IP, ARP or sockets, and it does not know which NIC the
   stack is using: it hands an ops table to netdev_register_usb() and
   net/netdev.c decides. That separation is why net/netdev.c was
   written first.
   ════════════════════════════════════════════════════════════════════ */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/usbnet.h"
#include "../include/netdev.h"
#include "../include/klog.h"

/* ── klog is NOT printf, v0.8.7.1 ─────────────────────────────────────
   It implements %d %u %x %s %c %% and ONE optional pad digit 1-8, so
   %4x prints 00c1. Anything else -- %02X, %04X, %ld -- hits the
   default case, which prints the specifier LITERALLY and DOES NOT
   consume its argument. Every later specifier in the same line then
   takes the wrong one.

   The first `usb eth` on the HP printed:

       eth0  %04X:%04X  if 1452 alt 4776  ep %02X/%02X

   1452 is 0x05AC and 4776 is 0x12A8 -- the vendor and product ids,
   shifted into the interface and alternate-setting columns. The MAC
   line was entirely literal. tools/check_klog_fmt.py refuses it now. */

/* ── the vendor control requests ──────────────────────────────────────
   Both are bmRequestType 0xC0 -- IN, vendor, device -- and both are
   sent with wIndex 2, which is the interface number of the Ethernet
   interface in configuration 3. That the index is also 2 is a
   coincidence worth not relying on, so it is named. */
#define IPHETH_REQ_GET_MAC      0x00
#define IPHETH_REQ_CARRIER      0x45
#define IPHETH_CTRL_TYPE_IN     0xC0
#define IPHETH_CTRL_INDEX       2
#define IPHETH_CTRL_BUF         64
#define IPHETH_CARRIER_ON       0x04

/* ── the framing, which is the unconfirmed part ───────────────────────
   TX: the frame is copied into a buffer of exactly IPHETH_TX_LEN and
       zero-padded, and the whole buffer is sent. Not the frame length
       -- a FIXED length, every time.
   RX: the device prepends IPHETH_RX_SKIP bytes before the Ethernet
       header. The published Linux driver drops them and calls the
       reason alignment.

   Both are what `ipheth` does. Neither has been seen on a wire by this
   kernel. usbnet_dump() prints what actually arrives. */
#define IPHETH_TX_LEN   1514
#define IPHETH_RX_SKIP  2
#define IPHETH_RX_LEN   (IPHETH_TX_LEN + IPHETH_RX_SKIP)

/* A carrier check is a CONTROL TRANSFER, and net_ready() can be asked
   on any screen redraw. Rate-limited rather than cached for ever: a
   phone whose hotspot is switched off mid-session should be noticed,
   and a control transfer per redraw should not happen. */
#define IPHETH_CARRIER_MS  1000

#define USBNET_MAX  2

typedef struct {
    int      used;
    int      dev;          /* index into the USB core's device table    */
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
    uint8_t  ifnum;
    uint8_t  mac[6];
    int      have_mac;

    /* carrier, and when it was last actually asked */
    int      carrier;
    uint32_t carrier_ms;
    int      carrier_asked;

    /* ── what the first transfer really contained ──────────────────
       Kept so `usb eth` can print it. The whole reason this driver
       ships with a dump command is that the framing above is read off
       somebody else's driver, and a receive path that silently drops
       two bytes cannot be asked whether there were two bytes to
       drop. */
    uint8_t  first[32];
    int      first_len;
    int      have_first;

    uint32_t rx_frames, rx_runts, rx_errs;
    uint32_t tx_frames, tx_errs;
} usbnet_t;

static usbnet_t g_n[USBNET_MAX];
static int      g_nn;

/* One buffer each, not one per NIC: nothing here is re-entrant and
   net_poll() is called from one place. Two phones tethered at once
   would share them, which is correct because they would also share the
   single-threaded poll. */
static uint8_t g_rx[IPHETH_RX_LEN];
static uint8_t g_tx[IPHETH_TX_LEN];
static uint8_t g_ctl[IPHETH_CTRL_BUF];

extern uint32_t klog_now_ms(void);

/* ════════════════════════════════════════════════════════════════════
   The NIC layer's ops
   ════════════════════════════════════════════════════════════════════ */

static usbnet_t *unit_of(int unit)
{
    if (unit < 0 || unit >= USBNET_MAX) return 0;
    if (!g_n[unit].used) return 0;
    return &g_n[unit];
}

static int un_send(int unit, const void *data, uint16_t len)
{
    usbnet_t        *n = unit_of(unit);
    const usb_dev_t *d;
    int              r;

    if (!n) return -1;
    /* A frame longer than the fixed transmit length cannot be padded
       into it, and truncating an Ethernet frame is worse than refusing
       to send it: the far end sees a short frame with a valid header. */
    if (len == 0 || len > IPHETH_TX_LEN) { n->tx_errs++; return -1; }

    d = usb_device(n->dev);
    if (!d || !d->valid) { n->tx_errs++; return -1; }

    /* THE WHOLE BUFFER, zero-padded. Sending `len` bytes instead is the
       obvious thing and is what the device does not accept -- see
       include/usbnet.h on what is confirmed here and what is not. */
    memcpy(g_tx, data, len);
    if (len < IPHETH_TX_LEN) memset(g_tx + len, 0, IPHETH_TX_LEN - len);

    r = usb_bulk(d, n->ep_out, 0, g_tx, IPHETH_TX_LEN, (int)n->mps_out);
    if (r < 0) {
        n->tx_errs++;
        /* A stalled bulk OUT is the ordinary way a device says it has
           lost track. Clearing it here means the next frame has a
           chance rather than every frame after the first failure
           failing too -- the discipline usb/msc.c's bot_cmd() set. */
        if (r == USB_ESTALL) usb_clear_halt(d, n->ep_out, 0);
        return -1;
    }
    n->tx_frames++;
    return 0;
}

static uint16_t un_recv(int unit, void *buf, uint16_t maxlen)
{
    usbnet_t        *n = unit_of(unit);
    const usb_dev_t *d;
    int              r, len;

    if (!n || !buf || maxlen == 0) return 0;
    d = usb_device(n->dev);
    if (!d || !d->valid) return 0;

    /* NON-BLOCKING. net_poll() runs from the shell's idle loop and
       a driver cannot depend on the bottom half, so a receive that waits
       is a shell that stops answering -- v0.8.5's keyboard at one
       character per second, one endpoint type along. */
    r = usb_bulk_poll(d, n->ep_in, 1, g_rx, IPHETH_RX_LEN, (int)n->mps_in);
    if (r == 0) return 0;                  /* nothing waiting: normal   */
    if (r < 0) {
        n->rx_errs++;
        if (r == USB_ESTALL) usb_clear_halt(d, n->ep_in, 1);
        return 0;
    }

    /* Recorded BEFORE anything is stripped, and only once. This is the
       evidence that settles whether IPHETH_RX_SKIP is 2, 0 or
       something else, and it has to be the bytes as they arrived. */
    if (!n->have_first) {
        int k = r < (int)sizeof n->first ? r : (int)sizeof n->first;
        memcpy(n->first, g_rx, (uint32_t)k);
        n->first_len  = r;
        n->have_first = 1;
    }

    len = r - IPHETH_RX_SKIP;
    if (len <= 0) { n->rx_runts++; return 0; }
    if (len > (int)maxlen) len = (int)maxlen;
    memcpy(buf, g_rx + IPHETH_RX_SKIP, (uint32_t)len);
    n->rx_frames++;
    return (uint16_t)len;
}

static void un_mac(int unit, uint8_t mac[6])
{
    usbnet_t *n = unit_of(unit);
    int       k;
    if (!mac) return;
    for (k = 0; k < 6; k++) mac[k] = n && n->have_mac ? n->mac[k] : 0;
}

static int un_link(int unit)
{
    usbnet_t        *n = unit_of(unit);
    const usb_dev_t *d;
    uint32_t         now;
    int              r;

    if (!n) return 0;
    d = usb_device(n->dev);
    if (!d || !d->valid) return 0;

    now = klog_now_ms();
    /* Asked at most once a second. The first call always asks, because
       `carrier_asked` starts clear -- a NIC reporting "down" purely
       because nothing had got round to looking would be the worst
       possible first impression of this driver. */
    if (n->carrier_asked && (now - n->carrier_ms) < IPHETH_CARRIER_MS)
        return n->carrier;

    memset(g_ctl, 0, sizeof g_ctl);
    r = usb_control(d, IPHETH_CTRL_TYPE_IN, IPHETH_REQ_CARRIER, 0,
                    IPHETH_CTRL_INDEX, g_ctl, IPHETH_CTRL_BUF);
    n->carrier_ms    = now;
    n->carrier_asked = 1;
    /* A device that will not answer the question is not a device with
       no carrier -- it is a device that has gone away, and the NIC
       layer's own index check is what notices that. Reporting the last
       known answer rather than inventing "down" keeps one failed
       control transfer from dropping a working link. */
    if (r < 0) return n->carrier;
    n->carrier = (g_ctl[0] == IPHETH_CARRIER_ON) ? 1 : 0;
    return n->carrier;
}

/* The bus word. It does NOT begin with "USB", and that is deliberate:
   tools/check_bus_label.py guards a literal that is, or starts with,
   one of the disk-bus words, and this is a NIC name rather than a disk
   label. Naming it "USB Ethernet" would trip a check written for a
   different question, and an exemption added to silence that is an
   exemption that will hide a real finding later. */
static const netdev_ops_t IPHETH_OPS = {
    "Apple USB Ethernet", un_send, un_recv, un_mac, un_link
};

/* ════════════════════════════════════════════════════════════════════
   Finding one
   ════════════════════════════════════════════════════════════════════ */

/* usb_survey_config()'s observer. It records whether the configuration
   it is walking holds an interface with the Apple Ethernet triple, and
   which interface number that was. */
typedef struct { int found; uint8_t ifnum; } survey_t;

static void survey_cb(void *ctx, uint8_t num, uint8_t alt, uint8_t nep,
                      uint8_t cls, uint8_t sub, uint8_t proto)
{
    survey_t *s = (survey_t *)ctx;
    (void)alt; (void)nep;
    if (s->found) return;
    if (cls == IPHETH_IF_CLASS && sub == IPHETH_IF_SUBCLASS &&
        proto == IPHETH_IF_PROTOCOL) {
        s->found = 1;
        s->ifnum = num;
    }
}

static int get_mac(usb_dev_t *d, usbnet_t *n)
{
    int r;
    memset(g_ctl, 0, sizeof g_ctl);
    r = usb_control(d, IPHETH_CTRL_TYPE_IN, IPHETH_REQ_GET_MAC, 0,
                    IPHETH_CTRL_INDEX, g_ctl, IPHETH_CTRL_BUF);
    /* Six bytes, exactly. A short answer is not an address with the
       rest assumed zero: net/arp.c would put it in a reply and claim
       it, and a NIC that lies about who it is poisons every ARP cache
       on the network it is attached to. */
    if (r < 6) return r < 0 ? r : USB_EXFER;
    memcpy(n->mac, g_ctl, 6);
    n->have_mac = 1;
    return USB_OK;
}

/* Bring one device up as an Apple Ethernet NIC. Returns USB_OK, or a
   negative USB_E* -- and logs WHICH step refused, because every one of
   them fails for a different reason and "tethering did not work" is
   all of them wearing one face. That is v0.8.6.5's lesson, which was
   about `update` naming which of four reasons a stick was not used. */
static int bring_up(int devidx, usbnet_t *n)
{
    usb_dev_t      *d;
    const usb_ep_t *in, *out;
    survey_t        s;
    int             ci, r, cfg = -1;

    d = (usb_dev_t *)usb_device(devidx);
    if (!d || !d->valid) return USB_ENODEV;

    /* Which configuration holds the interface. Surveying changes
       nothing on the device -- see usb_survey_config() -- which matters
       because this runs against a phone somebody is using. */
    for (ci = 0; ci < (int)d->nconfigs; ci++) {
        s.found = 0; s.ifnum = 0;
        if (usb_survey_config(d, (uint8_t)ci, survey_cb, &s) < 0) continue;
        if (s.found) { cfg = ci; break; }
    }
    if (cfg < 0) return USB_ENODEV;        /* not a tethering device    */

    klog(KLOG_INFO, "usbnet: %4x:%4x cfg %d if %u is Apple Ethernet",
         (uint32_t)d->vendor, (uint32_t)d->product, (uint32_t)cfg,
         (uint32_t)s.ifnum);

    r = usb_select_config(d, (uint8_t)cfg, (int)s.ifnum);
    if (r < 0) {
        klog(KLOG_WARN, "usbnet: SET_CONFIGURATION %d refused (%d)",
             (uint32_t)cfg, (uint32_t)r);
        return r;
    }

    /* Alt 0 of this interface has no endpoints at all, which is why
       alternate-setting support is in this release. */
    r = usb_set_interface(d, s.ifnum, IPHETH_ALT);
    if (r < 0) {
        klog(KLOG_WARN, "usbnet: SET_INTERFACE %u alt %u refused (%d)",
             (uint32_t)s.ifnum, (uint32_t)IPHETH_ALT, (uint32_t)r);
        return r;
    }

    in  = usb_find_ep(d, USB_EP_BULK, 1);
    out = usb_find_ep(d, USB_EP_BULK, 0);
    if (!in || !out) {
        klog(KLOG_WARN, "usbnet: alt %u has no bulk pair (in %d out %d)",
             (uint32_t)IPHETH_ALT, in ? 1 : 0, out ? 1 : 0);
        return USB_ENODEV;
    }

    n->dev     = devidx;
    n->ifnum   = s.ifnum;
    n->ep_in   = in->addr;
    n->ep_out  = out->addr;
    n->mps_in  = in->mps;
    n->mps_out = out->mps;

    r = get_mac(d, n);
    if (r < 0) {
        /* Refused here rather than registered without an address. A NIC
           in the table with a MAC of all zeroes is the exact failure
           net/netdev.c's cached MAC exists to stop, and registering one
           would put it back. */
        klog(KLOG_WARN, "usbnet: GET_MACADDR refused (%d) -- not "
             "registering", (uint32_t)r);
        return r;
    }

    klog(KLOG_INFO, "usbnet: ep %2x in / %2x out, %u/%u bytes",
         (uint32_t)n->ep_in, (uint32_t)n->ep_out,
         (uint32_t)n->mps_in, (uint32_t)n->mps_out);
    return USB_OK;
}

int usbnet_probe(void)
{
    int i, found = 0;

    for (i = 0; i < usb_device_count() && g_nn < USBNET_MAX; i++) {
        const usb_dev_t *d = usb_device(i);
        usbnet_t         cand;
        int              k;

        if (!d || !d->valid) continue;

        /* Already ours? A rescan calls this again, and bringing the
           same phone up twice would switch an interface that is
           working. */
        for (k = 0; k < USBNET_MAX; k++)
            if (g_n[k].used && g_n[k].dev == i) break;
        if (k < USBNET_MAX) continue;

        memset(&cand, 0, sizeof cand);
        if (bring_up(i, &cand) != USB_OK) continue;

        for (k = 0; k < USBNET_MAX; k++) if (!g_n[k].used) break;
        if (k == USBNET_MAX) break;

        cand.used = 1;
        g_n[k] = cand;
        if (netdev_register_usb(&IPHETH_OPS, k) < 0) {
            /* The NIC table is full or refused it. Giving the slot back
               rather than leaving a `used` entry nothing dispatches to:
               an entry in one table and not the other is the two-sides
               -disagreeing failure this release keeps running into. */
            memset(&g_n[k], 0, sizeof g_n[k]);
            continue;
        }
        g_nn++;
        found++;
    }
    return found;
}

void usbnet_release(void)
{
    int k;
    /* The NIC layer first, then ours. A netdev entry pointing at a
       slot this file has already cleared is a dispatch into a zeroed
       ops table, and the order is the only thing stopping it. */
    for (k = netdev_count() - 1; k >= 0; k--)
        if (netdev_kind(k) == NET_BUS_USBNET) netdev_unregister(k);
    memset(g_n, 0, sizeof g_n);
    g_nn = 0;
}

int usbnet_count(void) { return g_nn; }

int usbnet_dump(void)
{
    int k, shown = 0;

    for (k = 0; k < USBNET_MAX; k++) {
        usbnet_t        *n = &g_n[k];
        const usb_dev_t *d;
        int              i;

        if (!n->used) continue;
        d = usb_device(n->dev);
        shown++;

        klog(KLOG_INFO, "eth%d  %4x:%4x  if %u alt %u  ep %2x/%2x",
             (uint32_t)k,
             d ? (uint32_t)d->vendor : 0, d ? (uint32_t)d->product : 0,
             (uint32_t)n->ifnum, d ? (uint32_t)d->altsetting : 0,
             (uint32_t)n->ep_in, (uint32_t)n->ep_out);
        klog(KLOG_INFO, "      mac %2x:%2x:%2x:%2x:%2x:%2x",
             (uint32_t)n->mac[0], (uint32_t)n->mac[1], (uint32_t)n->mac[2],
             (uint32_t)n->mac[3], (uint32_t)n->mac[4], (uint32_t)n->mac[5]);
        klog(KLOG_INFO, "      carrier %d  rx %u/%u/%u  tx %u/%u",
             (uint32_t)n->carrier, n->rx_frames, n->rx_runts, n->rx_errs,
             n->tx_frames, n->tx_errs);

        /* ── THE BYTES, UNINTERPRETED ──────────────────────────────
           This is what the dump is for. IPHETH_RX_SKIP is 2 because
           somebody else's driver says so, and until these bytes have
           been read off a real phone that is all it is. An Ethernet
           frame's first six bytes are a destination MAC, so if the two
           bytes really are padding this line starts with two bytes
           that are not part of one -- and the broadcast frames a
           freshly tethered link is full of make that easy to see:
           FF FF FF FF FF FF is hard to mistake. */
        if (!n->have_first) {
            klog(KLOG_INFO, "      first transfer: nothing received yet");
        } else {
            char line[64];
            int  p = 0, lim = n->first_len;
            if (lim > 16) lim = 16;
            for (i = 0; i < lim && p < (int)sizeof line - 4; i++) {
                const char *hx = "0123456789ABCDEF";
                line[p++] = hx[(n->first[i] >> 4) & 0xF];
                line[p++] = hx[n->first[i] & 0xF];
                line[p++] = ' ';
            }
            line[p] = 0;
            klog(KLOG_INFO, "      first transfer: %d bytes", n->first_len);
            klog(KLOG_INFO, "      %s", line);
        }
    }
    return shown;
}
