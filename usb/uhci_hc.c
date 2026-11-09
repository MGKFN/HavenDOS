/*
 * usb/uhci_hc.c — UHCI, behind the core's host-controller interface.
 *
 * ── WHY drivers/uhci.c IS NOT EDITED ─────────────────────────────────
 *
 * RM-1003 asks for UHCI to be moved behind the core in the same release
 * that adds xHCI, "where its 110 existing checks catch any mistake". The
 * cheapest way to let those checks do that job is not to touch the file
 * they test. So drivers/uhci.c is unchanged, tests/test_uhci.c still
 * compiles it and still tests exactly what it tested before, and this
 * file is the adapter that puts it behind usb_hc_ops_t.
 *
 * What that buys: above this line nothing knows a UHCI exists. The
 * device table, the descriptor walk, the address counter and every class
 * driver see usb_dev_t and usb_control(). The second back-end lands
 * beside this one rather than beside the driver.
 *
 * ── THE ONE THING UHCI CANNOT DO HONESTLY ────────────────────────────
 *
 * It has no interrupt transfers. include/uhci.h says so in its own
 * header comment: the frame list points every one of its 1024 entries at
 * the same control queue head, because "this kernel has no periodic
 * (interrupt or isochronous) transfers".
 *
 * A UHCI transfer descriptor does not actually distinguish the two -- an
 * interrupt endpoint and a bulk endpoint are the same TD with a different
 * endpoint number -- so intr() below runs the transfer through the bulk
 * queue, and it works. What is lost is the SCHEDULING: a real interrupt
 * endpoint is polled by the controller every bInterval frames whether or
 * not anybody asked, and this is polled only when something calls. A
 * device with nothing to report NAKs, the TD stays active, and the
 * transfer ends in UHCI_ETIMEOUT rather than in "no report this time".
 *
 * For a boot keyboard that is the difference between a key being noticed
 * in a millisecond and being noticed when the shell next polls, which is
 * what this kernel does for PS/2 anyway. It is written down here rather
 * than discovered later, and it is not a problem on the machine this
 * release is aimed at: that one has no UHCI at all.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/usb.h"
#include "../include/uhci.h"

/* The core's address counter, so two controllers cannot both hand out
   address 1 and make the `usb` listing ambiguous. */
extern uint8_t usb_core_next_address(void);
extern void    sleep_ms(uint32_t ms);

/* ── usb_dev_t <-> uhci_dev_t ─────────────────────────────────────────
 *
 * The transfer entry points in drivers/uhci.c take a uhci_dev_t, and
 * they read four fields out of it: hc, addr, lowspeed and maxpacket0.
 * Rebuilding one per call is cheaper than keeping two copies of a device
 * in step, and a copy that can drift is how a device ends up addressed
 * as itself on the wrong controller. */
static void to_uhci(const usb_hc_t *h, const usb_dev_t *d, uhci_dev_t *u)
{
    memset(u, 0, sizeof *u);
    u->valid      = 1;
    u->hc         = (uint8_t)h->index;
    u->port       = d->port;
    u->addr       = d->addr;
    u->lowspeed   = (d->speed == USB_SPEED_LOW) ? 1 : 0;
    u->maxpacket0 = d->maxpacket0 ? d->maxpacket0 : 8;
}

/* UHCI's error codes and the core's are the same numbers on purpose --
   include/usb.h says so -- but going through a function means a future
   divergence is a compile error here rather than a silent remapping. */
static int xlate(int r)
{
    switch (r) {
    case UHCI_OK:       return USB_OK;
    case UHCI_ETIMEOUT: return USB_ETIMEOUT;
    case UHCI_ESTALL:   return USB_ESTALL;
    case UHCI_EXFER:    return USB_EXFER;
    case UHCI_ENODEV:   return USB_ENODEV;
    case UHCI_EPARAM:   return USB_EPARAM;
    case UHCI_ENOMEM:   return USB_ENOMEM;
    default:            return r;    /* a byte count, which is positive */
    }
}

static int hc_port_count(usb_hc_t *h)
{
    return uhci_port_count(h->index);
}

static int hc_port_connected(usb_hc_t *h, int port)
{
    return uhci_port_connected(h->index, port);
}

static int hc_attach(usb_hc_t *h, int port, usb_dev_t *out)
{
    uhci_dev_t u;
    int r = uhci_probe_port(h->index, port, &u);
    if (r != UHCI_OK) return xlate(r);

    out->addr         = u.addr;
    out->speed        = u.lowspeed ? USB_SPEED_LOW : USB_SPEED_FULL;
    out->maxpacket0   = u.maxpacket0;
    out->vendor       = u.vendor;
    out->product      = u.product;
    out->release      = u.release;
    out->dev_class    = u.dev_class;
    out->dev_subclass = u.dev_subclass;
    out->dev_protocol = u.dev_protocol;
    out->nconfigs     = u.nconfigs;
    out->slot         = 0;            /* xHCI's field; nothing here uses it */
    return USB_OK;
}

/* ── a device behind a hub, v0.8.6 ───────────────────────────────────
 *
 * drivers/uhci.c is still not edited, so this cannot call
 * uhci_probe_port(): that function resets a ROOT port, which for a
 * hub-attached device would reset the hub and everything else on it.
 *
 * It does not need to. A hub is transparent on UHCI -- there is no route
 * string, no slot and no transaction translator, and the three arguments
 * carrying them are ignored here, which is the same asymmetry
 * configure() exists for. What a device behind a hub needs is exactly
 * what a device on a root port needs minus the reset: ask the thing
 * answering at address 0 how big its default pipe is, give it an
 * address, and read its device descriptor over the pipe that now exists.
 *
 * All three steps go through uhci_control(), which is the unchanged
 * driver's own public transfer entry point, so its 110 checks still
 * cover the path this uses.
 *
 * ── WHY THIS IS WRITTEN AT ALL, GIVEN THE MACHINE ────────────────────
 *
 * The HP has no UHCI. This back-end will not run on it, and the one
 * that will is xHCI. It is here because an interface with a single
 * implementation is a wrapper that proves nothing -- include/usb.h
 * opens with that argument and this release is where attach_path() would
 * otherwise have become the exception to it. With two, the compiler
 * keeps them honest and tests/test_hub.c can check the core's route
 * arithmetic against a back-end that does not use it.
 *
 * Unproved on hardware: no machine here has a UHCI with a hub on it.
 * QEMU's `-usb` gives one, which is how it is exercised at all.
 */
static int hc_attach_path(usb_hc_t *h, int rootport, uint32_t route,
                          uint8_t speed, uint32_t tt_slot, uint8_t tt_port,
                          usb_dev_t *out)
{
    usb_dev_t z;
    uint8_t   desc[18];
    uint8_t   addr;
    int       r;

    /* A hub is transparent here. Named and discarded rather than cast to
       void in a clump, so the next person can see WHICH parts of the
       xHCI model do not apply. */
    (void)route;      /* no route string: addressing is by address      */
    (void)tt_slot;    /* no transaction translator object               */
    (void)tt_port;

    /* Talk to whatever is answering at address 0. Eight bytes, because
       byte 7 is the default pipe's packet size and asking for more than
       the pipe can carry before you know its size is how a full-speed
       device returns a short read and looks broken. */
    memset(&z, 0, sizeof z);
    z.valid      = 1;
    z.hc         = (uint8_t)h->index;
    z.port       = (uint8_t)rootport;
    z.addr       = 0;
    z.speed      = speed;
    z.maxpacket0 = 8;

    memset(desc, 0, sizeof desc);
    r = usb_control(&z, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR,
                    (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 8);
    if (r < 8) return (r < 0) ? r : USB_EXFER;
    if (desc[7]) z.maxpacket0 = desc[7];

    /* The address comes from the core's counter, so two controllers --
       and now two tiers -- cannot both hand out address 1 and make the
       `usb` listing ambiguous. */
    addr = usb_core_next_address();
    if (!addr) return USB_ENOMEM;

    r = usb_control(&z, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_ADDRESS, addr, 0, 0, 0);
    if (r < 0) return r;
    /* The specification gives a device 2 ms to adopt it, and a device
       asked a question too early answers at neither address. */
    sleep_ms(5);

    z.addr = addr;

    memset(desc, 0, sizeof desc);
    r = usb_control(&z, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR,
                    (uint16_t)(USB_DESC_DEVICE << 8), 0, desc, 18);
    if (r < 18) return (r < 0) ? r : USB_EXFER;

    out->addr         = addr;
    out->speed        = speed;
    out->maxpacket0   = desc[7] ? desc[7] : z.maxpacket0;
    out->dev_class    = desc[4];
    out->dev_subclass = desc[5];
    out->dev_protocol = desc[6];
    out->vendor       = (uint16_t)(desc[8]  | (desc[9]  << 8));
    out->product      = (uint16_t)(desc[10] | (desc[11] << 8));
    out->release      = (uint16_t)(desc[12] | (desc[13] << 8));
    out->nconfigs     = desc[17];
    out->slot         = 0;            /* xHCI's field; nothing here uses it */
    return USB_OK;
}

static int hc_control(usb_hc_t *h, const usb_dev_t *d, uint8_t rtype,
                      uint8_t req, uint16_t value, uint16_t index,
                      void *data, uint16_t len)
{
    uhci_dev_t u;
    to_uhci(h, d, &u);
    return xlate(uhci_control(&u, rtype, req, value, index, data, len));
}

static int hc_bulk(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                   void *data, int len, int mps)
{
    uhci_dev_t u;
    to_uhci(h, d, &u);
    return xlate(uhci_bulk(&u, ep, in, data, len, mps));
}

/* See the header comment: the same TD, through the bulk queue. */
static int hc_intr(usb_hc_t *h, const usb_dev_t *d, uint8_t ep, int in,
                   void *data, int len, int mps)
{
    uhci_dev_t u;
    to_uhci(h, d, &u);
    return xlate(uhci_bulk(&u, ep, in, data, len, mps));
}

static void hc_reset_toggle(usb_hc_t *h, const usb_dev_t *d, uint8_t ep,
                            int in)
{
    uhci_dev_t u;
    to_uhci(h, d, &u);
    uhci_bulk_reset_toggle(&u, ep, in);
}

/* Nothing to do. A UHCI endpoint exists because the device says it does;
   there is no controller-side object to create. This is the asymmetry
   include/usb.h exists to carry -- on xHCI the same call is a Configure
   Endpoint command and every endpoint beyond the default pipe is dead
   without it. */
static int hc_configure(usb_hc_t *h, usb_dev_t *d)
{
    (void)h; (void)d;
    return USB_OK;
}

/* Designated, not positional -- see the note on XHCI_OPS.

   detach is deliberately absent too, v0.8.6.1. On xHCI the controller
   owns the device and a slot has to be handed back with Disable Slot or
   `usb rescan` runs the controller out of them -- which is what the HP
   did. Here an address is just a number this kernel hands out, and
   usb_enumerate() resets the counter before it walks, so there is
   nothing held and nothing to give back. That is the same asymmetry
   configure() exists for.

   intr_poll is deliberately absent: UHCI needs no non-blocking poll,
   because a UHCI transfer descriptor records a NAK as a status bit the
   driver reads rather than retrying silently, so hc_intr() returns
   promptly on its own. usb_intr_poll() falls back to it. */
static const usb_hc_ops_t UHCI_OPS = {
    .name           = "UHCI",
    .port_count     = hc_port_count,
    .port_connected = hc_port_connected,
    .attach         = hc_attach,
    .attach_path    = hc_attach_path,
    .control        = hc_control,
    .bulk           = hc_bulk,
    .intr           = hc_intr,
    .reset_toggle   = hc_reset_toggle,
    .configure      = hc_configure,
};

/* Called by usb_init(). Brings the hardware up through the unchanged
   driver, then registers one core controller per UHCI the driver found
   -- an ICH has two to five, and an EHCI's companions are split two
   ports at a time, so registering only the first finds a device just in
   case it was plugged into the right socket. */
int uhci_hc_register(void)
{
    int n, i, reg = 0;

    if (!uhci_init()) return 0;

    n = uhci_hc_count();
    for (i = 0; i < n; i++) {
        if (usb_register_hc(&UHCI_OPS, i, uhci_io_base(i), USB_HC_UHCI) >= 0)
            reg++;
    }
    return reg;
}
