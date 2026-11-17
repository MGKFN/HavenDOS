/*
 * drivers/pci_assign.c -- a memory window for a BAR the firmware left
 * at zero. v0.9.6.6.
 *
 * THE HP'S TRACKPAD. Its two Serial-IO I2C controllers (00:15.0 and
 * 00:15.1) answer in config space with BAR0 at zero: the firmware gave
 * them no memory window, so there is nothing to talk to the trackpad
 * through. HANDOFF has said since 0.8.8.4 that the fix is PCI resource
 * assignment, which this kernel had never done; Windows and Linux both
 * do it (Linux: pci_assign_unassigned_resources). Kyan, 17 November:
 * "trackpad still don't, still the memory window error".
 *
 * ── WHERE THE WINDOW GOES ─────────────────────────────────────────────
 *
 * A real allocator takes the host bridge's windows from ACPI's _CRS.
 * There is no AML interpreter here, so this places the new BAR where
 * the firmware has already proved the root complex decodes: directly
 * after a BAR it assigned itself ABOVE 4 GB. On the HP every PCH device
 * sits at 0x60_xxxxxxxx (the xHCI at 0x60_02120000), so a 4 KB window
 * just past one of them is inside the same firmware window.
 *
 * Never below 4 GB. That range holds the local APIC, the I/O APIC, the
 * HPET, the SPI flash and the TPM, none of which are BARs and none of
 * which this file could see; above 4 GB there is nothing but BARs.
 *
 * ── NOT TOUCHING ANYONE ELSE'S BAR ─────────────────────────────────────
 *
 * The overlap test needs every other BAR's SIZE, and sizing a BAR means
 * writing all ones to it -- on the display controller and the xHCI,
 * which are running. So it is never done. A BAR is aligned to its own
 * size, so the lowest set bit of its base is an UPPER bound on its size:
 * [base, base + lowbit(base)) always covers it. That over-reserves, which
 * costs nothing but address space, of which there is plenty above 4 GB.
 * The bound is capped at 1 GB so one very aligned BAR cannot reserve
 * the whole window; nothing on bus 0 but the display has a BAR that big,
 * and the display's BARs are each a range of their own anyway.
 *
 * Bridge windows (a root port's memory and prefetchable ranges) are
 * reserved whole: whatever is behind a bridge lives inside them.
 *
 * The planner below is pure -- ranges in, an address out -- so
 * tests/test_pci_assign.c drives it with the HP's layout.
 */
#include "../include/types.h"
#include "../include/pci.h"
#include "../include/pci_assign.h"

#define GB4      0x100000000ull
#define CAP      0x40000000ull          /* 1 GB: see the header */
#define MAX_BAR  0x01000000ull          /* 16 MB: more is not a controller's register file */

static uint64_t lowbit(uint64_t v){ return v & (~v + 1); }

static int overlaps(const pci_range_t *r, int n, uint64_t a, uint64_t size)
{
    for(int i = 0; i < n; i++)
        if(a < r[i].end && r[i].base < a + size) return 1;
    return 0;
}

/* A place right after something the firmware put above 4 GB -- a BAR
   or a bridge window, both of which it decodes -- where `size` bytes,
   aligned to `size`, overlap nothing. Of those, the one CLOSEST to what
   it follows: a gap inside the cluster of PCH devices, where the
   firmware's window certainly is, beats the far side of a big BAR's
   bound (the display's 256 MB aperture is bounded at 1 GB; the address
   after that bound is the lowest candidate on the HP, and a gigabyte
   from anything known to decode). 0 when there is none. */
uint64_t pci_plan_window(const pci_range_t *used, int n, uint64_t size)
{
    uint64_t best = 0, best_d = 0;
    if(!size || (size & (size - 1)) || size > MAX_BAR) return 0;
    for(int i = 0; i < n; i++){
        uint64_t a, dist;
        if(used[i].base < GB4) continue;                    /* anchors: above 4 GB only */
        a = (used[i].end + size - 1) & ~(size - 1);
        if(a < used[i].end) continue;                       /* wrapped */
        if(overlaps(used, n, a, size)) continue;
        dist = a - used[i].base;
        if(!best || dist < best_d || (dist == best_d && a < best)){ best = a; best_d = dist; }
    }
    return best;
}

/* The conservative extent of one BAR whose size is not measured. */
uint64_t pci_bar_bound(uint64_t base)
{
    uint64_t b = lowbit(base);
    if(!b || b > CAP) b = CAP;
    return b;
}

/* A bound may also stop at the next range's base: the firmware never
   gives two BARs the same addresses, so whatever starts above a BAR is
   past its end. Without this the display's 16 MB register BAR at
   0x60_00000000 -- aligned to 128 GB, so bounded at 1 GB -- would cover
   every PCH device after it, the xHCI included, and push the new window
   a gigabyte away from anything the firmware is known to decode. */
void pci_trim_ranges(pci_range_t *r, int n)
{
    for(int i = 0; i < n; i++){
        if(!r[i].bar) continue;                /* a bridge window is exact */
        for(int j = 0; j < n; j++)
            if(j != i && r[j].base > r[i].base && r[j].base < r[i].end)
                r[i].end = r[j].base;
    }
}

/* ── collecting what is in use ───────────────────────────────────────── */
int pci_used_ranges(pci_range_t *out, int max, const pci_dev_t *skip)
{
    int n = 0;
    for(int i = 0; i < pci_count() && n < max; i++){
        const pci_dev_t *d = pci_get(i);
        if(!d || d == skip) continue;
        for(int b = 0; b < 6 && n < max; b++){
            uint32_t hi = 0, lo;
            uint64_t base;
            if(d->bar[b] & 1) continue;                     /* I/O */
            lo = pci_bar_mem64(d, b, &hi);
            if(pci_bar_is_64(d, b)) b++;                    /* its high half */
            base = ((uint64_t)hi << 32) | lo;
            if(!base) continue;
            out[n].base = base;
            out[n].end  = base + pci_bar_bound(base);
            out[n].bar  = 1;
            n++;
        }
        if(d->class == PCI_CLASS_BRIDGE && d->subclass == PCI_SUB_PCI_BRIDGE && n + 2 <= max){
            uint32_t mw = pci_cfg_read32(d->bus, d->dev, d->fn, 0x20);
            uint32_t pw = pci_cfg_read32(d->bus, d->dev, d->fn, 0x24);
            uint64_t mb = (uint64_t)(mw & 0xFFF0u) << 16;
            uint64_t ml = ((uint64_t)(mw >> 16 & 0xFFF0u) << 16) | 0xFFFFFu;
            uint64_t pb = (uint64_t)(pw & 0xFFF0u) << 16;
            uint64_t pl = ((uint64_t)(pw >> 16 & 0xFFF0u) << 16) | 0xFFFFFu;
            if((pw & 0xF) == 1){                            /* 64-bit prefetchable */
                pb |= (uint64_t)pci_cfg_read32(d->bus, d->dev, d->fn, 0x28) << 32;
                pl |= (uint64_t)pci_cfg_read32(d->bus, d->dev, d->fn, 0x2C) << 32;
            }
            if(ml > mb){ out[n].base = mb; out[n].end = ml + 1; out[n].bar = 0; n++; }
            if(pl > pb){ out[n].base = pb; out[n].end = pl + 1; out[n].bar = 0; n++; }
        }
    }
    pci_trim_ranges(out, n);
    return n;
}

/* ── the write ─────────────────────────────────────────────────────────
 * Memory decoding off for the whole of it, so the device never answers
 * at a half-written address; the BAR is sized (it is unassigned, so
 * nothing is decoding it), the window planned, written, and read back.
 * A BAR that does not hold what was written is put back to zero and
 * decoding is left off: a device answering somewhere nobody chose is
 * worse than a device that does not answer. */
const char *pci_assign_why(int r)
{
    switch(r){
    case PA_OK:        return "assigned";
    case PA_NOT_MEM:   return "it is an I/O BAR, not memory";
    case PA_32BIT:     return "it is a 32-bit BAR, and below 4 GB there is no "
                              "place this kernel can prove is free";
    case PA_SIZE:      return "it did not report a size (sizing read back zero)";
    case PA_TOO_BIG:   return "it asks for more than 16 MB";
    case PA_NO_ROOM:   return "no firmware-assigned BAR above 4 GB to place it after";
    case PA_STUCK:     return "the address written did not read back";
    case PA_ASSIGNED:  return "it already has a window";
    default:           return "?";
    }
}

int pci_assign_mem_bar(const pci_dev_t *d, int n, uint64_t *addr_out)
{
    uint8_t  B, D, F, reg;
    uint16_t cmd;
    uint32_t lo0, hi0 = 0, rl, rh = 0;
    uint64_t size, a;
    pci_range_t used[PCI_ASSIGN_MAX_RANGES];
    int nused, is64;

    if(addr_out) *addr_out = 0;
    if(!d || n < 0 || n > 5) return PA_NOT_MEM;
    B = d->bus; D = d->dev; F = d->fn; reg = (uint8_t)(0x10 + 4 * n);
    lo0 = pci_cfg_read32(B, D, F, reg);
    if(lo0 & 1) return PA_NOT_MEM;
    is64 = ((lo0 >> 1) & 3u) == 2u;
    if(!is64) return PA_32BIT;
    if(n >= 5) return PA_NOT_MEM;
    hi0 = pci_cfg_read32(B, D, F, (uint8_t)(reg + 4));
    if((lo0 & ~0xFu) || hi0) return PA_ASSIGNED;

    cmd = pci_cfg_read16(B, D, F, 0x04);
    pci_cfg_write16(B, D, F, 0x04, (uint16_t)(cmd & ~0x0002u));   /* memory decode off */

    pci_cfg_write32(B, D, F, reg, 0xFFFFFFFFu);
    pci_cfg_write32(B, D, F, (uint8_t)(reg + 4), 0xFFFFFFFFu);
    rl = pci_cfg_read32(B, D, F, reg);
    rh = pci_cfg_read32(B, D, F, (uint8_t)(reg + 4));
    pci_cfg_write32(B, D, F, reg, lo0);
    pci_cfg_write32(B, D, F, (uint8_t)(reg + 4), hi0);
    {
        uint64_t mask = ((uint64_t)rh << 32) | (rl & ~0xFu);
        size = mask ? (~mask + 1) : 0;
    }
    if(!size){ pci_cfg_write16(B, D, F, 0x04, cmd); return PA_SIZE; }
    if(size > MAX_BAR){ pci_cfg_write16(B, D, F, 0x04, cmd); return PA_TOO_BIG; }
    if(size < 0x1000) size = 0x1000;          /* a page: what the mapper deals in */

    nused = pci_used_ranges(used, PCI_ASSIGN_MAX_RANGES, d);
    a = pci_plan_window(used, nused, size);
    if(!a){ pci_cfg_write16(B, D, F, 0x04, cmd); return PA_NO_ROOM; }

    pci_cfg_write32(B, D, F, reg, (uint32_t)a | (lo0 & 0xFu));
    pci_cfg_write32(B, D, F, (uint8_t)(reg + 4), (uint32_t)(a >> 32));
    rl = pci_cfg_read32(B, D, F, reg);
    rh = pci_cfg_read32(B, D, F, (uint8_t)(reg + 4));
    if((rl & ~0xFu) != (uint32_t)a || rh != (uint32_t)(a >> 32)){
        pci_cfg_write32(B, D, F, reg, lo0);
        pci_cfg_write32(B, D, F, (uint8_t)(reg + 4), hi0);
        return PA_STUCK;                       /* decoding stays off */
    }
    pci_set_bar(d, n, rl);
    pci_set_bar(d, n + 1, rh);
    pci_cfg_write16(B, D, F, 0x04, (uint16_t)(cmd | 0x0002u));
    if(addr_out) *addr_out = a;
    return PA_OK;
}
