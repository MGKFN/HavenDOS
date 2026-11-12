/* The whole-disk installer's partition size, and which FAT it gets.
 *
 * Every disk over 2 GB failed this once, on every bus, with E15 at step 4
 * of 9 -- after the partition table and GRUB had been written, so the disk
 * was left unbootable, and the message on screen suggested the disk might
 * be too SMALL. The size was the whole disk with no upper bound, and FAT16
 * stops at 65524 clusters of 64 sectors.
 *
 * v0.9.3 "Gaia": the 2047 MB that fixed it became the FAT16/FAT32
 * boundary, and the cap is 32 GB. Two things changed in this suite to
 * match, and one thing did not:
 *   - it links fs/instsize.c, THE functions fs/install.c calls, where it
 *     used to carry its own copy of the rule -- a copy that would have
 *     gone on passing after the installer changed;
 *   - every size is checked against the formatter of the type it gets,
 *     fat16_geom() below the boundary and fat32_geom() above it;
 *   - and, unchanged, the MBR entry and the volume must describe the same
 *     partition, because that is the bug this file was written for.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fat16fmt.h"
#include "fat32fmt.h"
#include "install_add.h"

#define PART_START_SECT 2048u

static int pass = 0, total = 0;
static void ck(int c, const char *what){
    total++; if(c) pass++; else printf("  FAIL: %s\n", what);
}

int main(void){
    /* The sizes a person actually has, plus the boundary either side. */
    static const uint64_t mb[] = {16,64,256,512,1024,2047,2048,2049,2050,
                                  4096,8192,16384,32768,32769,65536,131072,
                                  262144,524288,1048576,2097152};
    char msg[112];

    for(unsigned i = 0; i < sizeof mb / sizeof *mb; i++){
        uint64_t dsects = mb[i] * 2048ull;
        uint32_t part   = havendos_wholedisk_sectors(dsects);
        int      fs     = instadd_fs_for(part);
        uint32_t spans  = 0;
        int      have   = 0;

        if(fs == 16){
            fat16_geom_t g; memset(&g, 0, sizeof g);
            have = fat16_geom(part, &g); spans = g.part_sectors;
        } else {
            fat32_geom_t g; memset(&g, 0, sizeof g);
            have = fat32_geom(part, &g); spans = g.part_sectors;
            snprintf(msg, sizeof msg, "%llu MB disk: FAT32 has >= 65525 clusters",
                     (unsigned long long)mb[i]);
            ck(have && g.clusters >= FAT32_MIN_CLUSTERS, msg);
        }

        snprintf(msg, sizeof msg, "%llu MB disk: a FAT%d geometry exists",
                 (unsigned long long)mb[i], fs);
        ck(have == 1, msg);

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
        ck(spans == part, msg);

        snprintf(msg, sizeof msg, "%llu MB disk: FAT16 at or under 2047 MB, FAT32 above",
                 (unsigned long long)mb[i]);
        ck(fs == (part > INSTADD_FAT16_MAX_SECTORS ? 32 : 16), msg);
    }

    /* A disk at or under the cap is used whole, not truncated to it. */
    ck(havendos_wholedisk_sectors(1024ull * 2048ull) == 1024u * 2048u - PART_START_SECT,
       "1 GB disk is used whole");
    ck(havendos_wholedisk_sectors(16384ull * 2048ull) == 16384u * 2048u - PART_START_SECT,
       "16 GB disk is used whole -- the 2047 MB cap is gone");
    ck(instadd_fs_for(havendos_wholedisk_sectors(16384ull * 2048ull)) == 32,
       "16 GB disk gets FAT32");

    /* A disk over the cap stops exactly at it. */
    ck(havendos_wholedisk_sectors(65536ull * 2048ull) == INSTADD_MAX_SECTORS,
       "64 GB disk is capped at INSTADD_MAX_SECTORS");
    ck(havendos_wholedisk_sectors(2097152ull * 2048ull) == INSTADD_MAX_SECTORS,
       "2 TB disk is capped at INSTADD_MAX_SECTORS");

    /* Too small for a partition at all: 0, not a wrapped-around 4 GB. */
    ck(havendos_wholedisk_sectors(2048ull) == 0, "a 1 MB disk has no partition");
    ck(havendos_wholedisk_sectors(0) == 0, "a 0-sector disk has no partition");

    /* The boundary is genuinely the largest FAT16 can take: one megabyte
       more must not work as FAT16, or the boundary is arbitrary rather
       than derived -- and that one megabyte more must work as FAT32, or
       there is a size no installer can format. */
    fat16_geom_t over; memset(&over, 0, sizeof over);
    ck(fat16_geom(INSTADD_FAT16_MAX_SECTORS + INSTADD_ALIGN, &over) == 0,
       "one MB above the boundary has no FAT16 geometry");
    fat32_geom_t over32; memset(&over32, 0, sizeof over32);
    ck(fat32_geom(INSTADD_FAT16_MAX_SECTORS + INSTADD_ALIGN, &over32) == 1,
       "one MB above the boundary has a FAT32 geometry");
    ck(instadd_fs_for(INSTADD_FAT16_MAX_SECTORS) == 16
       && instadd_fs_for(INSTADD_FAT16_MAX_SECTORS + 1) == 32,
       "the boundary itself is FAT16, one sector more is FAT32");
    ck(fat32_geom(INSTADD_MAX_SECTORS, &over32) == 1,
       "the 32 GB cap has a FAT32 geometry");

    printf("%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
