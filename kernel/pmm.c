#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"

#define PMM_BLOCK_SIZE 4096
#define PMM_BLOCKS_PER_BYTE 8
/* ── HOW MUCH PHYSICAL MEMORY THIS KERNEL CAN SEE ────────────────────
   v0.8.8.5: was 2 GB, which threw away half of what a flat 32-bit map
   can reach. kernel/main.c already caps ram_bytes at 4 GB when it
   walks the multiboot memory map -- `if(top>0x100000000ULL) top =
   0x100000000ULL` -- so on a machine with 8 or 16 GB the loader
   reported close to 4 GB and this clamp then halved it. Kyan asked
   for "more ram by maybe making it 64 bit"; the first 2 GB of the
   answer needed no such thing.

   WRITTEN TO AVOID A 32-BIT OVERFLOW. The obvious spelling,

       (4096UL*1024*1024/PMM_BLOCK_SIZE)

   is WRONG on this target: unsigned long is 32 bits here, the
   multiplication happens before the division, and 4096*1024*1024 is
   exactly 2^32 -- so it wraps to 0 and MAX_BLOCKS becomes 0. The
   bitmap would then be a zero-length array and total_blocks would
   clamp to nothing. Grouping the division with the megabyte keeps
   every intermediate inside 32 bits: 1 MB is 256 blocks, so 4096 MB
   is 4096 * 256.

   128 KB of bitmap in bss, up from 64. */
#define BLOCKS_PER_MB  (1024UL*1024UL/PMM_BLOCK_SIZE)   /* 256 */
#define MAX_BLOCKS     (4096UL * BLOCKS_PER_MB)         /* 4 GB */

static uint8_t bitmap[MAX_BLOCKS/8];
static uint32_t total_blocks=0, free_blocks=0;

static void set_bit(int b){ bitmap[b/8]|=(1<<(b%8)); }
static void clr_bit(int b){ bitmap[b/8]&=~(1<<(b%8)); }
static int  tst_bit(int b){ return bitmap[b/8]&(1<<(b%8)); }

/* v0.7.12 FIX — the comment said 4 MB, the loop reserved 256 blocks, and
   256 * 4096 is 1 MB.  The kernel image is loaded at 0x100000 and the linked
   binary is well over 700 KB, so blocks 256 upward — THE KERNEL'S OWN TEXT,
   RODATA AND BSS — were marked free and pmm_alloc() would hand them out.
   Nothing calls pmm_alloc() today, which is the only reason this has not
   detonated; it is a loaded gun pointed at the first caller.

   v0.7.18 FIX — 1024 blocks is 4 MB and the kernel image is 6.31 MB, so the
   gun was still loaded: 2.31 MB of live BSS sat above the reserved mark and
   pmm_alloc() would have returned pointers straight into it.  A fixed
   constant cannot track a growing image, so the boundary now comes from the
   linker's own `_kernel_end` symbol and is correct by construction.  This
   matters now because v0.7.18 moves the big scratch buffers onto pmm, which
   makes pmm_alloc() live for the first time. */
/* PMM_KERNEL_END_ADDR is the end of the linked kernel image.  tests/test_pmm.c
   overrides it to walk the boundary about; nothing else may redefine it. */
#ifndef PMM_KERNEL_END_ADDR
extern uint8_t _kernel_end;          /* defined by boot/linker.ld */
#define PMM_KERNEL_END_ADDR ((uint32_t)&_kernel_end)
#endif

static uint32_t reserved_blocks = 1024;

void pmm_init(uint32_t mem_size){
    total_blocks=mem_size/PMM_BLOCK_SIZE;
    if(total_blocks>MAX_BLOCKS)total_blocks=MAX_BLOCKS;
    free_blocks=total_blocks;
    memset(bitmap,0,sizeof(bitmap));

    /* Everything below the end of the kernel image is off limits: low
       memory, the image itself, and one spare block of slack. */
    uint32_t kend = PMM_KERNEL_END_ADDR;
    uint32_t rsvd = (kend + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE + 1;
    if(rsvd < 1024) rsvd = 1024;          /* never trust a short symbol */
    if(rsvd > total_blocks) rsvd = total_blocks;
    reserved_blocks = rsvd;

    for(uint32_t i=0;i<rsvd;i++)set_bit((int)i);
    free_blocks-=rsvd;
}
void *pmm_alloc(void){
    for(uint32_t i=reserved_blocks;i<total_blocks;i++){
        if(!tst_bit(i)){set_bit(i);free_blocks--;return(void*)(i*PMM_BLOCK_SIZE);}
    }
    return NULL;
}
void pmm_free(void *p){
    int b=(uint32_t)p/PMM_BLOCK_SIZE;
    if(b>=(int)reserved_blocks&&(uint32_t)b<total_blocks&&tst_bit(b)){clr_bit(b);free_blocks++;}
}

/* ── contiguous runs ──────────────────────────────────────────────────
   The big scratch buffers (the 2 MB update staging area, the 512 KB HTTP
   body) need one flat block of memory, not a page list, because callers
   index them as plain arrays.  A first-fit scan is plenty: these are
   allocated a handful of times per boot, never in a loop. */
void *pmm_alloc_contig(uint32_t nblocks){
    if(!nblocks || nblocks > total_blocks) return NULL;
    uint32_t run = 0;
    for(uint32_t i=reserved_blocks;i<total_blocks;i++){
        if(tst_bit(i)){ run = 0; continue; }
        if(++run == nblocks){
            uint32_t start = i + 1 - nblocks;
            for(uint32_t k=0;k<nblocks;k++){ set_bit((int)(start+k)); free_blocks--; }
            return (void*)(start * PMM_BLOCK_SIZE);
        }
    }
    return NULL;
}
void pmm_free_contig(void *p, uint32_t nblocks){
    if(!p || !nblocks) return;
    uint32_t start = (uint32_t)p / PMM_BLOCK_SIZE;
    for(uint32_t k=0;k<nblocks;k++){
        uint32_t b = start + k;
        if(b>=reserved_blocks && b<total_blocks && tst_bit((int)b)){
            clr_bit((int)b); free_blocks++;
        }
    }
}
uint32_t pmm_reserved_blocks(void){ return reserved_blocks; }
uint32_t pmm_free_blocks(void){ return free_blocks; }
uint32_t pmm_total_blocks(void){ return total_blocks; }
