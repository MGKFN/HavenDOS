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

#define HID_MAX 4

typedef struct {
    const usb_dev_t *dev;
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

/* ── discovery ─────────────────────────────────────────────────────── */
int usb_hid_init(void)
{
    int i;
    g_nhid = 0;
    /* Before the walk, not after: this function is re-run by
       `usb rescan` and by the hot-plug client, and a pointer count
       that survived a mouse being unplugged would keep claiming a
       cursor for a device that is gone. Same reason usbnet_release()
       runs before usb_enumerate(). */
    mouse_usb_forget_all();

    for (i = 0; i < usb_device_count() && g_nhid < HID_MAX; i++) {
        const usb_dev_t *d = usb_device(i);
        const usb_ep_t  *e;
        hid_t *h;

        if (!d || !d->valid || !d->config) continue;
        if (d->if_class != 3) continue;                  /* not HID     */
        if (d->if_subclass != HID_SUBCLASS_BOOT) {
            klog(KLOG_WARN, "USB HID %4x:%4x is not boot protocol; "
                            "this kernel has no report parser",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }
        if (d->if_protocol != HID_PROTO_KEYBOARD &&
            d->if_protocol != HID_PROTO_MOUSE) continue;

        e = usb_find_ep(d, USB_EP_INTR, 1);
        if (!e) {
            klog(KLOG_WARN, "USB HID %4x:%4x has no interrupt IN endpoint",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }

        /* Boot protocol, and report only on change. SET_IDLE is allowed
           to STALL -- plenty of devices do -- and that is not a failure,
           so its result is deliberately not checked. SET_PROTOCOL is. */
        usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    HID_REQ_SET_IDLE, 0, d->ifnum, 0, 0);
        if (usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                        HID_REQ_SET_PROTOCOL, 0, d->ifnum, 0, 0) < 0) {
            klog(KLOG_WARN, "USB HID %4x:%4x refused boot protocol",
                 (uint32_t)d->vendor, (uint32_t)d->product);
            continue;
        }

        h = &g_hid[g_nhid++];
        memset(h, 0, sizeof *h);
        h->dev   = d;
        h->ep    = e->addr;
        h->mps   = e->mps;
        h->proto = d->if_protocol;

        if (h->proto == HID_PROTO_MOUSE) mouse_usb_attach();

        klog(KLOG_OK, "USB %s %4x:%4x on endpoint %2x, %u-byte reports",
             h->proto == HID_PROTO_KEYBOARD ? "keyboard" : "mouse",
             (uint32_t)d->vendor, (uint32_t)d->product,
             (uint32_t)h->ep, (uint32_t)h->mps);
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
