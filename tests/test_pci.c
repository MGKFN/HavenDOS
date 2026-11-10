/* The PCI enumeration, against a simulated PIIX3.
 *
 * The scan this replaced looked at FUNCTION 0 only, on every one of its three
 * copies.  On a PIIX3 -- QEMU's southbridge, and the real chipset in a great
 * many machines of that era -- the pieces sit like this:
 *
 *     00:01.0   ISA bridge        header type 0x80, "I have more functions"
 *     00:01.1   IDE controller    function 1
 *     00:01.2   USB (UHCI)        function 2
 *
 * So the USB controller was not merely undetected, it was undetectable: no
 * amount of driver work would have found hardware the enumerator never
 * looked at.  The config space below is that layout, and the checks are that
 * every function is seen and that each driver can find its own device.
 *
 * io.h is replaced by defining IO_H, so the real pci.c runs here unmodified
 * against a simulated bus rather than a port.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── a simulated PCI bus ──────────────────────────────────────────────── */
#define IO_H
static inline void io_wait(void) {}
static void     outb(uint16_t p, uint8_t v)  { (void)p; (void)v; }
static void     outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
static uint8_t  inb(uint16_t p)              { (void)p; return 0xFF; }
static uint16_t inw(uint16_t p)              { (void)p; return 0xFFFF; }
static void     outl(uint16_t port, uint32_t val);
static uint32_t inl(uint16_t port);

typedef struct { uint8_t bus, dev, fn; uint8_t pm_cap; uint32_t cfg[64]; } fake_fn_t;

/* Build the first 64 bytes of a config header. */
static void mk(fake_fn_t *f, uint8_t bus, uint8_t dev, uint8_t fn,
               uint16_t ven, uint16_t did, uint8_t cls, uint8_t sub,
               uint8_t progif, uint8_t hdrtype, uint8_t irq)
{
    memset(f, 0, sizeof(*f));
    f->bus = bus; f->dev = dev; f->fn = fn;
    f->cfg[0] = ((uint32_t)did << 16) | ven;                 /* 0x00 */
    f->cfg[1] = 0x02000000u;                                  /* 0x04 status/cmd */
    f->cfg[2] = ((uint32_t)cls << 24) | ((uint32_t)sub << 16)
              | ((uint32_t)progif << 8);                      /* 0x08 */
    f->cfg[3] = ((uint32_t)hdrtype << 16);                    /* 0x0C */
    f->cfg[15] = irq;                                         /* 0x3C */
}

static fake_fn_t bus_devs[8];
static int       nbus_devs;
static uint32_t  cur_addr;

/* The PIIX3 as QEMU presents it with -usb and an AC97. */
static void build_bus(void)
{
    nbus_devs = 0;
    fake_fn_t *f;

    f = &bus_devs[nbus_devs++];                 /* host bridge */
    mk(f, 0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, 0x00, 0x00, 0xFF);

    f = &bus_devs[nbus_devs++];                 /* ISA bridge, MULTIFUNCTION */
    mk(f, 0, 1, 0, 0x8086, 0x7000, 0x06, 0x01, 0x00, 0x80, 0xFF);

    f = &bus_devs[nbus_devs++];                 /* IDE, function 1 */
    mk(f, 0, 1, 1, 0x8086, 0x7010, 0x01, 0x01, 0x80, 0x00, 0xFF);
    f->cfg[4] = 0x0000C001;                     /* BAR0 io 0xC000 */
    f->cfg[5] = 0x0000C009;                     /* BAR1 io 0xC008 */
    f->cfg[6] = 0x0000C011;                     /* BAR2 io 0xC010 */
    f->cfg[7] = 0x0000C019;                     /* BAR3 io 0xC018 */

    f = &bus_devs[nbus_devs++];                 /* UHCI, function 2 */
    mk(f, 0, 1, 2, 0x8086, 0x7020, 0x0C, 0x03, 0x00, 0x00, 11);
    f->cfg[8] = 0x0000C041;                     /* BAR4 io 0xC040 */

    f = &bus_devs[nbus_devs++];                 /* RTL8139 */
    mk(f, 0, 3, 0, 0x10EC, 0x8139, 0x02, 0x00, 0x00, 0x00, 10);
    f->cfg[4] = 0x0000C101;                     /* BAR0 io 0xC100 */

    f = &bus_devs[nbus_devs++];                 /* AC'97 */
    mk(f, 0, 4, 0, 0x8086, 0x2415, 0x04, 0x01, 0x00, 0x00, 9);
    f->cfg[4] = 0x0000C201;                     /* BAR0 io 0xC200, mixer */
    f->cfg[5] = 0x0000C301;                     /* BAR1 io 0xC300, bus master */
}

static fake_fn_t *lookup(uint8_t bus, uint8_t dev, uint8_t fn)
{
    for (int i = 0; i < nbus_devs; i++)
        if (bus_devs[i].bus == bus && bus_devs[i].dev == dev
            && bus_devs[i].fn == fn) return &bus_devs[i];
    return 0;
}

static void outl(uint16_t port, uint32_t val)
{
    if (port == 0xCF8) { cur_addr = val; return; }
    if (port != 0xCFC) return;
    uint8_t bus = (cur_addr >> 16) & 0xFF, dev = (cur_addr >> 11) & 0x1F;
    uint8_t fn  = (cur_addr >> 8) & 7,     reg = cur_addr & 0xFC;
    fake_fn_t *f = lookup(bus, dev, fn);
    if (!f) return;
    /* v0.8.9.6: a D3hot -> D0 write on a function without
       No_Soft_Reset resets it -- BARs and command register to zero,
       which is what pci_set_d0() has to put back. */
    if (f->pm_cap && reg == f->pm_cap + 4
        && (f->cfg[reg / 4] & 3u) == 3u && (val & 3u) == 0u) {
        for (int i = 4; i <= 9; i++) f->cfg[i] = 0;
        f->cfg[1] &= 0xFFFF0000u;
    }
    f->cfg[reg / 4] = val;
}

static uint32_t inl(uint16_t port)
{
    if (port != 0xCFC) return 0xFFFFFFFFu;
    uint8_t bus = (cur_addr >> 16) & 0xFF, dev = (cur_addr >> 11) & 0x1F;
    uint8_t fn  = (cur_addr >> 8) & 7,     reg = cur_addr & 0xFC;
    fake_fn_t *f = lookup(bus, dev, fn);
    if (!f) return 0xFFFFFFFFu;          /* no device answers */
    return f->cfg[reg / 4];
}

#include "../include/types.h"
#include "../include/vga.h"

/* pci.c reports what it finds through the kernel log now, so the log is
   linked in here too.  Its output is captured rather than drawn, which also
   lets the checks below read back what the scan actually said. */
static char klog_screen[16384];
static int  klog_screen_len;
void vga_puts(const char *s)
{
    while (*s && klog_screen_len < (int)sizeof(klog_screen) - 1)
        klog_screen[klog_screen_len++] = *s++;
    klog_screen[klog_screen_len] = 0;
}
void vga_set_color(vga_color_t a, vga_color_t b) { (void)a; (void)b; }
static uint32_t v_ms;
uint32_t boot_ms(void) { return v_ms; }

#include "../include/klog.h"
#include "../kernel/klog.c"
#include "../include/pci.h"
#include "../drivers/pci.c"

/* Does any log line contain this? */
static int klog_mentions(const char *needle)
{
    for (int i = 0; i < klog_count(); i++) {
        const char *h = klog_text(i);
        for (int a = 0; h[a]; a++) {
            int b = 0;
            while (needle[b] && h[a + b] == needle[b]) b++;
            if (!needle[b]) return 1;
        }
    }
    return 0;
}

/* ── checks ───────────────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if (cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what,
                           detail ? detail : ""); }
}
static void head(const char *t) { printf("\n%s\n", t); }

static int g_delays_seen;
void count_delay(uint32_t ms){ (void)ms; g_delays_seen++; }

int main(void)
{
    klog_set_echo(0);     /* the scan's log lines are inspected, not printed */
    printf("PCI enumeration\n");
    printf("===============\n");
    build_bus();
    pci_init();

    head("every function is seen, not just function 0");
    ck("all six functions enumerated", pci_count() == 6, "");
    {
        int fn2 = 0;
        for (int i = 0; i < pci_count(); i++)
            if (pci_get(i)->dev == 1 && pci_get(i)->fn == 2) fn2 = 1;
        ck("00:01.2 is in the table", fn2,
           "the multifunction bit on 00:01.0 was not followed, so the USB "
           "controller is invisible");
    }

    head("the USB controller can be found by class");
    {
        const pci_dev_t *u = pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB,
                                            PCI_PROGIF_UHCI, 0);
        ck("a UHCI controller is found", u != 0, "");
        if (u) {
            ck("at 00:01.2", u->bus == 0 && u->dev == 1 && u->fn == 2, "");
            ck("its I/O BAR is BAR4", pci_bar_io(u, 4) == 0xC040, "");
            ck("BAR4 is not reported as memory", pci_bar_mem(u, 4) == 0, "");
            ck("its interrupt line is readable", u->irq == 11, "");
        }
    }

    head("and the audio controller, which comes next");
    {
        const pci_dev_t *a = pci_find_class(PCI_CLASS_MULTIMEDIA,
                                            PCI_SUB_AUDIO, PCI_PROGIF_ANY, 0);
        ck("an audio device is found", a != 0, "");
        if (a) {
            ck("it is the AC'97", a->vendor == 0x8086 && a->device == 0x2415, "");
            ck("mixer and bus-master BARs both read",
               pci_bar_io(a, 0) == 0xC200 && pci_bar_io(a, 1) == 0xC300, "");
        }
    }

    head("nth lets a second controller of the same kind be reached");
    ck("there is no second UHCI here",
       pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB, PCI_PROGIF_UHCI, 1) == 0,
       "");

    head("what already worked still works");
    ck("the IDE controller is found", pci_ide_found() == 1, "");
    ck("primary channel from BAR0", pci_ide_pri_base() == 0xC000, "");
    ck("primary control is BAR1 + 2", pci_ide_pri_ctrl() == 0xC00A, "");
    ck("secondary channel from BAR2", pci_ide_sec_base() == 0xC010, "");
    {
        uint16_t rtl = 0; uint32_t e1k = 0;
        pci_scan_net2(&rtl, &e1k);
        ck("the RTL8139 is still found", rtl == 0xC100, "");
        ck("and no e1000 is invented", e1k == 0, "");
        uint16_t only = 0;
        pci_scan_net(&only);
        ck("the single-NIC scan agrees", only == 0xC100, "");
    }

    head("enabling a device sets the bits a driver needs");
    {
        const pci_dev_t *u = pci_find_class(PCI_CLASS_SERIAL, PCI_SUB_USB,
                                            PCI_PROGIF_UHCI, 0);
        pci_enable(u, 1);
        uint16_t cmd = pci_cfg_read16(u->bus, u->dev, u->fn, 0x04);
        ck("I/O space enabled", (cmd & 1) != 0, "");
        ck("memory space enabled", (cmd & 2) != 0, "");
        ck("bus master enabled, or it cannot do DMA", (cmd & 4) != 0, "");
        uint32_t whole = pci_cfg_read32(u->bus, u->dev, u->fn, 0x04);
        ck("the status half was not clobbered by a 16-bit write",
           (whole >> 16) == 0x0200,
           "a 16-bit config write must read, splice and write back");
    }

    head("the scan says what it found, in the kernel log");
    {
        /* pci.c used to build a private text log that nothing read -- it
           described the bus to an audience of zero for several releases.
           The lines go to the kernel log now, where the boot screen and
           dmesg both show them, so the scan being silent is a real fault
           the suite can catch. */
        ck("the USB controller it found is named in the log",
           klog_mentions("8086:7020"),
           "the device at 00:01.2 is the one three earlier scans could not "
           "see at all; the log is how you find out whether it is there");
        ck("and the IDE ports it chose are too", klog_mentions("IDE: primary"), "");
    }

    head("pci_set_d0: a function left in D3 comes back with its BARs");
    {
        nbus_devs = 1;
        fake_fn_t *f = &bus_devs[0];
        mk(f, 0, 0x15, 0, 0x8086, 0x51E8, 0x0C, 0x80, 0, 0, 0);
        f->cfg[1] = 0x00100006u;          /* status: cap list; cmd: mem+bm */
        f->cfg[4] = 0x4000000Cu;          /* BAR0: 64-bit prefetchable   */
        f->cfg[5] = 0x00000060u;          /* BAR1: its high dword         */
        f->cfg[13] = 0x80;                /* capabilities at 0x80          */
        f->cfg[0x80/4] = 0x00009005u;     /* MSI (05), next 0x90           */
        f->cfg[0x90/4] = 0x00030001u;     /* PM (01), end of list          */
        f->cfg[0x94/4] = 0x00000003u;     /* PMCSR: D3hot                  */
        f->pm_cap = 0x90;
        pci_init();
        const pci_dev_t *d = pci_get(0);
        g_delays_seen = 0;
        int was = pci_set_d0(d, count_delay);
        ck("it reports the state it was in", was == 3, "");
        ck("PMCSR now says D0", (f->cfg[0x94/4] & 3u) == 0, "");
        ck("the 10 ms recovery was waited for", g_delays_seen == 1, "");
        ck("BAR0 survived the reset the transition caused",
           f->cfg[4] == 0x4000000Cu, "");
        ck("  ... and its high dword", f->cfg[5] == 0x00000060u, "");
        ck("  ... and memory decoding is back on",
           (f->cfg[1] & 0x0006u) == 0x0006u, "");
        ck("a second call finds it on and touches nothing",
           pci_set_d0(d, count_delay) == 0 && g_delays_seen == 1, "");
        f->cfg[1] &= ~0x00100000u;        /* no capability list at all */
        ck("a function with no capability list answers -1",
           pci_set_d0(d, count_delay) == -1, "");
    }

    head("a bus with nothing on it does not invent devices");
    {
        nbus_devs = 0;
        pci_init();
        ck("no devices", pci_count() == 0, "");
        ck("IDE falls back to the legacy ports",
           pci_ide_found() == 0 && pci_ide_pri_base() == 0x1F0, "");
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
