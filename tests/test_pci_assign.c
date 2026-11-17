/* tests/test_pci_assign.c -- drivers/pci_assign.c against a simulated bus.
 * v0.9.6.6.
 *
 * The HP's trackpad sits behind two Serial-IO I2C controllers whose BAR0
 * the firmware left at zero. drivers/pci_assign.c gives such a BAR a
 * window. What can go wrong is all about OTHER devices: a window on top
 * of the xHCI is a dead keyboard, one on top of the display a garbled
 * screen, and sizing a live BAR moves it. So the bus below is the HP's
 * layout as its own `lspci` showed it (the xHCI at 0x60_02120000, the
 * display's registers at 0x60_00000000, its aperture at 0x40_00000000),
 * every BAR has a TRUE size the planner is never told, and the checks
 * are against those true extents.
 *
 * Writes are counted per function and per register, so "nobody else's
 * BAR was written" and "decoding was off for every write" are counts,
 * not hopes.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../include/types.h"
#include "../include/pci.h"
#include "../include/pci_assign.h"

static int total, fails;
static void ck(const char *name, int ok, long v)
{
    total++;
    if(ok) printf("  ok    %s\n", name);
    else { fails++; printf("  FAIL  %s  (%ld)\n", name, v); }
}
static void head(const char *s){ printf("\n%s\n", s); }

/* ── the simulated bus ─────────────────────────────────────────────── */
typedef struct {
    pci_dev_t d;
    uint32_t  cfg[64];
    uint64_t  size[6];        /* the TRUE size of each BAR; 0 = none     */
    int       stuck;          /* the BAR ignores address writes          */
    int       bar_writes;     /* BAR register writes, all of them        */
    int       writes_decode_on;   /* ... made with memory decoding on    */
} fn_t;

#define NF 24
static fn_t F[NF];
static int  nf;
static int  set_bar_calls;

static fn_t *at(uint8_t b, uint8_t d, uint8_t f)
{
    for(int i = 0; i < nf; i++)
        if(F[i].d.bus == b && F[i].d.dev == d && F[i].d.fn == f) return &F[i];
    return 0;
}

int pci_count(void){ return nf; }
const pci_dev_t *pci_get(int i){ return (i >= 0 && i < nf) ? &F[i].d : 0; }
void pci_set_bar(const pci_dev_t *d, int n, uint32_t v)
{
    set_bar_calls++;
    for(int i = 0; i < nf; i++) if(&F[i].d == d) F[i].d.bar[n] = v;
}

uint32_t pci_cfg_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t reg)
{
    fn_t *x = at(b, d, f);
    return x ? x->cfg[reg / 4] : 0xFFFFFFFFu;
}
void pci_cfg_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint32_t v)
{
    fn_t *x = at(b, d, f);
    if(!x) return;
    if(reg >= 0x10 && reg <= 0x24 && !(x->d.class == PCI_CLASS_BRIDGE)){
        int n = (reg - 0x10) / 4;
        x->bar_writes++;
        if(x->cfg[1] & 2u) x->writes_decode_on++;
        /* a BAR keeps its low flag bits and only the bits its size lets
           it hold: all ones read back as the size mask */
        {
            int hihalf = n > 0 && ((x->cfg[4 + n - 1] >> 1) & 3u) == 2u &&
                         x->size[n - 1] && !(x->cfg[4 + n - 1] & 1);
            if(hihalf){
                uint64_t m = ~(x->size[n - 1] - 1);
                if(!x->stuck || v == 0xFFFFFFFFu || !v)
                    x->cfg[4 + n] = v & (uint32_t)(m >> 32);
            } else if(x->size[n]){
                uint64_t m = ~(x->size[n] - 1);
                uint32_t flags = x->cfg[4 + n] & 0xFu;
                if(!x->stuck || v == 0xFFFFFFFFu || !(v & ~0xFu))
                    x->cfg[4 + n] = (v & (uint32_t)m & ~0xFu) | flags;
            }
        }
        return;
    }
    x->cfg[reg / 4] = v;
}
uint16_t pci_cfg_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg)
{
    return (uint16_t)(pci_cfg_read32(b, d, f, reg) >> ((reg & 2) * 8));
}
void pci_cfg_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint16_t v)
{
    fn_t *x = at(b, d, f);
    if(!x) return;
    {
        uint32_t w = x->cfg[reg / 4], sh = (reg & 2) * 8;
        w = (w & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
        x->cfg[reg / 4] = w;
    }
}

#include "../drivers/pci_assign.c"

/* A function, with BARs given as (base, true size, 64-bit?). The table's
   copy (d.bar) mirrors config space, as pci_init() reads it. */
static fn_t *mkfn(uint8_t b, uint8_t d, uint8_t f, uint8_t cls, uint8_t sub)
{
    fn_t *x = &F[nf++];
    memset(x, 0, sizeof *x);
    x->d.bus = b; x->d.dev = d; x->d.fn = f; x->d.class = cls; x->d.subclass = sub;
    x->cfg[1] = 0x00000006u;                         /* memory + bus master on */
    return x;
}
static void bar(fn_t *x, int n, uint64_t base, uint64_t size, int is64)
{
    x->size[n] = size;
    x->cfg[4 + n] = (uint32_t)base | (is64 ? 4u : 0u);
    x->d.bar[n] = x->cfg[4 + n];
    if(is64){
        x->cfg[5 + n] = (uint32_t)(base >> 32);
        x->d.bar[n + 1] = x->cfg[5 + n];
    }
}

#define G(x) ((uint64_t)(x))
static fn_t *I2C0, *I2C1, *XHCI;

/* The HP, from its own lspci (19-20 Oct) and 0.8.5.1's note on where
   the xHCI is. Sizes are the parts' documented ones. */
static void build_hp(void)
{
    fn_t *x;
    nf = 0; set_bar_calls = 0;
    x = mkfn(0, 0x02, 0, 0x03, 0x00);                /* Iris Xe */
    bar(x, 0, G(0x6000000000), 0x1000000, 1);        /* GTTMMADR 16 MB */
    bar(x, 2, G(0x4000000000), 0x10000000, 1);       /* aperture 256 MB */
    x = mkfn(0, 0x04, 0, 0x11, 0x80);                /* Dynamic Tuning */
    bar(x, 0, G(0x6002100000), 0x20000, 1);
    x = mkfn(0, 0x06, 0, PCI_CLASS_BRIDGE, PCI_SUB_PCI_BRIDGE);   /* root port, NVMe */
    x->cfg[0x20 / 4] = 0x60106010u;                  /* memory 0x6010_0000..0x601F_FFFF */
    x->cfg[0x24 / 4] = 0x0001FFF1u;                  /* prefetchable: none (base > limit) */
    x = mkfn(0, 0x08, 0, 0x08, 0x80);                /* GNA */
    bar(x, 0, G(0x600213A000), 0x1000, 1);
    XHCI = x = mkfn(0, 0x14, 0, PCI_CLASS_SERIAL, PCI_SUB_USB);
    bar(x, 0, G(0x6002120000), 0x10000, 1);          /* 64 KB */
    x = mkfn(0, 0x14, 2, 0x05, 0x00);                /* shared SRAM */
    bar(x, 0, G(0x6002134000), 0x4000, 1);
    bar(x, 2, G(0x6002139000), 0x1000, 1);
    I2C0 = x = mkfn(0, 0x15, 0, PCI_CLASS_SERIAL, 0x80);
    bar(x, 0, 0, 0x1000, 1); bar(x, 2, 0, 0x1000, 1);
    x->cfg[1] = 0;                                   /* firmware left it off */
    I2C1 = x = mkfn(0, 0x15, 1, PCI_CLASS_SERIAL, 0x80);
    bar(x, 0, 0, 0x1000, 1); bar(x, 2, 0, 0x1000, 1);
    x->cfg[1] = 0;
    x = mkfn(0, 0x16, 0, 0x07, 0x80);                /* HECI */
    bar(x, 0, G(0x6002138000), 0x1000, 1);
    x = mkfn(0, 0x1F, 3, 0x04, 0x01);                /* HD Audio */
    bar(x, 0, G(0x6002130000), 0x4000, 1);
    bar(x, 4, G(0x6001000000), 0x100000, 1);
    x = mkfn(0, 0x1F, 4, 0x0C, 0x05);                /* SMBus */
    bar(x, 0, G(0x600213B000), 0x100, 1);
    x = mkfn(0, 0x1F, 5, PCI_CLASS_SERIAL, 0x80);    /* SPI flash: 32-bit, below 4 GB */
    bar(x, 0, 0xFE010000u, 0x1000, 0);
}

static uint64_t bar_of(const fn_t *x, int n)
{
    uint64_t lo = x->cfg[4 + n] & ~0xFu;
    if(((x->cfg[4 + n] >> 1) & 3u) == 2u) lo |= (uint64_t)x->cfg[5 + n] << 32;
    return lo;
}

/* Does [a, a+sz) touch any OTHER function's BAR, at its TRUE size, or a
   bridge's window? */
static int hits_anyone(const fn_t *self, uint64_t a, uint64_t sz)
{
    for(int i = 0; i < nf; i++){
        const fn_t *x = &F[i];
        if(x == self) continue;
        if(x->d.class == PCI_CLASS_BRIDGE){
            uint32_t mw = x->cfg[0x20 / 4];
            uint64_t mb = (uint64_t)(mw & 0xFFF0u) << 16, ml = ((uint64_t)(mw >> 16 & 0xFFF0u) << 16) | 0xFFFFF;
            if(ml > mb && a < ml + 1 && mb < a + sz) return 1;
            continue;
        }
        for(int n = 0; n < 6; n++){
            uint64_t b;
            if(!x->size[n]) continue;
            b = bar_of(x, n);
            if(b && a < b + x->size[n] && b < a + sz) return 1;
        }
    }
    return 0;
}

static int others_written(const fn_t *a, const fn_t *b)
{
    int w = 0;
    for(int i = 0; i < nf; i++) if(&F[i] != a && &F[i] != b) w += F[i].bar_writes;
    return w;
}

int main(void)
{
    printf("test_pci_assign -- a memory window for the HP's trackpad controllers\n");

    head("the planner: the HP's ranges");
    {
        pci_range_t r[PCI_ASSIGN_MAX_RANGES]; int n; uint64_t a;
        build_hp();
        n = pci_used_ranges(r, PCI_ASSIGN_MAX_RANGES, &I2C0->d);
        ck("every assigned memory BAR and the bridge window is a range", n == 13, n);
        a = pci_plan_window(r, n, 0x1000);
        ck("a window is found", a != 0, (long)(a >> 32));
        ck("  ... above 4 GB", a >= G(0x100000000), (long)(a >> 32));
        ck("  ... 4 KB aligned", (a & 0xFFF) == 0, (long)(a & 0xFFF));
        ck("  ... touching nobody, at anybody's TRUE size", !hits_anyone(I2C0, a, 0x1000), (long)a);
        ck("  ... within 64 MB of the xHCI, inside the firmware's window",
           a > G(0x6002120000) - 0x4000000 && a < G(0x6002120000) + 0x4000000, (long)(a - G(0x6000000000)));
        /* the display's register BAR is aligned to 128 GB: bounded only by
           its alignment it would cover the whole PCH, and the window would
           land a gigabyte past anything the firmware decodes */
        ck("  ... and NOT past the display's 1 GB bound (the trim works)",
           a < G(0x6040000000), (long)(a >> 20));
        /* the closest free gap to what it follows: just past the SMBus
           controller, inside the PCH cluster -- not 1 GB past the
           display's aperture, which is the LOWEST free address */
        ck("  ... in the PCH cluster's first free gap", a == G(0x600213C000), (long)(a - G(0x6000000000)));
    }

    head("the planner: what bounds a BAR it never sized");
    ck("a base's lowest set bit bounds its size", pci_bar_bound(G(0x6002120000)) == 0x20000,
       (long)pci_bar_bound(G(0x6002120000)));
    ck("  ... capped at 1 GB", pci_bar_bound(G(0x6000000000)) == 0x40000000,
       (long)pci_bar_bound(G(0x6000000000)));
    {
        pci_range_t r[3] = {
            { G(0x6000000000), G(0x6040000000), 1 },     /* the display, bounded */
            { G(0x6002120000), G(0x6002140000), 1 },
            { G(0x6002140000), G(0x6002141000), 1 },
        };
        pci_trim_ranges(r, 3);
        ck("a bound stops at the next BAR's base", r[0].end == G(0x6002120000), (long)(r[0].end >> 20));
        ck("  ... and a bound already short of it is untouched", r[1].end == G(0x6002140000), (long)r[1].end);
    }
    {
        pci_range_t r[1] = { { 0xFE000000u, 0xFE100000u, 1 } };
        ck("nothing above 4 GB: no window (below 4 GB is never guessed)",
           pci_plan_window(r, 1, 0x1000) == 0, 0);
    }
    {
        pci_range_t r[1] = { { G(0x6002120000), G(0x6002130000), 1 } };
        ck("a size that is not a power of two is refused", pci_plan_window(r, 1, 0x1800) == 0, 0);
        ck("  ... and so is one over 16 MB", pci_plan_window(r, 1, 0x2000000) == 0, 0);
        ck("the window is placed right after the BAR",
           pci_plan_window(r, 1, 0x1000) == G(0x6002130000), (long)pci_plan_window(r, 1, 0x1000));
    }
    {
        pci_range_t r[2] = {
            { G(0x6002120000), G(0x6002130000), 1 },
            { G(0x6002130000), G(0x6002200000), 0 },     /* a bridge window right after */
        };
        uint64_t a = pci_plan_window(r, 2, 0x1000);
        ck("a bridge window is stepped over, never into", a >= G(0x6002200000), (long)a);
    }
    {
        /* the end byte: a range that reaches ONE byte into the candidate
           is still in the way */
        pci_range_t r[2] = {
            { G(0x6002120000), G(0x6002121000), 1 },
            { G(0x6002121000), G(0x6002121001), 1 },
        };
        uint64_t a = pci_plan_window(r, 2, 0x1000);
        ck("a range one byte into the window is still in the way", a == G(0x6002122000), (long)(a & 0xFFFFFF));
    }
    {
        /* a device behind a root port: its BAR is inside the bridge's
           window, and the window is exact -- trimming it at that BAR
           would hand the rest of it to the planner */
        pci_range_t r[3] = {
            { G(0x6002120000), G(0x6002130000), 1 },
            { G(0x6002130000), G(0x6002200000), 0 },
            { G(0x6002140000), G(0x6002180000), 1 },
        };
        uint64_t a;
        pci_trim_ranges(r, 3);
        ck("a bridge window is never trimmed at a BAR inside it", r[1].end == G(0x6002200000), (long)(r[1].end & 0xFFFFFF));
        a = pci_plan_window(r, 3, 0x1000);
        ck("  ... so the window is not placed behind the bridge",
           a && !(a >= G(0x6002130000) && a < G(0x6002200000)), (long)(a & 0xFFFFFF));
    }

    head("the write: both of the HP's controllers");
    {
        uint64_t a0 = 0, a1 = 0; int r0, r1;
        build_hp();
        r0 = pci_assign_mem_bar(&I2C0->d, 0, &a0);
        ck("00:15.0 is assigned", r0 == PA_OK, r0);
        ck("  ... where it says", bar_of(I2C0, 0) == a0 && a0, (long)a0);
        ck("  ... the table says so too", (I2C0->d.bar[0] & ~0xFu) == (uint32_t)a0 &&
           I2C0->d.bar[1] == (uint32_t)(a0 >> 32), (long)I2C0->d.bar[1]);
        ck("  ... the 64-bit type bits are kept", ((I2C0->cfg[4] >> 1) & 3u) == 2u, (long)I2C0->cfg[4]);
        ck("  ... memory decoding is on after", (I2C0->cfg[1] & 2u) != 0, (long)I2C0->cfg[1]);
        ck("  ... and was OFF for every BAR write", I2C0->writes_decode_on == 0, I2C0->writes_decode_on);
        ck("  ... touching nobody", !hits_anyone(I2C0, a0, 0x1000), (long)a0);
        r1 = pci_assign_mem_bar(&I2C1->d, 0, &a1);
        ck("00:15.1 is assigned too", r1 == PA_OK, r1);
        ck("  ... a different window, not on top of 00:15.0's", a1 != a0 && !hits_anyone(I2C1, a1, 0x1000),
           (long)(a1 - a0));
        ck("no other device's BAR was written, ever", others_written(I2C0, I2C1) == 0,
           others_written(I2C0, I2C1));
        ck("BAR2 of each was left alone (only what was asked)",
           bar_of(I2C0, 2) == 0 && bar_of(I2C1, 2) == 0, (long)bar_of(I2C0, 2));
        ck("asked again, a BAR that has a window is not moved",
           pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_ASSIGNED && bar_of(I2C0, 0) == a0, 0);
    }

    head("the refusals, each named");
    {
        build_hp();
        I2C0->cfg[4] = 0; I2C0->d.bar[0] = 0;                       /* a 32-bit BAR */
        I2C0->bar_writes = 0;
        ck("a 32-bit BAR is refused", pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_32BIT, 0);
        ck("  ... before anything is written", I2C0->bar_writes == 0, I2C0->bar_writes);
        ck("  ... and the reason says why", strstr(pci_assign_why(PA_32BIT), "32-bit") != 0, 0);

        build_hp();
        I2C0->size[0] = 0;                                          /* reads back zero */
        I2C0->cfg[1] = 0x0004u;
        ck("a BAR that reports no size is refused", pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_SIZE, 0);
        ck("  ... the command register is as it was", I2C0->cfg[1] == 0x0004u, (long)I2C0->cfg[1]);

        build_hp();
        nf = 0;                                                     /* QEMU-like: nothing above 4 GB */
        { fn_t *x = mkfn(0, 3, 0, PCI_CLASS_SERIAL, 0x80); bar(x, 0, 0, 0x1000, 1); I2C0 = x;
          x = mkfn(0, 4, 0, 0x01, 0x06); bar(x, 5, 0xFEBF0000u, 0x1000, 0); }
        ck("no BAR above 4 GB to follow: refused", pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_NO_ROOM, 0);
        ck("  ... the BAR is still zero", bar_of(I2C0, 0) == 0, (long)bar_of(I2C0, 0));

        build_hp();
        I2C0->stuck = 1;
        ck("an address that does not stick is refused", pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_STUCK, 0);
        ck("  ... the BAR is put back to zero", bar_of(I2C0, 0) == 0, (long)bar_of(I2C0, 0));
        ck("  ... decoding left OFF", (I2C0->cfg[1] & 2u) == 0, (long)I2C0->cfg[1]);
        ck("  ... and the table was not told otherwise", set_bar_calls == 0, set_bar_calls);

        ck("an I/O BAR is not this file's", (I2C0->cfg[4] = 0x2001u, pci_assign_mem_bar(&I2C0->d, 0, 0) == PA_NOT_MEM), 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
