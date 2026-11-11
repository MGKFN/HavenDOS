/* ════════════════════════════════════════════════════════════════════
   test_usbnet.c -- Apple USB Ethernet, against a phone that answers.

   ── WHY THIS SUITE IS SHAPED THE WAY IT IS ───────────────────────────

   usb/ethernet.c is the first driver in this tree written against a
   protocol NOBODY HERE HAS SEEN ON A WIRE. The descriptors are
   confirmed -- `usb cfg` on the HP, 25 October, found class FF/FD/01 on
   interface 2 of configuration index 3, with its endpoints on alt 1 and
   alt 2 and NONE on alt 0. The FRAMING is not: the two bytes prepended
   to a received frame, the fixed 1514-byte padded transmit and the two
   vendor control requests are what the published Linux driver does, and
   no Apple document says so.

   A suite cannot test whether a guess about somebody else's firmware is
   right. What it CAN do, and what this one does, is pin the guess down
   so precisely that the HP can refute it in one screen:

     - the transmit really sends 1514 bytes for a 60-byte frame, and the
       padding really is zero
     - the receive really drops exactly two bytes, and really records
       the transfer BEFORE dropping them
     - every refusal is distinct, so "tethering did not work" is never
       four reasons wearing one face -- v0.8.6.5's lesson, which was
       about `update` naming which of four reasons a stick was not used

   ── THE CORE IS REAL HERE ────────────────────────────────────────────

   tests/test_msc.c stubs usb_control() and usb_bulk() and tests the
   driver alone. This suite links the REAL usb/core.c and registers a
   simulated host controller through usb_register_hc(), then drives
   usb_enumerate() -- so usb_select_config(), usb_set_interface() and
   the descriptor walk are the kernel's own, and what is simulated is
   the controller and the phone behind it.

   That matters because the driver's whole job is three requests in the
   right order against a device that can refuse any of them, and a
   stubbed core would let the driver pass while the core's half of
   alternate settings was wrong.
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

#include "../include/types.h"
#include "../include/usb.h"
#include "../include/netdev.h"

/* ── the clock usb/ethernet.c rate-limits its carrier check by ──────── */
static uint32_t g_ms;
uint32_t klog_now_ms(void){ return g_ms; }

/* klog goes nowhere here, but the driver's refusal messages are part of
   what it does -- so they are COUNTED, and a test can require that a
   refusal said something. A driver that fails silently is the thing
   v0.8.6.5 was about. */
static int g_logs;
void klog(int level, const char *fmt, ...){ (void)level; (void)fmt; g_logs++; }

/* ════════════════════════════════════════════════════════════════════
   The simulated phone
   ════════════════════════════════════════════════════════════════════

   Four configurations, booting into index 0 which is PTP, with Apple
   USB Ethernet on interface 2 of index 3 -- the layout `usb cfg`
   actually printed. Alt 0 of that interface has no endpoints and alts 1
   and 2 have a bulk pair each, which is the detail the whole release
   turns on.
   ════════════════════════════════════════════════════════════════════ */

#define EP_IN   0x83
#define EP_OUT  0x04

/* index 0: still image only -- what the phone presents until told. */
static const uint8_t CFG0[] = {
    9, 2, 25,0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 6, 1, 1, 0,
    7, 5, 0x81, 2, 64,0, 0
};
/* index 1: audio, with an alternate that carries the endpoint. No
   Ethernet anywhere in it -- the driver must walk past this one. */
static const uint8_t CFG1[] = {
    9, 2, 43,0, 3, 2, 0, 0xA0, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 4, 1, 0, 0, 1, 2, 0, 0,
    9, 4, 1, 1, 1, 1, 2, 0, 0,
    7, 5, 0x82, 1, 64,0, 1
};
/* index 2: PTP plus usbmux. Still no Ethernet. */
static const uint8_t CFG2[] = {
    9, 2, 34,0, 2, 3, 0, 0xA0, 50,
    9, 4, 0, 0, 0, 6, 1, 1, 0,
    9, 4, 1, 0, 1, 0xFF, 0xFE, 0x02, 0,
    7, 5, 0x81, 2, 64,0, 0
};
/* index 3: THE ONE. Ethernet on interface 2, endpoints on alts 1 and 2,
   nothing on alt 0. */
static const uint8_t CFG3[] = {
    9, 2, 75,0, 3, 4, 0, 0xA0, 50,
    9, 4, 0, 0, 0, 6, 1, 1, 0,            /* if 0: PTP                 */
    9, 4, 1, 0, 1, 0xFF, 0xFE, 0x02, 0,   /* if 1: usbmux              */
    7, 5, 0x81, 2, 64,0, 0,
    9, 4, 2, 0, 0, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 0: NO ENDPOINTS  */
    9, 4, 2, 1, 2, 0xFF, 0xFD, 0x01, 0,   /* if 2 alt 1: the pair      */
    7, 5, EP_IN,  2, 0x00,0x02, 0,
    7, 5, EP_OUT, 2, 0x00,0x02, 0,
    9, 4, 2, 2, 2, 0xFF, 0xFD, 0x01, 0    /* if 2 alt 2: NCM, unused   */
};

static const uint8_t *g_cfgs[4] = { CFG0, CFG1, CFG2, CFG3 };
static const uint16_t g_cfglen[4] = {
    (uint16_t)sizeof CFG0, (uint16_t)sizeof CFG1,
    (uint16_t)sizeof CFG2, (uint16_t)sizeof CFG3
};

static const uint8_t PHONE_MAC[6] = { 0x02, 0x1C, 0xB3, 0x44, 0x55, 0x66 };

/* What the phone can be told to do wrong. Every one of these is a thing
   a real one does: a locked phone refuses the configuration change, an
   untrusted one may offer the interface and refuse to switch it, and a
   phone mid-handshake answers the MAC request short. */
static struct {
    int  nconfigs;
    int  refuse_setcfg;
    int  refuse_setif;
    int  refuse_mac;
    int  short_mac;        /* answer the MAC request with 3 bytes      */
    int  refuse_carrier;
    int  carrier_byte;
    int  stall_out;        /* stall the next bulk OUT                  */
    int  no_ethernet;      /* configuration 3 without the interface    */
    int  one_ep;           /* alt 1 with its OUT endpoint not bulk     */

    /* what the host did */
    int  cfg_index_asked;
    int  set_cfg_value;
    int  set_if_num, set_if_alt, set_ifs, set_if_bad_recip;
    int  mac_reqs, carrier_reqs, clear_halts;
    int  out_calls;
    int  out_len;                 /* the length of the LAST bulk OUT   */
    uint8_t out_buf[2048];

    /* a frame waiting to be received, with its two-byte prefix already
       on the front -- because that is how it arrives */
    uint8_t rx[2048];
    int     rx_len;
    int     in_polls;
    int     stall_in;
} ph;

static void phone_reset(void)
{
    memset(&ph, 0, sizeof ph);
    ph.nconfigs     = 4;
    ph.carrier_byte = 0x04;        /* IPHETH_CARRIER_ON                */
    ph.cfg_index_asked = -1;
    ph.set_if_num = ph.set_if_alt = -1;
    g_ms = 10000;
    g_logs = 0;
}

/* ── the simulated controller ──────────────────────────────────────── */

static int s_port_count(struct usb_hc *h){ (void)h; return 1; }
static int s_port_connected(struct usb_hc *h, int p){ (void)h; return p == 0; }

static int s_attach(struct usb_hc *h, int port, usb_dev_t *out)
{
    (void)h;
    if(port != 0) return USB_ENODEV;
    out->addr       = 2;
    out->speed      = USB_SPEED_HIGH;
    out->maxpacket0 = 64;
    out->vendor     = 0x05AC;
    out->product    = 0x12A8;
    out->dev_class  = 0x06;
    out->nconfigs   = (uint8_t)ph.nconfigs;
    return USB_OK;
}

static int s_control(struct usb_hc *h, const usb_dev_t *d, uint8_t rtype,
                     uint8_t req, uint16_t value, uint16_t index,
                     void *data, uint16_t len)
{
    (void)h; (void)d;

    /* GET_DESCRIPTOR(CONFIGURATION, index) */
    if((rtype & 0x80u) && req == USB_REQ_GET_DESCRIPTOR &&
       (value >> 8) == USB_DESC_CONFIG){
        int idx = value & 0xFFu;
        const uint8_t *src;
        uint8_t blob[128];
        int n;
        ph.cfg_index_asked = idx;
        if(idx < 0 || idx >= ph.nconfigs) return USB_ESTALL;
        src = g_cfgs[idx];
        n   = (int)g_cfglen[idx];
        memcpy(blob, src, (size_t)n);
        /* Configuration 3 without the Ethernet interface: its class
           triple blanked, so the survey walks every configuration and
           finds nothing -- which is a phone whose hotspot has never
           been switched on. */
        if(idx == 3 && ph.no_ethernet){
            /* bInterfaceClass is byte 5 of each interface descriptor.
               The offsets are written out rather than guessed at: the
               first version of this blanked byte 30, which is inside an
               ENDPOINT descriptor, so the Ethernet interface was still
               there and the test failed by finding it. */
            blob[34 + 5] = 0x03;     /* if 2 alt 0                     */
            blob[43 + 5] = 0x03;     /* if 2 alt 1                     */
            blob[66 + 5] = 0x03;     /* if 2 alt 2                     */
        }
        /* alt 1 with its second endpoint made an INTERRUPT one, so the
           interface offers one bulk endpoint and not a pair.
           bmAttributes is byte 3 of an endpoint descriptor; the OUT one
           starts at 59. */
        if(idx == 3 && ph.one_ep) blob[59 + 3] = 3;
        if(n > (int)len) n = (int)len;
        if(data) memcpy(data, blob, (size_t)n);
        return n;
    }

    if(!(rtype & 0x80u) && req == USB_REQ_SET_CONFIGURATION){
        if(ph.refuse_setcfg) return USB_ESTALL;
        ph.set_cfg_value = (int)value;
        return 0;
    }

    if(!(rtype & 0x80u) && req == USB_REQ_SET_INTERFACE){
        if((rtype & 0x1Fu) != USB_RECIP_INTERFACE){
            ph.set_if_bad_recip++;
            return USB_ESTALL;
        }
        if(ph.refuse_setif) return USB_ESTALL;
        ph.set_if_alt = (int)value;
        ph.set_if_num = (int)index;
        ph.set_ifs++;
        return 0;
    }

    /* The two VENDOR requests. bmRequestType 0xC0 -- IN, vendor,
       device -- and wIndex 2. */
    if(rtype == 0xC0u && req == 0x00u){          /* GET_MACADDR        */
        ph.mac_reqs++;
        if(index != 2) return USB_ESTALL;
        if(ph.refuse_mac) return USB_ESTALL;
        if(data){
            memcpy(data, PHONE_MAC, 6);
            return ph.short_mac ? 3 : 6;
        }
        return USB_EPARAM;
    }
    if(rtype == 0xC0u && req == 0x45u){          /* CARRIER_CHECK      */
        ph.carrier_reqs++;
        if(index != 2) return USB_ESTALL;
        if(ph.refuse_carrier) return USB_ESTALL;
        if(data && len > 0){
            ((uint8_t*)data)[0] = (uint8_t)ph.carrier_byte;
            return 1;
        }
        return USB_EPARAM;
    }

    /* CLEAR_FEATURE(ENDPOINT_HALT) */
    if(!(rtype & 0x80u) && (rtype & 0x1Fu) == USB_RECIP_ENDPOINT &&
       req == USB_REQ_CLEAR_FEATURE && value == USB_FEATURE_ENDPOINT_HALT){
        ph.clear_halts++;
        return 0;
    }
    return USB_ESTALL;
}

/* ── THE BOUNDS THE REAL BACK-END HAS, v0.8.7.1 ───────────────────────
 *
 * This controller used to accept a transfer of any length, and that is
 * why the suite passed while the driver could not send or receive a
 * single byte on the HP.
 *
 * usb/xhci_hc.c copies every synchronous transfer through g_bounce and
 * every asynchronous IN through g_abuf, and REFUSES anything longer
 * than the buffer -- before the controller is touched. Those buffers
 * were sized for a keyboard report and a 512-byte sector:
 *
 *     g_bounce   1024 bytes   ->  x_data() refused a 1514-byte frame
 *     XHCI_ABUF_SZ  64 bytes  ->  x_intr_poll() refused a 1516-byte read
 *
 * `usb eth` on the laptop reported rx 0/0/19440 and tx 0/48 -- nineteen
 * thousand failures, forty-eight refused frames, and not one byte
 * requested from the device.
 *
 * A simulated back-end that is MORE PERMISSIVE than the real one is the
 * same mistake as a mock that cannot refuse, which include/usb.h has
 * warned about since v0.8.5. So these are the real limits, stated here
 * and compared against the kernel's own constants by
 * tools/check_usb_buffers.py -- the suite covers the behaviour, the
 * build check covers the numbers. */
#define SIM_SYNC_MAX   2048      /* sizeof g_bounce[0]  in xhci_hc.c */
#define SIM_ASYNC_MAX  2048      /* XHCI_ABUF_SZ        in xhci_hc.c */

static int g_sync_refused, g_async_refused;

static int s_bulk(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{
    (void)h; (void)d; (void)mps;
    if(len < 0 || len > SIM_SYNC_MAX){ g_sync_refused++; return USB_EPARAM; }
    if(in){
        /* The blocking path. The driver must NOT use it to receive --
           see s_intr_poll. Counted so a test can prove it did not. */
        ph.in_polls += 1000;
        return USB_ETIMEOUT;
    }
    if(ep != EP_OUT) return USB_ENODEV;
    if(ph.stall_out){ ph.stall_out = 0; return USB_ESTALL; }
    ph.out_calls++;
    ph.out_len = len;
    if(len > 0 && len <= (int)sizeof ph.out_buf && data)
        memcpy(ph.out_buf, data, (size_t)len);
    return len;
}

/* The NON-BLOCKING IN poll: >0 bytes, 0 nothing yet, <0 a failure. The
   whole receive path depends on this contract, because net_poll() runs
   from the shell's idle loop. */
static int s_intr_poll(struct usb_hc *h, const usb_dev_t *d, uint8_t ep,
                       int in, void *data, int len, int mps)
{
    int n;
    (void)h; (void)d; (void)mps;
    if(!in) return USB_EPARAM;
    if(len < 0 || len > SIM_ASYNC_MAX){ g_async_refused++; return USB_EPARAM; }
    if(ep != EP_IN) return USB_ENODEV;
    ph.in_polls++;
    if(ph.stall_in){ ph.stall_in = 0; return USB_ESTALL; }
    if(ph.rx_len == 0) return 0;                 /* nothing waiting    */
    n = ph.rx_len;
    if(n > len) n = len;
    if(data) memcpy(data, ph.rx, (size_t)n);
    ph.rx_len = 0;
    return n;
}

static int  s_configure(struct usb_hc *h, usb_dev_t *d){ (void)h; (void)d; return USB_OK; }
static void s_reset_toggle(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in)
{ (void)h; (void)d; (void)ep; (void)in; }

static const usb_hc_ops_t SIM_OPS = {
    .name           = "SIM",
    .port_count     = s_port_count,
    .port_connected = s_port_connected,
    .attach         = s_attach,
    .control        = s_control,
    .bulk           = s_bulk,
    .intr           = s_bulk,
    .reset_toggle   = s_reset_toggle,
    .intr_poll      = s_intr_poll,
    .configure      = s_configure,
};

/* ── the three things the core calls that are not here ────────────────
   usb/core.c is the kernel's own, so it calls the hub walk and both
   back-end registrations. This suite has no hubs and registers its own
   controller through the public usb_register_hc(), so these are stubs
   -- and usb_hub_scan() returning 0 is not a convenience: a hub found
   on a bus with no hub on it would be a bug in the core worth seeing. */
int  usb_hub_scan(void)      { return 0; }
int  uhci_hc_register(void)  { return 0; }
int  xhci_hc_register(void)  { return 0; }

/* The real core, and the real driver. */
#define NET_NO_HW 1
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../net/netdev.c"
/* v0.8.8.4: usb/core.c prints WHERE an xHCI attach failed, not just
   the code, because "no answer (-4)" named four different bugs at
   once. This suite stubs the controller, so the stub says so rather
   than linking to an empty string. */
const char *xhci_attach_why(void){ return "(xHCI stubbed in this suite)"; }

#include "../usb/core.c"
#include "../usb/ethernet.c"

/* Build an Ethernet frame with the device's two-byte prefix already on
   it, which is how one arrives. */
static void queue_rx(const uint8_t *frame, int flen, uint8_t p0, uint8_t p1)
{
    ph.rx[0] = p0;
    ph.rx[1] = p1;
    memcpy(ph.rx + 2, frame, (size_t)flen);
    ph.rx_len = flen + 2;
}

/* A fresh machine: one simulated controller, one phone, nothing
   registered. The core's controller table is not resettable from
   outside, so the controller is registered ONCE and the device table is
   rebuilt by usb_enumerate(). */
static void machine(void)
{
    phone_reset();
    g_sync_refused = g_async_refused = 0;
    usbnet_release();
    netdev_reset();
    usb_enumerate();
}

int main(void)
{
    uint8_t mac[6], buf[2048];
    int i;

    printf("usbnet: Apple USB Ethernet, against a phone that answers\n");
    usb_register_hc(&SIM_OPS, 0, 0, USB_HC_NONE);

    /* ── the fixtures check themselves first ──────────────────────────
       A configuration descriptor whose wTotalLength disagrees with its
       own length is a blob the walk is entitled to read short, so every
       test after it would be testing the wrong thing for a reason
       nothing printed. CFG3's was 76 for a 75-byte blob on the first
       run. */
    head("the descriptor fixtures describe their own length");
    for(i = 0; i < 4; i++){
        uint32_t tot = (uint32_t)g_cfgs[i][2] | ((uint32_t)g_cfgs[i][3] << 8);
        ck(i == 3 ? "configuration 3 -- the Ethernet one"
                  : "a configuration fixture",
           tot == (uint32_t)g_cfglen[i], (long)tot);
    }
    ck("and interface 2 of configuration 3 is where it is indexed",
       CFG3[34 + 2] == 2 && CFG3[34 + 3] == 0 &&
       CFG3[43 + 2] == 2 && CFG3[43 + 3] == 1 &&
       CFG3[66 + 2] == 2 && CFG3[66 + 3] == 2, CFG3[43 + 3]);

    /* ════════════════════════════════════════════════════════════════
       1. Finding it
       ════════════════════════════════════════════════════════════════ */
    head("the phone boots into PTP, and the driver walks past it");
    machine();
    {
        const usb_dev_t *d = usb_device(0);
        ck("the device enumerated", d && d->valid, d ? 1 : 0);
        ck("into configuration index 0", d && d->cfgidx == 0,
           d ? d->cfgidx : -1);
        ck("which is still image -- not Ethernet",
           d && d->if_class == 6, d ? d->if_class : -1);
        ck("no NIC yet", netdev_count() == 0, netdev_count());
    }

    head("the probe surveys every configuration and finds FF/FD/01");
    {
        int n = usbnet_probe();
        const usb_dev_t *d = usb_device(0);
        ck("one NIC found", n == 1, n);
        ck("and registered with the NIC layer", netdev_count() == 1,
           netdev_count());
        ck("as a USB NIC", netdev_kind(0) == NET_BUS_USBNET,
           netdev_kind(0));
        ck("named for what it is",
           strcmp(netdev_bus_name(0), "Apple USB Ethernet") == 0, 0);
        ck("the device is in configuration index 3",
           d && d->cfgidx == 3, d ? d->cfgidx : -1);
        ck("SET_CONFIGURATION carried its VALUE, 4, not its index",
           ph.set_cfg_value == 4, ph.set_cfg_value);
        ck("interface 2 was adopted", d && d->ifnum == 2,
           d ? d->ifnum : -1);
        ck("AT ALTERNATE SETTING 1, where the endpoints are",
           d && d->altsetting == 1, d ? d->altsetting : -1);
        ck("SET_INTERFACE was sent once", ph.set_ifs == 1, ph.set_ifs);
        ck("with the interface in wIndex and the alt in wValue",
           ph.set_if_num == 2 && ph.set_if_alt == 1, ph.set_if_num);
        ck("ADDRESSED TO THE INTERFACE, not the device",
           ph.set_if_bad_recip == 0, ph.set_if_bad_recip);
        ck("two endpoints", d && d->nep == 2, d ? d->nep : -1);
        ck("the MAC was asked for exactly once", ph.mac_reqs == 1,
           ph.mac_reqs);
    }

    head("the MAC is the phone's, and the NIC layer cached it");
    netdev_mac(0, mac);
    ck("all six bytes", memcmp(mac, PHONE_MAC, 6) == 0, mac[0]);
    ck("and asking again does not ask the phone", ph.mac_reqs == 1,
       ph.mac_reqs);

    head("probing again does not switch an interface that is working");
    {
        int ifs = ph.set_ifs, n;
        n = usbnet_probe();
        ck("nothing new found", n == 0, n);
        ck("and no SET_INTERFACE was sent", ph.set_ifs == ifs,
           ph.set_ifs - ifs);
        ck("one NIC still", netdev_count() == 1, netdev_count());
    }

    /* ════════════════════════════════════════════════════════════════
       2. Every refusal, distinctly
       ════════════════════════════════════════════════════════════════

       v0.8.6.5: `update` saying WHICH of four reasons the stick was not
       used, because each has a different fix and "it only found the
       ESP" was all four wearing one face. The same applies here with
       five reasons, and four of them are things a phone really does. */
    /* ── v0.8.9.9: THE MUX GOT THERE FIRST ───────────────────────────
       On the HP the order was the other way round -- Ethernet at boot,
       usbmux at `pair` -- and the mux took the Ethernet interface away.
       Both orders go through usb_claim_interface() now. This one checks
       the Ethernet side: with the mux already holding interface 1 of
       the same configuration, the probe must not select it again, and
       must find ITS OWN bulk pair rather than the first one on the
       phone -- which is the mux's 0x81. */
    head("the mux already holds interface 1: Ethernet joins, not replaces");
    machine();
    {
        usb_dev_t *d = (usb_dev_t *)usb_device(0);
        int n;
        ck("usbmux claims index 3, interface 1",
           d && usb_claim_interface(d, 3, 1, 0) == USB_OK, 0);
        ph.set_cfg_value = -1;
        n = usbnet_probe();
        ck("the NIC is found", n == 1, n);
        ck("WITHOUT a second SET_CONFIGURATION",
           ph.set_cfg_value == -1, ph.set_cfg_value);
        ck("its receive endpoint is Ethernet's, not the mux's 0x81",
           g_n[0].ep_in == EP_IN, g_n[0].ep_in);
        ck("and its send endpoint too", g_n[0].ep_out == EP_OUT,
           g_n[0].ep_out);
        ck("the mux's endpoint is still in the table",
           d && usb_find_ep_if(d, 1, USB_EP_BULK, 1) &&
           usb_find_ep_if(d, 1, USB_EP_BULK, 1)->addr == 0x81, 0);
        ck("three endpoints in all", d && d->nep == 3, d ? d->nep : -1);
    }

    head("a phone with no Ethernet interface anywhere");
    machine();
    ph.no_ethernet = 1;
    {
        /* Enumeration ALREADY sent one -- setup_config() selects
           descriptor index 0, whose bConfigurationValue is 1. So the
           check is that the PROBE did not send another, which is a
           different question from whether any was ever sent, and the
           first version of this conflated them. */
        int before = ph.set_cfg_value;
        int n;
        ck("enumeration left the phone in its boot configuration",
           before == 1, before);
        n = usbnet_probe();
        ck("nothing found", n == 0, n);
        ck("nothing registered", netdev_count() == 0, netdev_count());
        ck("and the probe sent no SET_CONFIGURATION of its own",
           ph.set_cfg_value == before, ph.set_cfg_value);
        ck("though every configuration WAS surveyed",
           ph.cfg_index_asked == 3, ph.cfg_index_asked);
    }

    head("a phone that refuses SET_CONFIGURATION -- a locked one");
    machine();
    ph.refuse_setcfg = 1;
    {
        int n = usbnet_probe();
        ck("nothing registered", n == 0 && netdev_count() == 0, n);
        ck("and it said so", g_logs > 0, g_logs);
        ck("no SET_INTERFACE followed a refused configuration",
           ph.set_ifs == 0, ph.set_ifs);
    }

    head("a phone that refuses SET_INTERFACE -- an untrusted one");
    machine();
    ph.refuse_setif = 1;
    {
        int n = usbnet_probe();
        ck("nothing registered", n == 0 && netdev_count() == 0, n);
        ck("the configuration WAS selected first",
           ph.set_cfg_value == 4, ph.set_cfg_value);
        ck("and the MAC was never asked for", ph.mac_reqs == 0,
           ph.mac_reqs);
    }

    head("a phone that refuses the MAC request");
    machine();
    ph.refuse_mac = 1;
    {
        int n = usbnet_probe();
        ck("NOT registered", n == 0 && netdev_count() == 0, n);
        ck("the interface was switched, so it got that far",
           ph.set_ifs == 1, ph.set_ifs);
    }

    head("an interface offering ONE bulk endpoint is not a NIC");
    machine();
    ph.one_ep = 1;
    {
        /* A NIC that can send and not receive is not half a NIC, it is
           a link that silently never carries a reply. Registering one
           puts it in the table and makes it the active adapter. */
        int n = usbnet_probe();
        ck("NOT registered", n == 0 && netdev_count() == 0, n);
        ck("it got as far as switching the interface", ph.set_ifs == 1,
           ph.set_ifs);
        ck("and never asked for the MAC", ph.mac_reqs == 0, ph.mac_reqs);
    }

    head("A SHORT MAC IS NOT AN ADDRESS -- three bytes is refused");
    machine();
    ph.short_mac = 1;
    {
        int n = usbnet_probe();
        /* Registering it would put a NIC in the table whose address is
           half the phone's and half zero, and net/arp.c would claim it
           in a reply -- poisoning every ARP cache on the network. The
           cached MAC in net/netdev.c exists to stop exactly that, and
           registering a short answer would put it back. */
        ck("NOT registered", n == 0 && netdev_count() == 0, n);
        ck("and the NIC layer holds no half-address",
           netdev_count() == 0, netdev_count());
    }

    head("a NIC layer that is FULL leaves this driver holding nothing");
    machine();
    {
        /* NETDEV_MAX is four. Filling it with other adapters makes
           netdev_register_usb() refuse, and the slot this driver had
           already filled in has to go back -- an entry in one table
           and not the other is the two-sides-disagreeing failure this
           release keeps running into. */
        static const netdev_ops_t filler = { "SIM-filler", 0, un_recv, 0, 0 };
        int n;
        for(i = 0; i < NETDEV_MAX; i++)
            netdev_register(&filler, NET_BUS_E1000, i);
        ck("the NIC table is full", netdev_count() == NETDEV_MAX,
           netdev_count());
        n = usbnet_probe();
        ck("the probe registers nothing", n == 0, n);
        ck("THE DRIVER HOLDS NO UNIT EITHER", usbnet_count() == 0,
           usbnet_count());
        ck("...and its slot is not marked used", g_n[0].used == 0,
           g_n[0].used);
        ck("the NIC table is unchanged", netdev_count() == NETDEV_MAX,
           netdev_count());
    }

    /* ════════════════════════════════════════════════════════════════
       3. Transmit: the fixed length, which is the odd part
       ════════════════════════════════════════════════════════════════ */
    head("a frame the size of the buffer sends exactly one transfer");
    machine();
    usbnet_probe();
    {
        static uint8_t big[1514];
        memset(big, 0x5A, sizeof big);
        ck("it goes", netdev_send(0, big, 1514) == 0, 0);
        ck("one bulk OUT of 1514", ph.out_calls == 1 && ph.out_len == 1514,
           ph.out_len);
    }

    head("TX sends 1514 BYTES for a 60-byte frame, zero-padded");
    {
        uint8_t frame[60];
        int     j, zeros = 1;
        /* THE FULL-LENGTH FRAME ABOVE RUNS FIRST, deliberately: the
           transmit buffer is a static, so on a fresh probe it is
           already zero and a missing memset cannot be seen. The
           harness caught that -- removing the padding was MISSED --
           and the fix is to dirty the buffer with a real frame before
           asking whether the padding is clean. */
        for(j = 0; j < 60; j++) frame[j] = (uint8_t)(0xA0 + j);
        ph.out_calls = 0;
        ck("the frame goes", netdev_send(0, frame, 60) == 0, 0);
        ck("one bulk OUT", ph.out_calls == 1, ph.out_calls);
        ck("OF 1514 BYTES, not 60 -- the thing that is easy to get "
           "wrong", ph.out_len == 1514, ph.out_len);
        ck("the frame is at the front, unaltered",
           memcmp(ph.out_buf, frame, 60) == 0, ph.out_buf[0]);
        for(j = 60; j < 1514; j++) if(ph.out_buf[j]){ zeros = 0; break; }
        ck("and every byte after it is ZERO, not the last frame's 0x5A",
           zeros, j);
    }

    head("THE WIRE: a frame too long is refused and reaches no endpoint");
    {
        static uint8_t over[1600];
        memset(over, 0x11, sizeof over);
        ph.out_calls = 0;
        /* Truncating it would be worse than refusing: the far end gets
           a short frame with a valid header and no way to know. */
        ck("1515 bytes is refused", netdev_send(0, over, 1515) < 0, 0);
        ck("...without a bulk OUT", ph.out_calls == 0, ph.out_calls);
        ck("a zero-length frame is refused", netdev_send(0, over, 0) < 0, 0);
        ck("...without a bulk OUT", ph.out_calls == 0, ph.out_calls);
    }

    head("a stalled bulk OUT is cleared, so the NEXT frame goes");
    {
        uint8_t frame[64];
        int halts = ph.clear_halts;
        memset(frame, 0x77, sizeof frame);
        ph.out_calls = 0;
        ph.stall_out = 1;
        ck("the stalled frame is reported as failed",
           netdev_send(0, frame, 64) < 0, 0);
        ck("the halt was cleared", ph.clear_halts == halts + 1,
           ph.clear_halts - halts);
        ck("and the next frame goes", netdev_send(0, frame, 64) == 0, 0);
        ck("...reaching the endpoint", ph.out_calls == 1, ph.out_calls);
    }

    /* ════════════════════════════════════════════════════════════════
       4. Receive: the two bytes, and the evidence about them
       ════════════════════════════════════════════════════════════════ */
    head("RX drops exactly two bytes from the front");
    machine();
    usbnet_probe();
    {
        uint8_t frame[64];
        int     r;
        for(i = 0; i < 64; i++) frame[i] = (uint8_t)(0x10 + i);
        queue_rx(frame, 64, 0xDE, 0xAD);
        r = (int)netdev_recv(0, buf, sizeof buf);
        ck("the frame comes out at its own length", r == 64, r);
        ck("WITHOUT the two prefix bytes",
           memcmp(buf, frame, 64) == 0, buf[0]);
        ck("so the first byte out is the frame's, not 0xDE",
           buf[0] == 0x10, buf[0]);
    }

    head("NOTHING WAITING IS NOT AN ERROR, and must not block");
    {
        int polls = ph.in_polls;
        ck("an empty endpoint answers 0", netdev_recv(0, buf, sizeof buf)
           == 0, 0);
        ck("the NON-BLOCKING poll was used", ph.in_polls == polls + 1,
           ph.in_polls - polls);
        /* s_bulk adds 1000 to in_polls for a blocking IN. net_poll()
           runs from the shell's idle loop and nothing here runs on a
           timer, so a receive that waits is a shell that stops
           answering -- v0.8.5's keyboard at one character per second,
           one endpoint type along. */
        ck("...and the BLOCKING path was not", ph.in_polls < 1000,
           ph.in_polls);
    }

    head("A MAXIMUM-SIZE FRAME needs room for the prefix as well");
    {
        static uint8_t frame[1514];
        int r;
        /* IPHETH_RX_LEN is the transmit length PLUS the two prefix
           bytes. Without the extra room a full-length frame is read
           short and the last two bytes of every maximum-size packet
           are lost -- which the harness caught this suite missing,
           because nothing in it ever received a full-length frame. */
        for(i = 0; i < 1514; i++) frame[i] = (uint8_t)(i & 0xFF);
        queue_rx(frame, 1514, 0x11, 0x22);
        r = (int)netdev_recv(0, buf, sizeof buf);
        ck("all 1514 bytes come out", r == 1514, r);
        ck("...including the last two",
           buf[1512] == frame[1512] && buf[1513] == frame[1513],
           buf[1513]);
    }

    head("a transfer with nothing but the prefix is a runt, not a frame");
    {
        /* THE COUNTERS, not the return value. A two-byte transfer
           yields a zero-length frame either way -- memcpy of nothing
           returns nothing -- so `netdev_recv() == 0` cannot tell a
           refused runt from a counted phantom. The harness caught
           that. rx_frames is the wire. */
        uint32_t frames = g_n[0].rx_frames, runts = g_n[0].rx_runts;
        ph.rx_len = 2;
        ck("a two-byte transfer yields no frame",
           netdev_recv(0, buf, sizeof buf) == 0, 0);
        ck("...and is NOT counted as one",
           g_n[0].rx_frames == frames, (long)(g_n[0].rx_frames - frames));
        ck("...it is counted as a runt",
           g_n[0].rx_runts == runts + 1,
           (long)(g_n[0].rx_runts - runts));
        ph.rx_len = 1;
        ck("and neither does a single byte",
           netdev_recv(0, buf, sizeof buf) == 0, 0);
        ck("...also a runt", g_n[0].rx_runts == runts + 2,
           (long)(g_n[0].rx_runts - runts));
    }

    head("a frame longer than the caller's buffer is truncated, not "
         "overrun");
    {
        static uint8_t frame[1000];
        int r;
        memset(frame, 0x3C, sizeof frame);
        queue_rx(frame, 1000, 0, 0);
        memset(buf, 0, 200);
        r = (int)netdev_recv(0, buf, 128);
        ck("128 bytes come out", r == 128, r);
        ck("the 129th byte of the caller's buffer is untouched",
           buf[128] == 0, buf[128]);
    }

    head("a stalled bulk IN is cleared and reported as nothing yet");
    {
        int halts = ph.clear_halts;
        ph.stall_in = 1;
        ck("no frame", netdev_recv(0, buf, sizeof buf) == 0, 0);
        ck("the halt was cleared", ph.clear_halts == halts + 1,
           ph.clear_halts - halts);
    }

    head("THE EVIDENCE: the first transfer is kept BEFORE anything is "
         "stripped");
    machine();
    usbnet_probe();
    {
        uint8_t frame[64];
        memset(frame, 0xFF, 6);               /* a broadcast destination */
        for(i = 6; i < 64; i++) frame[i] = (uint8_t)i;
        queue_rx(frame, 64, 0x01, 0x02);
        netdev_recv(0, buf, sizeof buf);
        /* usbnet_dump() prints these. The two bytes in front of a
           broadcast's FF FF FF FF FF FF are what settles whether
           IPHETH_RX_SKIP is 2 -- and a receive path that silently
           stripped them could not be asked. */
        ck("the length as it arrived, 66 and not 64",
           g_n[0].first_len == 66, g_n[0].first_len);
        ck("byte 0 is the PREFIX, not the frame", g_n[0].first[0] == 0x01,
           g_n[0].first[0]);
        ck("byte 1 too", g_n[0].first[1] == 0x02, g_n[0].first[1]);
        ck("and the broadcast starts at byte 2",
           g_n[0].first[2] == 0xFF && g_n[0].first[7] == 0xFF,
           g_n[0].first[2]);
        ck("the dump reports one adapter", usbnet_dump() == 1, 0);
    }

    head("...and it is the FIRST one, not the latest");
    {
        uint8_t other[64];
        memset(other, 0x99, sizeof other);
        queue_rx(other, 64, 0x55, 0x66);
        netdev_recv(0, buf, sizeof buf);
        ck("still the first transfer's prefix", g_n[0].first[0] == 0x01,
           g_n[0].first[0]);
    }

    /* ════════════════════════════════════════════════════════════════
       5. Carrier
       ════════════════════════════════════════════════════════════════ */
    head("the carrier byte is 0x04 and nothing else will do");
    machine();
    usbnet_probe();
    {
        ph.carrier_byte = 0x04;
        ck("0x04 is a carrier", netdev_link_up(0) == 1, 0);
        g_ms += 2000;
        ph.carrier_byte = 0x00;
        ck("0x00 is not", netdev_link_up(0) == 0, 0);
        g_ms += 2000;
        ph.carrier_byte = 0x05;
        ck("and neither is 0x05 -- it is one value, not a flag",
           netdev_link_up(0) == 0, 0);
    }

    head("A CARRIER CHECK IS A CONTROL TRANSFER, so it is rate-limited");
    {
        int reqs;
        g_ms += 2000;
        ph.carrier_byte = 0x04;
        reqs = ph.carrier_reqs;
        ck("the first ask reaches the phone",
           (netdev_link_up(0), ph.carrier_reqs == reqs + 1),
           ph.carrier_reqs - reqs);
        reqs = ph.carrier_reqs;
        for(i = 0; i < 50; i++) netdev_link_up(0);
        ck("fifty more within the second reach it NOT AT ALL",
           ph.carrier_reqs == reqs, ph.carrier_reqs - reqs);
        ck("...and all fifty answered up", netdev_link_up(0) == 1, 0);
        g_ms += 1001;
        ck("after a second it asks again",
           (netdev_link_up(0), ph.carrier_reqs == reqs + 1),
           ph.carrier_reqs - reqs);
    }

    head("THE FIRST SECOND OF UPTIME: the first check still asks");
    {
        /* `carrier_asked` starting clear is what makes the first call
           ask. Dropping it is only observable while the clock is still
           below the rate limit -- which is the first second after
           boot, when the shell is drawing its first screen. The
           harness caught this suite missing it, because every other
           carrier test runs with the clock well past 1000 ms. */
        int reqs;
        machine();
        g_ms = 400;                 /* 0.4 s since boot */
        usbnet_probe();
        reqs = ph.carrier_reqs;
        ph.carrier_byte = 0x04;
        ck("the link reports UP on the very first ask",
           netdev_link_up(0) == 1, 0);
        ck("...because it really asked the phone",
           ph.carrier_reqs == reqs + 1, ph.carrier_reqs - reqs);
    }

    head("a phone that will not answer keeps its LAST known carrier");
    machine();
    usbnet_probe();
    {
        g_ms += 2000;
        ph.carrier_byte = 0x04;
        ck("carrier is up", netdev_link_up(0) == 1, 0);
        g_ms += 2000;
        ph.refuse_carrier = 1;
        /* "Down" would be an invention. One failed control transfer
           dropping a working link is worse than a stale answer, and a
           device that has actually gone away is noticed by the NIC
           layer's own index check. */
        ck("a refused check does not invent a dead link",
           netdev_link_up(0) == 1, 0);
        ck("...but it did ask", ph.carrier_reqs > 0, ph.carrier_reqs);
    }

    /* ════════════════════════════════════════════════════════════════
       6. Letting go
       ════════════════════════════════════════════════════════════════ */
    head("release unregisters from the NIC layer AND clears the driver");
    machine();
    usbnet_probe();
    ck("one NIC", netdev_count() == 1, netdev_count());
    usbnet_release();
    ck("none after release", netdev_count() == 0, netdev_count());
    ck("and the driver holds no unit", usbnet_count() == 0,
       usbnet_count());
    ck("a send through the layer reaches nothing",
       netdev_send(0, "x", 1) < 0, 0);

    head("a rescan: release, enumerate, probe -- and it comes back");
    {
        int n;
        usb_enumerate();
        n = usbnet_probe();
        ck("found again", n == 1, n);
        ck("registered again", netdev_count() == 1, netdev_count());
        netdev_mac(0, mac);
        ck("with the same address",
           memcmp(mac, PHONE_MAC, 6) == 0, mac[0]);
        ck("and it is the active NIC, there being no other",
           netdev_active() == 0, netdev_active());
    }

    head("a NIC that is up is what net_ready() would be asking about");
    {
        ck("the active NIC reports its link",
           netdev_link_up(netdev_active()) == 1, 0);
        ck("and its bus word is the phone's",
           strcmp(netdev_bus_name(netdev_active()),
                  "Apple USB Ethernet") == 0, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       7. The back-end's buffers, which the first boot on metal found
       ════════════════════════════════════════════════════════════════

       The driver's frame is 1514 bytes out and 1516 in. The real xHCI
       back-end copies both through fixed buffers and refuses anything
       longer WITHOUT touching the controller, so a driver asking for
       more than they hold does not fail slowly -- it never reaches the
       device at all, and every counter it keeps says "error".

       `usb eth` on the HP: rx 0/0/19440, tx 0/48. */
    head("every transfer the driver makes fits the back-end's buffers");
    machine();
    usbnet_probe();
    {
        static uint8_t big[1514];
        uint8_t frame[64];
        memset(big, 0x5A, sizeof big);
        memset(frame, 0x22, sizeof frame);

        ck("a full-length frame is not refused by the back-end",
           (netdev_send(0, big, 1514) == 0) && g_sync_refused == 0,
           g_sync_refused);
        ck("...and it really reached the controller", ph.out_calls > 0,
           ph.out_calls);
        ck("the transmit length is within the synchronous bound",
           ph.out_len <= SIM_SYNC_MAX, ph.out_len);

        queue_rx(frame, 64, 0, 0);
        ck("a receive is not refused by the back-end",
           (netdev_recv(0, buf, sizeof buf) == 64) && g_async_refused == 0,
           g_async_refused);
        ck("...and the poll really reached the controller",
           ph.in_polls > 0, ph.in_polls);
    }

    head("and a back-end that REFUSES is reported, not silently retried");
    {
        /* The failure mode that actually happened: the back-end says
           USB_EPARAM before the controller is touched. The driver must
           count that as an error rather than as "nothing waiting",
           because "nothing waiting" is indistinguishable from a quiet
           link and that is how 19,440 failures looked like idleness. */
        uint32_t errs = g_n[0].rx_errs;
        int polls = ph.in_polls;
        ph.rx_len = 0;
        ck("an idle endpoint is NOT an error",
           (netdev_recv(0, buf, sizeof buf) == 0) &&
           g_n[0].rx_errs == errs, (long)(g_n[0].rx_errs - errs));
        ck("...and it did reach the controller", ph.in_polls == polls + 1,
           ph.in_polls - polls);
        ph.stall_in = 1;
        ck("a refused poll IS an error",
           (netdev_recv(0, buf, sizeof buf) == 0) &&
           g_n[0].rx_errs == errs + 1, (long)(g_n[0].rx_errs - errs));
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
