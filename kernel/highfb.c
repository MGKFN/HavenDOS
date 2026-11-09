/* ================================================================
   kernel/highfb.c -- mapping a framebuffer that sits above 4 GB
   ================================================================
   See include/highfb.h for why this exists.  Everything here runs
   before idt_init(), so there is no handler for anything that goes
   wrong: the code is written to check first and act second.
   ================================================================ */

#include "types.h"
#include "io.h"
#include "highfb.h"

/* The tables and the two map routines are file-static in the kernel and
   visible to the test, which includes this file.  Nothing else changes. */
#ifdef HIGHFB_TEST
#define HIGHFB_STATIC
#else
#define HIGHFB_STATIC static
#endif

/* Page-directory flags.  PWT (write-through) rather than plain
   write-back: a framebuffer is memory-mapped I/O, and letting writes
   sit in a cache line means pixels reach the panel whenever the line
   happens to be evicted.  Write-combining would be better still, but
   that needs MTRRs or PAT and this is a diagnostic path. */
#define PDE_P     0x001u
#define PDE_RW    0x002u
#define PDE_PWT   0x008u
#define PDE_PCD   0x010u      /* cache disable -- see highfb_map_io */
#define PDE_PS    0x080u      /* this entry is a large page */

/* ---------------------------------------------------------------
   Entry builders.  Kept as pure functions so they can be tested.

   PSE-36, 4 MB page (Intel SDM vol 3, "Page-Directory Entry that
   Maps a 4-MByte Page"):
       31:22  physical address bits 31:22
       21     reserved, must be zero
       20:13  physical address bits 39:32     <- the PSE-36 extension
       12     PAT
       7      PS, must be 1
       ...    the usual P/RW/US/PWT/PCD/A/D bits below

   PAE, 2 MB page:
       51:21  physical address bits 51:21
       20:13  reserved, must be zero
       7      PS
   --------------------------------------------------------------- */
uint32_t highfb_pse36_pde(uint32_t phys_hi, uint32_t phys_lo, uint32_t flags)
{
    return (phys_lo & 0xFFC00000u)            /* bits 31:22 */
         | ((phys_hi & 0xFFu) << 13)          /* bits 39:32 into 20:13 */
         | (flags & 0x1FFFu)                  /* never into 20:13 or above */
         | PDE_PS;
}

uint64_t highfb_pae_pde(uint32_t phys_hi, uint32_t phys_lo, uint32_t flags)
{
    uint64_t phys = ((uint64_t)phys_hi << 32) | phys_lo;
    return (phys & 0x000FFFFFFFE00000ULL)     /* bits 51:21 */
         | (uint64_t)(flags & 0x0FFFu)
         | PDE_PS;
}

/* ---------------------------------------------------------------
   Tables.  Static, because there is no allocator yet at this point
   in the boot and because they must stay alive for the life of the
   machine.  4 KB alignment is architectural; the PAE PDPT needs 32.
   --------------------------------------------------------------- */
HIGHFB_STATIC uint32_t pse_pd[1024]      __attribute__((aligned(4096)));
HIGHFB_STATIC uint64_t pae_pd[4][512]    __attribute__((aligned(4096)));
HIGHFB_STATIC uint64_t pae_pdpt[4]       __attribute__((aligned(32)));

static int g_mech = 0;
int highfb_mechanism(void){ return g_mech; }

/* The virtual window.  3.5 GB: on any machine with 4 GB or more of
   RAM this lies inside the PCI MMIO hole rather than over real
   memory, and at the point this runs the PMM has not been
   initialised, so nothing has been handed out from anywhere. */
/* The virtual windows.  3.5 GB upward: on any machine with 4 GB or more of
   RAM this lies inside the PCI MMIO hole rather than over real memory, and
   at the point this runs the PMM has not been initialised, so nothing has
   been handed out from anywhere.

   There is more than one window because the framebuffer is not the only
   thing a firmware puts above 4 GB -- an NVMe controller's BAR0 lands there
   on exactly the same machines, for exactly the same reason.  Each
   successful call takes the next slot, so a second caller cannot silently
   overwrite the first one's mapping. */
#define WINDOW_BASE   0xE0000000u
#define WINDOW_STRIDE 0x01000000u     /* 16 MB apart */
#define WINDOW_MAX    8
HIGHFB_STATIC uint32_t g_window_next = 0;
#define WINDOW_VIRT   (WINDOW_BASE + g_window_next * WINDOW_STRIDE)
#define PSE_PAGE      0x00400000u     /* 4 MB */
#define PAE_PAGE      0x00200000u     /* 2 MB */

static void cpuid1(uint32_t *edx)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                             : "a"(1u));
    (void)a; (void)b; (void)c;
    *edx = d;
}

static int have_pse36(void){ uint32_t d; cpuid1(&d); return (d >> 17) & 1u; }
static int have_pae  (void){ uint32_t d; cpuid1(&d); return (d >>  6) & 1u; }

/* Paging is switched on with the kernel identity-mapped, so the
   instruction after the write to CR0 is at the same address it was
   before.  The jump is there to flush the prefetch queue.
 *
 * tests/test_highfb.c includes THIS FILE and defines HIGHFB_TEST, so the
 * table-building below is exercised for real rather than against a copy of
 * itself -- the control-register writes are all that gets stubbed out.  A
 * test carrying its own copy of the thing it tests stops testing it the
 * moment the two drift. */
#ifdef HIGHFB_TEST
uint32_t highfb_test_cr3, highfb_test_cr4;
int      highfb_test_flushes;
static void tlb_flush(void){ highfb_test_flushes++; }
/* Lets one test process run several independent scenarios. */
void highfb_test_reset(void);
static void enable_paging(uint32_t cr3, uint32_t cr4_bit)
{
    highfb_test_cr3 = cr3;
    highfb_test_cr4 = cr4_bit;
}
#else
/* Reloading CR3 is the portable way to drop every non-global TLB entry; the
   new window's pages have no entries yet, but a stale negative entry for the
   same virtual address would otherwise survive. */
static void tlb_flush(void)
{
    __asm__ volatile("mov %%cr3, %%eax\n\tmov %%eax, %%cr3"
                     ::: "eax", "memory");
}
static void enable_paging(uint32_t cr3, uint32_t cr4_bit)
{
    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "mov %%cr4, %%eax\n\t"
        "or  %1, %%eax\n\t"
        "mov %%eax, %%cr4\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        "jmp 1f\n1:\n\t"
        :: "r"(cr3), "r"(cr4_bit) : "eax", "memory");
}
#endif

/* One flag per mechanism.  Only one is ever used on a given machine --
   PSE-36 is preferred and PAE is the fallback -- but sharing a single flag
   would mean whichever ran first silently suppressed the other's table
   build, which is the kind of thing that works until the day a CPU without
   PSE-36 turns up. */
HIGHFB_STATIC int g_pse_built = 0;
HIGHFB_STATIC int g_pae_built = 0;

HIGHFB_STATIC int map_pse36(uint32_t hi, uint32_t lo, uint32_t bytes,
                     uint32_t *virt_out, uint32_t extra)
{
    /* Identity-map the whole low 4 GB, so every address the rest of
       the kernel already uses keeps meaning what it meant.  Built once:
       a second caller is adding a window, not starting over, and
       rebuilding would wipe the window the first caller is using. */
    if(!g_pse_built)
        for(uint32_t i = 0; i < 1024u; i++)
            pse_pd[i] = highfb_pse36_pde(0u, i * PSE_PAGE, PDE_P | PDE_RW);

    /* Then redirect the window.  The framebuffer base is rounded down
       to a page boundary and the offset handed back to the caller, so
       a base that is not 4 MB aligned still lands correctly. */
    uint32_t off   = lo & (PSE_PAGE - 1u);
    uint32_t base  = lo & ~(PSE_PAGE - 1u);
    uint32_t pages = (off + bytes + PSE_PAGE - 1u) / PSE_PAGE;
    if(pages == 0u || pages > 64u) return 0;          /* 256 MB ceiling */

    for(uint32_t p = 0; p < pages; p++){
        uint32_t v    = WINDOW_VIRT + p * PSE_PAGE;
        uint32_t phys = base + p * PSE_PAGE;
        uint32_t carry_hi = hi + (phys < base ? 1u : 0u);   /* 32-bit wrap */
        pse_pd[v >> 22] =
            highfb_pse36_pde(carry_hi, phys, PDE_P | PDE_RW | extra);
    }

    if(!g_pse_built){
        enable_paging((uint32_t)pse_pd, 0x00000010u);  /* CR4.PSE */
        g_pse_built = 1;
    } else {
        tlb_flush();   /* paging is already on; the new entries need telling */
    }
    *virt_out = WINDOW_VIRT + off;
    g_window_next++;
    g_mech = 1;
    return 1;
}

HIGHFB_STATIC int map_pae(uint32_t hi, uint32_t lo, uint32_t bytes,
                   uint32_t *virt_out, uint32_t extra)
{
    if(!g_pae_built)
    for(uint32_t d = 0; d < 4u; d++){
        for(uint32_t e = 0; e < 512u; e++){
            uint64_t phys = ((uint64_t)d << 30) | ((uint64_t)e << 21);
            pae_pd[d][e] = highfb_pae_pde((uint32_t)(phys >> 32),
                                          (uint32_t)phys,
                                          PDE_P | PDE_RW);
        }
        pae_pdpt[d] = ((uint32_t)pae_pd[d]) | PDE_P;   /* PDPTE: P only */
    }

    uint32_t off   = lo & (PAE_PAGE - 1u);
    uint32_t base  = lo & ~(PAE_PAGE - 1u);
    uint32_t pages = (off + bytes + PAE_PAGE - 1u) / PAE_PAGE;
    if(pages == 0u || pages > 128u) return 0;          /* 256 MB ceiling */

    for(uint32_t p = 0; p < pages; p++){
        uint32_t v    = WINDOW_VIRT + p * PAE_PAGE;
        uint32_t phys = base + p * PAE_PAGE;
        uint32_t carry_hi = hi + (phys < base ? 1u : 0u);
        pae_pd[v >> 30][(v >> 21) & 0x1FFu] =
            highfb_pae_pde(carry_hi, phys, PDE_P | PDE_RW | extra);
    }

    if(!g_pae_built){
        enable_paging((uint32_t)pae_pdpt, 0x00000020u); /* CR4.PAE */
        g_pae_built = 1;
    } else {
        tlb_flush();
    }
    *virt_out = WINDOW_VIRT + off;
    g_window_next++;
    g_mech = 2;
    return 1;
}

#ifdef HIGHFB_TEST
void highfb_test_reset(void){
    g_window_next = 0; g_pse_built = 0; g_pae_built = 0;
    g_mech = 0; highfb_test_flushes = 0;
}
#endif

static int map_any(uint32_t hi, uint32_t lo, uint32_t bytes,
                   uint32_t *virt_out, uint32_t extra)
{
    if(!virt_out || !bytes) return 0;
    if(!hi)                 return 0;     /* already reachable; not our job */
    if(g_window_next >= WINDOW_MAX) return 0;

    if(have_pse36()) return map_pse36(hi, lo, bytes, virt_out, extra);
    if(have_pae())   return map_pae  (hi, lo, bytes, virt_out, extra);
    return 0;
}

int highfb_map(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes,
               uint32_t *virt_out)
{
    /* A framebuffer: write-through. Reads of it are rare and writes must
       reach the panel, which is what PWT buys. */
    return map_any(phys_hi, phys_lo, bytes, virt_out, PDE_PWT);
}

/* v0.8.5.1 -- DEVICE REGISTERS ARE NOT A FRAMEBUFFER.
   PWT is write-through: writes go to memory, and READS MAY STILL BE
   CACHED. For a framebuffer that is merely slow. For a controller
   register it is wrong in a way that is very hard to see: a driver
   polling USBSTS for the reset bit to clear can read the same cached
   dword forever, and the symptom is a controller that "hangs during
   reset" with no bad write anywhere to find.
   PCD disables caching for the window, which is what MMIO needs.

   Being straight about this one: on most real machines the firmware's
   MTRRs already mark the above-4G PCI range uncacheable, and the
   effective type is the stricter of the two, so PWT would probably have
   worked. "Probably, because the firmware happened to" is not a
   guarantee this tree is willing to rest a driver on, and the fix is one
   bit. drivers/nvme.c mapped its registers this way too and has the same
   latent problem; it is fixed by the same change. */
int highfb_map_io(uint32_t phys_hi, uint32_t phys_lo, uint32_t bytes,
                  uint32_t *virt_out)
{
    return map_any(phys_hi, phys_lo, bytes, virt_out, PDE_PCD | PDE_PWT);
}
