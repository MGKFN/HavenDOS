#pragma once
#include "types.h"

/* Reaching a framebuffer the firmware put above 4 GB.
 *
 * A 32-bit kernel with no paging cannot form a physical address wider than
 * 32 bits, so a GOP framebuffer at (say) 0x4_00000000 is simply out of
 * reach -- which is what a 16 GB UEFI laptop produces once Above-4G
 * decoding moves PCI MMIO above the top of RAM.  GRUB is 64-bit and draws
 * on it happily; the kernel it hands over to cannot.
 *
 * Two mechanisms can address it without making the kernel 64-bit:
 *
 *   PSE-36  4 MB pages in an ordinary two-level table.  The page-directory
 *           entry carries physical bits 39:32 in its bits 20:13, giving a
 *           40-bit physical address.  One 4 KB page directory in total.
 *   PAE     Three-level tables, 2 MB pages, 36-bit physical.  A PDPT plus
 *           four page directories, 16 KB in total.
 *
 * PSE-36 is tried first because it is a quarter of the code on the path
 * that will actually run; PAE is the fallback for a CPU that reports no
 * PSE-36.  Both identity-map the whole low 4 GB first, so physical ==
 * virtual stays true for every other part of the kernel, and only one
 * window is redirected.
 *
 * Returns 1 and sets *virt_out on success, 0 if neither mechanism is
 * available or the request is not mappable.  On failure nothing has been
 * enabled and the machine is exactly as it was.
 */
int highfb_map(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes,
               uint32_t *virt_out);

/* The same, for DEVICE REGISTERS rather than a framebuffer: the window is
 * mapped cache-DISABLED, not merely write-through. A driver polling a
 * status register through a cacheable window can read one stale dword
 * forever, which looks like a controller that hung rather than a mapping
 * that lied. Every MMIO BAR above 4 GB must come through here; a
 * framebuffer should not, because it does not need the slowdown. */
int highfb_map_io(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes,
                  uint32_t *virt_out);

/* Which mechanism ended up being used: 0 none, 1 PSE-36, 2 PAE. */
int highfb_mechanism(void);

/* v0.8.9.6. Whether the framebuffer is write-combining (1) -- the
   difference between a usable desktop on real hardware and one that
   repaints a cell at a time. highfb_wc_low() does for a framebuffer
   below 4 GB what highfb_map() does for one above it. highfb_nowc is
   the `nowc` command-line switch. */
int  highfb_wc(void);
int  highfb_wc_low(uint32_t phys_lo, uint32_t bytes);
extern int highfb_nowc;

/* The entry builders, exposed so tests/test_highfb.c can check their bit
 * layout against the Intel manual.  A misplaced bit here is a wild write
 * to an address nobody chose, which on real hardware looks exactly like a
 * kernel that never started -- the very failure this code exists to fix. */
uint32_t highfb_pse36_pde(uint32_t phys_hi, uint32_t phys_lo, uint32_t flags);
uint64_t highfb_pae_pde  (uint32_t phys_hi, uint32_t phys_lo, uint32_t flags);
