/*
 * usb/core.c — the device table, the descriptor walk, and the one place
 * that decides which controller a device is on.
 *
 * include/usb.h says why this file exists and where the line between it
 * and a back-end falls. The short version: the back-end owns everything
 * up to "there is an addressed device on this port whose default pipe
 * works", because UHCI and xHCI disagree about who owns an address; the
 * core owns everything after it, because they agree about all of that.
 *
 * Nothing in this file names a controller. That is the point.
 */

#include "../include/kpoll.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
/* Why the last xHCI attach failed -- see the note in
   usb/xhci_hc.c. Declared here rather than in a header
   because it is one function and one caller. */
extern const char *xhci_attach_why(void);
#include "../include/klog.h"

/* ════════════════════════════════════════════════════════════════════
   1. The configuration descriptor walk
   ════════════════════════════════════════════════════════════════════

   Pure -- no I/O, no globals, no allocation -- because it is the part
   most worth testing on its own and the part most likely to be handed
   something malformed. Every length in here came off the wire from
   somebody else's firmware, and a cheap keyboard's firmware is not a
   careful piece of software.

   Three refusals in particular are not theoretical:

     bLength == 0    an infinite loop, and the classic way a descriptor
                     parser hangs the machine it is enumerating.
     bLength < 2     there is no type byte to read, so the walk cannot
                     even decide what it is looking at.
     a descriptor
     running past
     the buffer      a wTotalLength that promises more than was read is
                     normal (a short read), but a descriptor whose own
                     length leaves the buffer is not, and reading it is
                     reading our own memory and calling it a device. */
/* ── v0.8.6.4: the same walk, with somebody watching ─────────────────
   `usb cfg` surveys EVERY configuration a device offers and lists every
   interface in each, which needs a walk over the descriptor blob that
   does not select anything. That is this walk with the selecting part
   switched off, so it is this walk -- usb_parse_config() takes an
   optional observer and a want_if of USB_IF_NONE, rather than a second
   copy of a parser whose whole reason for being pure is that it is the
   part most likely to be handed something malformed.

   The observer is called for every interface descriptor, alternate
   settings included, with the fields raw off the wire. It cannot
   refuse: reporting what a device claims is the entire job. */
int usb_parse_config(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                     int want_if)
{
    return usb_walk_config_alt(d, buf, len, want_if, 0, 0, 0);
}

int usb_parse_config_alt(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                         int want_if, int want_alt)
{
    return usb_walk_config_alt(d, buf, len, want_if, want_alt, 0, 0);
}

int usb_walk_config(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                    int want_if, usb_if_cb cb, void *ctx)
{
    return usb_walk_config_alt(d, buf, len, want_if, 0, cb, ctx);
}

/* Three wrappers and one walker. include/usb.h says why alternate
   setting 0 stayed the default of all three: the phone this release
   exists for has NO ENDPOINTS on alt 0 of its Ethernet interface, so
   "take alt 0" is exactly wrong there -- but it is right everywhere
   else, and every caller that predates this release means it. */
int usb_walk_config_alt(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                        int want_if, int want_alt,
                        usb_if_cb cb, void *ctx)
{
    uint32_t i;
    int      in_iface = 0;      /* are we inside the interface we chose */
    int      seen_iface = 0;

    if (!d || !buf) return USB_EPARAM;
    /* An alternate setting is a byte on the wire. A caller asking for
       one that cannot exist is a caller with a bug, and answering
       "no such interface" for it would send whoever hits it looking at
       the device. */
    if (want_alt < 0 || want_alt > 255) return USB_EPARAM;

    /* USB_IF_NONE is a survey: walk the blob, report what is in it,
       CHANGE NOTHING. A survey that cleared d->config would unconfigure
       a working device in the kernel's own table while the device
       itself carried on configured -- the two disagreeing is worse
       than not looking. */
    if (want_if != USB_IF_NONE) {
        d->config = 0;
        d->ifnum = d->if_class = d->if_subclass = d->if_protocol = 0;
        d->altsetting = 0;
        d->nep = 0;
    }

    /* The configuration descriptor itself: 9 bytes, type 2. */
    if (len < 9)            return USB_EXFER;
    if (buf[0] < 9)         return USB_EXFER;
    if (buf[1] != USB_DESC_CONFIG) return USB_EXFER;

    /* wTotalLength is what the device SAYS the whole blob is. It is
       allowed to be longer than what was read -- a short read is a real
       thing -- but a value smaller than the descriptor in hand is a
       device contradicting itself. */
    {
        uint32_t total = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
        if (total < 9) return USB_EXFER;

        /* ── wTotalLength IS THE BOUND, v0.8.6.4 ──────────────────────
           It used to be read for this check and then ignored, and the
           walk ran to the end of whatever was HANDED to it instead.

           Those two differ. A controller that reports no residual gives
           back the length that was ASKED for, not the length the device
           sent, so the tail of the buffer is whatever was there before
           -- zeros, here, because the fetch clears it first. The walk
           then read a zero bLength past the last real descriptor and
           refused the whole configuration as malformed.

           Found while writing the survey test, by a descriptor blob
           that claimed more than it carried -- which is what a device
           contradicting itself looks like, and what this parser is
           pure in order to survive. tests/test_xhci.c keeps one.

           A total LARGER than what was read is a short read and stays
           bounded by len, which is what the `i + dlen > len` break
           below has always handled. */
        if (total < len) len = total;
    }

    if (buf[5] == 0) return USB_EXFER;      /* 0 means "unconfigured" */
    if (want_if != USB_IF_NONE) d->config = buf[5];

    i = buf[0];
    while (i + 2u <= len) {
        uint8_t dlen = buf[i];
        uint8_t type = buf[i + 1];

        if (dlen < 2)        return USB_EXFER;   /* would never advance */
        if (i + dlen > len)  break;              /* short read: stop here */

        if (type == USB_DESC_INTERFACE) {
            if (dlen < 9) return USB_EXFER;
            {
                uint8_t num = buf[i + 2];
                uint8_t alt = buf[i + 3];
                if (cb) cb(ctx, num, alt, buf[i + 4],
                           buf[i + 5], buf[i + 6], buf[i + 7]);
                /* Alternate settings other than 0 are a different
                   configuration of the same interface, and adopting one
                   by ACCIDENT is how a device ends up with endpoints it
                   was never told to use. So the setting is named rather
                   than refused: want_alt is 0 for every caller that
                   predates v0.8.7, which is the behaviour this check
                   had when it read `alt == 0`.

                   Adopting one on purpose is what tethering needs --
                   the Apple Ethernet interface has its bulk endpoints
                   on alt 1 and alt 2 and nothing at all on alt 0. */
                if ((int)alt == want_alt && !seen_iface &&
                    want_if != USB_IF_NONE &&
                    (want_if < 0 || (int)num == want_if)) {
                    d->ifnum       = num;
                    d->altsetting  = alt;
                    d->if_class    = buf[i + 5];
                    d->if_subclass = buf[i + 6];
                    d->if_protocol = buf[i + 7];
                    d->nep         = 0;
                    in_iface   = 1;
                    seen_iface = 1;
                } else if (in_iface) {
                    in_iface = 0;      /* the chosen interface ended here */
                }
            }
        } else if (type == USB_DESC_ENDPOINT && in_iface) {
            if (dlen < 7) return USB_EXFER;
            if (d->nep < USB_MAX_EP) {
                usb_ep_t *e = &d->ep[d->nep];
                e->addr     = buf[i + 2];
                e->attr     = (uint8_t)(buf[i + 3] & 3u);
                e->mps      = (uint16_t)((uint32_t)buf[i + 4] |
                                         ((uint32_t)buf[i + 5] << 8));
                e->interval = buf[i + 6];
                e->iface    = d->ifnum;   /* v0.8.9.9: whose it is */
                e->keep     = 0;
                /* A zero-length packet size is a device that cannot
                   carry anything. Refusing it here means a class driver
                   never divides by it. */
                if (e->mps == 0) return USB_EXFER;
                d->nep++;
            }
            /* More endpoints than the table holds is not an error: the
               ones that fit are the ones a boot-protocol device needs,
               and refusing the device entirely would be worse. */
        }
        i += dlen;
    }

    /* A survey selects nothing, so "no interface was selected" is not
       a failure of a survey -- it is what a survey does. */
    if (!seen_iface && want_if != USB_IF_NONE) return USB_EXFER;
    return USB_OK;
}

const usb_ep_t *usb_find_ep(const usb_dev_t *d, int attr, int in)
{
    int i;
    if (!d) return 0;
    for (i = 0; i < d->nep; i++) {
        const usb_ep_t *e = &d->ep[i];
        if ((e->attr & 3) != (attr & 3)) continue;
        if (in && !(e->addr & 0x80))     continue;
        if (!in && (e->addr & 0x80))     continue;
        return e;
    }
    return 0;
}

const usb_ep_t *usb_find_ep_if(const usb_dev_t *d, int iface, int attr,
                               int in)
{
    int i;
    if (!d) return 0;
    for (i = 0; i < d->nep; i++) {
        const usb_ep_t *e = &d->ep[i];
        if ((int)e->iface != iface)      continue;
        if ((e->attr & 3) != (attr & 3)) continue;
        if (in && !(e->addr & 0x80))     continue;
        if (!in && (e->addr & 0x80))     continue;
        return e;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
   2. The controller table
   ════════════════════════════════════════════════════════════════════ */

static usb_hc_t  g_hc[USB_MAX_HC];
static int       g_nhc;
static usb_dev_t g_dev[USB_MAX_DEV];
static int       g_ndev;

/* The next address to hand out, kept here rather than in a back-end so
   that two controllers cannot both decide to call a device number 1.
   USB addresses are per-bus and would not actually collide, but a single
   counter costs nothing and makes the `usb` listing readable. */
static uint8_t   g_next_addr = 1;

uint8_t usb_core_next_address(void)
{
    if (g_next_addr == 0 || g_next_addr > 127) g_next_addr = 1;
    return g_next_addr++;
}

int usb_register_hc(const usb_hc_ops_t *ops, int index, uint32_t base,
                    uint8_t kind)
{
    if (!ops || g_nhc >= USB_MAX_HC) return USB_ENOMEM;
    g_hc[g_nhc].ops   = ops;
    g_hc[g_nhc].index = index;
    g_hc[g_nhc].base  = base;
    g_hc[g_nhc].kind  = kind;
    return g_nhc++;
}

int             usb_hc_count(void) { return g_nhc; }
const usb_hc_t *usb_hc(int i)
{
    return (i >= 0 && i < g_nhc) ? &g_hc[i] : 0;
}

/* THE function that names a controller. Everything that prints one asks
   this, so the listing and the error message cannot disagree -- which is
   exactly what four screens did in 0.8.3.1. */
const char *usb_hc_name(const usb_hc_t *h)
{
    if (!h || !h->ops) return "none";
    return h->ops->name ? h->ops->name : "unknown";
}

int             usb_device_count(void) { return g_ndev; }
const usb_dev_t *usb_device(int i)
{
    return (i >= 0 && i < g_ndev) ? &g_dev[i] : 0;
}

/* ════════════════════════════════════════════════════════════════════
   3. Transfers
   ════════════════════════════════════════════════════════════════════ */

static usb_hc_t *hc_of(const usb_dev_t *d)
{
    if (!d || d->hc >= (uint8_t)g_nhc) return 0;
    return &g_hc[d->hc];
}

/* ── v0.8.8: THE GUARD, AND WHY IT IS A WRAPPER ──────────────────────
 *
 * kernel_poll() runs periodic work from the shell's idle loop, and the
 * work it runs reaches this layer.  Every entry point below owns a
 * module-level buffer that is not reentrant, so a poll that arrived
 * while one of them was part-way through would corrupt it -- the
 * once-an-hour impossibility include/kpoll.h is about.
 *
 * It is a WRAPPER round an unchanged inner function rather than an
 * enter/leave pair added to the body, and that is the whole reason it
 * is safe to add this late: the body is not edited, so there is no
 * return path that can be missed, and the pairing is one line a grep
 * can read.  tools/check_kpoll_guard.py holds the list and refuses an
 * entry point that loses its bracket. */
static int usb_control_inner(const usb_dev_t *d, uint8_t rtype, uint8_t req,
                uint16_t value, uint16_t index, void *data, uint16_t len)
{
    usb_hc_t *h = hc_of(d);
    if (!h || !h->ops->control) return USB_ENODEV;
    return h->ops->control(h, d, rtype, req, value, index, data, len);
}

int usb_control(const usb_dev_t *d, uint8_t rtype, uint8_t req,
                uint16_t value, uint16_t index, void *data, uint16_t len)
{
    int r;
    kpoll_enter();
    r = usb_control_inner(d, rtype, req, value, index, data, len);
    kpoll_leave();
    return r;
}

int usb_bulk(const usb_dev_t *d, uint8_t ep, int in, void *data, int len,
             int mps)
{
    usb_hc_t *h = hc_of(d);
    if (!h || !h->ops->bulk) return USB_ENODEV;
    return h->ops->bulk(h, d, ep, in, data, len, mps);
}

int usb_interrupt(const usb_dev_t *d, uint8_t ep, int in, void *data,
                  int len, int mps)
{
    usb_hc_t *h = hc_of(d);
    if (!h || !h->ops->intr) return USB_ENODEV;
    return h->ops->intr(h, d, ep, in, data, len, mps);
}

void usb_reset_toggle(const usb_dev_t *d, uint8_t ep, int in)
{
    usb_hc_t *h = hc_of(d);
    if (!h || !h->ops->reset_toggle) return;
    h->ops->reset_toggle(h, d, ep, in);
}

/* A halted endpoint needs two things and they are easy to do by halves:
   the device has to be told to clear the halt, and the host's data
   toggle has to go back to DATA0. Doing only the first leaves the two
   ends disagreeing about the toggle and every later transfer times out,
   which looks exactly like a dead device. */
int usb_clear_halt(const usb_dev_t *d, uint8_t ep, int in)
{
    int r;
    uint16_t idx = (uint16_t)((ep & 0x0F) | (in ? 0x80 : 0x00));
    if (!d) return USB_EPARAM;
    r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                    USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
                    idx, 0, 0);
    if (r < 0) return r;
    usb_reset_toggle(d, ep, in);
    return USB_OK;
}

/* ════════════════════════════════════════════════════════════════════
   4. Enumeration
   ════════════════════════════════════════════════════════════════════ */

/* One buffer, used while a device is being set up and never held. 512
   bytes covers a keyboard, a mouse, a mass-storage stick and a USB
   Ethernet adapter with room over; a composite device with more
   interfaces than that is read short, and the walk stops cleanly at the
   last descriptor that fits rather than reading past it. */
static uint8_t g_cfgbuf[512];

/* ── usb_survey_config, v0.8.6.4 ─────────────────────────────────────
   Fetch configuration descriptor INDEX idx -- not bConfigurationValue,
   the index, which is what wValue's low byte carries -- and report
   every interface in it.

   setup_config() below has always asked for index 0, because every
   device this kernel had met offered exactly one configuration. An
   iPhone does not: it enumerated on the HP as class 06, Still Image,
   which is PTP and is its default. Whether anything else is on offer is
   a question only the device can answer, and this is the asking.

   NOTHING IS SELECTED. No SET_CONFIGURATION, and *d is not modified --
   see the USB_IF_NONE note in usb_walk_config(). Surveying a phone that
   is working must not stop it working.

   g_cfgbuf is borrowed. It is described below as "used while a device
   is being set up and never held", and a survey runs from the shell
   long after that, with nothing else enumerating. */
/* ── ONE descriptor fetch, v0.8.7 ─────────────────────────────────────
   Nine bytes for wTotalLength, then the whole blob: asking for all of
   it without knowing its size means either guessing or a 64 KB buffer.

   This was written twice -- setup_config() and usb_survey_config() --
   and v0.8.7 needed it in two more places, for usb_select_config() and
   usb_set_interface(). Four copies of a two-step fetch whose first step
   decides the length of the second is four chances to bound one of them
   differently, and this tree spent 0.8.6 deleting three copies of a
   dispatch that had drifted in all three.

   Returns the number of bytes read into g_cfgbuf, or a negative
   USB_E*. `idx` is a descriptor INDEX, which is what wValue's low byte
   carries. */
static int fetch_config(usb_dev_t *d, uint8_t idx)
{
    int      r;
    uint32_t total;
    uint16_t val = (uint16_t)((USB_DESC_CONFIG << 8) | idx);

    memset(g_cfgbuf, 0, 9);
    r = usb_control(d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, val, 0, g_cfgbuf, 9);
    if (r < 0) return r;
    if (r < 9) return USB_EXFER;

    total = (uint32_t)g_cfgbuf[2] | ((uint32_t)g_cfgbuf[3] << 8);
    if (total < 9) return USB_EXFER;
    /* A blob longer than the buffer is read short and the walk stops at
       the last descriptor that fits. Reported as a short read rather
       than guessed at -- a survey that silently omitted the interface
       somebody is looking for would be worse than one that says so. */
    if (total > sizeof g_cfgbuf) total = sizeof g_cfgbuf;

    memset(g_cfgbuf, 0, total);
    return usb_control(d, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_GET_DESCRIPTOR, val, 0, g_cfgbuf,
                       (uint16_t)total);
}

int usb_survey_config(usb_dev_t *d, uint8_t idx, usb_if_cb cb, void *ctx)
{
    int r;

    if (!d) return USB_EPARAM;
    r = fetch_config(d, idx);
    if (r < 0) return r;

    return usb_walk_config(d, g_cfgbuf, (uint32_t)r, USB_IF_NONE, cb, ctx);
}

/* Select the configuration at descriptor index `idx`, adopting
   `want_if` from it at alternate setting 0, and tell the controller
   what endpoints it has.

   One function for the boot path and for usb_select_config(), because
   the difference between them is an index and an interface number, and
   writing SET_CONFIGURATION twice is how a kernel ends up with two
   ideas of what "configured" means. */
static int do_set_config(usb_hc_t *h, usb_dev_t *d, uint8_t idx,
                         int want_if)
{
    int r = fetch_config(d, idx);
    if (r < 0) return r;

    r = usb_parse_config(d, g_cfgbuf, (uint32_t)r, want_if);
    if (r != USB_OK) return r;

    /* SET_CONFIGURATION, then let the controller know what it just
       agreed to. On UHCI the second step does nothing; on xHCI nothing
       beyond the default pipe works without it. */
    r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_CONFIGURATION, d->config, 0, 0, 0);
    if (r < 0) { d->config = 0; return r; }

    if (h->ops->configure) {
        r = h->ops->configure(h, d);
        if (r != USB_OK) { d->config = 0; return r; }
    }
    d->cfgidx = idx;
    return USB_OK;
}

static int setup_config(usb_hc_t *h, usb_dev_t *d)
{
    if (d->nconfigs == 0) return USB_OK;   /* nothing to choose */
    return do_set_config(h, d, 0, -1);
}

/* ════════════════════════════════════════════════════════════════════
   4a. Choosing a configuration, and switching an interface  (v0.8.7)
   ════════════════════════════════════════════════════════════════════

   include/usb.h carries the argument. The short version is the iPhone
   on the HP: four configurations, booted into index 0 which is PTP, and
   tethering on interface 2 of index 3 -- whose ALT 0 HAS NO ENDPOINTS.
   Getting to it is SET_CONFIGURATION to a non-default index and then
   SET_INTERFACE to a non-zero alternate setting, and this kernel could
   do neither.

   Both take a non-const device, because both change what the device IS.
   That is the difference from usb_survey_config(), which takes one for
   the buffer alone and promises not to.
   ════════════════════════════════════════════════════════════════════ */

static usb_hc_t *hc_of_mut(usb_dev_t *d)
{
    if (!d || d->hc >= (uint8_t)g_nhc) return 0;
    return &g_hc[d->hc];
}

int usb_select_config(usb_dev_t *d, uint8_t idx, int want_if)
{
    usb_hc_t *h = hc_of_mut(d);
    if (!d) return USB_EPARAM;
    if (!h)  return USB_ENODEV;
    /* A device that reports N configurations has indices 0..N-1, and
       asking past that is a driver bug rather than something the device
       should be troubled with. `usb cfg` prints nconfigs, so whoever
       chose the index has seen the number. */
    if (d->nconfigs == 0)        return USB_ENODEV;
    if (idx >= d->nconfigs)      return USB_EPARAM;
    return do_set_config(h, d, idx, want_if);
}

int usb_set_interface(usb_dev_t *d, uint8_t ifnum, uint8_t alt)
{
    usb_hc_t *h = hc_of_mut(d);
    usb_dev_t next;
    int       r;

    if (!d) return USB_EPARAM;
    if (!h)  return USB_ENODEV;
    /* An unconfigured device has no interfaces to switch. SET_INTERFACE
       to one would be refused by the device, but asking is worse than
       not asking: the kernel would have had to decide what its own
       table said in the meantime. */
    if (d->config == 0) return USB_ENODEV;

    /* ── THE SCRATCH COPY, AND WHY IT IS NOT CAUTION ──────────────────
       The endpoints of an alternate setting are in the configuration
       descriptor, so the walk has to run BEFORE SET_INTERFACE is sent
       -- there is no other way to know what the new setting has. The
       walk writes d->ifnum, d->altsetting and the whole endpoint array.

       If the request is then refused, a device left on its old setting
       faces a kernel whose table describes the new one. The next bulk
       transfer goes to an endpoint address the device does not have at
       the moment, which on xHCI is a transfer the controller retries
       until it times out and on UHCI is a stall -- and neither says
       "the interface did not switch".

       So the walk runs into a copy, and the copy is adopted only once
       the device and the controller have both agreed. */
    next = *d;
    r = fetch_config(d, d->cfgidx);
    if (r < 0) return r;
    r = usb_parse_config_alt(&next, g_cfgbuf, (uint32_t)r,
                             (int)ifnum, (int)alt);
    if (r != USB_OK) return r;

    /* The walk adopts the FIRST interface matching both numbers, and
       refuses the blob if it finds none -- so reaching here means the
       device really offers this setting. Checking anyway: a walk that
       adopted interface 3 for a request for interface 2 would be a bug
       in the walk, and this is the one caller that can tell. */
    if (next.ifnum != ifnum || next.altsetting != alt) return USB_EXFER;

    /* SET_INTERFACE: wValue is the alternate setting, wIndex is the
       interface, and the recipient is the INTERFACE rather than the
       device. Sending it to the device is a stall that looks exactly
       like a device without alternate settings. */
    r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                    USB_REQ_SET_INTERFACE, alt, ifnum, 0, 0);
    if (r < 0) return r;

    /* The device has switched. From here the kernel's table MUST follow
       it, even if the controller then refuses -- the alternative is a
       table describing a setting the device has left. */
    *d = next;

    if (h->ops->configure) {
        /* On xHCI this DROPS the previous setting's endpoints as well
           as adding this one's. Dropping is new in v0.8.7 for exactly
           this call: nothing before it configured a slot twice. See
           x_configure(). */
        r = h->ops->configure(h, d);
        if (r != USB_OK) {
            /* The device is on the new setting and the controller is
               not. Reporting the failure with nep zeroed is the only
               state both sides can be described by: there are no
               usable endpoints, which is true, rather than a list of
               endpoints the controller has not been told about. */
            d->nep = 0;
            return r;
        }
    }
    return USB_OK;
}

/* ── v0.8.9.9: two interfaces on one device ─────────────────────────
   include/usb.h has the contract. The case it exists for, on Kyan's HP,
   11 November: the phone was brought up as a NIC at boot (configuration
   index 3, interface 2, alt 1), then `pair` opened usbmux -- which
   called usb_select_config() on the SAME configuration for interface 1.
   SET_CONFIGURATION put interface 2 back on alt 0, which has no
   endpoints, and the core's table was rebuilt with interface 1's two
   endpoints and nothing else, so x_configure() dropped the Ethernet
   pair at the controller too. The NIC stayed registered, pointing at
   endpoints that no longer existed, and every frame it was given
   failed: `ifconfig dhcp` reported 0 asked, 0 offered, 0 acknowledged,
   because the DISCOVER never left the laptop.

   Neither driver was wrong about its own interface. The core simply
   could not hold two. */
int usb_claim_interface(usb_dev_t *d, uint8_t cfgidx, uint8_t ifnum,
                        uint8_t alt)
{
    usb_hc_t *h = hc_of_mut(d);
    usb_dev_t one, next;
    int       r, i;

    if (!d) return USB_EPARAM;
    if (!h)  return USB_ENODEV;
    if (d->nconfigs == 0)   return USB_ENODEV;
    if (cfgidx >= d->nconfigs) return USB_EPARAM;

    /* Not in this configuration: select it. Every interface is reset by
       SET_CONFIGURATION, so there is nothing held to keep -- this is
       the old path exactly. */
    if (d->config == 0 || d->cfgidx != cfgidx) {
        r = do_set_config(h, d, cfgidx, (int)ifnum);
        if (r != USB_OK) return r;
        if (alt == 0) return USB_OK;
        return usb_set_interface(d, ifnum, alt);
    }

    /* Already in it. Walk the new interface into a scratch copy -- the
       walker resets the endpoint table, which is the thing that must
       NOT happen to the real one. */
    r = fetch_config(d, cfgidx);
    if (r < 0) return r;
    one = *d;
    r = usb_parse_config_alt(&one, g_cfgbuf, (uint32_t)r, (int)ifnum,
                             (int)alt);
    if (r != USB_OK) return r;
    if (one.ifnum != ifnum || one.altsetting != alt) return USB_EXFER;

    /* Everything already held for OTHER interfaces stays, marked keep;
       anything held for this interface is replaced by the new setting's
       endpoints. */
    next = *d;
    next.nep = 0;
    for (i = 0; i < d->nep; i++) {
        if (d->ep[i].iface == ifnum) continue;
        next.ep[next.nep] = d->ep[i];
        next.ep[next.nep].keep = 1;
        next.nep++;
    }
    for (i = 0; i < one.nep; i++) {
        if (next.nep >= USB_MAX_EP) return USB_EPARAM;
        next.ep[next.nep] = one.ep[i];
        next.ep[next.nep].keep = 0;
        next.nep++;
    }
    next.ifnum       = one.ifnum;
    next.altsetting  = one.altsetting;
    next.if_class    = one.if_class;
    next.if_subclass = one.if_subclass;
    next.if_protocol = one.if_protocol;

    /* SET_INTERFACE only for a non-zero alternate. After
       SET_CONFIGURATION every interface is on alt 0 already, and a
       device is allowed to STALL SET_INTERFACE on an interface that has
       only one setting -- which the mux interface is. */
    if (alt != 0) {
        r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD |
                        USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
                        alt, ifnum, 0, 0);
        if (r < 0) return r;
    }

    *d = next;
    if (h->ops->configure) {
        r = h->ops->configure(h, d);
        if (r != USB_OK) {
            /* The device has the interface and the controller does not.
               Keep only what was already running, which is true. */
            int k = 0;
            for (i = 0; i < d->nep; i++)
                if (d->ep[i].keep) d->ep[k++] = d->ep[i];
            d->nep = (uint8_t)k;
            for (i = 0; i < d->nep; i++) d->ep[i].keep = 0;
            return r;
        }
    }
    for (i = 0; i < d->nep; i++) d->ep[i].keep = 0;
    return USB_OK;
}

/* Hand every device back to the controller that owns it.
 *
 * Called at the top of usb_enumerate(), before the table is cleared,
 * because after that there is nothing left to say WHICH devices to free.
 *
 * In reverse order, which is not decoration: a device behind a hub must
 * be released before the hub it hangs off, or the controller is asked to
 * free a parent while a child still references it. The table is built
 * parents-first -- a hub is always found before what is on it -- so
 * walking it backwards releases children first.
 *
 * usb_hc_ops_t::detach is optional and UHCI does not supply one; see the
 * note there for why, and for what this leak cost on the HP. */
static void release_all(void)
{
    int i;
    for (i = g_ndev - 1; i >= 0; i--) {
        usb_dev_t *d = &g_dev[i];
        usb_hc_t  *h;
        if (!d->valid) continue;
        if (d->hc >= (uint8_t)g_nhc) continue;
        h = &g_hc[d->hc];
        if (h->ops->detach) h->ops->detach(h, d);
        d->valid = 0;
    }
}

static int usb_enumerate_inner(void)
{
    int c, p;

    /* Before the table is cleared, not after: once g_ndev is zero there
       is no record of what the controller is still holding. */
    release_all();

    g_ndev = 0;
    g_next_addr = 1;

    for (c = 0; c < g_nhc && g_ndev < USB_MAX_DEV; c++) {
        usb_hc_t *h = &g_hc[c];
        int nports = h->ops->port_count ? h->ops->port_count(h) : 0;

        for (p = 0; p < nports && g_ndev < USB_MAX_DEV; p++) {
            usb_dev_t d;
            int r;

            if (h->ops->port_connected && !h->ops->port_connected(h, p))
                continue;
            if (!h->ops->attach) continue;

            memset(&d, 0, sizeof d);
            d.hc   = (uint8_t)c;
            d.port = (uint8_t)p;

            r = h->ops->attach(h, p, &d);
            if (r != USB_OK) {
                /* THE CODE ALONE IS NOT AN ANSWER. -4 can mean the
                   controller is invalid, the command ring refused
                   ENABLE_SLOT, ADDRESS_DEVICE was rejected, or the
                   device never returned its descriptor -- four
                   different bugs. The HP printed "no answer (-4)" for
                   port 12 across five releases and it said nothing
                   about which. (v0.8.8.4) */
                klog(KLOG_WARN, "USB %s hc%u port %u: no answer (%d)",
                     usb_hc_name(h), (uint32_t)c, (uint32_t)p, r);
                if (h->kind == USB_HC_XHCI)
                    klog(KLOG_WARN, "    %s", xhci_attach_why());
                continue;
            }
            /* attach() fills the addressing and device-descriptor fields.
               The controller index is the core's and is set here, not
               there, so a back-end cannot put a device on the wrong
               controller by filling in its own number. */
            d.hc    = (uint8_t)c;
            d.port  = (uint8_t)p;
            d.valid = 1;
            /* A root-port device's path, v0.8.6: route string empty,
               tier 0, no parent, no transaction translator. Set here
               rather than left zero by luck, because usb_attach_behind()
               reads these off the PARENT to build a child's and a field
               that is right only because memset ran is a field somebody
               will move. */
            d.route       = 0;
            d.rootport    = (uint8_t)p;
            d.tier        = 0;
            d.parent      = -1;
            d.parent_port = 0;
            d.tt_slot     = 0;
            d.tt_port     = 0;

            r = setup_config(h, &d);
            if (r != USB_OK) {
                /* A device that answered its device descriptor but not
                   its configuration is still worth listing -- `usb -vv`
                   showing it with no interface is how somebody finds out
                   why it does not work. */
                klog(KLOG_WARN, "USB %s hc%u port %u: %4x:%4x configured "
                                "badly (%d)", usb_hc_name(h),
                     (uint32_t)c, (uint32_t)p,
                     (uint32_t)d.vendor, (uint32_t)d.product, r);
            }

            g_dev[g_ndev++] = d;
            klog(KLOG_OK, "USB %s hc%u port %u: %4x:%4x class %2x, "
                          "address %u, %u endpoint(s)",
                 usb_hc_name(h), (uint32_t)c, (uint32_t)p,
                 (uint32_t)d.vendor, (uint32_t)d.product,
                 (uint32_t)(d.if_class ? d.if_class : d.dev_class),
                 (uint32_t)d.addr, (uint32_t)d.nep);
        }
    }

    /* Then everything behind a hub, v0.8.6. After the root-port walk and
       not during it, because usb_hub_scan() appends to this same table
       and asks a hub questions over the pipe the walk above has only
       just finished building. */
    usb_hub_scan();

    return g_ndev;
}

/* usb_enumerate() is wrapped for the same reason usb_control() is, and
   the reason is sharper here: it REBUILDS the device table, so a poll
   that re-entered it would be walking a table being rewritten under it
   -- and v0.8.7 already paid for the one-layer-up version of that
   (a NIC holding an index into a table usb_enumerate() rebuilds). */
int usb_enumerate(void)
{
    int r;
    kpoll_enter();
    r = usb_enumerate_inner();
    kpoll_leave();
    return r;
}

/* ════════════════════════════════════════════════════════════════════
   Behind a hub
   ════════════════════════════════════════════════════════════════════

   usb/hub.c owns the hub class -- the descriptor, port power, port reset
   and the status bits. This owns how a device is ADDRESSED, which is the
   same line include/usb.h draws between the core and a back-end, drawn
   once more one level up.

   The route string is why. xHCI needs the path to a device as four bits
   per tier, port numbers one-based, tier one in bits 0..3, and a child's
   route is its parent's with its own port shifted into the next nibble.
   That arithmetic belongs beside the device table and nowhere else: a
   hub driver doing it would be the second place that knows how a device
   is addressed, which is the mistake include/usb.h was written after. */
int usb_attach_behind(int hub, int port, uint8_t speed)
{
    usb_dev_t  d;
    usb_dev_t  parent;
    usb_hc_t  *h;
    uint32_t   route;
    int        r;

    if (hub < 0 || hub >= g_ndev) return USB_EPARAM;
    if (port < 0 || port > 14)    return USB_EPARAM;
    if (g_ndev >= USB_MAX_DEV)    return USB_ENOMEM;

    /* A COPY of the parent, taken before anything is appended.
       g_dev[] is an array and usb_attach_behind() appends to it; holding
       a pointer into it across the append would be a pointer to whatever
       the table looks like afterwards. The table does not reallocate
       today, which is exactly the kind of fact that stops being true
       quietly. */
    parent = g_dev[hub];
    if (!parent.valid) return USB_EPARAM;

    /* Five nibbles of route string, so five tiers below the root, which
       is also USB's own limit. Stopping here is what makes the breadth
       loop in usb/hub.c safe against a hub that reports itself as its
       own downstream device. */
    if (parent.tier + 1 >= USB_MAX_TIER) return USB_EPARAM;

    h = &g_hc[parent.hc];
    if (!h->ops->attach_path) {
        /* Stated, not silent. A back-end that cannot address a device
           through a hub makes every device behind one invisible, and
           "nothing appeared" is the least useful possible answer.
           usb/hub.c turns this into one log line with the reason. */
        return USB_EPARAM;
    }

    memset(&d, 0, sizeof d);
    d.hc          = parent.hc;
    d.rootport    = parent.rootport;
    d.port        = parent.rootport;   /* what 0.8.5's screens print */
    d.tier        = (uint8_t)(parent.tier + 1);
    d.parent      = (int8_t)hub;
    d.parent_port = (uint8_t)port;
    /* The child's route: the parent's, with this port one-based in the
       nibble for the child's tier. A tier-1 device (behind one hub) uses
       bits 0..3, which is why the shift is 4*parent.tier and not
       4*d.tier -- off by one here puts the device at a path the
       controller will look for down a different socket. */
    d.route       = parent.route |
                    (((uint32_t)(port + 1) & 0xFu) << (4u * parent.tier));

    /* The transaction translator, for a low or full speed device behind
       a high speed hub: the hub does the speed conversion and the
       controller has to be told which hub and which port, or the device
       never answers at all.

       If the PARENT already has one, it is inherited -- the translator
       belongs to the highest-speed hub in the path, not to the nearest
       one, so a full-speed device two hubs down still points at the hub
       that is doing the conversion. */
    if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
        if (parent.tt_slot) {
            d.tt_slot = parent.tt_slot;
            d.tt_port = parent.tt_port;
        } else if (parent.speed == USB_SPEED_HIGH ||
                   parent.speed == USB_SPEED_SUPER) {
            d.tt_slot = parent.slot;
            d.tt_port = (uint8_t)(port + 1);
        }
    }

    route = d.route;
    r = h->ops->attach_path(h, (int)d.rootport, route, speed,
                            d.tt_slot, d.tt_port, &d);
    if (r != USB_OK) return r;

    /* attach_path() fills the addressing and device-descriptor fields.
       The path is the core's and is restored here, for the same reason
       the controller index is in usb_enumerate(): a back-end cannot put
       a device somewhere it is not by filling in its own numbers.

       `route` is in this list because tests/test_hub.c's simulated
       back-end scribbles over every path field -- which a real one could
       -- and the first version of this block restored five of the six.
       The route is the one the CORE built; a back-end's idea of it is
       the second place that knows how a device is addressed, which is
       the mistake include/usb.h was written after. */
    d.hc          = parent.hc;
    d.rootport    = parent.rootport;
    d.port        = parent.rootport;
    d.route       = route;
    d.tier        = (uint8_t)(parent.tier + 1);
    d.parent      = (int8_t)hub;
    d.parent_port = (uint8_t)port;
    d.valid       = 1;

    r = setup_config(h, &d);
    if (r != USB_OK)
        klog(KLOG_WARN, "USB %s hub %u port %u: %4x:%4x configured "
                        "badly (%d)", usb_hc_name(h),
             (uint32_t)parent.addr, (uint32_t)(port + 1),
             (uint32_t)d.vendor, (uint32_t)d.product, r);

    g_dev[g_ndev] = d;
    klog(KLOG_OK, "USB %s hub %u port %u: %4x:%4x class %2x, address %u, "
                  "%u endpoint(s), route %5x",
         usb_hc_name(h), (uint32_t)parent.addr, (uint32_t)(port + 1),
         (uint32_t)d.vendor, (uint32_t)d.product,
         (uint32_t)(d.if_class ? d.if_class : d.dev_class),
         (uint32_t)d.addr, (uint32_t)d.nep, d.route);
    return g_ndev++;
}

/* The back-ends. Declared here rather than in a header because this is
   the only place that may call them, and the order of these two calls is
   the order devices appear in the listing. */
extern int uhci_hc_register(void);
extern int xhci_hc_register(void);

/* One interrupt IN poll that does not block -- see usb_hc_ops_t::intr_poll.
   A back-end without one gets the blocking path, which is what UHCI uses:
   a UHCI transfer descriptor records a NAK as a status the driver can
   read, so a poll there returns promptly on its own. */
/* ── ONE non-blocking IN poll, with two blocking fallbacks, v0.8.7 ────
 *
 * usb_hc_ops_t::intr_poll is named for the endpoint type it was written
 * for, and it is endpoint-type AGNOSTIC: x_intr_poll() works off the
 * DCI and the endpoint's ring, and x_bulk() and x_intr() are both
 * x_data(). So the same op serves a BULK IN endpoint, which is what a
 * USB NIC's receive path is.
 *
 * That matters because nothing in this kernel runs periodically.
 * net_poll() is called from the shell's idle loop, so a receive that
 * blocks is a shell that stops responding -- and a bulk IN endpoint
 * with no packet waiting NAKs, which an xHCI retries for ever rather
 * than completing. That is v0.8.5 shipping a keyboard at one character
 * per second, exactly, one endpoint type along.
 *
 * The only difference between the two names below is which blocking
 * call they fall back to on a back-end with no intr_poll -- UHCI, where
 * a NAK is a status bit the driver reads rather than a silent retry, so
 * both return promptly. One body, because two would be two. */
static int in_poll(const usb_dev_t *d, uint8_t ep, int in, void *data,
                   int len, int mps, int bulk)
{
    usb_hc_t *h;
    if (!d || !d->valid || d->hc >= (uint8_t)g_nhc) return USB_EPARAM;
    h = &g_hc[d->hc];
    if (h->ops->intr_poll)
        return h->ops->intr_poll(h, d, ep, in, data, len, mps);
    return bulk ? usb_bulk(d, ep, in, data, len, mps)
                : usb_interrupt(d, ep, in, data, len, mps);
}

int usb_intr_poll(const usb_dev_t *d, uint8_t ep, int in, void *data,
                  int len, int mps)
{
    return in_poll(d, ep, in, data, len, mps, 0);
}

int usb_bulk_poll(const usb_dev_t *d, uint8_t ep, int in, void *data,
                  int len, int mps)
{
    return in_poll(d, ep, in, data, len, mps, 1);
}

int usb_init(void)
{
    g_nhc  = 0;
    g_ndev = 0;

    /* UHCI first: on a machine with an EHCI and its companions, the
       companions are the bus this kernel can actually drive, and taking
       them before xHCI registers keeps the numbering stable on the
       machines that have had it since 0.8 Beta. */
    uhci_hc_register();
    xhci_hc_register();

    if (g_nhc == 0) return 0;
    return usb_enumerate();
}
