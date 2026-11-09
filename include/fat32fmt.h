/* ================================================================
   include/fat32fmt.h -- making a FAT32 volume, for an ESP we own
   ================================================================
   v0.8.8.8.  HavenDOS could only install where somebody else had
   already made an EFI System Partition.  fs/esp.c finds one, checks
   it, and writes into it; if there is none, esp_find() returns
   ESP_E_NOESP and the install stops.  In practice that meant: there
   must be a Windows on the disk.

   Creating our own ESP needs a FAT32 writer, because fs/esp.c writes
   through fs/fat32.c and because the UEFI specification's filesystem
   for a partition on a fixed disk is FAT32.  Firmware will usually
   read a FAT16 ESP -- most installer ISOs ship one -- but "usually"
   is not a property to build a boot path on when the failure is a
   machine that will not start.

   WHY THE GEOMETRY IS COMPUTED, AGAIN.  include/fat16fmt.h tells the
   story of a FAT sized by a constant while the BPB described the real
   partition, and of a volume that worked only because nothing ever
   allocated far enough into it to notice.  The arithmetic here is
   circular in the same way -- the FAT's size depends on the cluster
   count, which depends on the data area, which depends on the FAT's
   size -- and it is iterated to a fixed point for the same reason.

   This header carries no I/O.  fat32_geom() is arithmetic, so
   tests/test_fat32fmt.c can check it against fsck.fat and mtools
   across many partition sizes without a disk anywhere.
   ================================================================ */
#ifndef FAT32FMT_H
#define FAT32FMT_H
#include "types.h"

/* Specification bounds.  At or below the first it is FAT16 -- and a
   volume whose cluster count lands under it is not "a small FAT32",
   it is a FAT16 with the wrong string in the BPB, which is exactly
   the class of bug fat16fmt.h was written about. */
#define FAT32_MIN_CLUSTERS  65525u
#define FAT32_MAX_CLUSTERS  268435444u

/* 32 reserved sectors is what every mkfs writes and what the backup
   boot sector at sector 6 and the FSInfo at sector 1 assume. */
#define FAT32_RSVD_SECTS    32u
#define FAT32_BACKUP_BOOT   6u
#define FAT32_FSINFO_SECT   1u

typedef struct {
    uint32_t part_sectors;      /* what was asked for                   */
    uint8_t  sects_per_clust;
    uint8_t  num_fats;          /* 2                                     */
    uint16_t rsvd_sects;        /* FAT32_RSVD_SECTS                      */
    uint32_t sects_per_fat;     /* computed, not assumed                 */
    uint32_t fat1_start;        /* all partition-relative                */
    uint32_t fat2_start;
    uint32_t data_start;
    uint32_t root_clust;        /* 2                                     */
    uint32_t clusters;          /* usable data clusters, 2..clusters+1   */
} fat32_geom_t;

/* Fill g for a partition of part_sectors 512-byte sectors.
   Returns 1 if a valid FAT32 exists at that size, 0 if it does not:
   too small to reach FAT32_MIN_CLUSTERS at any cluster size, or too
   large.  A caller that formats anyway produces a volume whose BPB
   lies about what it is. */
int fat32_geom(uint32_t part_sectors, fat32_geom_t *g);

/* Write a volume: boot sector, its backup, FSInfo and its backup,
   both FATs, and an empty root cluster carrying the volume label.
   Partition-relative LBAs, through wr.

   hidden_sects is the partition's own start LBA on the disk, which
   goes in the BPB field of that name.

   volume_id is the serial.  It is a parameter and not a call to a
   clock inside this file so that the same inputs produce the same
   bytes -- a formatter whose output changes run to run cannot be
   compared against a fixture.

   Returns 1 on success, 0 on the first failed write. */
typedef int (*fat32_wr_fn)(void *ctx, uint32_t part_lba, const uint8_t *buf);

int fat32_format(fat32_wr_fn wr, void *ctx, const fat32_geom_t *g,
                 uint32_t hidden_sects, uint32_t volume_id,
                 const char *label);

#endif /* FAT32FMT_H */
