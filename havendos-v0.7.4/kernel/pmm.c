#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"

#define PMM_BLOCK_SIZE 4096
#define PMM_BLOCKS_PER_BYTE 8
#define MAX_BLOCKS (2048UL*1024*1024/PMM_BLOCK_SIZE) /* 2GB */

static uint8_t bitmap[MAX_BLOCKS/8];
static uint32_t total_blocks=0, free_blocks=0;

static void set_bit(int b){ bitmap[b/8]|=(1<<(b%8)); }
static void clr_bit(int b){ bitmap[b/8]&=~(1<<(b%8)); }
static int  tst_bit(int b){ return bitmap[b/8]&(1<<(b%8)); }

void pmm_init(uint32_t mem_size){
    total_blocks=mem_size/PMM_BLOCK_SIZE;
    if(total_blocks>MAX_BLOCKS)total_blocks=MAX_BLOCKS;
    free_blocks=total_blocks;
    memset(bitmap,0,sizeof(bitmap));
    /* mark first 4MB as used (kernel lives here) */
    for(int i=0;i<256;i++)set_bit(i);
    free_blocks-=256;
}
void *pmm_alloc(void){
    for(uint32_t i=256;i<total_blocks;i++){
        if(!tst_bit(i)){set_bit(i);free_blocks--;return(void*)(i*PMM_BLOCK_SIZE);}
    }
    return NULL;
}
void pmm_free(void *p){
    int b=(uint32_t)p/PMM_BLOCK_SIZE;
    if(b>=256&&(uint32_t)b<total_blocks&&tst_bit(b)){clr_bit(b);free_blocks++;}
}
uint32_t pmm_free_blocks(void){ return free_blocks; }
uint32_t pmm_total_blocks(void){ return total_blocks; }
