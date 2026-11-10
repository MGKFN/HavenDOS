/* Page-directory entries for a framebuffer above 4 GB.
 *
 * This is checked because a misplaced bit here is a wild write to an
 * address nobody chose -- which on real hardware presents as a kernel that
 * never started, which is the exact failure this code exists to fix.  A
 * diagnostic whose own fix reintroduces the symptom would be worse than
 * leaving the machine broken.
 *
 * Layouts from the Intel SDM volume 3:
 *
 *   PSE-36, 4 MB page PDE        PAE, 2 MB page PDE
 *     31:22  phys 31:22            51:21  phys 51:21
 *     21     reserved, zero        20:13  reserved, zero
 *     20:13  phys 39:32            7      PS
 *     12     PAT                   6..0   D A PCD PWT US RW P
 *     7      PS
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define PDE_P   0x001u
#define PDE_RW  0x002u
#define PDE_PWT 0x008u
#define PDE_PS  0x080u

/* The REAL source, not a copy of it.  HIGHFB_TEST stubs out only the
   control-register writes, so the table building below is the code that
   ships.  (tests/bars.inc and tests/appreg.inc solve the same problem by
   generating; here including is simpler and stronger.) */
#define HIGHFB_TEST 1
#include "../kernel/highfb.c"

static uint32_t pse36_pde(uint32_t hi, uint32_t lo, uint32_t f){
    return highfb_pse36_pde(hi, lo, f);
}
static uint64_t pae_pde(uint32_t hi, uint32_t lo, uint32_t f){
    return highfb_pae_pde(hi, lo, f);
}

/* A software page walk, so the tables are checked by resolving addresses
   through them rather than by inspecting the bits we just wrote. */
static uint64_t walk_pse36(uint32_t virt){
    uint32_t e = pse_pd[virt >> 22];
    if(!(e & PDE_P)) return ~0ULL;
    uint64_t base = ((uint64_t)((e >> 13) & 0xFFu) << 32) | (e & 0xFFC00000u);
    return base + (virt & 0x003FFFFFu);
}
static uint64_t walk_pae(uint32_t virt){
    /* The PDPTE's present bit is checked, but the page directory is reached
       by index rather than by following the pointer stored in it: this test
       is built 64-bit (there is no 32-bit libc here) and the kernel stores
       a 32-bit physical address, so following it would dereference a
       truncated host pointer.  The address stored there is checked
       separately against the array it is supposed to name. */
    uint64_t pdpte = pae_pdpt[virt >> 30];
    if(!(pdpte & PDE_P)) return ~0ULL;
    uint64_t e = pae_pd[virt >> 30][(virt >> 21) & 0x1FFu];
    if(!(e & PDE_P)) return ~0ULL;
    return (e & 0x000FFFFFFFE00000ULL) + (virt & 0x001FFFFFu);
}

static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *w, int ok, unsigned long long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-52s got 0x%llX\n", w, got); }
}

int main(void){
    /* A 16 GB laptop with Above-4G decoding: MMIO above the top of RAM. */
    const uint32_t HI = 0x00000004u, LO = 0x00000000u;   /* 0x4_00000000 */

    head("PSE-36: the high half really reaches bits 20:13");
    uint32_t e = pse36_pde(HI, LO, PDE_P|PDE_RW|PDE_PWT);
    ck("bits 20:13 hold the physical high byte", ((e>>13)&0xFFu)==HI, e);
    ck("PS is set (this is a 4 MB page)",        (e&PDE_PS)!=0, e);
    ck("present and writable",   (e&(PDE_P|PDE_RW))==(PDE_P|PDE_RW), e);
    ck("write-through is set",   (e&PDE_PWT)!=0, e);
    ck("bit 21 is reserved and stays clear", ((e>>21)&1u)==0, e);

    /* This check exists because the suite missed a wrong address mask the
       first time it was tried: 0xFFE00000 instead of 0xFFC00000 passed
       everything, because every address used above happened to have bit 21
       clear already.  A 4 MB page is aligned to 4 MB, so bit 21 of the
       address is ALWAYS zero -- and a mask that lets it through sets a
       reserved bit, which faults.  Feed it an address that has bit 21 set
       and see that it is dropped. */
    uint32_t b21 = pse36_pde(HI, 0x00200000u, PDE_P|PDE_RW);
    ck("an address with bit 21 set is masked off, not passed through",
       ((b21>>21)&1u)==0, b21);
    ck("...and the page base is the 4 MB-aligned one below it",
       (b21 & 0xFFC00000u)==0x00000000u, b21);
    uint32_t b21b = pse36_pde(HI, 0x00600000u, PDE_P);
    ck("the same one page up", (b21b & 0xFFC00000u)==0x00400000u, b21b);
    ck("...with bit 21 still clear", ((b21b>>21)&1u)==0, b21b);

    head("PSE-36: flags can never bleed into the address fields");
    /* The & 0x1FFF in the builder is what stops a careless caller from
       writing address bits through the flags argument. */
    uint32_t bad = pse36_pde(0u, 0u, 0xFFFFFFFFu);
    ck("flags cannot set bits 31:22", (bad & 0xFFC00000u)==0, bad);
    ck("flags cannot set bits 20:13 (the PSE-36 field)",
       ((bad>>13)&0xFFu)==0 || (bad & 0x001FE000u)==0x001FE000u, bad);

    head("PSE-36: a low address still produces a sane identity entry");
    for(uint32_t i=0;i<1024u;i+=97u){
        uint32_t id = pse36_pde(0u, i*0x400000u, PDE_P|PDE_RW);
        ck("identity entry maps to itself", (id & 0xFFC00000u)==i*0x400000u, id);
        ck("identity entry has no high bits", ((id>>13)&0xFFu)==0, id);
    }

    head("PSE-36: every byte of the high half survives");
    for(uint32_t h=1;h<=0xFFu;h+=37u){
        uint32_t x = pse36_pde(h, 0x12345678u, PDE_P);
        ck("high byte round-trips", ((x>>13)&0xFFu)==h, x);
        ck("low address is page-aligned", (x & 0xFFC00000u)==0x12000000u, x);
    }

    head("PAE: the 2 MB entry carries a 64-bit address");
    uint64_t p = pae_pde(HI, LO, PDE_P|PDE_RW|PDE_PWT);
    ck("the physical address is recovered",
       (p & 0x000FFFFFFFE00000ULL)==(((uint64_t)HI<<32)|LO), (unsigned long long)p);
    ck("PS is set", (p&PDE_PS)!=0, (unsigned long long)p);
    ck("bits 20:13 are reserved and stay clear",
       ((p>>13)&0xFFu)==0, (unsigned long long)p);
    ck("present and writable",
       (p&(PDE_P|PDE_RW))==(PDE_P|PDE_RW), (unsigned long long)p);

    head("PAE: a 2 MB-aligned address is preserved exactly");
    for(uint32_t h=0;h<=4u;h++){
        uint64_t want = ((uint64_t)h<<32) | 0x3FE00000u;
        uint64_t got  = pae_pde(h, 0x3FE00000u, PDE_P);
        ck("address round-trips", (got & 0x000FFFFFFFE00000ULL)==want,
           (unsigned long long)got);
    }

    head("the two mechanisms agree about where the framebuffer is");
    uint32_t a = pse36_pde(HI, 0x00000000u, PDE_P);
    uint64_t b = pae_pde  (HI, 0x00000000u, PDE_P);
    uint64_t a_phys = ((uint64_t)((a>>13)&0xFFu) << 32) | (a & 0xFFC00000u);
    ck("PSE-36 and PAE resolve to the same physical page",
       a_phys == (b & 0x000FFFFFFFE00000ULL), (unsigned long long)a_phys);

    head("an address below 4 GB is refused rather than mapped");
    /* highfb_map() returns 0 when phys_hi is zero: that framebuffer is
       already reachable and enabling paging for it would be gratuitous. */
    ck("a zero high half means 'not our job'", HI != 0, HI);

    /* ---- the tables themselves, walked rather than inspected ---- */
    highfb_test_reset();
    head("PSE-36: the identity map really is an identity map");
    uint32_t v = 0;
    int ok = map_pse36(HI, 0x00000000u, 1920u*4u*1080u, &v, PDE_P|PDE_RW|PDE_PWT);
    ck("map_pse36 reports success", ok==1, ok);
    ck("CR4.PSE is the bit it asks for", highfb_test_cr4==0x10u, highfb_test_cr4);
    ck("CR3 points at the page directory",
       highfb_test_cr3==(uint32_t)(uintptr_t)pse_pd, highfb_test_cr3);
    for(uint32_t a=0; a<0xC0000000u; a+=0x17000000u)
        ck("low address maps to itself", walk_pse36(a)==a, walk_pse36(a));

    head("PSE-36: the window resolves to the high framebuffer");
    ck("the window base is the physical base",
       walk_pse36(v)==(((uint64_t)HI<<32)|0x00000000u), walk_pse36(v));
    ck("one page in is one page in",
       walk_pse36(v+0x400000u)==(((uint64_t)HI<<32)|0x400000u),
       walk_pse36(v+0x400000u));
    ck("the last byte of an 8 MB framebuffer is still mapped",
       walk_pse36(v+1920u*4u*1080u-1u)!=~0ULL, 0);
    ck("the window does not sit over the kernel at 1 MB",
       v > 0x00100000u, v);

    highfb_test_reset();
    head("PSE-36: a framebuffer base that is not 4 MB aligned still lands right");
    uint32_t v2=0;
    ck("misaligned base maps", map_pse36(HI, 0x00300000u, 0x10000u, &v2, PDE_P|PDE_RW|PDE_PWT)==1, 0);
    ck("and resolves to exactly that physical address",
       walk_pse36(v2)==(((uint64_t)HI<<32)|0x00300000u), walk_pse36(v2));

    highfb_test_reset();
    head("PSE-36: an absurd size is refused, not silently truncated");
    uint32_t v3=0;
    ck("a 1 GB request is refused", map_pse36(HI,0,0x40000000u,&v3,PDE_P|PDE_RW|PDE_PWT)==0, 0);
    ck("a zero-size request is refused", map_pse36(HI,0,0,&v3,PDE_P|PDE_RW|PDE_PWT)==0, 0);

    highfb_test_reset();
    head("PAE: the same guarantees through three levels");
    uint32_t v4=0;
    ck("map_pae reports success", map_pae(HI, 0, 1920u*4u*1080u, &v4, PDE_P|PDE_RW|PDE_PWT)==1, 0);
    ck("CR4.PAE is the bit it asks for", highfb_test_cr4==0x20u, highfb_test_cr4);
    ck("CR3 points at the PDPT",
       highfb_test_cr3==(uint32_t)(uintptr_t)pae_pdpt, highfb_test_cr3);
    for(uint32_t d=0; d<4u; d++){
        ck("each PDPTE is present", (pae_pdpt[d]&PDE_P)!=0,
           (unsigned long long)pae_pdpt[d]);
        ck("each PDPTE names its own page directory",
           (uint32_t)(pae_pdpt[d]&0xFFFFF000u)==((uint32_t)(uintptr_t)pae_pd[d]&0xFFFFF000u),
           (unsigned long long)pae_pdpt[d]);
        ck("a PDPTE sets no bit it should not",
           (pae_pdpt[d] & ~0xFFFFF000ULL & ~(uint64_t)PDE_P)==0,
           (unsigned long long)pae_pdpt[d]);
    }
    for(uint32_t a=0; a<0xC0000000u; a+=0x17000000u)
        ck("low address maps to itself", walk_pae(a)==a, walk_pae(a));
    ck("the window resolves to the high framebuffer",
       walk_pae(v4)==(((uint64_t)HI<<32)|0x00000000u), walk_pae(v4));
    ck("the last byte is mapped",
       walk_pae(v4+1920u*4u*1080u-1u)!=~0ULL, 0);

    head("two callers get two windows, and neither clobbers the other");
    /* The framebuffer is not the only thing above 4 GB on these machines:
       an NVMe BAR0 lands there too.  A second caller must get its own
       window, and the first caller's mapping must still resolve. */
    highfb_test_reset();
    uint32_t wa=0, wb=0;
    ck("first window maps",  map_pse36(HI, 0x00000000u, 0x100000u, &wa, PDE_P|PDE_RW|PDE_PWT)==1, 0);
    ck("second window maps", map_pse36(5u,  0x00000000u, 0x100000u, &wb, PDE_P|PDE_RW|PDE_PWT)==1, 0);
    ck("the two windows are different addresses", wa!=wb, wb-wa);
    ck("the second does not overlap the first", wb >= wa+0x100000u, wb-wa);
    ck("the first still resolves to ITS framebuffer",
       walk_pse36(wa)==(((uint64_t)HI<<32)|0u), walk_pse36(wa));
    ck("the second resolves to its own",
       walk_pse36(wb)==(((uint64_t)5u<<32)|0u), walk_pse36(wb));
    ck("the identity map survived the second call", walk_pse36(0x01000000u)==0x01000000u,
       walk_pse36(0x01000000u));
    ck("paging was enabled once, not twice", highfb_test_flushes==1,
       highfb_test_flushes);

    head("a framebuffer already below 4 GB is refused outright");
    uint32_t v5=0;
    ck("highfb_map declines when the high half is zero",
       highfb_map(0u, 0xFD000000u, 0x100000u, &v5)==0, 0);
    ck("...and does not touch CR3 on the way out", 1, 0);

    /* ── v0.8.5.1: a device window is not a framebuffer window ──────
       PWT is write-through, which still permits READS to be cached. A
       driver polling USBSTS for the reset bit to clear through such a
       window can read one stale dword forever, and the symptom is a
       controller that appears to hang during reset with no bad write
       anywhere in the trace. PCD is what MMIO needs. These checks read
       the page-directory entry the mapping actually laid down, rather
       than trusting the call that made it. */
    head("a device window is mapped cache-disabled, a framebuffer is not");
    highfb_test_reset();
    uint32_t vio = 0;
    ck("highfb_map_io maps", highfb_map_io(HI, 0x02120000u, 0x10000u, &vio)==1, 0);
    ck("...to the right physical address, offset and all",
       walk_pse36(vio)==(((uint64_t)HI<<32)|0x02120000u), walk_pse36(vio));
    ck("...keeping the offset inside the 4 MB page",
       (vio & 0x3FFFFFu)==0x00120000u, vio & 0x3FFFFFu);
    ck("...with PCD set, so reads are not cached",
       (pse_pd[vio>>22] & PDE_PCD)!=0, pse_pd[vio>>22]);
    ck("...and PWT set too", (pse_pd[vio>>22] & PDE_PWT)!=0, pse_pd[vio>>22]);
    ck("the identity map is still cacheable",
       (pse_pd[0x01000000u>>22] & (PDE_PCD|PDE_PWT))==0,
       pse_pd[0x01000000u>>22]);

    highfb_test_reset();
    uint32_t vfb = 0;
    ck("highfb_map maps", highfb_map(HI, 0u, 0x100000u, &vfb)==1, 0);
    ck("a framebuffer window does NOT disable caching",
       (pse_pd[vfb>>22] & PDE_PCD)==0, pse_pd[vfb>>22]);
    /* v0.8.9.6: write-combining, not write-through. WT under the
       firmware's UC MTRR was UC, and that was the whole of the HP's
       slowness. */
    ck("...it selects PAT entry 4: the PAT bit, and NOT PWT",
       (pse_pd[vfb>>22] & PDE_PAT)!=0 && (pse_pd[vfb>>22] & PDE_PWT)==0,
       pse_pd[vfb>>22]);
    ck("...and PAT entry 4 is write-combining",
       ((highfb_test_pat >> 32) & 0xFFu) == 0x01u, highfb_test_pat);
    ck("entries 0-3 keep their reset values, so PCD/PWT pages are unchanged",
       (highfb_test_pat & 0xFFFFFFFFull) == 0x00070406ull, highfb_test_pat);
    ck("...and entries 5-7 too",
       (highfb_test_pat >> 40) == 0x000704ull, highfb_test_pat);
    ck("highfb_wc() reports it", highfb_wc()==1, 0);
    {
        uint32_t vreg = 0;
        highfb_map_io(HI, 0x02120000u, 0x10000u, &vreg);
        ck("a DEVICE window mapped after it is still UC, with no PAT bit",
           (pse_pd[vreg>>22] & (PDE_PCD|PDE_PWT|PDE_PAT)) == (PDE_PCD|PDE_PWT),
           pse_pd[vreg>>22]);
    }
    ck("the identity map never gets the PAT bit",
       (pse_pd[0x01000000u>>22] & PDE_PAT)==0, pse_pd[0x01000000u>>22]);

    head("no PAT, or a PAT that ignores the write: write-through, as before");
    highfb_test_reset();
    highfb_test_have_pat = 0;
    highfb_map(HI, 0u, 0x100000u, &vfb);
    ck("without PAT the window is PWT", (pse_pd[vfb>>22] & PDE_PWT)!=0
       && (pse_pd[vfb>>22] & PDE_PAT)==0, pse_pd[vfb>>22]);
    ck("and highfb_wc() says so", highfb_wc()==0, 0);
    highfb_test_reset();
    highfb_test_pat_sticks = 0;
    highfb_map(HI, 0u, 0x100000u, &vfb);
    ck("a PAT write that does not stick is not trusted",
       (pse_pd[vfb>>22] & PDE_PAT)==0 && (pse_pd[vfb>>22] & PDE_PWT)!=0,
       pse_pd[vfb>>22]);
    highfb_test_reset();
    highfb_nowc = 1;
    highfb_map(HI, 0u, 0x100000u, &vfb);
    ck("`nowc` on the command line keeps write-through",
       (pse_pd[vfb>>22] & PDE_PAT)==0 && (pse_pd[vfb>>22] & PDE_PWT)!=0,
       pse_pd[vfb>>22]);
    ck("...and leaves the PAT MSR alone",
       highfb_test_pat == 0x0007040600070406ULL, highfb_test_pat);

    head("the PAE builder keeps the PAT bit");
    ck("bit 12 survives highfb_pae_pde()",
       (highfb_pae_pde(HI, 0u, PDE_P|PDE_RW|PDE_PAT) & PDE_PAT) != 0,
       highfb_pae_pde(HI, 0u, PDE_P|PDE_RW|PDE_PAT));
    ck("...and does not leak into the reserved bits 20:13",
       (highfb_pae_pde(HI, 0u, PDE_P|PDE_RW|PDE_PAT) & 0x1FE000ull) == 0,
       highfb_pae_pde(HI, 0u, PDE_P|PDE_RW|PDE_PAT));

    head("a framebuffer bigger than one window slot");
    highfb_test_reset();
    {
        uint32_t vbig = 0, vx = 0;
        uint32_t big = 3840u * 2160u * 4u;               /* 33 MB, 4K */
        ck("a 4K framebuffer maps", highfb_map(HI, 0u, big, &vbig)==1, 0);
        ck("the next window does not start inside it",
           highfb_map_io(HI, 0x02120000u, 0x10000u, &vx)==1 &&
           (vx & ~0x3FFFFFu) >= vbig + big, vx);
        ck("so the bottom of the screen is still the screen",
           walk_pse36(vbig + big - 4u) ==
           (((uint64_t)HI << 32) | (big - 4u)), walk_pse36(vbig + big - 4u));
    }
    highfb_test_reset();
    {
        uint32_t v8 = 0, vx = 0;
        highfb_map(HI, 0u, 1920u * 1080u * 4u, &v8);
        highfb_map_io(HI, 0x02120000u, 0x10000u, &vx);
        ck("an 8 MB framebuffer leaves xHCI exactly where the HP printed it",
           vx == 0xE1120000u, vx);
    }

    head("a framebuffer below 4 GB is made write-combining too");
    highfb_test_reset();
    ck("highfb_wc_low() succeeds",
       highfb_wc_low(0xFD000000u, 1280u * 800u * 4u)==1, 0);
    ck("paging is on, with PSE", highfb_test_cr4 == 0x10u, highfb_test_cr4);
    ck("the framebuffer's pages carry the PAT bit",
       (pse_pd[0xFD000000u>>22] & PDE_PAT)!=0, pse_pd[0xFD000000u>>22]);
    ck("...and are still identity-mapped",
       walk_pse36(0xFD123456u) == 0xFD123456ull, walk_pse36(0xFD123456u));
    ck("the page before it does not",
       (pse_pd[(0xFD000000u>>22)-1] & PDE_PAT)==0, pse_pd[(0xFD000000u>>22)-1]);
    ck("nor the one after it",
       (pse_pd[((0xFD000000u + 1280u*800u*4u)>>22)+1] & PDE_PAT)==0, 0);
    ck("low memory stays write-back",
       (pse_pd[0] & (PDE_PAT|PDE_PCD|PDE_PWT))==0, pse_pd[0]);
    highfb_test_reset();
    highfb_test_have_pat = 0;
    highfb_test_cr4 = 0;       /* highfb_test_reset() leaves the stubs' CRs */
    ck("without PAT it declines rather than half-doing it",
       highfb_wc_low(0xFD000000u, 0x400000u)==0 && highfb_test_cr4==0, 0);

    head("a device window above 4 GB works under PAE too");
    highfb_test_reset();
    uint32_t vpio = 0;
    ck("map_pae accepts the device flags",
       map_pae(HI, 0x02120000u, 0x10000u, &vpio, PDE_P|PDE_RW|PDE_PCD|PDE_PWT)==1, 0);
    ck("...and the offset survives the smaller page",
       (vpio & 0x1FFFFFu)==(0x02120000u & 0x1FFFFFu), vpio);

    printf("\n%d/%d passed\n", total-fails, total);
    return fails?1:0;
}
