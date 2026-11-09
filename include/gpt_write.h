#pragma once
#include "types.h"
#include "gpt.h"

/* Adding a partition to a GUID Partition Table that already has one on it.
 *
 * fs/gpt.c reads a GPT so the installer can REFUSE a disk. This adds the
 * other half: putting HavenDOS onto free space on that same disk, beside
 * whatever is already there, without disturbing any of it.
 *
 * ── THE BLAST RADIUS, STATED EXACTLY ──────────────────────────────────
 *
 * Adding a partition writes FOUR SECTORS and no others:
 *
 *     1. the ONE sector of the backup entry array holding the new entry
 *     2. the backup header            (the last LBA on the disk)
 *     3. the ONE sector of the primary entry array holding the new entry
 *     4. the primary header           (LBA 1)
 *
 * A 128-entry array is 32 sectors. Thirty-one of them are never written,
 * so "every other partition entry is left untouched" is true by
 * CONSTRUCTION rather than by care: there is no code path here that can
 * put a byte in them. The entry array is READ in full, because the CRC
 * covers all of it, and written one sector at a time.
 *
 * That is also why there is no gpt_write_table() and never should be.
 * Rewriting a table wholesale is the operation that loses somebody's
 * disk, and the way to not have that bug is to not have that function.
 *
 * ── ORDER ─────────────────────────────────────────────────────────────
 *
 * Backup entries, backup header, primary entries, primary header. The
 * primary is what firmware reads, so it changes last: if power is lost at
 * any point, the primary is either wholly old or wholly new, and both are
 * a disk that boots. Neither header is ever zeroed, not even briefly.
 *
 * ── WHAT IT REFUSES ───────────────────────────────────────────────────
 *
 * Before writing anything: both headers must verify by CRC, the entry
 * array must verify by CRC, the requested range must lie inside
 * FirstUsableLBA..LastUsableLBA, it must not overlap any existing
 * partition by a single sector, and there must be a free entry slot. A
 * disk that fails any of those is a disk this code does not understand
 * well enough to write to, and the right response to that is to stop.
 */

/* Error codes. Negative, distinct, and surfaced by the installer so a
   refusal says which one it was rather than "install failed". */
#define GPTW_OK              0
#define GPTW_E_READ        (-1)   /* a sector would not read               */
#define GPTW_E_WRITE       (-2)   /* a sector would not write              */
#define GPTW_E_NOTGPT      (-3)   /* no GPT signature at LBA 1             */
#define GPTW_E_HDRCRC      (-4)   /* primary header fails its own CRC      */
#define GPTW_E_ENTCRC      (-5)   /* entry array fails the header's CRC    */
#define GPTW_E_BACKUP      (-6)   /* backup header missing or disagrees    */
#define GPTW_E_GEOM        (-7)   /* range outside FirstUsable..LastUsable */
#define GPTW_E_OVERLAP     (-8)   /* range touches an existing partition   */
#define GPTW_E_NOSLOT      (-9)   /* every entry slot is in use            */
#define GPTW_E_NOSPACE    (-10)   /* no free gap big enough                */
#define GPTW_E_PARAM      (-11)   /* a caller passed nonsense              */
#define GPTW_E_UNSUPP     (-12)   /* entry size or count we will not touch */
#define GPTW_E_MISMATCH   (-13)   /* the slot is not the partition you named */
#define GPTW_E_EMPTY      (-14)   /* the slot is already unused              */

/* A run of unallocated sectors between partitions, or after the last one. */
typedef struct {
    uint64_t first_lba;      /* inclusive */
    uint64_t last_lba;       /* inclusive */
    uint64_t sectors;
} gpt_gap_t;

/* The type GUID HavenDOS claims for its own partition.
 *
 * Deliberately NOT Microsoft basic data (EBD0A0A2-...). A partition with
 * that type is one Windows mounts, assigns a drive letter and may offer to
 * format; an unknown type is one Windows leaves completely alone, which is
 * what we want a HavenDOS partition to be on somebody's daily-driver
 * laptop. The bytes spell HAVENDOS-PART-01, which will not collide with
 * anybody and says what it is in a hex dump. */
extern const uint8_t GPT_TYPE_HAVENDOS[16];

/* Read callbacks are the same shape fs/gpt.c uses: 0 on success. The write
 * callback matches. A NULL write function makes every call below a DRY RUN:
 * it validates everything and reports what it would have done, and cannot
 * put a byte on the disk. */
typedef int (*gpt_rd_fn)(void *ctx, uint64_t lba, void *buf);
typedef int (*gpt_wr_fn)(void *ctx, uint64_t lba, const void *buf);

/* Find unallocated gaps, largest first. Returns how many were filled in,
 * or a negative GPTW_E_*. */
int gpt_find_gaps(gpt_rd_fn rd, void *ctx, gpt_gap_t *out, int max);

/* Find the first gap of at least `need` sectors. GPTW_OK or a negative
 * code; GPTW_E_NOSPACE when nothing is big enough. */
int gpt_find_free(gpt_rd_fn rd, void *ctx, uint64_t need, gpt_gap_t *out);

/* Add a partition spanning first_lba..last_lba inclusive.
 *
 * Pass wr = NULL to validate without writing — every refusal above is
 * checked and the same code returned, so a dry run that returns GPTW_OK is
 * a real statement that the write would succeed.
 *
 * `written[]`, if given, receives the LBAs actually written (at most 4) and
 * *n_written their count, so a caller — or a test — can assert the blast
 * radius rather than trust it. */
int gpt_add_partition(gpt_rd_fn rd, gpt_wr_fn wr, void *ctx,
                      uint64_t first_lba, uint64_t last_lba,
                      const uint8_t type_guid[16], const char *name,
                      int *entry_index_out,
                      uint64_t *written, int *n_written);

/* Dump both GPT copies to a caller buffer so they can be written to
 * removable media before anything is changed, and put back by hand if the
 * worst happens. Needs (1 + entry_sectors) * 2 * 512 bytes; returns the
 * number of bytes used, or a negative code. */
int gpt_backup_table(gpt_rd_fn rd, void *ctx, uint8_t *buf, uint32_t buflen);

/* ── gpt_remove_partition ────────────────────────────────────────────
 * v0.8.8.8. This header used to say there is no gpt_write_table() and
 * never should be, because "the way to not have that bug is to not
 * have that function". That reasoning was about writing a WHOLE table
 * from memory, and it still stands -- there is still no such call.
 *
 * This is the narrow opposite of gpt_add_partition(): it clears ONE
 * entry and rewrites the same four sectors. It exists because
 * HavenDOS can now create partitions and a thing that can only be
 * undone by hand, in diskpart, with `delete partition override`, is a
 * worse answer than a thing that can undo itself.
 *
 * IT WILL NOT REMOVE A PARTITION YOU CANNOT NAME. The caller passes
 * the slot index AND the type GUID AND the first LBA it believes are
 * there, and all three must match or nothing is written
 * (GPTW_E_MISMATCH). An off-by-one in a caller's loop therefore
 * cannot delete the neighbour -- it has to be wrong about the same
 * partition three different ways.
 *
 * A NULL `wr` is a dry run, exactly as it is for gpt_add_partition:
 * every check runs and nothing is written.
 *
 * The data in the partition is NOT touched. Only the table entry is
 * cleared, so a mistaken removal is recoverable by re-adding an entry
 * with the same range -- which is also why the caller has to know the
 * range to remove it. */
int gpt_remove_partition(gpt_rd_fn rd, gpt_wr_fn wr, void *ctx,
                         int entry_index,
                         const uint8_t expect_type[16],
                         uint64_t expect_first_lba,
                         uint64_t *written, int *n_written);

/* The human-readable reason for a code, for the installer to print. */
const char *gpt_write_strerror(int code);
