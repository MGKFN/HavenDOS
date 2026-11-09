/* ════════════════════════════════════════════════════════════════════
   shell/i2ccmd.c -- the `i2c` command.  v0.8.8.3.

   THIS COMMAND EXISTS TO ANSWER ONE QUESTION IN ONE BOOT.

   The HP's trackpad does not answer the PS/2 aux port, and no
   keyboard appeared on USB either. Two Intel Serial-IO I2C
   controllers are on its PCI bus. So both are probably I2C-HID
   devices -- but an I2C device has no enumeration and no vendor ID,
   and the address lives in ACPI, which this kernel cannot read.

   `i2c scan` walks the bus and reports every address that answers.
   After that the HID layer can be written against a known address
   instead of a list of guesses.

   ── WHAT THE SCAN DOES AND DOES NOT DO ──────────────────────────────

   A ZERO-LENGTH READ at each address: put the address on the wire,
   look at the acknowledge, stop. Nothing is written, no register is
   named. A write to an unknown device could do something; a read
   cannot. 0x00-0x07 and 0x78-0x7F are skipped because the I2C
   specification reserves them -- 0x00 is the general-call address and
   every device on the bus listens to it.

   It cannot tell you WHAT answered. There is no identity register at
   a fixed place on I2C. What it gives you is a short list, and the
   addresses laptop touchpads actually use are few enough to name.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/i2c.h"
#include "../include/pci.h"

static void up(const char *s){ vga_puts(s); }

static void putu(uint32_t v, int width){
    char b[12]; int n = 0;
    if(v == 0) b[n++] = '0';
    while(v){ b[n++] = (char)('0' + v % 10); v /= 10; }
    while(width-- > n) vga_putchar(' ');
    while(n) vga_putchar(b[--n]);
}

static void puthex(uint32_t v, int digits){
    const char *h = "0123456789abcdef";
    while(digits--) vga_putchar(h[(v >> (digits * 4)) & 0xF]);
}

/* ── what is known to live at these addresses ────────────────────────
   NOT a database and not a claim. These are the addresses laptop
   touchpads and I2C-HID keyboards are commonly strapped to, and
   naming the likely candidate beside a number is the difference
   between a screen a person can act on and a list of hex. The scan
   cannot tell what answered; this says what it is worth trying. */
static const char *likely(uint8_t a)
{
    switch(a){
    case 0x05: return "Elan touchpad, often";
    case 0x06: return "Elan, sometimes";
    case 0x15: return "Synaptics touchpad, often";
    case 0x2c: return "Synaptics or Elan touchpad, very often";
    case 0x2a: return "touchpad, sometimes";
    case 0x3a: return "touchscreen, sometimes";
    case 0x4b: return "Atmel touch, sometimes";
    case 0x50: return "an EEPROM -- NOT a touchpad";
    case 0x68: return "an RTC -- NOT a touchpad";
    default:   return 0;
    }
}

static void show_buses(void)
{
    int n = i2c_bus_count(), i;

    if(n == 0){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("No I2C controller found.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("\n  'lspci' lists the bus. An Intel Serial-IO I2C reports\n");
        up("  class 0C, subclass 80. If one is there and this is not,\n");
        up("  the boot log says why -- either its BAR would not map,\n");
        up("  or the core did not identify itself as DesignWare.\n");
        return;
    }

    up("I2C controllers:\n\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  bus  pci       id          mapped     speed   fifo\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for(i = 0; i < n; i++){
        const pci_dev_t *d = i2c_bus_pci(i);
        up("   "); putu((uint32_t)i, 1); up("   ");
        if(d){
            puthex(d->bus, 2); up(":"); puthex(d->dev, 2);
            up("."); putu(d->fn, 1); up("   ");
            puthex(d->vendor, 4); up(":"); puthex(d->device, 4);
        } else up("   ?         ?   ");
        up("   "); puthex(i2c_bus_mmio(i), 8);
        up("   "); putu(i2c_bus_khz(i), 3); up(" kHz");
        up("   ");
        if(!i2c_bus_ready(i)){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            up("not ready");
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        up("\n");
    }
    up("\n  i2c scan        every address that answers, on every bus\n");
    up("  i2c scan <bus>  just one of them\n");
}

static void scan_one(int bus)
{
    uint8_t found[112];
    int n, i;

    up("  bus "); putu((uint32_t)bus, 1); up(": ");
    if(!i2c_bus_ready(bus)){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("the controller is not ready -- nothing to scan\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    n = i2c_scan(bus, found, (int)(sizeof found));
    if(n == 0){
        up("nothing answered (0x08 to 0x77)\n");
        return;
    }
    putu((uint32_t)n, 0);
    up(n == 1 ? " device answered\n" : " devices answered\n");
    for(i = 0; i < n && i < (int)(sizeof found); i++){
        const char *what = likely(found[i]);
        up("        0x"); puthex(found[i], 2);
        if(what){
            vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            up("   ");
            up(what);
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        up("\n");
    }
}

static void do_scan(const char *arg)
{
    int n = i2c_bus_count(), i;

    if(n == 0){ show_buses(); return; }

    up("Scanning. A zero-length read at each address -- nothing is\n");
    up("written and no register is named.\n\n");

    if(arg && arg[0]){
        int b = 0, any = 0;
        const char *s = arg;
        while(*s >= '0' && *s <= '9'){ b = b*10 + (*s - '0'); s++; any = 1; }
        if(!any || *s || b < 0 || b >= n){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            up("No such bus. 'i2c' lists them.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return;
        }
        scan_one(b);
    } else {
        for(i = 0; i < n; i++) scan_one(i);
    }

    up("\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  An address alone does not say what is there -- I2C has no\n");
    up("  identity register at a fixed place. What it does say is\n");
    up("  where to point the HID layer, which is the one thing that\n");
    up("  could not be worked out without this machine.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_i2c(const char *arg1, const char *arg2)
{
    if(!arg1 || !arg1[0]){ show_buses(); return; }
    if(!strcmp(arg1, "scan")){ do_scan(arg2); return; }

    /* v0.8.6.6's lesson: a command that quietly does something else
       when it does not understand you is worse than one that refuses. */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    up("i2c: no such option: "); up(arg1); up("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("  i2c             the controllers on this machine\n");
    up("  i2c scan [bus]  every address that answers\n");
}
