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

#define HEAP_SIZE      (256 * 1024)
#define HEAP_MAGIC_FREE  0xFEEBDAEDu   /* block is free   */
#define HEAP_MAGIC_USED  0xA110CA7Eu   /* block is in use */

static uint8_t heap_mem[HEAP_SIZE];

typedef struct block {
    uint32_t       magic;   /* HEAP_MAGIC_FREE or HEAP_MAGIC_USED */
    uint32_t       size;    /* payload bytes (NOT including this header) */
    struct block  *next;
} block_t;

static block_t *heap_head = NULL;

/* ── Init ───────────────────────────────────────────────────────── */
void heap_init(void)
{
    heap_head        = (block_t *)heap_mem;
    heap_head->magic = HEAP_MAGIC_FREE;
    heap_head->size  = HEAP_SIZE - sizeof(block_t);
    heap_head->next  = NULL;
}

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
