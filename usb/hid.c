/*
 * usb/hid.c — a USB keyboard and a USB mouse, in boot protocol.
 *
 * On the machine this release is aimed at there is no PS/2 port of
 * either kind. Until this file existed, that laptop could not be typed
 * at: `lspci` showed the xHCI, `usb` showed the controller, and the
 * keyboard did nothing, because nothing above the controller knew what
 * a keyboard was.
 *
 * ── WHY BOOT PROTOCOL, AND NOT THE REPORT DESCRIPTOR ─────────────────
 *
 * A HID device describes its reports in a report descriptor: a nested,
 * variable-length grammar of usage pages, logical minima, report sizes
 * and collections, which a general driver has to parse before it knows
 * what byte three of a report means.
 *
 * Boot protocol is the escape hatch the specification provides for
 * exactly this situation. A device that advertises bInterfaceSubClass 1
 * must, on SET_PROTOCOL(0), produce a FIXED report: eight bytes for a
 * keyboard, three or four for a mouse, in a layout every such device
 * agrees on. It is what a BIOS uses, and it is what makes a keyboard
 * usable in a kernel with no report-descriptor parser.
 *
 * The cost is honest and worth writing down: a device whose subclass is
 * 0 -- which is legal, and which some keyboards are -- is not driven by
 * this file. The report-descriptor parser that would fix that is real
 * work and is not in this release.
 *
 * ── WHERE THE INPUT GOES ─────────────────────────────────────────────
 *
 * Into the same two queues PS/2 feeds. A key from a USB keyboard and a
 * key from a PS/2 keyboard are indistinguishable by the time anything
 * reads them, and the mouse report is turned back into a PS/2 packet
 * and pushed through mouse_feed() so that the acceleration, the
 * clamping and the button-edge detection that drivers/mouse.c already
 * does are not written a second time. A second copy of that arithmetic
 * is a second place for the v0.7.17 acceleration-precedence bug to live.
 *
 * ── POLLED, LIKE EVERYTHING ELSE ─────────────────────────────────────
 *
 * usb_hid_poll() is called from the two places that read the keyboard,
 * so input is drained whenever anybody asks for input and never
 * otherwise. No interrupt, no timer. See include/uhci.h on why audio
 * will be the first thing here that genuinely needs one.
 */

#include "../include/keys.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/klog.h"

extern void keyboard_inject(int c);
extern void mouse_feed(uint8_t data);
/* v0.8.8.1: and the pointer-present count, which is drivers/mouse.c's
   because "is there a pointing device" is one question with two
   sources. A USB mouse used to feed coordinates nothing would draw --
   see the long note in that file. */
extern void mouse_usb_attach(void);
extern void mouse_usb_forget_all(void);

#define HID_SUBCLASS_BOOT  1
#define HID_PROTO_KEYBOARD 1
#define HID_PROTO_MOUSE    2
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

/* v0.9.2.1: 8, not 4. A wireless receiver alone is a keyboard AND a
   mouse now that every interface of it is driven (usb_hid_init). */
#define HID_MAX 8

typedef struct {
    const usb_dev_t *dev;
    uint8_t  ifnum;     /* which interface of the device (v0.9.2.1)   */
    uint8_t  ep;        /* interrupt IN endpoint address              */
    uint16_t mps;
    uint8_t  proto;     /* HID_PROTO_*                                */
    uint8_t  last[8];   /* the previous keyboard report               */
} hid_t;

static hid_t g_hid[HID_MAX];
static int   g_nhid;

/* ── HID usage codes to this kernel's key codes ─────────────────────
   The extended codes are the ones drivers/keyboard.c already produces
   for the PS/2 arrows and navigation keys, so a reader cannot tell
   which keyboard a key came from. Taken from that file rather than
   invented here: 0x148 is Up, 0x150 is Down, and so on. */
static const char K_LOWER[] =
    "\0\0\0\0" "abcdefghijklmnopqrstuvwxyz" "1234567890"
    "\n\x1b\b\t " "-=[]\\\0;'`,./";
static const char K_UPPER[] =
    "\0\0\0\0" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()"
    "\n\x1b\b\t " "_+{}|\0:\"~<>?";

static int usage_to_key(uint8_t u, uint8_t mods)
{
    int shift = (mods & 0x22) ? 1 : 0;      /* either shift          */
    int ctrl  = (mods & 0x11) ? 1 : 0;      /* either control        */

    if (u >= 0x04 && u <= 0x38) {
        char c = shift ? K_UPPER[u] : K_LOWER[u];
        if (!c) return 0;
        if (ctrl && c >= 'a' && c <= 'z') return c & 0x1F;
        if (ctrl && c >= 'A' && c <= 'Z') return c & 0x1F;
        return (int)(unsigned char)c;
    }
    switch (u) {
    case 0x49: return 0x152;   /* Insert    */
    case 0x4A: return 0x147;   /* Home      */
    case 0x4B: return 0x149;   /* Page Up   */
    case 0x4C: return 0x153;   /* Delete    */
    case 0x4D: return 0x14F;   /* End       */
    case 0x4E: return 0x151;   /* Page Down */
    case 0x4F: return 0x14D;   /* Right     */
    case 0x50: return 0x14B;   /* Left      */
    case 0x51: return 0x150;   /* Down      */
    case 0x52: return 0x148;   /* Up        */
    default:   return 0;
    }
}

/* ── one keyboard report ───────────────────────────────────────────
   A boot report lists the keys HELD, not the keys pressed. Emitting
   every key in every report would repeat a held key as fast as the
   endpoint is polled, so a key is emitted only when it appears in a
   report and was not in the one before -- which is also what makes
   holding shift work rather than producing a stream of nothing. */
#define MOD_GUI  0x88u      /* bit 3 left GUI, bit 7 right GUI */

static void keyboard_report(hid_t *h, const uint8_t *r)
{
    int i, j;

    /* ── the Windows key, which is a MODIFIER here ────────────────────
       On PS/2 it arrives as E0 5B and goes through the scancode table
       like any other key. On USB it is not in the key array at all:
       it is bit 3 (left) or bit 7 (right) of the modifier byte, so
       usage_to_key() never sees it and the loop below cannot find it.

       Edge-detected against the previous report's modifier byte for
       the same reason every other key here is: a boot report lists
       what is HELD, so without this a held Windows key would open the
       start menu as fast as the endpoint is polled.

       Both sides report KEY_SUPER. Nothing wants to tell them apart,
       and a start menu that opened from one side of the spacebar only
       would be worse than the key not working. */
    if ((r[0] & MOD_GUI) && !(h->last[0] & MOD_GUI))
        keyboard_inject(KEY_SUPER);

    for (i = 2; i < 8; i++) {
        uint8_t u = r[i];
        int was = 0;
        if (u == 0 || u == 1) continue;        /* 1 is "too many keys" */
        for (j = 2; j < 8; j++) if (h->last[j] == u) { was = 1; break; }
        if (was) continue;
        {
            int k = usage_to_key(u, r[0]);
            if (k) keyboard_inject(k);
        }
    }
    memcpy(h->last, r, 8);
}

/* ── one mouse report, turned back into a PS/2 packet ──────────────
   drivers/mouse.c already owns the acceleration, the divisor and the
   button-edge detection. Rebuilding that here would be a second copy of
   the arithmetic that carried the v0.7.17 precedence bug -- the one
   where every leftward movement got a 2x boost and the pointer drifted.
   One copy, fed from both sources. */
static void mouse_report(const uint8_t *r, int len)
{
    int dx = (int8_t)r[1];
    int dy = (int8_t)r[2];
    uint8_t b0;

    if (len < 3) return;

    /* HID counts Y downward; PS/2 counts it upward. */
    dy = -dy;

    b0 = (uint8_t)(0x08u |
                   ((r[0] & 1u) ? 0x01u : 0u) |     /* left   */
                   ((r[0] & 2u) ? 0x02u : 0u) |     /* right  */
                   ((r[0] & 4u) ? 0x04u : 0u));     /* middle */
    if (dx < 0) b0 |= 0x10u;
    if (dy < 0) b0 |= 0x20u;

    mouse_feed(b0);
    mouse_feed((uint8_t)(dx & 0xFF));
    mouse_feed((uint8_t)(dy & 0xFF));
}

/* ── discovery ───────────────────────────────────────────────────────

   ── v0.9.2.1: EVERY INTERFACE, NOT THE FIRST ─────────────────────────

   The core configures a device on its FIRST interface (usb/core.c,
   want_if -1), and until this release that interface was the only one
   this driver looked at. A plain wired mouse or keyboard has one
   interface and never noticed.

   A wireless receiver has several. The common layout -- Logitech's,
   and most gaming receivers' -- is a boot keyboard on interface 0, a
   boot mouse on interface 1 and a vendor HID interface after them, and
   a gaming keyboard often puts its N-key-rollover report on a second,
   non-boot interface. Kyan's HP, 11 November: the mouse's receiver did
   nothing at all. Its first interface is not the mouse.

   So every configured device whose selected interface is HID is
   SURVEYED (usb_survey_config: reads the descriptor, selects nothing),
   and each boot keyboard or boot mouse interface in it is driven --
   claimed beside the others with usb_claim_interface() if the core did
   not already select it, the same call that lets usbmux and Apple
   Ethernet share one iPhone. An interface already claimed by an earlier
   pass keeps its endpoints: a rescan must not reconfigure a keyboard
   somebody is typing on. */
typedef struct {
    int n;
    struct { uint8_t num, sub, proto; } f[HID_MAX];
} hid_survey_t;

static void hid_survey_cb(void *ctx, uint8_t num, uint8_t alt, uint8_t nep,
                          uint8_t cls, uint8_t sub, uint8_t proto)
{
    hid_survey_t *s = (hid_survey_t *)ctx;
    (void)nep;
    if (alt != 0 || cls != 3) return;          /* HID, default setting */
    if (s->n >= HID_MAX) return;
    s->f[s->n].num = num; s->f[s->n].sub = sub; s->f[s->n].proto = proto;
    s->n++;
}

/* Bring up ONE interface of a device. 1 if it is now driven. */
static int hid_bind(usb_dev_t *d, uint8_t num, uint8_t sub, uint8_t proto)
{
    const usb_ep_t *e;
    hid_t *h;

    if (g_nhid >= HID_MAX) return 0;
    if (sub != HID_SUBCLASS_BOOT) {
        klog(KLOG_WARN, "USB HID %4x:%4x interface %u is not boot protocol; "
                        "this kernel has no report parser for it",
             (uint32_t)d->vendor, (uint32_t)d->product, (uint32_t)num);
        return 0;
    }
    if (proto != HID_PROTO_KEYBOARD && proto != HID_PROTO_MOUSE) return 0;

    e = usb_find_ep_if(d, num, USB_EP_INTR, 1);
    if (!e) {
        /* Not held yet: claim it beside what the device already has. */
        int r = usb_claim_interface(d, d->cfgidx, num, 0);
        if (r < 0) {
            klog(KLOG_WARN, "USB HID %4x:%4x interface %u could not be "
                            "claimed (%d)", (uint32_t)d->vendor,
                 (uint32_t)d->product, (uint32_t)num, (uint32_t)r);
            return 0;
        }
        e = usb_find_ep_if(d, num, USB_EP_INTR, 1);
    }
    if (!e) {
        klog(KLOG_WARN, "USB HID %4x:%4x interface %u has no interrupt IN "
                        "endpoint", (uint32_t)d->vendor, (uint32_t)d->product,
             (uint32_t)num);
        return 0;
    }

    /* Boot protocol, and report only on change. SET_IDLE is allowed
       to STALL -- plenty of devices do -- and that is not a failure,
       so its result is deliberately not checked. SET_PROTOCOL is.
       Both go to THIS interface: wIndex is the interface number. */
    usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                HID_REQ_SET_IDLE, 0, num, 0, 0);
    if (usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_REQ_SET_PROTOCOL, 0, num, 0, 0) < 0) {
        klog(KLOG_WARN, "USB HID %4x:%4x interface %u refused boot protocol",
             (uint32_t)d->vendor, (uint32_t)d->product, (uint32_t)num);
        return 0;
    }

    h = &g_hid[g_nhid++];
    memset(h, 0, sizeof *h);
    h->dev   = d;
    h->ifnum = num;
    h->ep    = e->addr;
    h->mps   = e->mps;
    h->proto = proto;

    if (h->proto == HID_PROTO_MOUSE) mouse_usb_attach();

    klog(KLOG_OK, "USB %s %4x:%4x interface %u on endpoint %2x, %u-byte reports",
         h->proto == HID_PROTO_KEYBOARD ? "keyboard" : "mouse",
         (uint32_t)d->vendor, (uint32_t)d->product, (uint32_t)num,
         (uint32_t)h->ep, (uint32_t)h->mps);
    return 1;
}

int usb_hid_init(void)
{
    int i, k;
    g_nhid = 0;
    /* Before the walk, not after: this function is re-run by
       `usb rescan` and by the hot-plug client, and a pointer count
       that survived a mouse being unplugged would keep claiming a
       cursor for a device that is gone. Same reason usbnet_release()
       runs before usb_enumerate(). */
    mouse_usb_forget_all();

    for (i = 0; i < usb_device_count() && g_nhid < HID_MAX; i++) {
        usb_dev_t *d = (usb_dev_t *)usb_device(i);
        hid_survey_t sv;

        if (!d || !d->valid || !d->config) continue;
        if (d->if_class != 3) continue;                  /* not HID     */

        memset(&sv, 0, sizeof sv);
        if (usb_survey_config(d, d->cfgidx, hid_survey_cb, &sv) < 0 || !sv.n) {
            /* The survey failed: drive the one interface the core chose,
               which is all this driver ever did before 0.9.2.1. */
            hid_bind(d, d->ifnum, d->if_subclass, d->if_protocol);
            continue;
        }
        for (k = 0; k < sv.n && g_nhid < HID_MAX; k++)
            hid_bind(d, sv.f[k].num, sv.f[k].sub, sv.f[k].proto);
    }
    return g_nhid;
}

/* ── polling ───────────────────────────────────────────────────────
   Called from drivers/keyboard.c's two read paths, so USB input is
   drained whenever anything asks for input. A device with nothing to
   report NAKs; on xHCI that comes back as a short or failed transfer
   and is simply nothing to do, which is why a non-positive result is
   not an error here. */
void usb_hid_poll(void)
{
    int i;
    uint8_t rep[8];

    for (i = 0; i < g_nhid; i++) {
        hid_t *h = &g_hid[i];
        int want = (h->proto == HID_PROTO_KEYBOARD) ? 8 : 4;
        int r;

        if (want > (int)h->mps) want = (int)h->mps;
        if (want > (int)sizeof rep) want = (int)sizeof rep;
        memset(rep, 0, sizeof rep);

        /* usb_intr_poll(), not usb_interrupt(): a keyboard with no key
           pressed NAKs, and on xHCI that is a transfer which never
           completes rather than one that fails. v0.8.5 waited a full
           second for it on every poll. (v0.8.5.1) */
        r = usb_intr_poll(h->dev, h->ep, 1, rep, want, (int)h->mps);
        if (r <= 0) continue;

        if (h->proto == HID_PROTO_KEYBOARD) {
            if (r >= 8) keyboard_report(h, rep);
        } else {
            mouse_report(rep, r);
        }
    }
}

int usb_hid_count(void) { return g_nhid; }
