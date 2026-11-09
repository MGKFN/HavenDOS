#ifndef UHCI_H
#define UHCI_H
#include "types.h"

/* ── UHCI host controller ──────────────────────────────────────────────
 *
 * v0.7.20 made the USB controller findable.  This makes it work.
 *
 * UHCI is the Intel USB 1.1 host controller: eleven I/O registers behind
 * BAR4, and a schedule in main memory that the controller walks by DMA once
 * every millisecond.  The driver's job is to build that schedule.
 *
 * The shape of it:
 *
 *   frame_list[0..1023]  -- one entry per millisecond frame, all pointing
 *                           at the same place because this kernel has no
 *                           periodic (interrupt or isochronous) transfers
 *        |
 *        v
 *   qh_control  ---->  qh_bulk  ---->  (terminate)
 *        |                  |
 *        v                  v
 *   a chain of TDs     a chain of TDs
 *
 * A transfer is a chain of Transfer Descriptors hung off a Queue Head.  The
 * controller executes them, writes the result back into each TD, and
 * advances the queue head's element pointer as it goes.  When the element
 * pointer reaches the terminate bit the transfer is done.
 *
 * Everything here is POLLED.  The house rule is that no disk I/O depends on
 * an interrupt, because IRQs are unreliable on UTM SE's TCG, and USB is disk
 * I/O as far as the music player is concerned.  USBINTR is therefore left at
 * zero and the driver watches the queue head in memory instead.  Audio will
 * be the first thing in this kernel that genuinely needs an IRQ, and that is
 * a decision to make out loud when it arrives, not to smuggle in here.
 */

/* ── transfer results ─────────────────────────────────────────────────── */
#define UHCI_OK         0
#define UHCI_ETIMEOUT  (-1)   /* the controller never finished the chain   */
#define UHCI_ESTALL    (-2)   /* the device returned STALL: request refused */
#define UHCI_EXFER     (-3)   /* CRC, babble, bitstuff or buffer error     */
#define UHCI_ENODEV    (-4)   /* no controller, or nothing on the port     */
#define UHCI_EPARAM    (-5)
#define UHCI_ENOMEM    (-6)   /* transfer needs more TDs than the pool has */

/* ── USB request types, for uhci_control() ────────────────────────────── */
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

#define USB_DESC_DEVICE     1
#define USB_DESC_CONFIG     2
#define USB_DESC_STRING     3
#define USB_DESC_INTERFACE  4
#define USB_DESC_ENDPOINT   5

/* ── a device, as far as the host controller is concerned ─────────────── */
typedef struct {
    uint8_t  valid;        /* 0 when this slot holds nothing              */
    uint8_t  hc;           /* which controller it is behind               */
    uint8_t  port;         /* root port on that controller                */
    uint8_t  addr;         /* USB address assigned by uhci_probe_port()   */
    uint8_t  lowspeed;     /* 1.5 Mbit device: every TD needs the LS bit  */
    uint8_t  maxpacket0;   /* endpoint 0 packet size, from the descriptor */
    uint16_t vendor, product, release;
    uint8_t  dev_class, dev_subclass, dev_protocol;
    uint8_t  nconfigs;
} uhci_dev_t;

/* ── the controller ───────────────────────────────────────────────────── */

/* Finds EVERY UHCI controller through the PCI table, resets each one and
   starts its schedule.  Returns 1 if at least one is running, 0 if there is
   none.  Safe to call when there is no USB hardware at all.

   There is rarely just one: an ICH has two to five, and a USB 2.0 controller
   is normally an EHCI with three UHCI companions sharing its sockets two
   ports at a time.  Driving only the first works right up until something is
   plugged into a different socket. */
int      uhci_init(void);

int      uhci_present(void);
int      uhci_hc_count(void);

uint16_t uhci_io_base(int hc);
int      uhci_port_count(int hc);
uint8_t  uhci_hc_irq(int hc);
uint16_t uhci_portsc(int hc, int port);  /* raw PORTSC, 0 if no such port */
int      uhci_port_connected(int hc, int port);

/* Status registers, for the `usb` command and for diagnosing a dead bus. */
uint16_t uhci_usbcmd(int hc);
uint16_t uhci_usbsts(int hc);
uint16_t uhci_frnum(int hc);


/* ── transfers ────────────────────────────────────────────────────────── */

/* A control transfer on endpoint 0.  `len` bytes move in the direction
   given by the top bit of rtype.  Returns bytes transferred, or a negative
   UHCI_E* code. */
int uhci_control(const uhci_dev_t *d, uint8_t rtype, uint8_t req,
                 uint16_t value, uint16_t index, void *data, uint16_t len);

/* A bulk transfer.  `ep` is the endpoint number without the direction bit.
   Returns bytes transferred, or a negative UHCI_E* code.  The data toggle
   for each endpoint is kept by the driver across calls, as the USB spec
   requires; uhci_bulk_reset_toggle() puts it back to DATA0 after a
   CLEAR_FEATURE(ENDPOINT_HALT). */
int  uhci_bulk(const uhci_dev_t *d, uint8_t ep, int in, void *data, int len,
                int mps);
void uhci_bulk_reset_toggle(const uhci_dev_t *d, uint8_t ep, int in);

/* ── bringing a port up ───────────────────────────────────────────────── */

/* Resets the port and leaves it enabled.  Returns 1 on success. */
int uhci_port_reset(int hc, int port);

/* The whole first-contact sequence for one root port: reset, read the first
   eight bytes of the device descriptor at address 0 to learn the endpoint-0
   packet size, SET_ADDRESS, then read the full descriptor.  This is the
   proof that the controller works -- it is the smallest piece of work that
   needs the schedule, both transfer directions, a transfer with no data
   stage, and a second transfer to a re-addressed device.
   Returns UHCI_OK and fills *out, or a negative code. */
int uhci_probe_port(int hc, int port, uhci_dev_t *out);

/* Probes every root port of every controller.  Called at boot.  Returns the
   device count. */
int uhci_scan(void);

/* Whether a USB controller this kernel CANNOT drive is present -- an EHCI or
   xHCI part, which is what a machine newer than about 2009 has instead of
   UHCI.  The `usb` command reports it so that running HavenDOS on such a
   machine answers the question directly rather than looking like a broken
   driver. */
int uhci_other_controller(uint8_t *progif_out);

/* ── sharing sockets with an EHCI ──────────────────────────────────────
 *
 * A USB 2.0 controller is usually an EHCI plus UHCI companions sharing the
 * same physical sockets, and which one gets a device is decided by SPEED: a
 * USB 2.0 stick attaches to the EHCI at 480 Mbit and the companion sees an
 * empty port.  uhci_init() therefore asks the EHCI for its ports back, which
 * the EHCI specification provides for (CONFIGFLAG = 0 routes every port to
 * the companions -- the state the hardware resets into, and the state the
 * BIOS runs in).  The device re-attaches to a companion at full speed.
 *
 * The trade is 12 Mbit instead of 480, about 1 MB/s.  For a kernel with no
 * EHCI driver that is the difference between USB working and not at all.
 *
 * Returns 1 if an EHCI was found and its ports were taken. */
int uhci_ehci_released(void);

/* The EHCI's MMIO address, 0 if there is none.  For the `usb` command. */
uint32_t uhci_ehci_base(void);

int               uhci_device_count(void);
const uhci_dev_t *uhci_device(int i);

#endif
