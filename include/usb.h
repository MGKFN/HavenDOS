#pragma once
#include "types.h"

/* The USB core: one device table, one descriptor walk, one place that
 * assigns addresses and chooses configurations.
 *
 * ── WHY THIS EXISTS, AND WHY IT HAS TWO BACK-ENDS FROM THE FIRST DAY ──
 *
 * v0.8.3.1 was found to have shipped three bugs that were all one mistake:
 * the installer had one block layer and the filesystem had another, and
 * only one of them got taught about NVMe. Four screens each worked a bus
 * label out independently and only one of them knew, so the disk picker
 * said NVMe and the screen gated behind typing YES said VirtIO.
 *
 * drivers/uhci.c is controller-shaped all the way up: the device struct is
 * uhci_dev_t, the transfers are uhci_control() and uhci_bulk(), and the
 * enumeration walk lives inside the driver. Writing drivers/xhci.c beside
 * it would produce two device models, two enumeration walks, and two
 * places that decide which controller a device is on -- the same mistake,
 * in the layer that input, removable storage and the only available
 * network adapter all sit on.
 *
 * So this interface is born with TWO implementations rather than one, and
 * UHCI is moved onto it in the same release that adds xHCI -- in that
 * order, so that its 110 existing checks catch the move before any new
 * code can be blamed. An abstraction with a single implementation is a
 * wrapper and proves nothing; with two, the compiler keeps them honest.
 *
 * ── WHERE THE LINE BETWEEN CORE AND BACK-END IS ───────────────────────
 *
 * Not where it first looks. The obvious split -- "the core enumerates,
 * the driver moves bytes" -- does not survive contact with xHCI, because
 * the two controllers disagree about who owns a device's address:
 *
 *   UHCI  the driver is dumb. It sends SET_ADDRESS as an ordinary control
 *         transfer to address 0 and then puts the new address in every
 *         transfer descriptor itself.
 *
 *   xHCI  the CONTROLLER owns the device. The driver issues Enable Slot,
 *         builds an input context, and issues Address Device; the
 *         controller sends SET_ADDRESS on its own and remembers the
 *         result. Endpoints beyond the default pipe do not exist until a
 *         Configure Endpoint command has been accepted.
 *
 * So the back-end owns everything up to "there is an addressed device on
 * this port and its default pipe works", plus the endpoint configuration
 * step, and the core owns everything after: the configuration descriptor,
 * the interface and endpoint walk, SET_CONFIGURATION, the device table,
 * and the class drivers that read it.
 *
 * usb_hc_ops_t::configure() is a no-op for UHCI and real work for xHCI.
 * That asymmetry is the whole reason this header exists.
 */

/* ── transfer results ─────────────────────────────────────────────────
 * The same numbering drivers/uhci.c has always used, so a value that
 * reaches a caller means what it used to mean. */
#define USB_OK          0
#define USB_ETIMEOUT  (-1)   /* the controller never finished the chain   */
#define USB_ESTALL    (-2)   /* the device returned STALL: request refused */
#define USB_EXFER     (-3)   /* CRC, babble, bitstuff or buffer error     */
#define USB_ENODEV    (-4)   /* no controller, or nothing on the port     */
#define USB_EPARAM    (-5)
#define USB_ENOMEM    (-6)   /* more descriptors than the pool holds      */
#define USB_ECMD      (-7)   /* a controller command was refused (xHCI)   */

/* ── standard requests and descriptor types ───────────────────────────
 * These are the USB specification's, not ours, and drivers/uhci.c already
 * carried them. They live here now so that one copy exists. */
#define USB_DIR_OUT         0x00
#define USB_DIR_IN          0x80
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_DESCRIPTOR    7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE    11

#define USB_DESC_DEVICE     1
#define USB_DESC_CONFIG     2
#define USB_DESC_STRING     3
#define USB_DESC_INTERFACE  4
#define USB_DESC_ENDPOINT   5

#define USB_FEATURE_ENDPOINT_HALT 0

/* bmAttributes & 3 */
#define USB_EP_CONTROL  0
#define USB_EP_ISOC     1
#define USB_EP_BULK     2
#define USB_EP_INTR     3

/* Speeds. Low and full are what UHCI reports; high and super only ever
   come from xHCI. The numbering is the xHCI PORTSC speed field's, because
   that is the one with four values to express. */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

#define USB_MAX_EP      8    /* per device, beyond the default pipe       */
#define USB_MAX_DEV    16
#define USB_MAX_HC      8

typedef struct {
    uint8_t  addr;       /* bEndpointAddress, direction bit included     */
    uint8_t  attr;       /* bmAttributes & 3: bulk, interrupt, isoc      */
    uint8_t  interval;   /* bInterval, frames or 2^(n-1) microframes     */
    uint16_t mps;        /* wMaxPacketSize                               */
    /* ── v0.8.9.9: WHOSE ENDPOINT ───────────────────────────────────
       An iPhone is ONE device with TWO drivers on it: usbmux on
       interface 1 and Apple Ethernet on interface 2, both in
       configuration index 3. Until this release the core held one
       interface per device, so whichever driver came second selected
       the configuration again and the first lost its endpoints -- on
       the HP, pairing took the Ethernet interface away and every frame
       DHCP tried to send was refused before it reached the phone.
       See usb_claim_interface(). */
    uint8_t  iface;      /* bInterfaceNumber this endpoint belongs to    */
    uint8_t  keep;       /* configure(): already live, leave it alone    */
} usb_ep_t;

typedef struct usb_dev {
    uint8_t  valid;
    uint8_t  hc;           /* index into the core's controller table      */
    uint8_t  port;         /* root port on that controller                */
    uint8_t  addr;         /* USB address, assigned by the back-end       */
    uint8_t  speed;        /* USB_SPEED_*                                 */
    /* ── v0.8.6.7: SIXTEEN BITS, AND WHY ──────────────────────────
       A SuperSpeed device's endpoint 0 carries 512 bytes. This field
       was a uint8_t, so default_mps() returning 512 was truncated to
       ZERO on every SuperSpeed device -- and the correction path below
       then compared the descriptor's byte against that zero, decided
       they differed, and reprogrammed a working pipe.

       See mps0_from_desc() in usb/xhci_hc.c for the other half: at
       SuperSpeed the descriptor's bMaxPacketSize0 is an EXPONENT. */
    uint16_t maxpacket0;   /* endpoint 0 packet size, in BYTES          */

    uint16_t vendor, product, release;
    uint8_t  dev_class, dev_subclass, dev_protocol;
    uint8_t  nconfigs;

    /* the configuration and interface the core selected */
    uint8_t  config;       /* bConfigurationValue in use, 0 if none       */
    /* ── v0.8.7: WHICH DESCRIPTOR, not which VALUE ────────────────
       `config` is bConfigurationValue -- the number SET_CONFIGURATION
       takes -- and the two are not the same thing. GET_DESCRIPTOR takes
       an INDEX, and a device is free to number its configurations 1, 3,
       4, 7 while they sit at indices 0, 1, 2, 3.

       usb_set_interface() has to re-read the descriptor of the
       configuration IN USE in order to find an alternate setting's
       endpoints, so the index it was fetched by has to be remembered.
       Deriving it from `config` would be guessing at a mapping the
       device never promised. */
    uint8_t  cfgidx;       /* descriptor index `config` was fetched by    */
    uint8_t  ifnum;
    uint8_t  altsetting;   /* bAlternateSetting in use; 0 unless set      */
    uint8_t  if_class, if_subclass, if_protocol;
    uint8_t  nep;
    usb_ep_t ep[USB_MAX_EP];

    /* back-end scratch. xHCI keeps its slot id here; UHCI leaves it 0.
       Nothing in the core reads it. */
    uint32_t slot;

    /* ── WHERE ON THE BUS, v0.8.6 ─────────────────────────────────
       Until this release `port` was the whole answer, because only root
       ports were enumerated. A USB stick is far more likely to be behind
       a hub than a keyboard is, so 0.8.6 walks hubs -- and the two
       controllers disagree about what a device behind one even IS:

         UHCI  nothing. A device behind a hub is addressed by its
               address like any other; the hub is transparent.
         xHCI  the controller must be TOLD the path. The slot context
               carries a 20-bit Route String -- four bits per tier, tier
               one in bits 0..3, port numbers ONE-based -- plus the root
               hub port the whole path hangs off.

       So these fields are the xHCI model, carried by the core because
       the core is what builds a child's route from its parent's, and
       ignored by UHCI. `port` stays what it was for a root-port device,
       which is every device 0.8.5 could find.                        */
    uint32_t route;      /* xHCI route string; 0 for a root-port device  */
    uint8_t  rootport;   /* the root port the whole path hangs off       */
    uint8_t  tier;       /* 0 on a root port, 1 behind one hub, ...      */
    int8_t   parent;     /* index of the hub in the core's table, or -1  */
    uint8_t  parent_port;/* which port of that hub, zero-based           */

    /* Transaction-translator fields, for a LOW or FULL speed device
       behind a HIGH speed hub. The hub does the speed conversion and the
       controller has to know which hub and which of its ports, or the
       device never answers. Zero when the device's own speed is high or
       super, or when it is on a root port. */
    uint32_t tt_slot;    /* the hub's slot id (xHCI), 0 if none          */
    uint8_t  tt_port;    /* its port, one-based, 0 if none               */
} usb_dev_t;

/* ── the host controller interface ──────────────────────────────────── */

struct usb_hc;

typedef struct {
    const char *name;                 /* "UHCI", "xHCI" -- see usb_hc_name */

    int  (*port_count)(struct usb_hc *);
    int  (*port_connected)(struct usb_hc *, int port);

    /* Reset the port, give the device an address, and leave its default
       pipe usable. Fills the addressing and device-descriptor fields of
       *out and nothing else: the configuration walk belongs to the core
       and must not be duplicated here. */
    int  (*attach)(struct usb_hc *, int port, usb_dev_t *out);

    int  (*control)(struct usb_hc *, const usb_dev_t *, uint8_t rtype,
                    uint8_t req, uint16_t value, uint16_t index,
                    void *data, uint16_t len);
    int  (*bulk)(struct usb_hc *, const usb_dev_t *, uint8_t ep, int in,
                 void *data, int len, int mps);
    int  (*intr)(struct usb_hc *, const usb_dev_t *, uint8_t ep, int in,
                 void *data, int len, int mps);
    void (*reset_toggle)(struct usb_hc *, const usb_dev_t *, uint8_t ep,
                         int in);

    /* ── NON-BLOCKING interrupt IN, v0.8.5.1 ─────────────────────────
       Optional. A back-end that does not supply it falls back to intr(),
       which is correct but blocks.

       This exists because intr() CANNOT be used to poll a keyboard on
       xHCI. An interrupt IN endpoint with nothing to report NAKs, and an
       xHCI retries a NAK forever rather than completing the transfer
       with an error -- so intr() waits out its whole timeout on every
       poll with no keypress, and leaves a live transfer descriptor in a
       16-entry ring behind it. v0.8.5 shipped that: the keyboard worked,
       at about one character per second, and anything typed faster was
       dropped. It took booting it to see.

       The contract:
         >0  bytes were received into data
          0  nothing yet -- not an error, and the usual answer
         <0  a real failure

       It must never block. A back-end keeps ONE transfer outstanding per
       endpoint and reports its completion when it arrives, rather than
       starting one and waiting. */
    int  (*intr_poll)(struct usb_hc *, const usb_dev_t *, uint8_t ep,
                      int in, void *data, int len, int mps);

    /* ── ATTACH A DEVICE THAT IS NOT ON A ROOT PORT, v0.8.6 ──────────
       Optional. A back-end without one cannot enumerate behind a hub,
       and usb/hub.c says so in the log rather than finding nothing and
       leaving somebody to wonder why their stick is invisible.

       The difference from attach() is what has ALREADY happened: the hub
       driver has powered the port, reset it and read the device's speed
       off the hub's own port status. So this must NOT reset a root port
       -- doing so would reset a different device, or the whole path.

       `route` and `rootport` are the xHCI model (see usb_dev_t), and
       `tt_slot`/`tt_port` carry the transaction translator for a low or
       full speed device behind a high speed hub. A back-end for which a
       hub is transparent ignores all four, which is the same asymmetry
       configure() exists for. */
    int  (*attach_path)(struct usb_hc *, int rootport, uint32_t route,
                        uint8_t speed, uint32_t tt_slot, uint8_t tt_port,
                        usb_dev_t *out);

    /* ── GIVE A DEVICE BACK, v0.8.6.1 ───────────────────────────────
       Optional, and the exact counterpart of attach()/attach_path().

       THIS WAS MISSING AND IT WAS A LEAK. usb_enumerate() sets the
       device count back to zero and walks the bus again, which is what
       `usb rescan` does -- but on xHCI the CONTROLLER owns the device,
       so forgetting it on this side does not free anything on that
       side. Every rescan issued a fresh Enable Slot and never a Disable
       Slot.

       Found on the HP, which has 8 slots and 3 devices. The boot took
       slots 1, 2 and 3; the first `usb rescan` took 4, 5 and 6 -- the
       addresses are in the photograph. The second takes 7 and 8, the
       THIRD loses a device, and after that the bus reports nothing until
       the machine is rebooted. The per-slot transfer rings leak with
       them.

       QEMU never showed it because nothing rescanned four times.

       A back-end for which this means nothing does not supply it: on
       UHCI an address is a number this kernel hands out and
       usb_enumerate() resets the counter, so there is nothing held. That
       is the same asymmetry configure() exists for, and it is why this
       is an op rather than something the core does itself. */
    void (*detach)(struct usb_hc *, const usb_dev_t *);

    /* Tell the controller which endpoints the chosen configuration has.
       A no-op on UHCI, where endpoints need no setting up. On xHCI this
       is the Configure Endpoint command, and without it every endpoint
       beyond the default pipe returns USB_ECMD. Called by the core after
       SET_CONFIGURATION has been accepted. */
    int  (*configure)(struct usb_hc *, usb_dev_t *);
} usb_hc_ops_t;

typedef struct usb_hc {
    const usb_hc_ops_t *ops;
    int      index;      /* the back-end's own controller number          */
    uint32_t base;       /* I/O base (UHCI) or MMIO base (xHCI), to show   */
    uint8_t  kind;       /* USB_HC_*                                       */
} usb_hc_t;

#define USB_HC_NONE  0
#define USB_HC_UHCI  1
#define USB_HC_XHCI  2

/* ── the core ─────────────────────────────────────────────────────────  */

/* Register every controller the back-ends can find, then enumerate every
   port of every one. Returns the number of devices found. Safe on a
   machine with no USB hardware at all. */
int usb_init(void);

/* Enumerate again without re-registering controllers -- for a `usb rescan`
   and for the moment after a hub port settles. Returns the device count. */
int usb_enumerate(void);

/* Register one controller. Called by the back-ends from their own init,
   which is what makes the order of registration theirs and not the core's:
   on a machine with both, the devices on each are found in the order the
   controllers were registered, and that order is the order of the two
   calls in usb_init(). */
int usb_register_hc(const usb_hc_ops_t *ops, int index, uint32_t base,
                    uint8_t kind);

int                usb_hc_count(void);
const usb_hc_t    *usb_hc(int i);
const char        *usb_hc_name(const usb_hc_t *h);

int                usb_device_count(void);
const usb_dev_t   *usb_device(int i);

/* ── enumerating behind a hub, v0.8.6 ────────────────────────────────
 *
 * usb/hub.c owns the hub class: the hub descriptor, port power, port
 * reset and the port status bits. This is the half that belongs to the
 * core, because building a child's route string from its parent's is the
 * device table's job and not a class driver's -- a hub driver that did
 * its own arithmetic there would be the second place that knows how a
 * device is addressed.
 *
 * `hub` is an index into the core's table and must BE a hub; `port` is
 * zero-based on that hub. Returns the new device's index, or a negative
 * USB_E* value. The device is added to the table and configured, so a
 * hub found behind a hub is immediately scannable in turn. */
int usb_attach_behind(int hub, int port, uint8_t speed);

/* How many tiers of hub the route string can express: five nibbles, so
 * five tiers below the root. USB's own limit is the same number for the
 * same reason. usb/hub.c stops there rather than trusting a cable. */
#define USB_MAX_TIER 5

/* ── usb/hub.c ────────────────────────────────────────────────────────
 * Walk every hub in the table and bring up what is on its ports, by
 * tier, until a tier adds nothing. Called by usb_enumerate() after the
 * root-port walk, and again by `usb rescan`. Returns devices added.
 *
 * A hub found behind a hub is scanned on the next tier, so this is one
 * call and not a recursion the caller has to repeat. */
int usb_hub_scan(void);
int usb_hub_count(void);      /* hubs seen by the last scan             */
int usb_hub_devices(void);    /* devices found behind them              */

/* ── usb/hotplug.c, v0.8.8 ────────────────────────────────────────────
 * The one copy of the rescan sequence -- the order is the order the
 * layers stack and every step is there because of a bug, all named in
 * that file -- plus the periodic root-port check that calls it. Both
 * `usb rescan` and the kpoll client go through usb_rescan_all(); two
 * copies of this sequence would drift, which is what
 * tools/check_diskname_sync.py refuses one layer down. */
typedef struct {
    int devices;   /* usb_enumerate()   */
    int disks;     /* msc_rescan()      */
    int blkdevs;   /* blk_scan()        */
    int nics;      /* usbnet_probe()    */
    int hubs;
    int behind;
} usb_rescan_t;

void                usb_rescan_all(void);
const usb_rescan_t *usb_rescan_result(void);
void                usb_kpoll_register(void);
void                usb_hotplug_baseline(void);
uint32_t            usb_hotplug_changes(void);
uint32_t            usb_hotplug_rescans(void);
uint32_t            usb_hotplug_truncated(void);
/* v0.8.9.6: storms held off, and zero-device retries run. */
uint32_t            usb_hotplug_holds(void);
uint32_t            usb_hotplug_retries(void);

/* Transfers. These take the device and find its controller, so no caller
   above this line ever names a controller type -- which is the whole
   point, and the thing four screens got wrong in 0.8.3.1. */
int  usb_control(const usb_dev_t *d, uint8_t rtype, uint8_t req,
                 uint16_t value, uint16_t index, void *data, uint16_t len);
int  usb_bulk(const usb_dev_t *d, uint8_t ep, int in, void *data, int len,
              int mps);
/* Poll an interrupt IN endpoint without blocking: >0 bytes, 0 nothing
   yet, <0 a failure. This is what a driver polling a keyboard or a mouse
   should call -- usb_interrupt() below waits for a transfer that a NAKing
   device will not complete. (v0.8.5.1) */
int  usb_intr_poll(const usb_dev_t *d, uint8_t ep, int in, void *data,
                   int len, int mps);

/* The same, for a BULK IN endpoint. One op serves both -- see in_poll()
   in usb/core.c -- and the difference is only which blocking call it
   falls back to on a back-end without one. A USB NIC's receive path
   needs this: net_poll() runs from the shell's idle loop and nothing in
   a driver may not depend on the bottom half, so a receive that blocks is a shell that
   stops answering. (v0.8.7) */
int  usb_bulk_poll(const usb_dev_t *d, uint8_t ep, int in, void *data,
                   int len, int mps);

int  usb_interrupt(const usb_dev_t *d, uint8_t ep, int in, void *data,
                   int len, int mps);
void usb_reset_toggle(const usb_dev_t *d, uint8_t ep, int in);

/* Clear a halted endpoint: CLEAR_FEATURE(ENDPOINT_HALT) and then put the
   data toggle back to DATA0, which the specification requires and which
   is easy to forget separately. One function so it cannot be half done. */
int  usb_clear_halt(const usb_dev_t *d, uint8_t ep, int in);

/* Find the first endpoint of a kind and direction on a device. Returns 0
   if there is none, so a class driver that needs one can refuse early
   rather than part-way through a transfer. */
const usb_ep_t *usb_find_ep(const usb_dev_t *d, int attr, int in);
/* v0.8.9.9: the same, restricted to one interface. A device with two
   claimed interfaces has two bulk pairs, and the first match of the
   plain search belongs to whichever was parsed first. */
const usb_ep_t *usb_find_ep_if(const usb_dev_t *d, int iface, int attr,
                               int in);

/* ── the descriptor walk, exposed because it is worth testing alone ───
 *
 * Parses a configuration descriptor blob into the interface and endpoint
 * fields of *d. Pure: it does no I/O and takes the bytes it is given, so
 * tests/test_usb.c compiles it natively and feeds it descriptors captured
 * from real devices -- including the malformed ones, which is the point.
 * A length field inside a descriptor is a number that came off the wire
 * from somebody else's firmware, and the walk must not trust it.
 *
 * `want_if` selects which interface to adopt, or -1 for the first one.
 * Returns USB_OK, or USB_EXFER on a blob that does not parse.
 *
 * Alternate setting 0 -- see usb_walk_config_alt() for the other ones,
 * and for why this stayed the default. */
int usb_parse_config(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                     int want_if);

/* The same, adopting a named ALTERNATE SETTING of that interface.
 * usb_parse_config() is this with want_alt 0. */
int usb_parse_config_alt(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                         int want_if, int want_alt);

/* ── surveying every configuration, v0.8.6.4 ──────────────────────────
 * An iPhone enumerated on the HP on 25 October as class 06 -- Still
 * Image, which is PTP and is its DEFAULT configuration. Tethering lives
 * in a different one, and setup_config() has always asked for
 * descriptor index 0 because every device this kernel had met offered
 * exactly one.
 *
 * usb_survey_config() fetches configuration index `idx` and reports
 * every interface in it through `cb`, WITHOUT selecting anything: no
 * SET_CONFIGURATION, no change to *d. Surveying a phone that is working
 * must not stop it working.
 *
 * USB_IF_NONE as want_if means "walk, select nothing". usb_walk_config()
 * is the one walker both this and usb_parse_config() use. */
#define USB_IF_NONE (-2)
typedef void (*usb_if_cb)(void *ctx, uint8_t num, uint8_t alt,
                          uint8_t nep, uint8_t cls, uint8_t sub,
                          uint8_t proto);
int usb_walk_config(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                    int want_if, usb_if_cb cb, void *ctx);
int usb_survey_config(usb_dev_t *d, uint8_t idx, usb_if_cb cb, void *ctx);

/* ── ALTERNATE SETTINGS, v0.8.7 ───────────────────────────────────────
 *
 * The walker. usb_walk_config() is this with want_alt 0, which is what
 * it has always done and why every existing caller is unchanged.
 *
 * ── WHY ALT 0 WAS THE ONLY ONE, AND WHY THAT HAD TO CHANGE ──────────
 *
 * The walk has refused every alternate setting but 0 since it was
 * written, with a reason that is still right:
 *
 *   > Alternate settings other than 0 are a different configuration of
 *   > the same interface, and adopting one by accident is how a device
 *   > ends up with endpoints it was never told to use.
 *
 * The iPhone on the HP, 25 October, read with `usb cfg`:
 *
 *   configuration index 3:
 *     interface 2      class FF/FD/01  0 endpoints  <- Apple USB Ethernet
 *     interface 2 alt 1  class FF/FD/01  2 endpoints
 *     interface 2 alt 2  class FF/FD/01  2 endpoints
 *
 * Alt 0 of the interface tethering lives on has NO ENDPOINTS. It is an
 * interface that exists to be switched away from -- which is what
 * alternate settings are for, and what makes "take alt 0" exactly
 * wrong here rather than merely incomplete.
 *
 * So the refusal becomes a CHOICE: a caller names the setting it wants
 * and gets that one, and a caller that names nothing still gets alt 0.
 * Adopting one by accident is still not possible; adopting one on
 * purpose now is.
 *
 * d->altsetting records what was adopted, because SET_INTERFACE has to
 * be sent with it and the kernel's table must not claim a setting the
 * device was never told about. */
int usb_walk_config_alt(usb_dev_t *d, const uint8_t *buf, uint32_t len,
                        int want_if, int want_alt,
                        usb_if_cb cb, void *ctx);

/* ── choosing a configuration that is not the first, v0.8.7 ───────────
 *
 * setup_config() has always fetched descriptor index 0 and selected it,
 * correctly, because nothing this kernel had met offered more than one.
 * A phone offers four and boots into index 0, which is PTP.
 *
 * `idx` is a descriptor INDEX, as GET_DESCRIPTOR takes and as `usb cfg`
 * prints -- not a bConfigurationValue. `want_if` is the interface to
 * adopt from it, or -1 for the first; alternate setting 0, which is
 * what a device presents until it is told otherwise.
 *
 * On success the device is configured, the controller has been told
 * which endpoints exist, and d->cfgidx is `idx`. On failure *d is left
 * describing what the device is actually in, not what was asked for:
 * a table that claims a configuration the device refused is worse than
 * one that admits the request failed. */
int usb_select_config(usb_dev_t *d, uint8_t idx, int want_if);

/* ── switching an interface to another of its settings, v0.8.7 ────────
 *
 * SET_INTERFACE, and then the controller is told what the endpoints
 * are now -- which on xHCI means a Configure Endpoint that DROPS the
 * previous setting's endpoints as well as adding this one's. See
 * x_configure(): dropping is new in this release, because until now
 * nothing configured a slot twice.
 *
 * The configuration descriptor is re-read, because the endpoints of an
 * alternate setting are in it and the core does not keep the blob. It
 * is read by d->cfgidx, which is why that field exists.
 *
 * *d is not modified unless the whole thing succeeds. A device left
 * believing it is on one setting while the kernel's table says another
 * is the two-sides-disagreeing failure usb_walk_config() refuses to
 * create for a survey, and it is worse here: the endpoints would be
 * wrong rather than merely the bookkeeping. */
int usb_set_interface(usb_dev_t *d, uint8_t ifnum, uint8_t alt);

/* ── v0.8.9.9: usb_claim_interface ──────────────────────────────────
   Put interface `ifnum` at alternate `alt` of configuration index
   `cfgidx` into use WITHOUT disturbing any other interface already in
   use on the device.

   If the device is not in that configuration yet, it is selected --
   SET_CONFIGURATION, which resets every interface, so there is nothing
   else to keep. If it already IS in that configuration, the
   configuration is left alone: the new interface's endpoints are added
   beside the ones held, SET_INTERFACE is sent only for a non-zero
   alternate, and the controller is told to add the new endpoints and
   leave the running ones as they are (usb_ep_t.keep).

   This is how usbmux and Apple Ethernet share one iPhone. Returns
   USB_OK or a negative USB_E*. */
int usb_claim_interface(usb_dev_t *d, uint8_t cfgidx, uint8_t ifnum,
                        uint8_t alt);
