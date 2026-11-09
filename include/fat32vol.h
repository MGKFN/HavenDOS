#pragma once
#include "types.h"

/* Finding FAT32 volumes on the machine's disks.
 *
 * This is the partition-scan half of Phase A: the part that knows an MBR
 * type byte of 0x0B or 0x0C is a FAT32 hint, that the EFI System Partition
 * GUID in a GPT is the same hint on a UEFI disk, and that NEITHER of them
 * is an answer -- every candidate's boot sector is read and its cluster
 * count counted before it is called FAT32, because the type byte and the
 * "FAT32   " string are both routinely wrong on real media.
 *
 * It is separated from the `fat32` command so Phase B can use it.  B2 is
 * "mount the existing EFI System Partition -- the one already there, the
 * one Windows uses", and this is how that partition gets found.  Creating a
 * second ESP is legal and is the wrong answer; firmware will usually pick
 * the first and the install will look like it failed.
 *
 * Nothing here writes.  fat32_vol_mount_ro() hands fs/fat32.c a NULL write
 * function, so the refusal is in the driver rather than in the discipline
 * of whoever calls this.  That matters because the partition most likely to
 * come back from a scan belongs to somebody else's operating system.
 */

#define FAT32_MAX_VOLS 12

typedef struct {
    int      disk;        /* index into the installer's unified disk table */
    uint64_t lba;         /* first sector of the volume                    */
    uint64_t sectors;
    int      from_gpt;    /* 1 if the entry came from a GPT, 0 from an MBR */
    int      is_esp;      /* the EFI System Partition                      */
    uint8_t  mbr_type;    /* the type byte, 0 for a GPT entry              */
    uint8_t  fattype;     /* FATTYPE_12 / _16 / _32, from the cluster count
                             rather than from the type byte -- v0.8.6.1,
                             so one partition walk serves `fat32` AND
                             `fat16` instead of each growing its own.
                             Walking the tables twice is how the three
                             bus-dispatch switches v0.8.6 deleted began. */
    char     label[12];   /* BPB_VolLab -- the CONVENIENCE COPY, which is
                             allowed to disagree with the root directory's
                             volume-label entry, and on disks 0.8 Beta's
                             installer produced it did */
} fat32_vol_t;

/* Scan every disk and fill out[] with the FAT32 volumes found.  Returns how
 * many.  Runs the installer's disk scan itself, so it sees exactly the
 * disks the installer sees -- a second enumeration with its own indices is
 * the three-installers bug. */
int fat32_scan_volumes(fat32_vol_t *out, int max);

/* Every FAT volume of one type, from the same single walk.
 * `want` is FATTYPE_16 or FATTYPE_32; FATTYPE_NONE means every type.
 * v0.8.6.1, for the `fat16` command. */
int fat_scan_volumes(int want, fat32_vol_t *out, int max);

/* Every exFAT volume, from that same single walk. The same shape as
 * fat32_scan_volumes() so fs/update_src.c can try one and then the
 * other without knowing where either walk lives. v0.8.8.2. */
int exfat_scan_volumes(fat32_vol_t *out, int max);

/* Mount volume v READ ONLY through fs/fat32.c.  Returns 1 on success.
 * The caller must pair this with fat32_vol_release(). */
int fat32_vol_mount_ro(const fat32_vol_t *v);

/* The same, for an exFAT volume found with want = FATTYPE_EXFAT.
   v0.8.8.1. Read-only with no way to ask otherwise -- see
   include/exfat.h on why the write path is a release of its own. */
int exfat_vol_mount_ro(const fat32_vol_t *v);

/* Unmount, and put back whatever the VFS had mounted.  fs/fat32.c holds one
 * volume at a time, so borrowing it displaces the system's own. */
void fat32_vol_release(void);
