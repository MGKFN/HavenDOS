#pragma once
#include "types.h"

/* FAT32 for HavenDOS.
 *
 * This exists because an EFI System Partition is FAT32 and HavenDOS had
 * FAT12 and FAT16 only.  Everything in the 1.0 plan that installs beside
 * another operating system goes through an ESP, so this is the floor the
 * rest of it stands on.  USB sticks and anything else formatted this
 * decade are the same filesystem, which is the second reason.
 *
 * It is a SEPARATE DRIVER, not a generalisation of fs/fat16.c.  FAT16
 * works today and is what every installed HavenDOS boots from; folding the
 * two together to save duplication would put that code on the critical
 * path of a new filesystem for no user-visible gain.  Factor them later,
 * if ever.
 *
 * The differences that actually bite, all of them handled here:
 *   - BPB_FATSz16 is zero and the real size is in BPB_FATSz32.  Offset 36
 *     is only that field on FAT32; on FAT12/16 it is the start of the
 *     extended boot record, so the 16-bit field has to be consulted FIRST
 *     and the 32-bit one read only when it is zero.  Reading them the
 *     other way round reports a 16 MB FAT16 volume as having a FAT of
 *     1529413760 sectors.  Same shape of trap at offset 32 for the total
 *     sector count.
 *   - BPB_RootEntCnt is zero and the root is an ordinary cluster chain
 *     starting at BPB_RootClus, not a fixed region.  A root directory can
 *     therefore grow, and code that assumes a fixed root_sects span finds
 *     nothing past the first cluster.
 *   - FAT entries are 32 bits of which ONLY THE LOW 28 ARE THE CLUSTER.
 *     The top four bits are reserved and must be preserved on write and
 *     masked off on read, or chains look broken.
 *   - The FAT is too big to cache whole.  fs/fat16.c holds all of it in a
 *     128 KB array; the FAT on this fixture's 64 MB volume is 497 KB and
 *     on a real ESP it is larger still.  This driver keeps a one-sector
 *     window with write-back instead.
 *   - FSInfo carries a free-cluster count and a next-free hint that go
 *     stale if written clusters are not accounted for.  A stale free count
 *     is how a volume comes back wrong under Windows.
 */

/* fat_probe_type() return values. */
#define FATTYPE_NONE 0
#define FATTYPE_12   12
#define FATTYPE_16   16
#define FATTYPE_32   32
/* ── v0.8.8.1: NOT A FAT TYPE, and deliberately numbered away from
   the others. It is a value for fat_scan_volumes()'s `want` filter,
   so that `exfat` reuses the ONE partition walk in shell/fat32cmd.c
   rather than growing a second one. fat_probe_type() never returns
   it; it cannot, because an exFAT volume has no BPB to count.
   Walking the partition tables twice is how the three bus-dispatch
   switches v0.8.6 deleted began, and this is the cheaper mistake to
   avoid than the naming is to keep pure. */
#define FATTYPE_EXFAT 99

/* The type of a FAT volume is defined by its COUNT OF DATA CLUSTERS and by
 * nothing else: under 4085 is FAT12, under 65525 is FAT16, otherwise FAT32.
 * The partition type byte and the "FAT32   " string in the boot sector are
 * both routinely wrong on real media and neither is consulted here.
 * Takes a 512-byte boot sector.  Returns one of FATTYPE_*.
 */
int fat_probe_type(const uint8_t *boot_sector);

/* What a boot sector IS, as a word for a person: "FAT32", "FAT16",
 * "exFAT", "NTFS", "not formatted", "unknown"...  Never 0.
 *
 * Added in v0.8.6.1 because `fat32` reported "none found" for a 64 GB
 * USB stick that is simply exFAT, which left "this kernel cannot read
 * that filesystem" looking identical to "this kernel cannot see the
 * disk". Naming it does NOT mean it can be read -- fat_probe_type() is
 * what decides that. */
const char *fat_fs_name(const uint8_t *boot_sector);

/* The block device this filesystem sits on.  Both return 1 on success and
 * 0 on failure, matching the rd()/wr() convention in fs/fat16.c.
 *
 * It is a parameter rather than a compiled-in choice because an ESP is
 * wherever the partition table says it is, on whichever controller the
 * machine happens to have.  fs/fat16.c hardcodes LBA 2048 on VirtIO-or-ATA,
 * which is correct for a disk HavenDOS installed itself and useless for one
 * it did not.  Phase D's USB mass storage wires in here and nowhere else.
 */
typedef int (*fat32_read_fn )(void *ctx, uint64_t lba, uint8_t *buf);
typedef int (*fat32_write_fn)(void *ctx, uint64_t lba, const uint8_t *buf);

/* Mount the FAT32 volume whose boot sector is at part_lba.  Pass a NULL
 * write function for a read-only mount; every write entry point then
 * refuses rather than half-succeeding.  Returns 1 on success. */
int  fat32_mount_dev(fat32_read_fn rd, fat32_write_fn wr, void *ctx,
                     uint64_t part_lba);

/* Probe the local disks (VirtIO, then ATA) for an MBR partition of type
 * 0x0B or 0x0C that really is FAT32, and mount the first one.  Returns 1
 * if a volume is mounted.  NOT called on live media -- see vfs_init(). */
int  fat32_init(void);

int  fat32_detected(void);
void fat32_unmount(void);
int  fat32_flush(void);           /* FAT window + FSInfo to disk */

/* Geometry, for `disk` and for tests.  Zero when nothing is mounted. */
uint32_t fat32_total_clusters(void);
uint32_t fat32_free_clusters(void);
uint32_t fat32_cluster_bytes(void);
uint32_t fat32_root_cluster(void);

/* File API.  Paths are absolute or root-relative and may contain '/'.
 * Unlike fs/fat16.c there is no separate flat-root variant: the root is a
 * cluster chain like any other directory, so there is nothing for a flat
 * version to be faster at, and two entry points per operation is how
 * fat16_write() ended up writing "BOOT/GRU" into a root directory. */
int      fat32_read     (const char *path, char *buf, uint32_t maxlen);
int      fat32_read_at  (const char *path, uint32_t off, char *buf, uint32_t len);
uint32_t fat32_file_size(const char *path);
int      fat32_write    (const char *path, const char *buf, uint32_t len);
int      fat32_delete   (const char *path);
int      fat32_exists   (const char *path);
int      fat32_is_dir   (const char *path);
int      fat32_mkdir    (const char *path);
/* Removes an EMPTY directory. Refuses one with anything live in it,
   refuses a file (that is fat32_delete) and refuses the root. */
int      fat32_rmdir    (const char *path);
void     fat32_list     (void (*cb)(const char *name, int is_dir, uint32_t size));
void     fat32_list_dir (const char *dir,
                         void (*cb)(const char *name, int is_dir, uint32_t size));
