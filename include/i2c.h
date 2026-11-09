#pragma once
#include "types.h"
#include "pci.h"

/* ════════════════════════════════════════════════════════════════════
   include/i2c.h -- the Synopsys DesignWare I2C controller.  v0.8.8.3.

   WHY THIS EXISTS, AND WHAT IT CANNOT DO YET

   The HP's trackpad does not answer the PS/2 aux port and no keyboard
   appeared on USB either. The October `lspci` on that machine found

       00:15.0  8086:51E8  class 0C/80  Serial-IO I2C controller #0
       00:15.1  8086:51E9  class 0C/80  Serial-IO I2C controller #1

   which on a laptop is what the trackpad and the internal keyboard
   are attached to. Both are Intel LPSS wrappers around a **Synopsys
   DesignWare** I2C core, which is the same core on every Intel
   chipset since Baytrail and on most ARM SoCs -- so this driver is
   not HP-specific.

   ── THE PART THAT IS GENUINELY BLOCKED ──────────────────────────────

   A device on an I2C bus has no enumeration. There is no descriptor
   to ask for and no vendor ID to read: you must already know the
   seven-bit address. On a PC that address lives in ACPI, in the
   device's _CRS, and reaching it means an AML interpreter --
   RSDP, XSDT, DSDT, and a bytecode evaluator. kernel/machine.c's
   header already records that as "a release of its own" for \\_S5,
   and that is the easy half of ACPI.

   **SO THIS DRIVER SCANS.** i2c_scan() walks all 127 addresses and
   reports which ones acknowledge. That is a thing an I2C bus can be
   asked directly, it costs one boot, and it turns "the trackpad is
   probably at one of 0x05, 0x15, 0x2a, 0x2c or 0x4b" into a fact.

   A scan is not free of risk and the risk is named: a write to an
   unknown device can do something. So i2c_scan() issues a
   ZERO-LENGTH READ -- address the device, look at the ACK, stop. No
   register is named and no byte is written. That is the standard
   probe and it is what every `i2cdetect -r` does.

   ── WHAT THE BAR LOOKS LIKE ON THIS MACHINE ─────────────────────────

   Above 4 GB, like the xHCI. The firmware has Above-4G decoding on
   and puts MMIO at 0x60_xxxxxxxx, which is why v0.8.5's USB refused
   to start: pci_bar_mem() handed back the low half. pci_bar_mem64()
   and highfb_map_io() are the pair that handle it, and the mapping
   must be CACHE-DISABLED -- a device register read served from cache
   looks exactly like a controller that hung during reset.

   ── WHAT IS NOT HERE ────────────────────────────────────────────────

   - No interrupts. The touchpad signals a report through a GPIO, and
     this kernel has no GPIO driver. The I2C-HID layer on top of this
     will POLL, through kernel_poll(), which is a thing that only
     became possible in v0.8.8.
   - No 10-bit addressing, no clock stretching beyond the hardware's
     own, no multi-master. None of them are needed for HID.
   - No DMA. The FIFOs are 32 bytes deep on this core and a HID report
     is smaller than that.
   ════════════════════════════════════════════════════════════════════ */

#define I2C_MAX_BUS     4

/* Refusals, named. Every one of these is a different fix, and
   "i2c failed" is the sentence this project keeps deleting. */
#define I2C_OK          0
#define I2C_ENOBUS      1   /* no such bus index                        */
#define I2C_ENOTREADY   2   /* the controller never left reset          */
#define I2C_ENAK        3   /* nobody acknowledged that address         */
#define I2C_ETIMEOUT    4   /* the transfer did not finish              */
#define I2C_EBUSY       5   /* the bus was still busy when we started   */
#define I2C_EARB        6   /* arbitration lost -- another master       */
#define I2C_ETOOLONG    7   /* longer than the FIFO and no DMA          */
#define I2C_EREASONS    8

const char *i2c_why_name(int why);

/* ── discovery ──────────────────────────────────────────────────────
   Walks PCI for Serial-IO I2C controllers, maps each one's BAR (64-bit
   aware) and resets it. Returns how many came up. Safe to call twice;
   the second call re-reports rather than re-mapping. */
int  i2c_init(void);
int  i2c_bus_count(void);

/* What the bus is, for a screen a person reads. */
const pci_dev_t *i2c_bus_pci(int bus);
uint32_t         i2c_bus_mmio(int bus);     /* the mapped address       */
uint32_t         i2c_bus_khz(int bus);      /* the clock it came up at  */
int              i2c_bus_ready(int bus);

/* ── transfers ──────────────────────────────────────────────────────
   Seven-bit addresses. Both return I2C_OK or a named refusal. */
int i2c_write(int bus, uint8_t addr, const uint8_t *buf, uint32_t len);
int i2c_read (int bus, uint8_t addr, uint8_t *buf, uint32_t len);

/* Write then read with a REPEATED START and no stop between -- which
   is how every register on an I2C device is read, and doing it as two
   transfers lets another master interleave. */
int i2c_write_read(int bus, uint8_t addr,
                   const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen);

/* Does anything answer at this address? A zero-length read: address
   the device, look at the acknowledge, stop. Nothing is written. */
int i2c_probe(int bus, uint8_t addr);

/* Fill `out` with every address that answered, up to `max`. Returns
   how many. */
int i2c_scan(int bus, uint8_t *out, int max);
