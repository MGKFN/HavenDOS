#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/exfat.h -- exFAT, read-only.  v0.8.8.1.

   ── WHY THIS EXISTS ──────────────────────────────────────────────────

   A USB stick over 32 GB ships formatted exFAT, because that is what
   Windows does when you right-click Format on one. v0.8.6.1 taught
   `fat32` to NAME such a volume instead of printing "none found" --
   the honest thing to do with no driver, and it turned "the stick is
   not readable" back into a question with an answer. From then until
   now that is all HavenDOS could do with one: say what it was and
   refuse it.

   Reported from the HP on 31 October as "USB drives still not always
   recognised". The drive was recognised. It was unreadable, which
   looks the same from the outside.

   ── READ-ONLY, AND THAT IS NOT A STAGING POST ────────────────────────

   Nothing here writes, and the interface has no way to ask it to. That
   is deliberate rather than unfinished.

   exFAT's allocation state lives in TWO places that have to agree: the
   FAT, and an allocation bitmap in its own cluster chain. A writer
   must update both, and must also maintain the checksum over each
   directory entry SET. fs/fat16.c's writable borrow is this tree's
   record of how much care a write path needs on a filesystem whose
   structures are far simpler; the one guarantee this project makes
   about somebody's disk -- four sectors outside the new partition --
   is worth more than the convenience of writing to a stick.

   So the read path ships first and alone, and when a write path is
   written it will be a release of its own with the bitmap under test.

   ── WHAT IS AND IS NOT IMPLEMENTED ───────────────────────────────────

   Implemented: the boot sector, the FAT, the cluster heap, the
   directory entry set (0x85 file / 0xC0 stream / 0xC1 name), both
   cluster-walk forms -- a FAT chain AND the contiguous run the
   NoFatChain flag selects -- subdirectories to any depth the path
   buffer allows, and names of any length.

   NOT implemented, and each is refused by name rather than
   misunderstood:
     - the up-case table, so name matching is ASCII case-folded here
       rather than by the table on the volume. A name differing from
       what is on disk only outside ASCII will not be found.
     - TexFAT (the transactional variant, FileSystemRevision major 2).
     - a volume whose sector size is not 512, which is every USB stick
       made so far but is not every device.
   ════════════════════════════════════════════════════════════════════ */

/* The same read/write function shape fs/fat32.c takes, so a caller
   that already has a block device plumbed to one has it plumbed to
   both. The write slot is absent on purpose -- see above. */
typedef int (*exfat_read_fn)(void *ctx, uint64_t lba, uint8_t *buf);

/* ── refusals, by name ───────────────────────────────────────────────
   Not a bare zero. v0.8.7.7 spent a release on what a nameless
   refusal costs: a caller cannot tell "this is not exFAT" from "this
   is exFAT and I will not read it", and those send the reader to two
   different places. */
#define EXFAT_OK            0
#define EXFAT_ENOTEXFAT     1   /* no EXFAT signature in the boot sector */
#define EXFAT_EREVISION     2   /* a revision this reader does not know  */
#define EXFAT_ESECTORSIZE   3   /* sectors are not 512 bytes             */
#define EXFAT_EGEOMETRY     4   /* the boot sector contradicts itself    */
#define EXFAT_EIO           5   /* the device did not answer             */
#define EXFAT_ENOTMOUNTED   6
#define EXFAT_ENOTFOUND     7
#define EXFAT_EISDIR        8
#define EXFAT_ENOTDIR       9
#define EXFAT_EREASONS     10

int         exfat_mount_dev(exfat_read_fn rd, void *ctx, uint64_t part_lba);
void        exfat_unmount(void);
int         exfat_mounted(void);
int         exfat_why(void);          /* why the last mount refused */
const char *exfat_why_name(int why);

/* Geometry, for a screen that reports what it found. */
uint32_t exfat_cluster_bytes(void);
uint32_t exfat_cluster_count(void);
uint32_t exfat_root_cluster(void);
uint64_t exfat_volume_bytes(void);

/* ── listing ─────────────────────────────────────────────────────────
   One entry at a time through a cursor, rather than a callback or a
   table: a directory on a 2 TB volume has no bound worth allocating
   for, and the shell wants to page. `path` is "" or "/" for the root.

   Returns 1 while there is an entry, 0 at the end, negative EXFAT_E*
   on a failure -- and the three are distinguishable, which a function
   returning a pointer would not make them. */
typedef struct {
    char     name[256];
    uint64_t size;
    uint32_t first_cluster;
    int      is_dir;
    int      no_fat_chain;   /* the run is contiguous; see the header */
} exfat_dirent_t;

typedef struct {
    uint32_t cluster;        /* cluster being walked                  */
    uint32_t sector_in;      /* sector within that cluster            */
    uint32_t off;            /* byte offset within that sector        */
    int      started;
    int      done;
} exfat_dir_t;

int exfat_opendir(const char *path, exfat_dir_t *d);
int exfat_readdir(exfat_dir_t *d, exfat_dirent_t *out);

/* Look one path up without walking it twice. */
int exfat_stat(const char *path, exfat_dirent_t *out);

/* Read `len` bytes at `off` from a file. Returns bytes read, or a
   negative EXFAT_E*. Short reads at end-of-file are normal and are
   not a failure. */
int exfat_read(const char *path, uint64_t off, void *buf, uint32_t len);
