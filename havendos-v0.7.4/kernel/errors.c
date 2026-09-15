/*
 * errors.c — kernel_log() implementation for HavenDOS v0.6.8
 *
 * Writes structured error entries to VGA console.
 * Format: "ERR NNNN: message\n"
 *
 * Kept deliberately simple: no dynamic allocation, no recursion risk,
 * callable at any point after vga_init().
 */
#include "../include/errors.h"
#include "../include/types.h"
#include "../include/vga.h"

extern void utoa(uint32_t n, char *buf, int base);

void kernel_log(hd_err_t code, const char *msg)
{
    char numbuf[12];
    utoa((uint32_t)(code < 0 ? -code : code), numbuf, 10);

    /* Use dark grey — visible but does not hijack the active screen */
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("ERR ");
    vga_puts(numbuf);
    vga_puts(": ");
    vga_puts(msg);
    vga_puts("\n");
    /* Caller is responsible for restoring colour if needed */
}
