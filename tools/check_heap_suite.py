#!/usr/bin/env python3
"""check_heap_suite.py -- prove tests/test_heap.c can fail.

kernel/heap.c was the oldest untested file in the tree. Its own header
lists SEVEN bug fixes -- double-free detection, magic validation,
bounds checks, a krealloc() that dereferenced p-12 before validating p
-- and every one of them was found by reading, because until v0.8.8.5
there was no suite at all.

A suite written in one sitting beside a file that old agrees with it
by construction. This is what says otherwise.

── WHAT MAKES AN ALLOCATOR WRONG ─────────────────────────────────────

Not crashing. Every defect below produces an allocator that RUNS:

  overlapping blocks   two pointers into the same bytes. Any test that
                       looks at one allocation at a time passes. The
                       suite writes a distinct byte through every
                       payload and reads them all back afterwards,
                       which is the only shape of check that sees it.
  a hole never reused  an allocator that allocates forward for ever
                       passes every correctness check and runs out of
                       memory in a minute.
  a refusal that isn't a double free accepted, or a stray pointer
                       treated as a block header. The kernel keeps
                       running and corrupts the list for later.
  a pool it never got  heap_init() reporting a size it was refused.

── THE ONE THIS SUITE FOUND BEFORE ANY INJECTION ─────────────────────

heap_init() did not reset heap_mem, HEAP_SIZE and heap_from_pmm before
trying the physical allocator, so it was NOT IDEMPOTENT: after a
successful call, a later refused one still described the old pool and
reported success with memory it had not been given. The shipped kernel
calls it once, so it could not bite -- and the suite calls it four
times, which is how it surfaced.

Run:  python3 tools/check_heap_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "kernel/heap.c"
TEST = "tests/test_heap.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch"]

BUGS = [
    ("blocks are handed out overlapping",
     "the split leaves the new free block INSIDE the one just "
     "allocated, so the next allocation returns bytes the caller "
     "before it is still using. Both callers write and the second "
     "wins, which looks like data corruption in whatever used the "
     "memory rather than anything to do with the allocator",
     """                block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);""",
     """                block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size / 2);"""),

    ("a freed block is never reused",
     "kmalloc walks past free blocks instead of taking them, so the "
     "heap allocates forward for ever. Every correctness check passes "
     "and the machine runs out of memory in a minute of ordinary use",
     """        if(b->magic == HEAP_MAGIC_FREE && b->size >= size) {""",
     """        if(0 && b->magic == HEAP_MAGIC_FREE && b->size >= size) {"""),

    ("a block is handed out without being marked used",
     "it stays HEAP_MAGIC_FREE, so the very next kmalloc finds it "
     "again and hands the same bytes to a second caller. The same "
     "symptom as overlapping blocks, by a different route",
     """            b->magic = HEAP_MAGIC_USED;
            return (uint8_t *)b + sizeof(block_t);""",
     """            return (uint8_t *)b + sizeof(block_t);"""),

    ("a double free is accepted",
     "the second free coalesces a block that is already part of a "
     "free run, so the list gains a cycle or a block that overlaps "
     "its neighbour. The machine keeps running and the damage "
     "surfaces somewhere else entirely",
     """    if(b->magic == HEAP_MAGIC_FREE) {
        kernel_log(ERR_MEM_DOUBLE_FREE, "kfree: double-free detected");
        return;
    }""",
     """    if(b->magic == HEAP_MAGIC_FREE) {
        /* accepted */
    }"""),

    ("kfree trusts a pointer from outside the pool",
     "it reads the twelve bytes in front of any address it is given as "
     "a block header. A stray pointer then has its 'size' and 'next' "
     "believed, and the free list is spliced into memory that is not "
     "the heap",
     """    if(pp < heap_start || pp >= heap_end) {
        kernel_log(ERR_MEM_BAD_PTR, "kfree: pointer outside heap");
        return;
    }""",
     """    if(0) {
        kernel_log(ERR_MEM_BAD_PTR, "kfree: pointer outside heap");
        return;
    }"""),

    ("krealloc trusts a pointer from outside the pool",
     "the v0.7.12 defect exactly, restored: kfree() bounds-checks "
     "before treating the memory in front of a pointer as a header "
     "and krealloc() did not, so a stray pointer was dereferenced at "
     "p-12 before any validation could reject it",
     """    uint8_t *pp = (uint8_t *)p;
    if(pp < heap_mem + sizeof(block_t) || pp >= heap_mem + HEAP_SIZE) {
        kernel_log(ERR_MEM_BAD_PTR, "krealloc: pointer outside heap");
        return NULL;
    }""",
     """    uint8_t *pp = (uint8_t *)p;
    (void)pp;"""),

    ("krealloc copies the new size instead of the smaller of the two",
     "growing is fine and SHRINKING reads past the end of the old "
     "block -- into the next block's header, or off the end of the "
     "pool. The bytes it copies are then wrong in a way that depends "
     "on what was allocated next",
     """    size_t copy = b->size < sz ? b->size : sz;""",
     """    size_t copy = sz;"""),

    ("krealloc frees the original when the new allocation fails",
     "the caller is handed NULL and its pointer has already been "
     "freed, so the obvious recovery -- keep using the old buffer -- "
     "is a use-after-free. The header records this as a fix; it is "
     "the one defect here that punishes CORRECT caller behaviour",
     """    void *n = kmalloc(sz);
    if(!n) {
        /* BUG FIX: on failure, leave p intact and return NULL */
        return NULL;
    }""",
     """    void *n = kmalloc(sz);
    if(!n) {
        kfree(p);
        return NULL;
    }"""),

    ("an oversized request is not refused",
     "size is 4-byte aligned just below, and a request near SIZE_MAX "
     "wraps to a small number -- so (size_t)-1 becomes 0 and the "
     "allocator hands back a block for a caller that asked for four "
     "gigabytes and will write all of it",
     """    if(size > HEAP_SIZE - sizeof(block_t)) {""",
     """    if(0) {"""),

    ("heap_init is not idempotent",
     "THE DEFECT THIS SUITE FOUND BEFORE ANY INJECTION. Without the "
     "reset, a call that is refused by the physical allocator still "
     "describes the pool a PREVIOUS call was given, and reports "
     "success with memory it does not have. The shipped kernel calls "
     "it once, which is what kept it latent",
     """    heap_mem      = heap_static;
    HEAP_SIZE     = HEAP_STATIC;
    heap_from_pmm = 0;""",
     """    /* no reset */"""),

    ("the pool floor is dropped, so a tiny machine loses its heap",
     "an eighth of a 32 KB machine is 4 KB. Taking it trades the "
     "256 KB static array for a pool sixty-four times smaller, and "
     "the machine that could least afford it is the one that gets "
     "the smallest heap",
     """    if(want >= HEAP_WANT_MIN){""",
     """    if(want > 0){"""),

    ("the pool ceiling is dropped",
     "kmalloc is first-fit over a singly-linked list, so the free "
     "list is walked on every allocation. An unbounded pool on a "
     "large machine is millions of blocks to step through, and the "
     "allocator gets slower the more memory the machine has -- which "
     "is the opposite of the point",
     """    if(want > HEAP_WANT_MAX) want = HEAP_WANT_MAX;""",
     """    /* no ceiling */"""),

    ("the payload is not poisoned on free",
     "NOT A CORRECTNESS BUG and the header calls it a fix: poisoning "
     "is what turns a use-after-free from 'the old data is still "
     "there and it works' into something that fails close to the "
     "mistake. An allocator where freed memory keeps working is one "
     "where use-after-free ships",
     """    if(b->size <= HEAP_SIZE)
        memset(p, 0xDD, b->size);""",
     """    /* no poison */"""),
]

# Must NOT be caught: a comment reworded. Nothing about the allocator
# changes, so a catch here means the loop is restoring badly or the
# suite is asserting something that is not behaviour.
HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the allocator changed",
    """uint32_t heap_pool_bytes(void) { return HEAP_SIZE; }""",
    """/* the pool's size, for the boot log */
uint32_t heap_pool_bytes(void) { return HEAP_SIZE; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "heap", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        preamble="Proving tests/test_heap.c can fail.\n"
                 "kernel/heap.c was the oldest untested file in the\n"
                 "tree: seven bug fixes in its header, every one found\n"
                 "by reading, and no suite at all until now."))
