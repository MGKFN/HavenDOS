#ifndef PCI_H
#define PCI_H
#include "types.h"

/* ── one enumeration, kept ─────────────────────────────────────────────
 *
 * pci.c used to carry a separate bus walk per device family: one for IDE,
 * one for the RTL8139, one for both NICs.  Adding USB and audio would have
 * made five, each with its own copy of the config-space arithmetic.
 *
 * They also all shared a bug.  Every one of them looked at FUNCTION 0 only,
 * and a PCI device may have up to eight functions.  The UHCI controller on
 * a PIIX3 -- which is what QEMU gives you, and what most machines of that
 * era have -- sits at 00:01.2.  Function 2.  None of those scans could ever
 * have found it.
 *
 * So the bus is now walked ONCE, into a table, following bridges, and every
 * driver asks the table.  The old accessors still work; they read from it.
 */

#define PCI_MAX_DEVS 48

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class, subclass, progif;
    uint8_t  irq;               /* interrupt line, 0xFF when unassigned */
    uint32_t bar[6];            /* raw BAR values, as read */
} pci_dev_t;

/* Class codes this kernel looks for. */
#define PCI_CLASS_STORAGE   0x01
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUB_AUDIO       0x01     /* multimedia/audio, e.g. AC'97 */
#define PCI_CLASS_BRIDGE    0x06
#define PCI_SUB_PCI_BRIDGE  0x04
#define PCI_CLASS_SERIAL    0x0C
#define PCI_SUB_USB         0x03
#define PCI_PROGIF_UHCI     0x00
#define PCI_PROGIF_OHCI     0x10
#define PCI_PROGIF_EHCI     0x20
#define PCI_PROGIF_XHCI     0x30
#define PCI_PROGIF_ANY      0xFF

void  pci_init(void);

/* The table. */
int              pci_count(void);
const pci_dev_t *pci_get(int i);

/* nth == 0 is the first match.  progif may be PCI_PROGIF_ANY. */
const pci_dev_t *pci_find_class(uint8_t cls, uint8_t sub, uint8_t progif,
                                int nth);
const pci_dev_t *pci_find_id(uint16_t vendor, uint16_t device);

/* A BAR as something usable, or 0 if it is not of that kind. */
uint16_t pci_bar_io(const pci_dev_t *d, int n);
uint32_t pci_bar_mem(const pci_dev_t *d, int n);

/* The same BAR, both halves. A memory BAR of type 2 is 64 bits wide and
   the NEXT bar[] entry is its high dword, not a BAR of its own. On a
   UEFI machine with Above-4G decoding the high dword is where the MMIO
   actually is, and pci_bar_mem() returns 0 rather than the low half --
   see the note in drivers/pci.c. Returns the low dword, masked. */
uint32_t pci_bar_mem64(const pci_dev_t *d, int n, uint32_t *hi_out);

/* 1 if bar[n] is a 64-bit memory BAR, so bar[n+1] is its high half and
   must not be listed or used as a BAR in its own right. */
int      pci_bar_is_64(const pci_dev_t *d, int n);

/* Turn on I/O space, memory space and, optionally, bus mastering.  A device
   that is not bus master cannot do DMA, which is how both USB and audio
   move their data. */
void pci_enable(const pci_dev_t *d, int bus_master);

/* Raw config space, for the registers a driver needs that are not BARs. */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                         uint32_t val);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                         uint16_t val);

/* ── the original interface, unchanged for its callers ── */
int         pci_ide_found(void);
uint16_t    pci_ide_pri_base(void);
uint16_t    pci_ide_pri_ctrl(void);
uint16_t    pci_ide_sec_base(void);
uint16_t    pci_ide_sec_ctrl(void);
void        pci_scan_net(uint16_t *io_base_out);
void        pci_scan_net2(uint16_t *rtl_io, uint32_t *e1000_mmio);
#endif
