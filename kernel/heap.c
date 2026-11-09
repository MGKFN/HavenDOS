/*
 * heap.c — Kernel heap allocator for HavenDOS v0.6.8
 *
 * Fixed-size first-fit allocator over a 256KB static pool.
 *
 * BUG FIXES vs v0.6.8:
 *   - Magic values (HEAP_MAGIC_FREE / HEAP_MAGIC_USED) detect corruption
 *   - Block size sanity check in kmalloc (rejects implausibly large sizes)
 *   - kfree: validates magic before trusting block metadata
 *   - kfree: validates pointer is inside heap before any dereference
 *   - kfree: detects double-free via magic
 *   - krealloc: guards against NULL return from kmalloc
 *   - heap_check(): public integrity scanner, callable from shell/debug
 *
 * Alignment: all allocations are 4-byte aligned.
 * Thread safety: not applicable (HavenDOS is single-threaded).
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/errors.h"

/* ── THE FALLBACK POOL, AND WHY IT IS STILL HERE ─────────────────────
   v0.8.8.5. This file was "a fixed-size first-fit allocator over a
   256KB static pool", and that 256 KB was the whole of the kernel's
   dynamic memory no matter what the machine had. Kyan asked for "more
   ram by maybe making it 64 bit"; the first answer was that kmalloc
   could not reach a quarter of a megabyte on a machine with eight
   gigabytes in it, and that has nothing to do with the address width.

   The pool comes from the physical allocator now. This array remains
   as the fallback for a machine where that fails, and for the window
   before pmm_init() runs -- a kmalloc before the PMM exists has to
   land somewhere, and "somewhere" must not be address zero.

   256 KB of bss is the price of never having to reason about which
   of those two cases a given boot is in. */
#define HEAP_STATIC    (256 * 1024)
#define HEAP_MAGIC_FREE  0xFEEBDAEDu   /* block is free   */
#define HEAP_MAGIC_USED  0xA110CA7Eu   /* block is in use */

static uint8_t heap_static[HEAP_STATIC];

/* The pool actually in use: the static array, or a run of pages from
   the PMM. Every bound in this file is taken from these two and not
   from a constant, because the constant stopped being the answer. */
static uint8_t *heap_mem  = heap_static;
static uint32_t HEAP_SIZE = HEAP_STATIC;
static int      heap_from_pmm = 0;

typedef struct block {
    uint32_t       magic;   /* HEAP_MAGIC_FREE or HEAP_MAGIC_USED */
    uint32_t       size;    /* payload bytes (NOT including this header) */
    struct block  *next;
} block_t;

static block_t *heap_head = NULL;

/* ── Init ───────────────────────────────────────────────────────── */
/* ── HOW BIG A POOL TO ASK FOR ───────────────────────────────────────
   A fraction of what is free rather than a constant, because the
   machines this runs on differ by a factor of thirty: the HP has
   gigabytes and the oldest target this project has booted had 32 MB.
   An eighth of free memory, clamped to a floor of the old 256 KB and
   a ceiling of 64 MB.

   The CEILING is the part worth arguing. kmalloc() is first-fit over
   a singly-linked list, so a free list that has been fragmented by a
   long session is walked on every allocation -- 64 MB of 32-byte
   allocations would be two million blocks to step through. The point
   of this change is to stop a quarter of a megabyte being the whole
   budget, not to make the allocator something it is not. A real
   answer needs size classes, and that is its own release. */
#define HEAP_WANT_MAX   (64u * 1024u * 1024u)
#define HEAP_WANT_MIN   ((uint32_t)HEAP_STATIC)

extern void    *pmm_alloc_contig(uint32_t n);
extern uint32_t pmm_free_blocks(void);

void heap_init(void)
{
    uint32_t want, pages;
    void    *p;

    /* ── RE-ESTABLISH THE DEFAULT FIRST ───────────────────────────────
       FOUND BY tests/test_heap.c, which calls this more than once.
       Without these three lines heap_init() is not idempotent: if a
       previous call took a pool from the PMM and a later one is
       refused, heap_mem and HEAP_SIZE still describe the OLD pool,
       and the function reports success with a pool it did not get.

       In the shipped kernel heap_init() runs once, so it could not
       bite -- which is exactly the kind of latent wrong that waits
       for somebody to call a function twice. A function that cannot
       state its own postcondition is one edit from being wrong. */
    heap_mem      = heap_static;
    HEAP_SIZE     = HEAP_STATIC;
    heap_from_pmm = 0;

    /* Try the physical allocator first. kernel/main.c calls pmm_init()
       before this, which is the only reason it can be asked. */
    want = pmm_free_blocks() / 8u * 4096u;      /* an eighth of free */
    if(want > HEAP_WANT_MAX) want = HEAP_WANT_MAX;

    if(want >= HEAP_WANT_MIN){
        pages = (want + 4095u) / 4096u;
        p = pmm_alloc_contig(pages);
        if(p){
            heap_mem      = (uint8_t *)p;
            HEAP_SIZE     = pages * 4096u;
            heap_from_pmm = 1;
        }
    }

    heap_head        = (block_t *)heap_mem;
    heap_head->magic = HEAP_MAGIC_FREE;
    heap_head->size  = HEAP_SIZE - sizeof(block_t);
    heap_head->next  = NULL;
}

/* For the boot log, which must report what was MADE of the machine
   rather than what was asked for -- tools/check_boot_log.py exists
   because this project used to print the latter. */
uint32_t heap_pool_bytes(void) { return HEAP_SIZE; }
int      heap_pool_is_pmm(void){ return heap_from_pmm; }

/* ── Alloc ──────────────────────────────────────────────────────── */
void *kmalloc(size_t size)
{
    if(size == 0) return NULL;

    /* Sanity: reject requests larger than the entire heap payload.
       Catches integer overflows in callers (e.g. size = -1 as size_t). */
    if(size > HEAP_SIZE - sizeof(block_t)) {
        kernel_log(ERR_MEM_OOM, "kmalloc: requested size exceeds heap");
        return NULL;
    }

    size = (size + 3) & ~3u;   /* 4-byte align */

    block_t *b = heap_head;
    while(b) {
        /* BUG FIX: validate magic before trusting any block field */
        if(b->magic != HEAP_MAGIC_FREE && b->magic != HEAP_MAGIC_USED) {
            kernel_log(ERR_MEM_CORRUPT, "kmalloc: heap corruption detected");
            return NULL;
        }

        if(b->magic == HEAP_MAGIC_FREE && b->size >= size) {
            /* Split if the remainder would hold at least 16 payload bytes */
            if(b->size > size + sizeof(block_t) + 16) {
                block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
                nb->magic = HEAP_MAGIC_FREE;
                nb->size  = b->size - size - sizeof(block_t);
                nb->next  = b->next;
                b->size   = size;
                b->next   = nb;
            }
            b->magic = HEAP_MAGIC_USED;
            return (uint8_t *)b + sizeof(block_t);
        }
        b = b->next;
    }

    kernel_log(ERR_MEM_OOM, "kmalloc: out of heap memory");
    return NULL;
}

/* ── Free ───────────────────────────────────────────────────────── */
void kfree(void *p)
{
    if(!p) return;

    uint8_t *pp         = (uint8_t *)p;
    uint8_t *heap_start = heap_mem + sizeof(block_t);
    uint8_t *heap_end   = heap_mem + HEAP_SIZE;

    /* BUG FIX: bounds check before dereferencing as block header */
    if(pp < heap_start || pp >= heap_end) {
        kernel_log(ERR_MEM_BAD_PTR, "kfree: pointer outside heap");
        return;
    }

    block_t *b = (block_t *)((uint8_t *)p - sizeof(block_t));

    /* BUG FIX: magic-based double-free and corruption detection */
    if(b->magic == HEAP_MAGIC_FREE) {
        kernel_log(ERR_MEM_DOUBLE_FREE, "kfree: double-free detected");
        return;
    }
    if(b->magic != HEAP_MAGIC_USED) {
        kernel_log(ERR_MEM_CORRUPT, "kfree: corrupted block magic");
        return;
    }

    /* BUG FIX: poison the payload on free to catch use-after-free */
    if(b->size <= HEAP_SIZE)
        memset(p, 0xDD, b->size);

    b->magic = HEAP_MAGIC_FREE;

    /* Coalesce adjacent free blocks (forward only — no backward pointer) */
    block_t *cur = heap_head;
    while(cur && cur->next) {
        if(cur->magic == HEAP_MAGIC_FREE && cur->next->magic == HEAP_MAGIC_FREE) {
            cur->size += sizeof(block_t) + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* ── Realloc ─────────────────────────────────────────────────────── */
void *krealloc(void *p, size_t sz)
{
    if(!p)   return kmalloc(sz);
    if(sz == 0) { kfree(p); return NULL; }

    /* v0.7.12: kfree() bounds-checks the pointer before treating the memory in
       front of it as a header; krealloc() did not, so a stray pointer was
       dereferenced at p-12 before any validation could reject it. */
    uint8_t *pp = (uint8_t *)p;
    if(pp < heap_mem + sizeof(block_t) || pp >= heap_mem + HEAP_SIZE) {
        kernel_log(ERR_MEM_BAD_PTR, "krealloc: pointer outside heap");
        return NULL;
    }

    /* BUG FIX: validate magic before reading b->size */
    block_t *b = (block_t *)((uint8_t *)p - sizeof(block_t));
    if(b->magic != HEAP_MAGIC_USED) {
        kernel_log(ERR_MEM_CORRUPT, "krealloc: invalid block");
        return NULL;
    }

    void *n = kmalloc(sz);
    if(!n) {
        /* BUG FIX: on failure, leave p intact and return NULL */
        return NULL;
    }

    size_t copy = b->size < sz ? b->size : sz;
    memcpy(n, p, copy);
    kfree(p);
    return n;
}

/* ── heap_check(): integrity scanner ────────────────────────────── */
/*
 * Walks the entire block list and verifies:
 *   - Every block has a valid magic value
 *   - The block list terminates within heap_mem
 *   - No block overlaps heap_mem boundaries
 * Returns 1 if clean, 0 if corruption detected.
 * Suitable for calling from the shell 'meminfo' or 'selftest' commands.
 */
int heap_check(void)
{
    block_t *b    = heap_head;
    uint8_t *end  = heap_mem + HEAP_SIZE;
    int      count = 0;

    while(b) {
        uint8_t *bptr = (uint8_t *)b;

        if(bptr < heap_mem || bptr >= end) {
            kernel_log(ERR_MEM_CORRUPT, "heap_check: block outside heap");
            return 0;
        }
        if(b->magic != HEAP_MAGIC_FREE && b->magic != HEAP_MAGIC_USED) {
            kernel_log(ERR_MEM_CORRUPT, "heap_check: bad block magic");
            return 0;
        }
        if(bptr + sizeof(block_t) + b->size > end) {
            kernel_log(ERR_MEM_CORRUPT, "heap_check: block extends past heap end");
            return 0;
        }
        b = b->next;
        if(++count > 65536) {
            kernel_log(ERR_MEM_CORRUPT, "heap_check: list loop detected");
            return 0;
        }
    }
    return 1;
}

/* ── heap_stats(): free/used counts for 'meminfo' ───────────────── */
void heap_stats(uint32_t *free_bytes, uint32_t *used_bytes)
{
    uint32_t f = 0, u = 0;
    block_t *b = heap_head;
    while(b) {
        if(b->magic == HEAP_MAGIC_FREE) f += b->size;
        else                            u += b->size;
        b = b->next;
    }
    if(free_bytes) *free_bytes = f;
    if(used_bytes) *used_bytes = u;
}
