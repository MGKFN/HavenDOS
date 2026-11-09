/* ================================================================
   include/fat16fmt.h -- FAT16 geometry, computed instead of assumed
   ================================================================
   fs/install.c formatted every partition with a FAT of exactly 128
   sectors -- 65536 bytes, 32766 usable entries -- while writing the
   partition's REAL sector count into the BPB's total-sectors field.
   Those two numbers only agree on a partition of about 64 MB or less.

   On anything larger the volume is malformed from the moment it is
   created, because a FAT16 driver derives its cluster count from the
   BPB:

       clusters = (total_sectors - data_start) / sectors_per_cluster

   A 1 GB partition yields 524,215 clusters against a FAT that can
   address 32,766, and 524,215 is not a FAT16 cluster count at all --
   the specification's upper bound is 65,524. fsck.vfat says so
   plainly:

       Too many clusters (524215) for FAT16 filesystem.

   It has never been noticed because a fresh install allocates a few
   hundred clusters at the very start of the volume, so nothing ever
   reaches the part of the FAT that does not exist. The volume works
   by accident, and Windows shown such a partition reports it as
   corrupt and offers to format it.

   So the geometry is computed here, from the partition size, once,
   for both the whole-disk installer and the additive one. The
   computation is circular -- the FAT's size depends on the cluster
   count, which depends on the data area, which depends on the FAT's
   size -- so it is iterated to a fixed point, which is what every
   mkfs does.

   This header carries no I/O. fat16_geom() is arithmetic and nothing
   else, which is why tests/test_fat16fmt.c can check it against
   fsck.vfat across sixty partition sizes without a disk.
   ================================================================ */
#ifndef FAT16FMT_H
#define FAT16FMT_H
#include "types.h"

/* Specification bounds. Below the first it is FAT12, above the second
   it is FAT32; in both cases it is not this filesystem, whatever the
   BPB's "FAT16   " string says. */
#define FAT16_MIN_CLUSTERS  4085u
#define FAT16_MAX_CLUSTERS  65524u

typedef struct {
    uint32_t part_sectors;      /* what was asked for                    */
    uint8_t  sects_per_clust;   /* chosen: the smallest that fits        */
    uint8_t  num_fats;          /* 2                                      */
    uint16_t rsvd_sects;        /* 1 -- the boot sector                   */
    uint16_t root_ents;         /* 512                                    */
    uint32_t sects_per_fat;     /* computed, not assumed                  */
    uint32_t fat1_start;        /* all of these are partition-relative    */
    uint32_t fat2_start;
    uint32_t root_start;
    uint32_t root_sects;
    uint32_t data_start;
    uint32_t clusters;          /* usable data clusters, 2..clusters+1    */
} fat16_geom_t;

/* Fill g for a partition of part_sectors 512-byte sectors.
   Returns 1 if a valid FAT16 exists at that size, 0 if it does not --
   too small to be FAT16 (use FAT12) or too large for any cluster size
   (use FAT32). A caller that ignores the return value and formats
   anyway produces exactly the volume this file exists to prevent. */
int fat16_geom(uint32_t part_sectors, fat16_geom_t *g);

/* Write a volume: boot sector, both FATs, the root directory, and the
   volume label entry. Partition-relative LBAs, through wr.

   hidden_sects is the partition's own start LBA on the disk, which
   goes in the BPB field of that name. It is what tells a tool reading
   the volume in isolation where it sits, and the whole-disk installer
   and the additive one pass different values -- 2048 and wherever the
   GPT gap begins.

   Returns 1 on success, 0 on the first failed write. */
typedef int (*fat16_wr_fn)(void *ctx, uint32_t part_lba, const uint8_t *buf);

int fat16_format(fat16_wr_fn wr, void *ctx, const fat16_geom_t *g,
                 uint32_t hidden_sects, const char *label);

#endif /* FAT16FMT_H */
