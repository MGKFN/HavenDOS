/* ════════════════════════════════════════════════════════════════════
   test_usbhid.c -- USB keyboards and mice, on EVERY interface.

   ── WHY THIS SUITE EXISTS (v0.9.2.1) ─────────────────────────────────

   Kyan's HP, 11 November: an external USB keyboard and a mouse's
   wireless receiver, and neither did anything.

   usb/hid.c had only ever looked at the interface the core chose,
   which is the device's FIRST. A plain wired mouse has one interface
   and that was enough. A wireless receiver does not: the common
   layout is a boot keyboard on interface 0, a boot mouse on interface
   1 and a vendor HID interface after them, so the mouse half of a
   mouse's own receiver was never looked at.

   Like tests/test_usbnet.c this links the REAL usb/core.c -- the
   survey, usb_claim_interface() and the endpoint table are the
   kernel's -- and simulates the controller and the devices behind it.
   What it proves is that the driver finds every boot interface, sends
   its class requests to the right one, routes each endpoint's reports
   to the right queue, and does not reconfigure a device on a rescan.

   ── WHAT IT CANNOT PROVE ────────────────────────────────────────────

   That Kyan's receiver is laid out this way. `usb -vv` on the HP is
   the answer to that, and the release notes ask for it.
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
#include "../include/klog.h"

static uint32_t g_ms;
uint32_t klog_now_ms(void){ return g_ms; }
static int g_logs, g_warns;
void klog(int level, const char *fmt, ...)
{
    (void)fmt; g_logs++;
    if(level == KLOG_WARN) g_warns++;
}

/* ── where the input goes ──────────────────────────────────────────── */
static int g_keys[64], g_nkeys;
static uint8_t g_mouse[64]; static int g_nmouse;
static int g_mouse_attach;
void keyboard_inject(int c){ if(g_nkeys < 64) g_keys[g_nkeys++] = c; }
/* v0.9.6: the modifier byte, handed over on every report for Alt. */
static int g_mods = -1, g_nmods;
void keyboard_usb_mods(uint8_t m){ g_mods = m; g_nmods++; }
void mouse_feed(uint8_t b){ if(g_nmouse < 64) g_mouse[g_nmouse++] = b; }
static int g_wheel_fed;
void mouse_wheel_add(int n){ g_wheel_fed += n; }
void mouse_usb_attach(void){ g_mouse_attach++; }
void mouse_usb_forget_all(void){ g_mouse_attach = 0; }

/* ════════════════════════════════════════════════════════════════════
   The devices
   ════════════════════════════════════════════════════════════════════ */
#define HIDD(len) 9, 0x21, 0x11, 0x01, 0, 1, 0x22, (len), 0
/* A wireless receiver: boot keyboard on 0, boot mouse on 1, a vendor
   HID interface on 2 -- the layout of most of them. */
static const uint8_t RECEIVER[] = {
    9, 2, 84, 0, 3, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,  HIDD(59),  7, 5, 0x81, 3, 8, 0, 8,
    9, 4, 1, 0, 1, 3, 1, 2, 0,  HIDD(52),  7, 5, 0x82, 3, 8, 0, 1,
    9, 4, 2, 0, 1, 3, 0, 0, 0,  HIDD(83),  7, 5, 0x83, 3, 32, 0, 1,
};
/* A receiver whose FIRST interface is the vendor one: the core selects
   it, and before 0.9.2.1 nothing at all was driven. */
static const uint8_t VENDOR_FIRST[] = {
    9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,  HIDD(83),  7, 5, 0x81, 3, 32, 0, 1,
    9, 4, 1, 0, 1, 3, 1, 2, 0,  HIDD(52),  7, 5, 0x82, 3, 8, 0, 1,
};
/* A wired keyboard: one interface, as everything 0.8.5 met. */
static const uint8_t KEYBOARD[] = {
    9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,  HIDD(63),  7, 5, 0x81, 3, 8, 0, 10,
};
/* A gaming keyboard: boot keyboard on 0, its N-key-rollover report on
   a non-boot 1. */
static const uint8_t NKRO_KBD[] = {
    9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,  HIDD(63),  7, 5, 0x81, 3, 8, 0, 1,
    9, 4, 1, 0, 1, 3, 0, 0, 0,  HIDD(90),  7, 5, 0x82, 3, 32, 0, 1,
};
/* v0.9.7: a wheel mouse, as real ones are: its wheel only in REPORT
   protocol. One boot-mouse interface, and the descriptor below. */
static const uint8_t WHEELMOUSE[] = {
    9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 2, 0,  HIDD(66),  7, 5, 0x81, 3, 8, 0, 10,
};
/* report id 1: five buttons + 3 pad bits, X and Y 16-bit relative,
   the wheel 8-bit relative -- seven bytes with the id */
static const uint8_t WHEEL_RD[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x03, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0x80, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x06,
    0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0xC0, 0xC0,
};
/* a tablet-like one: X and Y ABSOLUTE (Input 0x02). Read as deltas, the
   pointer would fly to a corner with every report: boot protocol. */
static const uint8_t ABS_RD[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x03, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0x80, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x02,
    0xC0, 0xC0,
};
/* and one whose descriptor has no X and Y: it must stay in boot */
static const uint8_t NOXY_RD[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01, 0xC0,
};

/* Nine boot keyboards in one device: more than the table holds. */
static uint8_t NINE[9 + 9 * 25];

typedef struct {
    const uint8_t *cfg; int len;
    uint16_t vid, pid;
    int refuse_proto_if;          /* SET_PROTOCOL stalls on this, or -1 */
    uint8_t rep[8][8]; int replen[8];   /* a report waiting, per EP n */
    const uint8_t *rdesc; int rdesc_len;  /* v0.9.7: its report descriptor */
} dev_sim_t;

static dev_sim_t g_sim[2];
static int g_nports;
static int g_setcfg, g_configures, g_refuse_configure;
static int g_proto_idx[16], g_proto_val[16], g_nproto;
static int g_idle_idx[16], g_nidle;

static void sim_reset(void)
{
    memset(g_sim, 0, sizeof g_sim);
    g_nports = 0; g_setcfg = g_configures = g_refuse_configure = 0;
    g_nproto = g_nidle = 0;
    g_logs = g_warns = 0;
    g_nkeys = g_nmouse = 0;
    g_sim[0].refuse_proto_if = g_sim[1].refuse_proto_if = -1;
}
static void plug(int port, const uint8_t *cfg, int len, uint16_t vid, uint16_t pid)
{
    g_sim[port].cfg = cfg; g_sim[port].len = len;
    g_sim[port].vid = vid; g_sim[port].pid = pid;
    if(port + 1 > g_nports) g_nports = port + 1;
}

/* ── the simulated controller ──────────────────────────────────────── */
static int s_port_count(struct usb_hc *h){ (void)h; return 2; }
static int s_port_connected(struct usb_hc *h, int p){ (void)h; return p < g_nports && g_sim[p].cfg; }

static int s_attach(struct usb_hc *h, int port, usb_dev_t *out)
{
    (void)h;
    if(port >= g_nports || !g_sim[port].cfg) return USB_ENODEV;
    out->addr       = (uint8_t)(port + 2);
    out->speed      = USB_SPEED_FULL;
    out->maxpacket0 = 8;
    out->vendor     = g_sim[port].vid;
    out->product    = g_sim[port].pid;
    out->dev_class  = 0;
    out->nconfigs   = 1;
    return USB_OK;
}

static dev_sim_t *sim_of(const usb_dev_t *d)
{
    int p = (int)d->addr - 2;
    return (p >= 0 && p < 2) ? &g_sim[p] : 0;
}

static int s_control(struct usb_hc *h, const usb_dev_t *d, uint8_t rtype,
                     uint8_t req, uint16_t value, uint16_t index,
                     void *data, uint16_t len)
{
    dev_sim_t *s = sim_of(d);
    (void)h;
    if(!s) return USB_ENODEV;
    if((rtype & 0x80u) && req == USB_REQ_GET_DESCRIPTOR &&
       (value >> 8) == USB_DESC_CONFIG){
        int n = s->len;
        if((value & 0xFFu) != 0) return USB_ESTALL;
        if(n > (int)len) n = (int)len;
        if(data) memcpy(data, s->cfg, (size_t)n);
        return n;
    }
    if(!(rtype & 0x80u) && req == USB_REQ_SET_CONFIGURATION){ g_setcfg++; return 0; }
    if(rtype == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) &&
       req == USB_REQ_GET_DESCRIPTOR && value == 0x2200){
        int n = s->rdesc_len;
        if(!s->rdesc) return USB_ESTALL;
        if(n > (int)len) n = (int)len;
        if(data) memcpy(data, s->rdesc, (size_t)n);
        return n;
    }
    if(rtype == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) && req == 0x0A){
        if(g_nidle < 16) g_idle_idx[g_nidle++] = index;
        return USB_ESTALL;                  /* allowed to stall */
    }
    if(rtype == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) && req == 0x0B){
        if((int)index == s->refuse_proto_if) return USB_ESTALL;
        if(g_nproto < 16){ g_proto_val[g_nproto] = value; g_proto_idx[g_nproto++] = index; }
        return 0;
    }
    return USB_ESTALL;
}

static int s_bulk(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in,
                  void *data, int len, int mps)
{ (void)h; (void)d; (void)ep; (void)in; (void)data; (void)len; (void)mps; return USB_ETIMEOUT; }

static int s_intr_poll(struct usb_hc *h, const usb_dev_t *d, uint8_t ep,
                       int in, void *data, int len, int mps)
{
    dev_sim_t *s = sim_of(d);
    int n, k = ep & 7;
    (void)h; (void)mps;
    if(!in || !s) return USB_EPARAM;
    /* Only endpoints the controller was told about exist. */
    {
        int i, held = 0;
        for(i = 0; i < d->nep; i++) if(d->ep[i].addr == ep) held = 1;
        if(!held) return USB_ENODEV;
    }
    n = s->replen[k];
    if(!n) return 0;
    if(n > len) n = len;
    memcpy(data, s->rep[k], (size_t)n);
    s->replen[k] = 0;
    return n;
}

static int s_configure(struct usb_hc *h, usb_dev_t *d)
{
    (void)h; (void)d;
    g_configures++;
    if(g_refuse_configure && g_configures > 1) return USB_EXFER;
    return USB_OK;
}
static void s_reset_toggle(struct usb_hc *h, const usb_dev_t *d, uint8_t ep, int in)
{ (void)h; (void)d; (void)ep; (void)in; }

static const usb_hc_ops_t SIM_OPS = {
    .name = "SIM", .port_count = s_port_count, .port_connected = s_port_connected,
    .attach = s_attach, .control = s_control, .bulk = s_bulk, .intr = s_bulk,
    .reset_toggle = s_reset_toggle, .intr_poll = s_intr_poll, .configure = s_configure,
};

int  usb_hub_scan(void)      { return 0; }
int  uhci_hc_register(void)  { return 0; }
int  xhci_hc_register(void)  { return 0; }
#include "kpollguard.inc"
const char *xhci_attach_why(void){ return "(xHCI stubbed in this suite)"; }
/* v0.9.7.6: usb/core.c waits between attach tries; nothing here is
   slow to answer, so the wait is nothing */
void sleep_ms(uint32_t ms){ (void)ms; }

#include "../usb/core.c"
#include "../usb/hid_rd.c"
#include "../usb/hid.c"

static void report(int port, int ep, const uint8_t *r, int n)
{
    memcpy(g_sim[port].rep[ep & 7], r, (size_t)n);
    g_sim[port].replen[ep & 7] = n;
}
static int proto_sent_to(int idx)
{
    int i;
    for(i = 0; i < g_nproto; i++) if(g_proto_idx[i] == idx) return 1;
    return 0;
}

int main(void)
{
    static const uint8_t REP_A[8]   = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    static const uint8_t REP_NONE[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t MOVE[4]    = { 0x01, 5, (uint8_t)-3, 0 };
    int n, i, cfgs, setc;

    printf("usbhid: keyboards and mice, on every interface\n");
    usb_register_hc(&SIM_OPS, 0, 0, USB_HC_NONE);

    head("the fixtures describe their own length");
    ck("the receiver",      RECEIVER[2] == sizeof RECEIVER, RECEIVER[2]);
    ck("vendor-first",      VENDOR_FIRST[2] == sizeof VENDOR_FIRST, VENDOR_FIRST[2]);
    ck("the keyboard",      KEYBOARD[2] == sizeof KEYBOARD, KEYBOARD[2]);
    ck("the NKRO keyboard", NKRO_KBD[2] == sizeof NKRO_KBD, NKRO_KBD[2]);

    /* ── the receiver ─────────────────────────────────────────────── */
    head("a wireless receiver: keyboard on 0, mouse on 1, vendor on 2");
    sim_reset(); plug(0, RECEIVER, sizeof RECEIVER, 0x046D, 0xC52B);
    usb_enumerate();
    n = usb_hid_init();
    ck("TWO devices are driven from one receiver", n == 2, n);
    ck("  ... boot protocol was asked of interface 0 AND interface 1",
       proto_sent_to(0) && proto_sent_to(1), g_nproto);
    ck("  ... and NOT of the vendor interface 2", !proto_sent_to(2), g_nproto);
    ck("  ... which is named in the log as not boot protocol", g_warns >= 1, g_warns);
    ck("  ... a pointer is present", g_mouse_attach == 1, g_mouse_attach);
    ck("  ... the device was configured ONCE (claimed beside, not re-selected)",
       g_setcfg == 1, g_setcfg);
    report(0, 0x81, REP_A, 8);
    report(0, 0x82, MOVE, 4);
    usb_hid_poll();
    ck("a key on endpoint 81 arrives as 'a'", g_nkeys == 1 && g_keys[0] == 'a', g_nkeys);
    ck("movement on endpoint 82 arrives as a PS/2 packet: buttons, +5, +3",
       g_nmouse == 3 && g_mouse[0] == 0x09 && g_mouse[1] == 5 && g_mouse[2] == 3,
       g_nmouse ? g_mouse[0] : -1);
    report(0, 0x81, REP_NONE, 8); usb_hid_poll();
    ck("a release emits nothing", g_nkeys == 1, g_nkeys);

    head("a rescan does not reconfigure a keyboard somebody is typing on");
    cfgs = g_configures; setc = g_setcfg; g_nproto = 0;
    n = usb_hid_init();
    ck("still two", n == 2, n);
    /* After the claim the core's `ifnum` is interface 1, so a driver that
       sent its requests to d->ifnum would ask interface 1 twice here.
       The harness found that only a rescan could tell. */
    ck("  ... boot protocol re-sent to EACH interface, 0 and 1",
       proto_sent_to(0) && proto_sent_to(1) && g_nproto == 2, g_nproto);
    ck("  ... no SET_CONFIGURATION and no Configure Endpoint",
       g_configures == cfgs && g_setcfg == setc, g_configures - cfgs);
    report(0, 0x82, MOVE, 4); g_nmouse = 0; usb_hid_poll();
    ck("  ... and the mouse still moves", g_nmouse == 3, g_nmouse);
    ck("  ... a fourth byte of 0 scrolls nothing", g_wheel_fed == 0, g_wheel_fed);
    {   /* v0.9.6.3: the wheel, as most mice send it even in boot protocol */
        static const uint8_t WHEEL[4] = { 0x00, 0, 0, (uint8_t)-2 };
        report(0, 0x82, WHEEL, 4); g_nmouse = 0; usb_hid_poll();
        ck("a wheel byte of -2: two notches toward the user", g_wheel_fed == -2, g_wheel_fed);
        report(0, 0x82, WHEEL, 3); g_wheel_fed = 0; usb_hid_poll();
        ck("  ... a three-byte report has no wheel to read", g_wheel_fed == 0, g_wheel_fed);
    }

    /* ── vendor first ─────────────────────────────────────────────── */
    head("a receiver whose FIRST interface is not boot protocol");
    sim_reset(); plug(0, VENDOR_FIRST, sizeof VENDOR_FIRST, 0x1532, 0x0099);
    usb_enumerate();
    n = usb_hid_init();
    ck("the mouse on interface 1 is found (0.9.2 found nothing)", n == 1, n);
    ck("  ... claimed beside interface 0", proto_sent_to(1) && !proto_sent_to(0), g_nproto);
    report(0, 0x82, MOVE, 4); usb_hid_poll();
    ck("  ... and it moves", g_nmouse == 3, g_nmouse);

    /* ── the wired keyboard ───────────────────────────────────────── */
    head("a wired keyboard, one interface, as before");
    sim_reset(); plug(1, KEYBOARD, sizeof KEYBOARD, 0x04D9, 0x1503);
    usb_enumerate();
    n = usb_hid_init();
    ck("one keyboard", n == 1 && proto_sent_to(0), n);
    ck("  ... no pointer claimed for it", g_mouse_attach == 0, g_mouse_attach);
    report(1, 0x81, REP_A, 8); usb_hid_poll();
    ck("  ... it types", g_nkeys == 1 && g_keys[0] == 'a', g_nkeys);

    head("both at once, on two ports");
    sim_reset(); plug(0, RECEIVER, sizeof RECEIVER, 0x046D, 0xC52B);
    plug(1, KEYBOARD, sizeof KEYBOARD, 0x04D9, 0x1503);
    usb_enumerate();
    n = usb_hid_init();
    ck("three: the receiver's two and the keyboard", n == 3, n);
    report(1, 0x81, REP_A, 8); report(0, 0x82, MOVE, 4); usb_hid_poll();
    ck("  ... each to its own queue", g_nkeys == 1 && g_nmouse == 3, g_nkeys);

    head("a gaming keyboard: boot on 0, N-key rollover on 1");
    sim_reset(); plug(0, NKRO_KBD, sizeof NKRO_KBD, 0x3434, 0x0121);
    usb_enumerate();
    n = usb_hid_init();
    ck("the boot interface is driven, the NKRO one named and left",
       n == 1 && proto_sent_to(0) && !proto_sent_to(1) && g_warns >= 1, n);

    /* ── refusals ─────────────────────────────────────────────────── */
    head("an interface that refuses boot protocol does not take the other with it");
    sim_reset(); plug(0, RECEIVER, sizeof RECEIVER, 0x046D, 0xC52B);
    g_sim[0].refuse_proto_if = 1;
    usb_enumerate();
    n = usb_hid_init();
    ck("the keyboard half still works", n == 1 && g_mouse_attach == 0, n);
    report(0, 0x81, REP_A, 8); usb_hid_poll();
    ck("  ... and types", g_nkeys == 1, g_nkeys);

    head("a claim the controller refuses is logged, and the rest stands");
    sim_reset(); plug(0, RECEIVER, sizeof RECEIVER, 0x046D, 0xC52B);
    g_refuse_configure = 1;
    usb_enumerate();
    n = usb_hid_init();
    ck("only the keyboard (the mouse's endpoints were never added)",
       n == 1 && g_mouse_attach == 0 && g_warns >= 2, n);

    /* ── v0.9.6: Alt+Tab ───────────────────────────────────────────── */
    head("Alt+Tab is the task switcher's key, not a Tab");
    {
        static const uint8_t ALT_TAB[8]   = { 0x04, 0, 0x2B, 0, 0, 0, 0, 0 };
        static const uint8_t RALT_TAB[8]  = { 0x40, 0, 0x2B, 0, 0, 0, 0, 0 };
        static const uint8_t SALT_TAB[8]  = { 0x06, 0, 0x2B, 0, 0, 0, 0, 0 };
        static const uint8_t ALT_ONLY[8]  = { 0x04, 0, 0,    0, 0, 0, 0, 0 };
        static const uint8_t TAB[8]       = { 0x00, 0, 0x2B, 0, 0, 0, 0, 0 };
        sim_reset(); plug(1, KEYBOARD, sizeof KEYBOARD, 0x04D9, 0x1503);
        usb_enumerate(); usb_hid_init();
        g_nkeys = 0; g_nmods = 0;
        report(1, 0x81, ALT_ONLY, 8); usb_hid_poll();
        ck("Alt alone types nothing, and its state is handed over",
           g_nkeys == 0 && g_nmods == 1 && g_mods == 0x04, g_mods);
        report(1, 0x81, ALT_TAB, 8); usb_hid_poll();
        ck("left Alt + Tab is KEY_TASKSW", g_nkeys == 1 && g_keys[0] == KEY_TASKSW,
           g_nkeys ? g_keys[0] : -1);
        report(1, 0x81, REP_NONE, 8); usb_hid_poll();
        ck("  ... and the RELEASE is handed over, so the switcher can commit",
           g_mods == 0 && g_nkeys == 1, g_mods);
        report(1, 0x81, RALT_TAB, 8); usb_hid_poll();
        ck("right Alt + Tab too", g_nkeys == 2 && g_keys[1] == KEY_TASKSW,
           g_nkeys > 1 ? g_keys[1] : -1);
        report(1, 0x81, REP_NONE, 8); usb_hid_poll();
        report(1, 0x81, SALT_TAB, 8); usb_hid_poll();
        ck("Shift+Alt+Tab goes backwards", g_nkeys == 3 && g_keys[2] == KEY_TASKSW_BACK,
           g_nkeys > 2 ? g_keys[2] : -1);
        report(1, 0x81, REP_NONE, 8); usb_hid_poll();
        report(1, 0x81, TAB, 8); usb_hid_poll();
        ck("Tab without Alt is still a Tab", g_nkeys == 4 && g_keys[3] == '\t',
           g_nkeys > 3 ? g_keys[3] : -1);
    }

    head("more boot interfaces than the table holds");
    {
        uint8_t *p = NINE;
        int tot = 9 + 9 * 25;
        p[0] = 9; p[1] = 2; p[2] = (uint8_t)tot; p[3] = (uint8_t)(tot >> 8);
        p[4] = 9; p[5] = 1; p[6] = 0; p[7] = 0xA0; p[8] = 50;
        for(i = 0; i < 9; i++){
            uint8_t *q = p + 9 + i * 25;
            uint8_t ifd[25] = { 9, 4, (uint8_t)i, 0, 1, 3, 1, 1, 0, HIDD(63),
                                7, 5, (uint8_t)(0x81 + (i % 7)), 3, 8, 0, 10 };
            memcpy(q, ifd, 25);
        }
        sim_reset(); plug(0, NINE, tot, 0x1234, 0x5678);
        usb_enumerate();
        n = usb_hid_init();
        ck("capped at the table, no overrun", n <= 8 && n >= 1, n);
    }

    head("v0.9.7: a wheel mouse in REPORT protocol, from its own descriptor");
    {
        static const uint8_t R1[7] = { 0x01, 0x01, 5, 0, 0xFD, 0xFF, 0xFE };
        static const uint8_t R2[7] = { 0x02, 0x01, 9, 0, 9, 0, 1 };
        static const uint8_t BIG[7] = { 0x01, 0x00, 0x2C, 0x01, 0, 0, 0 };   /* x = 300 */
        int dx = 0;
        sim_reset();
        plug(0, WHEELMOUSE, sizeof WHEELMOUSE, 0x046D, 0xC077);
        g_sim[0].rdesc = WHEEL_RD; g_sim[0].rdesc_len = sizeof WHEEL_RD;
        usb_enumerate(); usb_hid_init();
        ck("the mouse is driven", usb_hid_count() == 1 && g_mouse_attach == 1, usb_hid_count());
        ck("  ... and was set to REPORT protocol (1), not boot (0)",
           g_nproto == 1 && g_proto_val[0] == 1 && g_proto_idx[0] == 0, g_nproto ? g_proto_val[0] : -1);
        report(0, 0x81, R1, 7); g_nmouse = 0; g_wheel_fed = 0; usb_hid_poll();
        ck("its report becomes one PS/2 packet: left button, X +5",
           g_nmouse == 3 && (g_mouse[0] & 0x07) == 1 && g_mouse[1] == 5, g_nmouse);
        ck("  ... Y -3 in HID is +3 in PS/2", g_mouse[2] == 3 && !(g_mouse[0] & 0x20), g_mouse[2]);
        ck("  ... and THE WHEEL: two notches toward the user", g_wheel_fed == -2, g_wheel_fed);
        report(0, 0x81, R2, 7); g_nmouse = 0; g_wheel_fed = 0; usb_hid_poll();
        ck("a report with another id is not taken for the pointer", g_nmouse == 0 && g_wheel_fed == 0, g_nmouse);
        report(0, 0x81, BIG, 7); g_nmouse = 0; usb_hid_poll();
        for(i = 1; i < g_nmouse; i += 3) dx += (int8_t)g_mouse[i];
        ck("a 300-count move is three packets and loses nothing", g_nmouse == 9 && dx == 300, dx);
    }
    {
        sim_reset();
        plug(0, WHEELMOUSE, sizeof WHEELMOUSE, 0x046D, 0xC077);
        g_sim[0].rdesc = NOXY_RD; g_sim[0].rdesc_len = sizeof NOXY_RD;
        usb_enumerate(); usb_hid_init();
        ck("a descriptor with no X and Y: boot protocol, as before",
           usb_hid_count() == 1 && g_nproto == 1 && g_proto_val[0] == 0, g_nproto ? g_proto_val[0] : -1);
        sim_reset();
        plug(0, WHEELMOUSE, sizeof WHEELMOUSE, 0x046D, 0xC077);
        g_sim[0].rdesc = ABS_RD; g_sim[0].rdesc_len = sizeof ABS_RD;
        usb_enumerate(); usb_hid_init();
        ck("an ABSOLUTE X and Y (a tablet): boot protocol, not deltas",
           usb_hid_count() == 1 && g_nproto == 1 && g_proto_val[0] == 0, g_nproto ? g_proto_val[0] : -1);
        sim_reset();
        plug(0, WHEELMOUSE, sizeof WHEELMOUSE, 0x046D, 0xC077);
        usb_enumerate(); usb_hid_init();
        ck("no descriptor at all (it stalls): boot protocol, as before",
           usb_hid_count() == 1 && g_nproto == 1 && g_proto_val[0] == 0, g_nproto ? g_proto_val[0] : -1);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
