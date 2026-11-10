/* shell/i2chidcmd.c -- `i2chid`, the screen that settles the trackpad.
 *
 * The question this answers cannot be answered from here: is the HP's
 * trackpad at one of the registers drivers/i2c_hid.c tries? So this
 * prints everything that would decide it -- every address that
 * answered, what was found there, and for a device that came up, the
 * shape of its report descriptor.
 *
 * One screen, photographable. The same approach as the HavenOS
 * "hardware report -- photograph this" boot entry, for the same
 * reason: a photograph of the right screen is worth more than ten
 * rounds of guessing. */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/theme.h"
#include "../include/i2c.h"
#include "../include/i2c_hid.h"

extern void vga_printf(const char *fmt, ...);

void cmd_i2chid(const char *arg)
{
    int nbus = i2c_bus_count();
    int nd = i2c_hid_count();
    int i;

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("I2C-HID -- pointing devices on the Serial-IO buses\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);

    if(nbus == 0){
        vga_puts("No I2C controller came up. Nothing to look at.\n");
        vga_puts("`dmesg` lines starting I2C say why: no BAR, powered\n"
                 "down, or not a DesignWare core.\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("On the HP these are 00:15.0 and 00:15.1; `lspci` shows\n"
                 "whether the firmware has them enabled.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    vga_printf("%d bus(es), %d HID device(s) found at boot.\n\n", nbus, nd);

    for(i = 0; i < nd; i++){
        const ihid_dev_t *d = i2c_hid_dev(i);
        if(!d) continue;
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_printf("bus %d  addr %02x  descriptor register %04x\n",
                   d->bus, d->addr, d->desc_reg);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_printf("  vid %04x  pid %04x  ver %04x\n",
                   d->vid, d->pid, d->ver);
        vga_printf("  report descriptor %u bytes at %04x, "
                   "input %u bytes at %04x\n",
                   d->report_desc_len, d->report_desc_reg,
                   d->max_input_len, d->input_reg);
        /* v0.8.9.6: the three steps, each with its answer. */
        if(d->reset_rc == IHID_OK)
            vga_printf("  reset: answered in %u ms\n", d->reset_ms);
        else {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_printf("  reset: %s after %u ms\n",
                       ihid_strerror(d->reset_rc), d->reset_ms);
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        if(d->up_rc != IHID_OK && d->up_rc != IHID_E_NOPTR){
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_printf("  report descriptor: %s\n", ihid_strerror(d->up_rc));
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        if(d->is_pointer){
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            vga_puts("  THIS IS THE POINTER, and it is attached to the cursor.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            vga_printf("  POINTER: %s X/Y, %d button(s)%s, report id %d\n",
                       d->rd.field[d->rd.i_x].relative ? "relative"
                                                       : "absolute",
                       d->rd.nbuttons,
                       d->rd.i_contacts >= 0 ? ", multitouch" : "",
                       d->rd.pointer_report_id);
            vga_printf("  %d field(s) parsed, report %u bits\n",
                       d->rd.nfields, d->rd.input_bits);
        } else {
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_puts("  found, but no X and Y in its report descriptor\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
        }
        vga_puts("\n");
    }

    /* The addresses that answered but held no descriptor are the
       interesting ones when nothing was found: they say the bus works
       and the register guess was wrong, which is a completely
       different problem from a dead bus. */
    if(nd == 0){
        int b;
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        vga_puts("No HID device found. Scanning again to show what IS there:\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        for(b = 0; b < nbus; b++){
            uint8_t addrs[16];
            int n, k;
            if(!i2c_bus_ready(b)){
                vga_printf("  bus %d: not ready\n", b);
                continue;
            }
            n = i2c_scan(b, addrs, 16);
            if(n <= 0){
                vga_printf("  bus %d: nothing answered\n", b);
                continue;
            }
            vga_printf("  bus %d: %d address(es) answered:", b, n);
            for(k = 0; k < n; k++) vga_printf(" %02x", addrs[k]);
            vga_puts("\n");
        }
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        vga_puts("\nAn address that answers with no descriptor at 01, 20,\n"
                 "00, 02, 05 or 10 is a device whose descriptor register\n"
                 "we have not guessed. Photograph this screen.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
    }
    (void)arg;
}
