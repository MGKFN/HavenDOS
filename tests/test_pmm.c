/* Compile the REAL kernel/pmm.c natively and attack the allocator.
   A bug here hands out memory that belongs to the kernel, so it is worth
   testing directly rather than inferring from a successful boot. */
#include <stdio.h>
#include <stdint.h>
/* NOT <string.h>: the OS include/string.h that pmm.c pulls in declares
   strncat with a uint32_t length, which conflicts with glibc's size_t one.
   pmm.c only needs memset, which the OS header declares compatibly. */

/* pmm.c pulls these in; provide the minimum it uses. */
#define PMM_TEST_BUILD 1
typedef unsigned int   uint32_t_k;

/* Stand in for the linker symbol. 0x3D0948 is where the real v0.7.18 image
   ends; the test moves it about to prove the reserve tracks it. */
uint8_t _kernel_end_storage[1];
extern uint8_t _kernel_end;

static uint32_t fake_kernel_end = 0x3D0948;

/* Use pmm.c's test seam rather than macro-replacing the extern declaration. */
#define PMM_KERNEL_END_ADDR (fake_kernel_end)

#include "../kernel/pmm.c"

static int fails = 0, total = 0;
static void ck(const char *w, int c, long got)
{
    total++;
    if (c) printf("  ok    %s\n", w);
    else { fails++; printf("  FAIL  %s  (got %ld)\n", w, got); }
}

#define BLK 4096

int main(void)
{
    printf("pmm allocator tests\n===================\n\n");

    printf("reserve tracks the kernel image\n");
    fake_kernel_end = 0x3D0948;            /* 3.81 MB, the real v0.7.18 */
    pmm_init(2047u * 1024 * 1024);
    uint32_t r1 = pmm_reserved_blocks();
    ck("3.81MB image reserves at least that much",
       (uint64_t)r1 * BLK >= 0x3D0948, (long)((uint64_t)r1 * BLK));

    fake_kernel_end = 0x650968;            /* 6.31 MB, the v0.7.17 image */
    pmm_init(2047u * 1024 * 1024);
    uint32_t r2 = pmm_reserved_blocks();
    ck("6.31MB image reserves MORE than the old fixed 4MB",
       (uint64_t)r2 * BLK > 4u * 1024 * 1024, (long)((uint64_t)r2 * BLK));
    ck("the old hard-coded 1024 blocks would NOT have covered it",
       1024u * BLK < 0x650968, 1024L * BLK);

    printf("\nno allocation may land inside the kernel image\n");
    fake_kernel_end = 0x650968;
    pmm_init(2047u * 1024 * 1024);
    int bad = 0;
    for (int i = 0; i < 400; i++) {
        void *p = pmm_alloc();
        if (!p) break;
        if ((uint32_t)(uintptr_t)p < 0x650968) bad++;
    }
    ck("400 single-page allocations all sit above the image", bad == 0, bad);

    printf("\ncontiguous runs\n");
    pmm_init(64u * 1024 * 1024);
    void *a = pmm_alloc_contig(512);        /* 2 MB, the update buffer */
    ck("2MB contiguous run succeeds", a != NULL, (long)(uintptr_t)a);
    ck("it is page aligned", a && ((uint32_t)(uintptr_t)a % BLK) == 0,
       (long)((uint32_t)(uintptr_t)a % BLK));
    ck("it sits above the reserved region",
       a && (uint32_t)(uintptr_t)a >= pmm_reserved_blocks() * BLK,
       (long)(uintptr_t)a);

    void *b = pmm_alloc_contig(128);        /* 512 KB, the HTTP buffer */
    ck("512KB contiguous run succeeds alongside it", b != NULL, (long)(uintptr_t)b);
    uint32_t ab = (uint32_t)(uintptr_t)a, bb = (uint32_t)(uintptr_t)b;
    ck("the two runs do not overlap",
       (bb >= ab + 512 * BLK) || (ab >= bb + 128 * BLK), (long)(bb - ab));

    printf("\nfree then reuse\n");
    uint32_t before = pmm_free_blocks();
    pmm_free_contig(a, 512);
    ck("freeing returns every page", pmm_free_blocks() == before + 512,
       (long)(pmm_free_blocks() - before));
    void *c = pmm_alloc_contig(512);
    ck("the run can be handed out again", c != NULL, (long)(uintptr_t)c);
    ck("and it reuses the same span", c == a, (long)(uintptr_t)c);

    printf("\nrefusal instead of corruption\n");
    pmm_init(8u * 1024 * 1024);             /* barely more than the reserve */
    void *huge = pmm_alloc_contig(4096);    /* 16 MB from an 8 MB machine */
    ck("an impossible run returns NULL rather than wrapping", huge == NULL,
       (long)(uintptr_t)huge);
    ck("a zero-length run returns NULL", pmm_alloc_contig(0) == NULL, 0);
    uint32_t fb = pmm_free_blocks();
    pmm_free_contig(NULL, 8);
    ck("freeing NULL changes nothing", pmm_free_blocks() == fb, 0);

    printf("\nfragmentation is handled\n");
    pmm_init(64u * 1024 * 1024);
    void *p1 = pmm_alloc_contig(4);
    void *p2 = pmm_alloc_contig(4);
    void *p3 = pmm_alloc_contig(4);
    (void)p1; (void)p3;
    pmm_free_contig(p2, 4);                 /* punch a 4-block hole */
    void *big = pmm_alloc_contig(64);       /* must skip the hole */
    ck("a run larger than the hole is placed past it", big != NULL,
       (long)(uintptr_t)big);
    void *fit = pmm_alloc_contig(4);        /* must reuse the hole */
    ck("a run that fits the hole reuses it", fit == p2, (long)(uintptr_t)fit);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
