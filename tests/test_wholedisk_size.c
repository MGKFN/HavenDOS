/* The whole-disk installer's partition size.
 *
 * Every disk over 2 GB failed this, on every bus, with E15 at step 4 of 9
 * -- after the partition table and GRUB had been written, so the disk was
 * left unbootable, and the message on screen suggested the disk might be
 * too SMALL. The size was taken as the whole disk with no upper bound, and
 * FAT16 stops at 65524 clusters of 64 sectors.
 *
 * Nothing caught it because the whole-disk path had no test for the size
 * it chooses, and every install the harness ran was on a 2 GB image or an
 * additive install into a gap that INSTADD_MAX_SECTORS already bounded.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fat16fmt.h"
#include "install_add.h"

#define PART_START_SECT 2048u

static int pass = 0, total = 0;
static void ck(int c, const char *what){
    total++; if(c) pass++; else printf("  FAIL: %s\n", what);
}

/* The installer's rule, as fs/install.c now applies it. */
static uint32_t whole_disk_part(uint64_t dsects){
    uint32_t n = (uint32_t)(dsects - PART_START_SECT);
    if(n > INSTADD_MAX_SECTORS) n = INSTADD_MAX_SECTORS;
    return n / INSTADD_ALIGN * INSTADD_ALIGN;
}

int main(void){
    /* The sizes a person actually has, plus the boundary either side. */
    static const uint64_t mb[] = {16,64,256,512,1024,2047,2048,2049,
                                  4096,8192,16384,32768,65536,131072,
                                  262144,524288,1048576,2097152};

    for(unsigned i = 0; i < sizeof mb / sizeof *mb; i++){
        uint64_t dsects = mb[i] * 2048ull;
        uint32_t part   = whole_disk_part(dsects);
        fat16_geom_t g; memset(&g, 0, sizeof g);

        char msg[96];
        snprintf(msg, sizeof msg, "%llu MB disk: a geometry exists",
                 (unsigned long long)mb[i]);
        ck(fat16_geom(part, &g) == 1, msg);

        snprintf(msg, sizeof msg, "%llu MB disk: partition fits the disk",
                 (unsigned long long)mb[i]);
        ck((uint64_t)part + PART_START_SECT <= dsects, msg);

        snprintf(msg, sizeof msg, "%llu MB disk: partition is MB-aligned",
                 (unsigned long long)mb[i]);
        ck(part % INSTADD_ALIGN == 0, msg);

        /* The MBR entry and the format must describe the same partition.
           They were computed separately, from the same wrong expression,
           and capping only one of them would be worse than capping
           neither: a 16 GB entry over a 2 GB filesystem. */
        snprintf(msg, sizeof msg, "%llu MB disk: geometry spans the entry",
                 (unsigned long long)mb[i]);
        ck(g.part_sectors == part, msg);
    }

    /* A disk at or under the cap is used whole, not truncated to it. */
    uint32_t small = whole_disk_part(1024ull * 2048ull);
    ck(small == 1024u * 2048u - PART_START_SECT, "1 GB disk is used whole");

    /* A disk over the cap stops exactly at it. */
    ck(whole_disk_part(16384ull * 2048ull) == INSTADD_MAX_SECTORS,
       "16 GB disk is capped at INSTADD_MAX_SECTORS");
    ck(whole_disk_part(2097152ull * 2048ull) == INSTADD_MAX_SECTORS,
       "2 TB disk is capped at INSTADD_MAX_SECTORS");

    /* And the cap is genuinely the largest FAT16 can take: one megabyte
       more must not work, or the cap is arbitrary rather than derived. */
    fat16_geom_t over; memset(&over, 0, sizeof over);
    ck(fat16_geom(INSTADD_MAX_SECTORS + INSTADD_ALIGN, &over) == 0,
       "one MB above the cap has no FAT16 geometry");

    printf("%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
