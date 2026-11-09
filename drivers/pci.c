/*
 * pci.c — PCI enumeration for HavenDOS
 *
 * The bus is walked ONCE at startup into a table, and every driver asks the
 * table.  Before v0.7.20 there were three separate walks -- one for IDE, one
 * for the RTL8139, one for both NICs -- each with its own copy of the
 * config-space arithmetic and its own hardcoded device list.  USB and audio
 * would have made five.
 *
 * All three also shared a bug that nothing had needed to notice yet: they
 * enumerated FUNCTION 0 only.  A PCI device may present up to eight
 * functions, and on a PIIX3 -- QEMU's default southbridge, and the real
 * chipset on a great many machines of that era -- the USB controller is
 * function 2 of device 1.  A scan that only looks at function 0 cannot see
 * it at all, so the USB work would have started by looking for hardware
 * that the enumerator was structurally incapable of finding.
 *
 * The walk follows PCI-to-PCI bridges to their secondary bus rather than
 * assuming everything lives on bus 0, because on real hardware it does not.
 */

#include "../include/types.h"
#include "../include/io.h"
#include "../include/pci.h"
#include "../include/klog.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* ── config space ─────────────────────────────────────────────────────── */

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)dev << 11)
         | ((uint32_t)fn  <<  8)
         | (reg & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, reg));
    return inl(PCI_CONFIG_DATA);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                     uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, reg));
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint16_t)(pci_cfg_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}

void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                     uint16_t val) {
    /* Config space is written 32 bits at a time, so a 16-bit write is a
       read, a splice and a write back.  Doing it any other way clobbers the
       neighbouring register -- which for the command register means wiping
       the status bits next to it. */
    uint32_t v = pci_cfg_read32(bus, dev, fn, reg);
    int shift = (reg & 2) * 8;
    v &= ~(0xFFFFu << shift);
    v |= ((uint32_t)val) << shift;
    pci_cfg_write32(bus, dev, fn, reg, v);
}

static uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint8_t)(pci_cfg_read32(bus, dev, fn, reg) >> ((reg & 3) * 8));
}

/* ── the table ────────────────────────────────────────────────────────── */

static pci_dev_t g_devs[PCI_MAX_DEVS];
static int       g_ndevs;

int pci_count(void) { return g_ndevs; }

const pci_dev_t *pci_get(int i) {
    return (i >= 0 && i < g_ndevs) ? &g_devs[i] : 0;
}

const pci_dev_t *pci_find_class(uint8_t cls, uint8_t sub, uint8_t progif,
                                int nth) {
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].class != cls || g_devs[i].subclass != sub) continue;
        if (progif != PCI_PROGIF_ANY && g_devs[i].progif != progif) continue;
        if (nth-- == 0) return &g_devs[i];
    }
    return 0;
}

const pci_dev_t *pci_find_id(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].vendor == vendor && g_devs[i].device == device)
            return &g_devs[i];
    return 0;
}


void pci_enable(const pci_dev_t *d, int bus_master) {
    if (!d) return;
    uint16_t cmd = pci_cfg_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0003;                          /* I/O space + memory space */
    if (bus_master) cmd |= 0x0004;
    pci_cfg_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}

/* ── the log ──────────────────────────────────────────────────────────
   This file used to keep its own 768-byte text buffer, built a nibble at a
   time, which pci_get_log() returned and NOTHING ever called -- the scan
   described itself to an audience of zero for several releases.  It writes
   to the kernel log now, where the boot screen and dmesg both read it. */

/* ── the walk ─────────────────────────────────────────────────────────── */

static void scan_bus(uint8_t bus, int depth);

static void record(uint8_t bus, uint8_t dev, uint8_t fn, int depth) {
    uint32_t id = pci_cfg_read32(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFFu || id == 0) return;

    uint8_t cls    = pci_cfg_read8(bus, dev, fn, 0x0B);
    uint8_t sub    = pci_cfg_read8(bus, dev, fn, 0x0A);
    uint8_t progif = pci_cfg_read8(bus, dev, fn, 0x09);

    if (g_ndevs < PCI_MAX_DEVS) {
        pci_dev_t *d = &g_devs[g_ndevs++];
        d->bus = bus; d->dev = dev; d->fn = fn;
        d->vendor   = (uint16_t)(id & 0xFFFF);
        d->device   = (uint16_t)(id >> 16);
        d->class    = cls;
        d->subclass = sub;
        d->progif   = progif;
        d->irq      = pci_cfg_read8(bus, dev, fn, 0x3C);
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_cfg_read32(bus, dev, fn, 0x10 + i * 4);

        klog(KLOG_INFO, "PCI %2x:%2x.%u  %4x:%4x  class %2x/%2x/%2x",
             (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
             (uint32_t)d->vendor, (uint32_t)d->device,
             (uint32_t)cls, (uint32_t)sub, (uint32_t)progif);
    }

    /* A bridge hides a whole further bus behind it.  Depth is bounded
       because a malformed or emulated bridge can otherwise point back at a
       bus already being scanned and the walk never returns. */
    if (cls == PCI_CLASS_BRIDGE && sub == PCI_SUB_PCI_BRIDGE && depth < 4) {
        uint8_t secondary = pci_cfg_read8(bus, dev, fn, 0x19);
        if (secondary && secondary != bus) scan_bus(secondary, depth + 1);
    }
}

static void scan_bus(uint8_t bus, int depth) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(bus, dev, 0, 0x00);
        if (id == 0xFFFFFFFFu || id == 0) continue;

        record(bus, dev, 0, depth);

        /* Bit 7 of the header type says the device has more than one
           function.  Without this the UHCI controller at 00:01.2 is
           invisible, and so is the second IDE channel on some chipsets. */
        uint8_t hdr = pci_cfg_read8(bus, dev, 0, 0x0E);
        if (hdr & 0x80)
            for (uint8_t fn = 1; fn < 8; fn++)
                record(bus, dev, fn, depth);
    }
}

/* ── what the rest of the kernel asks for ─────────────────────────────── */

static int      ide_found     = 0;
static uint16_t ide_pri_base  = 0x1F0;      /* legacy defaults */
static uint16_t ide_pri_ctrl  = 0x3F6;
static uint16_t ide_sec_base  = 0x170;
static uint16_t ide_sec_ctrl  = 0x376;

static uint16_t bar_to_port(uint32_t bar, uint16_t legacy) {
    if (bar == 0x00000000) return legacy;   /* not programmed */
    if (bar == 0x00000001) return legacy;   /* native mode flag */
    if (!(bar & 1))        return legacy;   /* memory mapped, not I/O */
    uint16_t port = (uint16_t)(bar & 0xFFFC);
    if (port < 0x100)      return legacy;   /* below anything valid */
    return port;
}

static uint16_t bar_to_ctrl(uint32_t bar, uint16_t legacy) {
    uint16_t base = bar_to_port(bar, 0);
    return base ? (uint16_t)(base + 2) : legacy;
}

static void find_ide(void) {
    for (int i = 0; i < g_ndevs; i++) {
        const pci_dev_t *d = &g_devs[i];
        if (d->class != PCI_CLASS_STORAGE) continue;
        /* IDE, AHCI, SCSI or a controller that declines to say. */
        if (d->subclass != 0x01 && d->subclass != 0x06 &&
            d->subclass != 0x00 && d->subclass != 0x80) continue;
        ide_pri_base = bar_to_port(d->bar[0], 0x1F0);
        ide_pri_ctrl = bar_to_ctrl(d->bar[1], 0x3F6);
        ide_sec_base = bar_to_port(d->bar[2], 0x170);
        ide_sec_ctrl = bar_to_ctrl(d->bar[3], 0x376);
        ide_found = 1;
        klog(KLOG_OK, "IDE: primary 0x%4x, secondary 0x%4x",
             (uint32_t)ide_pri_base, (uint32_t)ide_sec_base);
        return;
    }
    klog(KLOG_WARN, "IDE: no storage controller on the PCI bus");
}

void pci_init(void) {
    g_ndevs = 0;
    /* The legacy ports go back too, not just the found flag.  Leaving the
       previous scan's values behind means "no controller found" no longer
       implies "using the legacy ports", and a second call to pci_init()
       would hand ATA addresses belonging to hardware that is not there. */
    ide_found    = 0;
    ide_pri_base = 0x1F0; ide_pri_ctrl = 0x3F6;
    ide_sec_base = 0x170; ide_sec_ctrl = 0x376;
    scan_bus(0, 0);
    find_ide();
}

int         pci_ide_found(void)    { return ide_found;    }
uint16_t    pci_ide_pri_base(void) { return ide_pri_base; }
uint16_t    pci_ide_pri_ctrl(void) { return ide_pri_ctrl; }
uint16_t    pci_ide_sec_base(void) { return ide_sec_base; }
uint16_t    pci_ide_sec_ctrl(void) { return ide_sec_ctrl; }

/* ── network, now answered from the table ─────────────────────────────── */

void pci_scan_net(uint16_t *io_base_out)
{
    *io_base_out = 0;
    const pci_dev_t *d = pci_find_id(0x10EC, 0x8139);
    if (!d || d->class != PCI_CLASS_NETWORK) return;
    uint16_t port = bar_to_port(d->bar[0], 0);
    if (!port) return;
    pci_enable(d, 1);
    *io_base_out = port;
}

void pci_scan_net2(uint16_t *rtl_io, uint32_t *e1000_mmio)
{
    static const uint16_t e1000_ids[] = {
        0x100E,   /* 82540EM — QEMU's default */
        0x100F,   /* 82545EM */
        0x1004,   /* 82543GC */
        0x1008,   /* 82544GC */
        0x1010,   /* 82546EB */
        0x1016,   /* 82540EP */
        0x1026,   /* 82545GM */
        0x107C,   /* 82541PI */
    };
    *rtl_io = 0;
    *e1000_mmio = 0;

    for (int i = 0; i < g_ndevs; i++) {
        const pci_dev_t *d = &g_devs[i];
        if (d->class != PCI_CLASS_NETWORK) continue;
        pci_enable(d, 1);

        if (d->vendor == 0x10EC && d->device == 0x8139) {
            uint16_t port = bar_to_port(d->bar[0], 0);
            if (port) { *rtl_io = port; }
            continue;
        }
        if (d->vendor == 0x8086) {
            for (unsigned k = 0; k < sizeof(e1000_ids)/sizeof(e1000_ids[0]); k++) {
                if (d->device != e1000_ids[k]) continue;
                /* BAR1 carries the MMIO window on some of these parts and
                   BAR0 on others; take whichever is a memory BAR. */
                uint32_t mmio = pci_bar_mem(d, 1);
                if (!mmio) mmio = pci_bar_mem(d, 0);
                if (mmio) { *e1000_mmio = mmio; }
                break;
            }
        }
    }
}
