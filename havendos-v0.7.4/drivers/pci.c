/*
 * pci.c — PCI bus scanner for BOOT OS
 *
 * Scans all PCI devices using CF8/CFC config space mechanism.
 * Used to locate the IDE controller so ATA gets the real port
 * addresses instead of assuming legacy 0x1F0/0x170.
 *
 * PIIX/PIIX3 IDE (Intel 8086:7010 or 8086:7111) is what
 * QEMU/UTM SE exposes for IDE drives.
 *
 * pci_find_ide() returns 1 if found and fills in the two
 * channel base+ctrl port pairs. Returns 0 if not found
 * (caller falls back to legacy ports).
 */

#include "../include/types.h"
#include "../include/io.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* Build a PCI config space address */
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)dev << 11)
         | ((uint32_t)fn  <<  8)
         | (reg & 0xFC);
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, reg));
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, reg));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((reg & 2) * 8));
}

static uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, reg));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((reg & 3) * 8));
}

/* Result storage */
static int     ide_found      = 0;
static uint16_t ide_pri_base  = 0x1F0;  /* defaults = legacy */
static uint16_t ide_pri_ctrl  = 0x3F6;
static uint16_t ide_sec_base  = 0x170;
static uint16_t ide_sec_ctrl  = 0x376;
static char     pci_log[512];

static void plog(const char *s) {
    int l = 0; while(pci_log[l]) l++;
    int sl = 0; while(s[sl]) sl++;
    if (l + sl < 510) {
        int i = 0;
        while (s[i]) pci_log[l++] = s[i++];
        pci_log[l] = 0;
    }
}

static void plog_hex16(uint16_t v) {
    char buf[8];
    const char *hex = "0123456789ABCDEF";
    buf[0]='0'; buf[1]='x';
    buf[2]=hex[(v>>12)&0xF]; buf[3]=hex[(v>>8)&0xF];
    buf[4]=hex[(v>>4)&0xF];  buf[5]=hex[v&0xF];
    buf[6]=0;
    plog(buf);
}

/*
 * Interpret a BAR value for I/O space.
 * Returns the I/O port base, or 0 if not I/O or not set.
 * If the BAR reads as 0 or 1 (native IDE mode indicator),
 * we return the legacy default passed in.
 */
static uint16_t bar_to_port(uint32_t bar, uint16_t legacy) {
    if (bar == 0x00000000) return legacy;  /* not programmed */
    if (bar == 0x00000001) return legacy;  /* native mode flag */
    if (!(bar & 0x1))      return legacy;  /* memory-mapped, not I/O */
    uint16_t port = (uint16_t)(bar & 0xFFFC);
    if (port < 0x100)      return legacy;  /* sanity — below valid range */
    return port;
}

/* ctrl BAR is special — add 2 for the actual control register offset */
static uint16_t bar_to_ctrl(uint32_t bar, uint16_t legacy) {
    uint16_t base = bar_to_port(bar, 0);
    if (base == 0) return legacy;
    return base + 2;
}

void pci_init(void) {
    ide_found = 0;
    pci_log[0] = 0;

    plog("PCI devices:\n");

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0x00);
        if (id == 0xFFFFFFFF || id == 0x00000000) continue;

        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        uint8_t  class  = pci_read8(0, dev, 0, 0x0B);
        uint8_t  subcls = pci_read8(0, dev, 0, 0x0A);
        uint8_t  progif = pci_read8(0, dev, 0, 0x09);

        /* Log every device */
        char db[3]; db[0]='0'+(dev/10); db[1]='0'+(dev%10); db[2]=0;
        plog(" d"); plog(db); plog(" ");
        plog_hex16(vendor); plog(":"); plog_hex16(device);
        plog(" cl="); 
        char cb[3]; cb[0]="0123456789ABCDEF"[class>>4]; cb[1]="0123456789ABCDEF"[class&0xF]; cb[2]=0;
        plog(cb); plog("/");
        cb[0]="0123456789ABCDEF"[subcls>>4]; cb[1]="0123456789ABCDEF"[subcls&0xF];
        plog(cb); plog("/");
        cb[0]="0123456789ABCDEF"[progif>>4]; cb[1]="0123456789ABCDEF"[progif&0xF];
        plog(cb); plog("\n");

        /* Mass storage = class 0x01, any subclass */
        if (class == 0x01) {
            uint32_t bar0 = pci_read32(0, dev, 0, 0x10);
            uint32_t bar1 = pci_read32(0, dev, 0, 0x14);
            uint32_t bar2 = pci_read32(0, dev, 0, 0x18);
            uint32_t bar3 = pci_read32(0, dev, 0, 0x1C);
            plog("  BAR0="); plog_hex16((uint16_t)bar0);
            plog(" BAR1="); plog_hex16((uint16_t)bar1);
            plog(" BAR2="); plog_hex16((uint16_t)bar2);
            plog(" BAR3="); plog_hex16((uint16_t)bar3);
            plog("\n");

            /* Accept IDE (0x01) OR SATA AHCI (0x06) OR SCSI (0x00) OR unknown (0x80) */
            if (subcls == 0x01 || subcls == 0x06 ||
                subcls == 0x00 || subcls == 0x80) {
                ide_pri_base = bar_to_port(bar0, 0x1F0);
                ide_pri_ctrl = bar_to_ctrl(bar1, 0x3F6);
                ide_sec_base = bar_to_port(bar2, 0x170);
                ide_sec_ctrl = bar_to_ctrl(bar3, 0x376);
                ide_found = 1;
                plog("  ^-- using this\n");
                break;
            }
        }
    }

    if (!ide_found) plog("No storage ctrl found\n");
}

int      pci_ide_found(void)      { return ide_found;      }
uint16_t pci_ide_pri_base(void)   { return ide_pri_base;   }
uint16_t pci_ide_pri_ctrl(void)   { return ide_pri_ctrl;   }
uint16_t pci_ide_sec_base(void)   { return ide_sec_base;   }
uint16_t pci_ide_sec_ctrl(void)   { return ide_sec_ctrl;   }
const char *pci_get_log(void)     { return pci_log;         }

/* ── RTL8139 NIC detection ─────────────────────────────────────
 * Called by net_init(). Scans bus 0 for vendor 0x10EC device 0x8139
 * (class 0x02 = network). Returns BAR0 I/O base in *io_base_out.
 */
void pci_scan_net(uint16_t *io_base_out)
{
    *io_base_out = 0;
    plog("NET scan:\n");

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0x00);
        if (id == 0xFFFFFFFF || id == 0x00000000) continue;

        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        uint8_t  class  = pci_read8(0, dev, 0, 0x0B);

        /* RTL8139: vendor=10EC, device=8139, class=02 (network) */
        if (vendor == 0x10EC && device == 0x8139 && class == 0x02) {
            uint32_t bar0 = pci_read32(0, dev, 0, 0x10);
            uint16_t port = bar_to_port(bar0, 0);
            if (port) {
                *io_base_out = port;
                plog(" RTL8139 @ ");
                plog_hex16(port);
                plog("\n");

                /* Bus-master enable (bit 2 of command reg 0x04) */
                uint32_t cmd = pci_read32(0, dev, 0, 0x04);
                /* write back with bus-master bit set */
                outl(PCI_CONFIG_ADDR, pci_addr(0, dev, 0, 0x04));
                outl(PCI_CONFIG_DATA, cmd | 0x04);
                return;
            }
        }
    }
    plog(" No NIC found\n");
}

/*
 * pci_scan_net extended — detects RTL8139 (port I/O) AND e1000 (MMIO).
 * Returns io_base for RTL8139 or 0, and mmio_base for e1000 or 0.
 */
void pci_scan_net2(uint16_t *rtl_io, uint32_t *e1000_mmio)
{
    *rtl_io     = 0;
    *e1000_mmio = 0;
    plog("NET2 scan:\n");

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read32(0, dev, 0, 0x00);
        if (id == 0xFFFFFFFF || id == 0x00000000) continue;

        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        uint8_t  class  = pci_read8(0, dev, 0, 0x0B);
        if (class != 0x02) continue;   /* not a network controller */

        /* Enable bus master + MMIO/IO in command reg */
        uint32_t cmd = pci_read32(0, dev, 0, 0x04);
        outl(PCI_CONFIG_ADDR, pci_addr(0, dev, 0, 0x04));
        outl(PCI_CONFIG_DATA, cmd | 0x07);

        uint32_t bar0 = pci_read32(0, dev, 0, 0x10);
        uint32_t bar1 = pci_read32(0, dev, 0, 0x14);

        /* RTL8139: vendor=10EC device=8139 — uses I/O BAR */
        if (vendor == 0x10EC && device == 0x8139) {
            uint16_t port = bar_to_port(bar0, 0);
            if (port) { *rtl_io = port; plog(" RTL8139 found\n"); }
        }

        /* e1000 family: vendor=8086, various device IDs — uses MMIO BAR */
        if (vendor == 0x8086 && (
            device == 0x100E ||   /* 82540EM  — QEMU default  */
            device == 0x100F ||   /* 82545EM               */
            device == 0x1004 ||   /* 82543GC               */
            device == 0x1008 ||   /* 82544GC               */
            device == 0x1010 ||   /* 82546EB               */
            device == 0x1016 ||   /* 82540EP               */
            device == 0x1026 ||   /* 82545GM               */
            device == 0x107C      /* 82541PI               */
        )) {
            /* BAR0 = MMIO (bit0=0 means memory mapped) */
            uint32_t mmio = bar1 & ~0xF;   /* BAR1 is often the MMIO one */
            if (!mmio) mmio = bar0 & ~0xF;
            if (mmio) {
                *e1000_mmio = mmio;
                plog(" e1000 found\n");
            }
        }
    }
}
