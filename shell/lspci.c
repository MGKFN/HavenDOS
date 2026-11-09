/*
 * lspci.c — what is actually on the PCI bus
 *
 * pci.c has kept a log of its scan since the beginning and nothing has ever
 * printed it: pci_get_log() had no callers at all.  So the one question that
 * matters when a driver finds no hardware -- "is the device even there?" --
 * could only be answered by adding print statements and rebuilding.
 *
 * That mattered the moment USB work started.  The old scan looked at
 * function 0 only and could not see the UHCI controller at 00:01.2, and
 * without a way to list the bus that would have looked like a broken USB
 * driver rather than a blind enumerator.
 *
 *   lspci        one line per device, with what this kernel can drive
 *   lspci -v     BARs and interrupt line as well
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/pci.h"

/* The shared pager in shell/ui.c -- sized from vga_rows(), so this
   listing stops at the bottom of whatever grid is in use instead of
   scrolling off the top of it. */
#include "../include/ui.h"

extern void utoa(uint32_t v, char *buf, int base);

static void put_hex(uint32_t v, int digits)
{
    const char *hex = "0123456789ABCDEF";
    char buf[9];
    int i = 0;
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        buf[i++] = hex[(v >> shift) & 0xF];
    buf[i] = 0;
    vga_puts(buf);
}

/* The classes this kernel has a driver for, so the listing says which line
   is the thing you are looking for rather than leaving you to decode a
   class code by hand. */
static const char *describe(const pci_dev_t *d)
{
    switch (d->class) {
    case PCI_CLASS_STORAGE:
        if (d->subclass == 0x01) return "IDE controller";
        if (d->subclass == 0x06) return "SATA controller";
        if (d->subclass == 0x08) return "NVMe controller";
        return "storage controller";
    case PCI_CLASS_NETWORK:
        if (d->vendor == 0x10EC && d->device == 0x8139) return "RTL8139 network";
        if (d->vendor == 0x8086) return "e1000 network";
        return "network controller";
    case PCI_CLASS_MULTIMEDIA:
        if (d->subclass == PCI_SUB_AUDIO) {
            if (d->vendor == 0x8086 && d->device == 0x2415) return "AC'97 audio";
            return "audio device";
        }
        return "multimedia device";
    case PCI_CLASS_BRIDGE:
        if (d->subclass == 0x00) return "host bridge";
        if (d->subclass == 0x01) return "ISA bridge";
        if (d->subclass == PCI_SUB_PCI_BRIDGE) return "PCI bridge";
        return "bridge";
    case PCI_CLASS_SERIAL:
        if (d->subclass == PCI_SUB_USB) {
            switch (d->progif) {
            case PCI_PROGIF_UHCI: return "USB controller (UHCI)";
            case PCI_PROGIF_OHCI: return "USB controller (OHCI)";
            case PCI_PROGIF_EHCI: return "USB controller (EHCI)";
            case PCI_PROGIF_XHCI: return "USB controller (xHCI)";
            default:              return "USB controller";
            }
        }
        return "serial bus controller";
    case 0x00: return "unclassified";
    case 0x03: return "display controller";
    default:   return "device";
    }
}

void cmd_lspci(const char *arg1)
{
    int verbose = arg1 && (!strcmp(arg1, "-v") || !strcmp(arg1, "-vv"));
    int n = pci_count();

    if (n == 0) {
        vga_puts("No PCI devices. This machine has no PCI bus, or the\n");
        vga_puts("scan ran before pci_init().\n");
        return;
    }

    char num[12];
    utoa((uint32_t)n, num, 10);
    vga_puts(num);
    vga_puts(" PCI device");
    vga_puts(n == 1 ? ":\n\n" : "s:\n\n");

    ui_page_reset();
    for (int i = 0; i < n; i++) {
        const pci_dev_t *d = pci_get(i);

        put_hex(d->bus, 2); vga_puts(":");
        put_hex(d->dev, 2); vga_puts(".");
        put_hex(d->fn, 1);  vga_puts("  ");
        put_hex(d->vendor, 4); vga_puts(":");
        put_hex(d->device, 4); vga_puts("  ");
        put_hex(d->class, 2); vga_puts("/");
        put_hex(d->subclass, 2); vga_puts("/");
        put_hex(d->progif, 2); vga_puts("  ");
        vga_puts(describe(d));
        vga_puts("\n");
        if (!ui_page_line()) return;

        if (!verbose) continue;

        /* A 64-bit memory BAR occupies TWO bar[] slots and the second
           is its high dword, not a BAR. v0.8.5 listed it as one, which
           is why this machine appeared to have seven devices all with
           `BAR1 mem 0x00000060` -- one repeated high dword, printed
           seven times as though it were an address. (v0.8.5.1) */
        for (int b = 0; b < 6; b++) {
            uint16_t io  = pci_bar_io(d, b);
            uint32_t hi  = 0;
            uint32_t lo  = pci_bar_mem64(d, b, &hi);
            int      w64 = pci_bar_is_64(d, b);
            if (!io && !lo && !hi) { if (w64) b++; continue; }
            vga_puts("        BAR");
            put_hex((uint32_t)b, 1);
            if (io) { vga_puts(" io  0x"); put_hex(io, 4); }
            else {
                vga_puts(w64 ? " mem64 0x" : " mem 0x");
                if (hi) put_hex(hi, 8);
                put_hex(lo, 8);
            }
            vga_puts("\n");
            if (!ui_page_line()) return;
            if (w64) b++;        /* the high dword is not its own BAR */
        }
        if (d->irq && d->irq != 0xFF) {
            vga_puts("        IRQ ");
            utoa(d->irq, num, 10);
            vga_puts(num);
            vga_puts("\n");
            if (!ui_page_line()) return;
        }
    }
}
