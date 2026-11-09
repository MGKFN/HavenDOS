/* ════════════════════════════════════════════════════════════════════
   test_heap.c -- the kernel allocator, against the pool.

   kernel/heap.c IS THE OLDEST UNTESTED FILE IN THE TREE. It carries a
   list of seven bug fixes in its own header -- double-free detection,
   magic validation, bounds checks, a krealloc() that was
   dereferencing p-12 before validating p -- and until v0.8.8.5 not
   one of them had a test. Every one was found by reading.

   It is also the file that decides how much memory this kernel can
   use at all, which is what brought this suite into being: the pool
   was a 256 KB static array on machines with eight gigabytes.

   ── WHAT THIS SUITE IS, AND WHAT IT IS NOT ──────────────────────────

   NOT a check that the allocator's arithmetic matches this file's
   arithmetic. The temptation in an allocator suite is to compute the
   expected block layout and compare -- which is the mistake
   tests/test_usbmux.c made at 0.8.7.6, where "a fixture written from
   the same guess as the code agrees with it perfectly".

   So the checks here are about PROPERTIES THAT MUST HOLD however the
   blocks are laid out:

     - memory handed out is WRITABLE and does not overlap anything
       else handed out. Checked by writing a distinct byte through
       every pointer and reading them all back afterwards.
     - a pointer survives being freed and reallocated around.
     - the refusals are refusals: a double free, a pointer outside the
       pool, a size that would overflow.
     - heap_check() agrees the list is intact after every phase.

   The overlap check is the one that matters. An allocator that
   returns two pointers into the same bytes passes any test that only
   looks at one of them at a time, and fails this one on the first
   read-back.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── the pool, and the PMM under it ──────────────────────────────────
   kernel/heap.c asks pmm_alloc_contig() for its arena. A native run
   has no physical memory map, so this is the whole of the simulated
   one: a real array, handed out in 4 KB blocks, with a knob to make
   it fail so the fallback path can be reached.

   A REAL ARRAY and not a stub returning a fixed address, because the
   allocator writes headers into whatever it is given and a native run
   must own that memory. */
/* 80 MB. Big enough that the 64 MB ceiling can actually be REACHED:
   with a 16 MB pool, a request for 64 MB and a request for 2 GB are
   both refused and fall back to the static array, so the ceiling and
   its absence looked identical. The injection harness planted its
   removal and the suite missed it. */
#define SIM_BLOCKS 20480u                    /* 80 MB */
static uint8_t  *sim_pool;
static uint32_t  sim_used;
static uint32_t  sim_free_blocks = SIM_BLOCKS;
static int       sim_refuse      = 0;

void *pmm_alloc_contig(uint32_t n)
{
    if(sim_refuse) return NULL;
    if(n == 0 || sim_used + n > SIM_BLOCKS) return NULL;
    void *p = sim_pool + (size_t)sim_used * 4096u;
    sim_used += n;
    return p;
}
uint32_t pmm_free_blocks(void){ return sim_free_blocks; }

/* errors.h's logger, which the allocator calls on every refusal. The
   count is how a refusal is told apart from a silent wrong answer. */
static int   log_n = 0;
static int   log_last = 0;
void kernel_log(int code, const char *msg){ (void)msg; log_n++; log_last = code; }

#include "../include/errors.h"
#include "../kernel/heap.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %s%*sgot %ld\n", what,
                           (int)(what && strlen(what) < 54
                                 ? 56 - (int)strlen(what) : 2), "", got); }
}

static void fresh(uint32_t free_blocks, int refuse)
{
    sim_used = 0;
    sim_free_blocks = free_blocks;
    sim_refuse = refuse;
    memset(sim_pool, 0, (size_t)SIM_BLOCKS * 4096u);
    heap_init();
    log_n = 0;
}

#define NPTR 256
/* 256 of these must be MORE than the pool, or "the heap is full"
   never happens. The pool is an eighth of the simulated machine's
   free memory -- 10 MB of 80 -- so 64 KB each gives 16 MB of demand
   against it. It was 16 KB, which is 4 MB, and three exhaustion
   checks quietly stopped testing exhaustion the moment the pool grew
   past that. A constant tied to nothing is a check that expires. */
#define BIG_ALLOC  (64u * 1024u)
static void    *ptr[NPTR];
static uint32_t len[NPTR];

int main(void)
{
    sim_pool = malloc((size_t)SIM_BLOCKS * 4096u);
    if(!sim_pool){ printf("no memory for the simulated pool\n"); return 2; }

    printf("test_heap: the kernel allocator, against the pool\n");

    /* ══ WHERE THE POOL COMES FROM ══════════════════════════════════ */
    head("the pool comes from the physical allocator");
    fresh(SIM_BLOCKS, 0);
    ck("it is not the static array", heap_pool_is_pmm() == 1, 0);
    ck("  and it is bigger than the old 256 KB",
       heap_pool_bytes() > 256u * 1024u, (long)heap_pool_bytes());
    /* An eighth of 16 MB is 2 MB, under the 64 MB ceiling. */
    /* An eighth of 80 MB is 10 MB, under the 64 MB ceiling. */
    ck("  an eighth of 80 MB of free memory is 10 MB",
       heap_pool_bytes() == 10u * 1024u * 1024u, (long)heap_pool_bytes());
    ck("  the integrity scanner is happy with it", heap_check() == 1, 0);

    head("and falls back when the physical allocator has nothing");
    fresh(SIM_BLOCKS, 1);                     /* refuse every request */
    ck("it falls back to the static array", heap_pool_is_pmm() == 0, 0);
    ck("  which is the old 256 KB",
       heap_pool_bytes() == 256u * 1024u, (long)heap_pool_bytes());
    ck("  and the heap still works",
       (ptr[0] = kmalloc(1000)) != NULL, 0);
    ck("  and is still intact", heap_check() == 1, 0);
    kfree(ptr[0]);

    head("a machine too small to spare an eighth keeps the static pool");
    /* 8 blocks free is 32 KB; an eighth is 4 KB, under the 256 KB
       floor, so asking the PMM for it would trade a 256 KB pool for a
       4 KB one. */
    fresh(8, 0);
    ck("32 KB free: the static array is the better answer",
       heap_pool_is_pmm() == 0, 0);
    ck("  so the pool is still 256 KB",
       heap_pool_bytes() == 256u * 1024u, (long)heap_pool_bytes());

    head("and a very large machine is capped, not unbounded");
    /* 16 GB "free". An eighth of that is 2 GB and the ceiling is
       64 MB, so a capped heap asks for 64 MB -- which the 80 MB
       simulated pool CAN satisfy -- and an uncapped one asks for 2 GB,
       which it cannot, and falls back to 256 KB. The two answers are
       64 MB and 256 KB, which is what makes the ceiling testable.

       The first version of this check used a 16 MB pool, where both
       requests were refused and both fell back: the ceiling and its
       absence produced identical results and the check could not tell
       them apart. The injection harness said so. */
    fresh(4096u * 1024u, 0);
    ck("16 GB free: the pool is capped at exactly 64 MB",
       heap_pool_bytes() == 64u * 1024u * 1024u, (long)heap_pool_bytes());
    ck("  and it really did come from the allocator",
       heap_pool_is_pmm() == 1, 0);
    ck("  and is intact", heap_check() == 1, 0);

    /* ══ THE PROPERTY THAT MATTERS ══════════════════════════════════ */
    head("every allocation is writable and overlaps nothing");
    fresh(SIM_BLOCKS, 0);
    {
        int n = 0, i;
        uint32_t sizes[] = { 1, 7, 8, 9, 31, 32, 33, 100, 999, 4096, 17 };
        for(i = 0; i < NPTR; i++){
            uint32_t sz = sizes[i % (int)(sizeof sizes / sizeof sizes[0])];
            ptr[i] = kmalloc(sz);
            if(!ptr[i]) break;
            len[i] = sz;
            /* A byte unique to this allocation, through the whole
               payload: anything that overlaps will be overwritten. */
            memset(ptr[i], (int)(i & 0xFF), sz);
            n++;
        }
        ck("a few hundred allocations of mixed sizes succeed",
           n >= 200, n);

        int bad = 0;
        for(i = 0; i < n; i++){
            uint8_t *q = (uint8_t *)ptr[i];
            for(uint32_t k = 0; k < len[i]; k++)
                if(q[k] != (uint8_t)(i & 0xFF)){ bad++; break; }
        }
        ck("  and NOT ONE of them was overwritten by another",
           bad == 0, bad);

        int outside = 0;
        for(i = 0; i < n; i++){
            uint8_t *q = (uint8_t *)ptr[i];
            if(q < heap_mem || q + len[i] > heap_mem + HEAP_SIZE) outside++;
        }
        ck("  every one of them is inside the pool", outside == 0, outside);
        ck("  the list is intact", heap_check() == 1, 0);

        /* Free every other one, then fill the gaps again. The holes
           must be reusable: an allocator that never reuses a freed
           block passes every check above and runs out in a minute. */
        for(i = 0; i < n; i += 2) kfree(ptr[i]);
        ck("  freeing half of them leaves the list intact",
           heap_check() == 1, 0);
        int refilled = 0;
        for(i = 0; i < n; i += 2){
            ptr[i] = kmalloc(len[i]);
            if(ptr[i]){ memset(ptr[i], 0x5A, len[i]); refilled++; }
        }
        ck("  and the holes can be allocated again",
           refilled >= n / 2 - 1, refilled);

        int clobbered = 0;
        for(i = 1; i < n; i += 2){
            uint8_t *q = (uint8_t *)ptr[i];
            for(uint32_t k = 0; k < len[i]; k++)
                if(q[k] != (uint8_t)(i & 0xFF)){ clobbered++; break; }
        }
        ck("  without touching the ones still held", clobbered == 0,
           clobbered);
    }

    /* ══ THE REFUSALS ═══════════════════════════════════════════════ */
    head("the refusals are refusals");
    fresh(SIM_BLOCKS, 0);
    ck("kmalloc(0) returns NULL", kmalloc(0) == NULL, 0);
    log_n = 0;
    ck("a size larger than the pool is refused",
       kmalloc(heap_pool_bytes() + 1) == NULL, 0);
    ck("  and it is LOGGED, not silent", log_n == 1, log_n);
    log_n = 0;
    ck("(size_t)-1 is refused rather than wrapping",
       kmalloc((size_t)-1) == NULL, 0);
    ck("  and logged", log_n == 1, log_n);

    head("a double free is caught");
    fresh(SIM_BLOCKS, 0);
    ptr[0] = kmalloc(64);
    ck("the first free is accepted", (kfree(ptr[0]), log_n == 0), log_n);
    log_n = 0;
    kfree(ptr[0]);
    ck("  the second is refused and logged", log_n == 1, log_n);
    ck("  with the double-free code", log_last == ERR_MEM_DOUBLE_FREE,
       log_last);
    ck("  and the heap is still intact", heap_check() == 1, 0);

    head("a pointer that is not in the pool is refused");
    fresh(SIM_BLOCKS, 0);
    {
        static uint8_t stranger[64];
        log_n = 0;
        kfree(stranger);
        ck("kfree of a stack-ish address is refused", log_n == 1, log_n);
        ck("  with the bad-pointer code", log_last == ERR_MEM_BAD_PTR,
           log_last);
        log_n = 0;
        ck("krealloc of it returns NULL rather than reading p-12",
           krealloc(stranger, 128) == NULL, 0);
        ck("  and is logged", log_n == 1, log_n);
        ck("kfree(NULL) is a no-op, not an error",
           (log_n = 0, kfree(NULL), log_n == 0), log_n);
    }

    /* ══ krealloc ═══════════════════════════════════════════════════ */
    head("krealloc keeps the bytes");
    fresh(SIM_BLOCKS, 0);
    {
        uint8_t *q = kmalloc(100);
        int i, bad = 0;
        for(i = 0; i < 100; i++) q[i] = (uint8_t)(i * 7 + 1);
        q = krealloc(q, 400);
        ck("growing returns a pointer", q != NULL, 0);
        for(i = 0; i < 100; i++) if(q[i] != (uint8_t)(i * 7 + 1)) bad++;
        ck("  and all 100 original bytes survived", bad == 0, bad);

        bad = 0;
        q = krealloc(q, 20);
        ck("shrinking returns a pointer", q != NULL, 0);
        for(i = 0; i < 20; i++) if(q[i] != (uint8_t)(i * 7 + 1)) bad++;
        ck("  and the first 20 bytes survived that too", bad == 0, bad);

        ck("krealloc(NULL, n) is a kmalloc",
           (ptr[1] = krealloc(NULL, 50)) != NULL, 0);
        ck("krealloc(p, 0) frees and returns NULL",
           krealloc(ptr[1], 0) == NULL, 0);
        ck("  leaving the heap intact", heap_check() == 1, 0);
        kfree(q);
    }

    head("krealloc does not copy a neighbour's bytes when it grows");
    {
        /* MISSED at first. The shrink check above cannot see this:
           shrinking 100 to 20 copies 20 either way. It is GROWING
           that differs -- min(old,new) copies 100, the new size
           copies 400 and reads 300 bytes past the end of the block,
           through the next block's header and into whatever was
           allocated after it.

           Natively that read stays inside the pool, so it does not
           crash and the first 100 bytes are right either way. What
           gives it away is WHAT ARRIVES IN THE REST: a neighbour's
           data, in a buffer that should not contain it. */
        uint8_t *a, *b, *grown;
        int i, found = 0;
        fresh(SIM_BLOCKS, 0);
        a = kmalloc(100);
        b = kmalloc(600);                  /* immediately after a */
        ck("two adjacent blocks", a && b && b > a, (long)(b - a));
        for(i = 0; i < 100; i++) a[i] = 0x11;
        for(i = 0; i < 600; i++) b[i] = 0xC7;   /* the neighbour's mark */

        grown = krealloc(a, 400);
        ck("  growing the first one succeeds", grown != NULL, 0);
        for(i = 0; i < 100; i++) if(grown[i] != 0x11) found = -1;
        ck("  its own 100 bytes survived", found == 0, found);

        /* Sixteen of the neighbour's bytes in a row, anywhere in the
           grown buffer, means the copy ran past the old block. A run
           rather than one byte, so an incidental 0xC7 cannot fail
           it. */
        found = 0;
        for(i = 100; i + 16 <= 400; i++){
            int k = 0;
            while(k < 16 && grown[i + k] == 0xC7) k++;
            if(k == 16){ found = i; break; }
        }
        ck("  and the NEIGHBOUR's bytes are not in it", found == 0, found);
        kfree(grown);
        kfree(b);
    }

    head("krealloc leaves the original alone when it cannot grow");
    {
        /* MISSED at first: nothing ever made the kmalloc inside
           krealloc fail. A krealloc that frees the original and then
           returns NULL punishes the CORRECT caller -- the one that
           keeps using its old pointer because the call said it
           failed. */
        int n = 0, i, bad = 0;
        fresh(SIM_BLOCKS, 0);
        uint8_t *keep = kmalloc(200);
        for(i = 0; i < 200; i++) keep[i] = (uint8_t)(i ^ 0x5A);

        /* Fill everything else up. */
        while(n < NPTR && (ptr[n] = kmalloc(BIG_ALLOC)) != NULL) n++;
        ck("the heap is full", n > 0, n);

        log_n = 0;
        ck("  krealloc to a size that will not fit returns NULL",
           krealloc(keep, BIG_ALLOC) == NULL, 0);
        for(i = 0; i < 200; i++) if(keep[i] != (uint8_t)(i ^ 0x5A)) bad++;
        ck("  the original's bytes are untouched", bad == 0, bad);

        /* And it is still ALLOCATED: freeing it must be accepted, not
           reported as a double free. That is the check that separates
           "left alone" from "already freed". */
        log_n = 0;
        kfree(keep);
        ck("  and freeing it is accepted, not a double free",
           log_n == 0, log_last);
        for(i = 0; i < n; i++) kfree(ptr[i]);
        ck("  the heap is intact afterwards", heap_check() == 1, 0);
    }

    head("krealloc refuses a pointer from outside the pool, without reading it");
    {
        /* MISSED at first. Handing krealloc a stray pointer returned
           NULL whether or not it was bounds-checked, because the
           twelve bytes in front of a random address do not look like
           a valid used block -- so the magic check caught what the
           bounds check was supposed to.

           This makes them LOOK valid. Now only the bounds check can
           refuse it, and an unguarded krealloc proceeds: it allocates,
           copies, and hands back a non-NULL pointer to memory derived
           from somewhere that was never in the heap. */
        static uint8_t faked[256];
        block_t *hdr = (block_t *)faked;
        void *r;
        fresh(SIM_BLOCKS, 0);
        hdr->magic = HEAP_MAGIC_USED;
        hdr->size  = 64;
        hdr->next  = NULL;
        log_n = 0;
        r = krealloc(faked + sizeof(block_t), 128);
        ck("a convincing fake header outside the pool is still refused",
           r == NULL, 0);
        ck("  and logged as a bad pointer",
           log_n >= 1 && log_last == ERR_MEM_BAD_PTR, log_last);
        ck("  the heap is untouched", heap_check() == 1, 0);
    }

    head("freed memory is poisoned");
    {
        /* Not correctness: poisoning is what turns a use-after-free
           from "the old data is still there and it works" into
           something that fails near the mistake. An allocator where
           freed memory keeps reading correctly is one where
           use-after-free ships.

           Reading freed memory on purpose, which is the only way to
           check it. The pool is this process's own array, so the read
           is defined. */
        uint8_t *q;
        int i, poisoned = 0;
        fresh(SIM_BLOCKS, 0);
        q = kmalloc(128);
        for(i = 0; i < 128; i++) q[i] = 0xAA;
        kfree(q);
        for(i = 0; i < 128; i++) if(q[i] == 0xDD) poisoned++;
        ck("the payload is overwritten on free", poisoned == 128,
           poisoned);
        ck("  so a use-after-free reads poison, not the old value",
           q[0] != 0xAA, q[0]);
    }

    /* ══ exhaustion ═════════════════════════════════════════════════ */
    head("running out is reported, not guessed at");
    fresh(SIM_BLOCKS, 0);
    {
        int n = 0;
        while(n < NPTR && (ptr[n] = kmalloc(BIG_ALLOC)) != NULL) n++;
        ck("the pool fills up", n > 0 && n < NPTR, n);
        log_n = 0;
        ck("  the next allocation returns NULL",
           kmalloc(BIG_ALLOC) == NULL, 0);
        ck("  and says so", log_n == 1, log_n);
        ck("  with the out-of-memory code", log_last == ERR_MEM_OOM,
           log_last);
        ck("  the heap is still intact when full", heap_check() == 1, 0);
        for(int i = 0; i < n; i++) kfree(ptr[i]);
        ck("  and after freeing it all", heap_check() == 1, 0);
        ck("  a big allocation works again once freed",
           (ptr[0] = kmalloc(BIG_ALLOC)) != NULL, 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    free(sim_pool);
    return fails ? 1 : 0;
}
