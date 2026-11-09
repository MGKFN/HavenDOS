#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/usbnet.h -- Apple USB Ethernet, which is iPhone tethering.

   usb/ethernet.c is the driver. It registers through
   netdev_register_usb(), so nothing above the NIC layer knows a frame
   is leaving over USB -- which is the whole point of net/netdev.c and
   the reason that file was written before this one.

   ── WHAT IS CONFIRMED AND WHAT IS NOT ───────────────────────────────

   CONFIRMED, by `usb cfg` on the HP on 25 October 2026:

     addr 2  05AC:12A8  4 configurations, in use 1
       configuration index 3:
         interface 0        class 06/01/01  3 endpoints
         interface 1        class FF/FE/02  2 endpoints   usbmux
         interface 2        class FF/FD/01  0 endpoints   <- this driver
         interface 2 alt 1  class FF/FD/01  2 endpoints   <- this driver
         interface 2 alt 2  class FF/FD/01  2 endpoints   <- this driver

   So the descriptors are not a guess: the interface exists, it is
   vendor class FF/FD/01, it is in configuration index 3, and its
   endpoints are on the alternate settings rather than on alt 0.

   NOT CONFIRMED, and this is the honest part. The FRAMING below --
   the two bytes the device prepends to a received frame, the fixed
   1514-byte padded transmit, and the two vendor control requests --
   is what the published Linux `ipheth` driver does. It is not in any
   Apple document, this kernel has never exchanged a byte with the
   interface, and nothing here has been on hardware.

   So `usbnet_dump()` exists, and `usb eth` calls it. It prints the
   first bytes of the next transfer that arrives and the carrier byte
   verbatim, WITHOUT interpreting them -- because the first thing to
   establish on the HP is whether the framing in this file is the
   framing the phone uses, and a driver that silently strips two bytes
   cannot be asked that question.

   ── THE REMAINING UNKNOWN NOBODY CAN ANSWER FROM A DESK ─────────────

   Interface 1's FF/FE/02 is the Apple Mobile Device interface --
   usbmux, the pairing channel. Whether the Ethernet interface is
   merely OFFERED without a trust handshake or also USABLE is not
   written down anywhere. Selecting the configuration, switching the
   interface and seeing whether the bulk endpoints carry anything is
   the experiment, and the HP is the only thing that can run it.
   ════════════════════════════════════════════════════════════════════ */

/* The interface this driver claims. Vendor-specific, so the triple is
   the only thing that identifies it -- there is no class driver to
   fall back on and no device ID list to keep up to date. */
#define IPHETH_IF_CLASS     0xFF
#define IPHETH_IF_SUBCLASS  0xFD
#define IPHETH_IF_PROTOCOL  0x01

/* Alternate setting 1 is the one with endpoints. Alt 2 is offered as
   well and is NOT used: on a recent iOS it is the NCM framing, which is
   a different driver, and choosing between them by trying each is not
   something to do to somebody's phone before the simpler one is known
   to work. */
#define IPHETH_ALT          1

/* Find and bring up every Apple USB Ethernet interface on the bus, and
   register each with the NIC layer. Returns the number registered.
   Safe to call when there is no phone, no USB and no controller.

   Called after usb_init() and after net_init(), because a USB NIC
   appears when the bus is walked and the NIC table has to exist first.
   `usb rescan` calls it again. */
int usbnet_probe(void);

/* Let go of every registered Apple Ethernet NIC, before the USB device
   table is rebuilt. Without this a `usb rescan` leaves the NIC layer
   holding a device index into a table that has been re-enumerated --
   the shape of the v0.8.6.1 slot leak, in the layer above it. */
void usbnet_release(void);

/* What `usb eth` prints: the state of each registered NIC and the RAW
   first bytes of the next transfer that arrives on it. See the header
   above for why this is in the driver rather than in a scratch patch.
   Returns the number of NICs reported. */
int usbnet_dump(void);

/* How many were found, for the boot log and for `usb eth`. */
int usbnet_count(void);
