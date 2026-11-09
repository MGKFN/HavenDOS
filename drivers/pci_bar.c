/*
 * drivers/pci_bar.c -- decoding a BAR, and nothing else.
 *
 * These four functions read d->bar[] and perform no port I/O at all, so
 * they are in a file of their own and the test suites LINK it rather
 * than stubbing it. That distinction is the whole point of the split:
 * v0.8.5's xHCI suite stubbed pci_bar_mem() to hand back a working
 * address, so the suite could never have discovered that the real one
 * truncated a 64-bit BAR. A stub that is kinder than the function it
 * replaces tests the stub.
 *
 * Same rule as fs/gpt.c: it is LINKED into a suite, not #included.
 */
#include "../include/types.h"
#include "../include/pci.h"

uint16_t pci_bar_io(const pci_dev_t *d, int n) {
    if (!d || n < 0 || n > 5) return 0;
    uint32_t b = d->bar[n];
    if (!(b & 1)) return 0;                 /* memory BAR, not I/O */
    return (uint16_t)(b & 0xFFFC);
}

/* v0.8.5.1 -- 64-bit BARs, which this did not know about.

   A memory BAR whose type field (bits 2:1) is 2 is SIXTY-FOUR bits wide:
   the next BAR register is not a BAR at all, it is the high half of this
   one. Ignoring that had two consequences on a UEFI laptop with Above-4G
   decoding turned on, which is every machine of its generation:

     - `lspci` printed the high dword as though it were its own BAR. On
       the HP that is the line `BAR1 mem 0x00000060` under seven
       different devices -- not seven identical BARs, one repeated high
       dword. The xHCI's real base is 0x60_02120000, about 384 GiB.

     - pci_bar_mem() returned the LOW half of it. drivers/xhci_hc.c took
       0x02120000, read a capability register out of whatever happened to
       be at 33 MB, got a nonsense CAPLENGTH and refused to start -- which
       is exactly what it should do with a wrong address, and told nobody
       which address it had been given.

   Truncating is the dangerous half. A caller that gets 0 can say so; a
   caller that gets the bottom 32 bits of a 40-bit address reads somebody
   else's memory and calls it a controller. So pci_bar_mem() now returns
   0 for a BAR that is genuinely above 4 GB, and a driver that can map
   high memory asks for both halves with pci_bar_mem64(). */
uint32_t pci_bar_mem64(const pci_dev_t *d, int n, uint32_t *hi_out) {
    if (hi_out) *hi_out = 0;
    if (!d || n < 0 || n > 5) return 0;
    uint32_t b = d->bar[n];
    if (b & 1) return 0;                    /* I/O BAR, not memory */
    if (((b >> 1) & 3u) == 2u) {            /* 64-bit: high half follows */
        if (n >= 5) return 0;               /* no register left to hold it */
        if (hi_out) *hi_out = d->bar[n + 1];
    }
    return b & ~0xFu;
}

int pci_bar_is_64(const pci_dev_t *d, int n) {
    if (!d || n < 0 || n > 5) return 0;
    if (d->bar[n] & 1) return 0;
    return (((d->bar[n] >> 1) & 3u) == 2u);
}

uint32_t pci_bar_mem(const pci_dev_t *d, int n) {
    uint32_t hi = 0;
    uint32_t lo = pci_bar_mem64(d, n, &hi);
    if (hi) return 0;      /* above 4 GB: not expressible here, and the
                              low half alone is a wrong address, not a
                              partial one */
    return lo;
}
