/* include/migrate.h -- v0.9.6: moving an installed HavenDOS to FAT32.
 *
 * Every HavenDOS installed before 0.9.3 is FAT16, and FAT16 ends at
 * 2047 MB. `migrate` moves a running FAT16 install onto a new FAT32
 * partition on the same disk -- into unallocated space, or into an empty
 * partition the owner has made for it -- carries the accounts, settings
 * and files across, and puts BOTH in the boot menu: the new one first,
 * the old one as a fallback. Nothing is removed until the owner has booted
 * the new one and says so with `migrate finish`, which deletes the old
 * partition and takes it out of the menu.
 *
 * Two halves. fs/migrate_plan.c is pure -- names, menus, what a partition's
 * first sectors say it holds -- and is what tests/test_migrate.c links.
 * fs/migrate.c is the kernel half: the table, the volume, the copy.
 */
#ifndef MIGRATE_H
#define MIGRATE_H
#include "types.h"

/* Nothing smaller is worth moving to. 512 MB. */
#define MIG_MIN_SECTORS   (1048576ull)
#define MIG_ALIGN         2048u
#define MIG_MAX_TARGETS   8

/* ── the pure half (fs/migrate_plan.c) ─────────────────────────────── */

/* The files the system keeps under names FAT16 cannot hold (every one a
   dotfile), NULL-terminated. Read from FAT16 by these names -- the driver
   maps them -- and written to FAT32 under them, where the system will ask
   for them. tools/check_migrate_names.py fails the build if the source
   uses a name that is not 8.3-legal and is not in this list. */
extern const char *const MIG_LONG_NAMES[];

/* FAT16's names for a dotfile, as fat16_list() prints them: the 0.9.3 form
   ("_account") and the one before it (".acc"). out must hold 13. */
void mig_fat16_name(const char *real, char *out);
void mig_fat16_legacy_name(const char *real, char *out);
/* 1 if `listed` (a root entry as FAT16 lists it) is one of the above, so
   the copy has already carried it under its real name. */
int  mig_is_mapped_name(const char *listed);

/* What the first sectors of a partition say it holds: a filesystem's name,
   or NULL for nothing recognisable. s0 is its sector 0, s2 its sector 2
   (where ext2/3/4 keep their magic), s7 its sector 7 (a swap signature),
   s128 its sector 128 (btrfs). Any may be NULL if it could not be read --
   which the caller treats as "not empty". */
const char *mig_fs_signature(const uint8_t *s0, const uint8_t *s2,
                             const uint8_t *s7, const uint8_t *s128);

/* May `migrate` take a partition of this type at all? Microsoft basic data
   and Linux filesystem data -- what a disk tool makes when asked for "a new
   partition" -- and nothing else: never the ESP, Microsoft reserved,
   recovery, or a HavenDOS partition. */
int  mig_type_takeable(const uint8_t type_guid[16]);
const char *mig_type_name(const uint8_t type_guid[16]);

/* The volume serial as GRUB's --fs-uuid and the kernel's vol= spell it:
   "xxxx-xxxx". out must hold 10. */
void mig_serial_str(uint32_t serial, char *out);

/* The ESP's grub.cfg. old_serial 0 leaves the old FAT16 entry out (after
   `migrate finish`). Returns the length, or -1 if it does not fit. */
int  mig_esp_menu(char *out, int cap, uint32_t new_serial, uint32_t old_serial);
/* The menu on the new partition itself. */
int  mig_part_menu(char *out, int cap, uint32_t serial);

/* ── the kernel half (fs/migrate.c) ────────────────────────────────── */

#define MIG_T_GAP    1   /* unallocated space                            */
#define MIG_T_PART   2   /* an existing partition with no filesystem     */

typedef struct {
    int      kind;
    uint64_t first, last, sectors;   /* aligned; what the volume would get */
    int      slot;                   /* MIG_T_PART: its table slot         */
    uint8_t  type_guid[16];          /* MIG_T_PART: its type, to match     */
    char     name[37];               /* MIG_T_PART: its GPT name           */
    int      blank;                  /* MIG_T_PART: its first 64 sectors are
                                        all zero -- never used, not merely
                                        unformatted                        */
} mig_target_t;

typedef struct {
    int      disk;
    int      sys_fs;                 /* 16 or 32, 0 if no disk system     */
    uint64_t sys_first, sys_sectors;
    int      sys_slot;
    uint32_t sys_serial;
    int      has_esp;
    int      n_targets;
    mig_target_t targets[MIG_MAX_TARGETS];
    /* what was looked at and refused, for the screen */
    int      n_refused;
    struct { int slot; uint64_t sectors; char why[40]; } refused[MIG_MAX_TARGETS];
    /* when the system is FAT32: the old FAT16 install `finish` would remove */
    int      old_slot;               /* -1 if none                         */
    uint64_t old_first, old_sectors;
    uint32_t old_serial;
    int      n_old;                  /* how many FAT16 HavenDOS partitions */
    int      n_moved;                /* on FAT16: FAT32 HavenDOS ones -- a
                                        move done and not yet booted       */
    int      booted_by_serial;       /* the boot line names this volume    */
} mig_survey_t;

typedef struct {
    int      rc;
    int      step;                   /* where it stopped                   */
    uint64_t first, sectors;
    uint32_t serial;
    int      slot;
    int      n_files, n_dirs;
    uint32_t bytes;
    int      n_skipped;
    char     skipped[4][48];         /* the first few, by name             */
    int      esp_rc;
    uint32_t freed_mb;               /* finish: what came back            */
} mig_result_t;

#define MIG_OK            0
#define MIG_E_NOSYS     (-1)   /* not running from an installed HavenDOS   */
#define MIG_E_NOGPT     (-2)   /* the system disk has no GPT               */
#define MIG_E_NOESP     (-3)   /* no EFI System Partition                  */
#define MIG_E_ALREADY32 (-4)   /* already FAT32                            */
#define MIG_E_TARGET    (-5)   /* that target is not one of the survey's   */
#define MIG_E_ESPPLAN   (-6)   /* the boot menu could not be written: dry   */
#define MIG_E_TABLE     (-7)   /* the partition table change failed        */
#define MIG_E_INSTALL   (-8)   /* formatting or the kernel copy failed     */
#define MIG_E_COPY      (-9)   /* a file did not copy                      */
#define MIG_E_ESP      (-10)   /* the boot menu write failed               */
#define MIG_E_NOMEM    (-11)   /* no buffer for the copy                   */
#define MIG_E_NOT32    (-12)   /* finish: not running from FAT32           */
#define MIG_E_NOTBOOTED (-13)  /* finish: not booted by the new menu entry */
#define MIG_E_NOOLD    (-14)   /* finish: no old FAT16 install to remove   */
#define MIG_E_MANYOLD  (-15)   /* finish: more than one -- refuses to pick  */
#define MIG_E_READONLY (-16)   /* no write path to this disk              */
#define MIG_E_TWO      (-17)   /* a move is already done; boot it, finish  */

typedef void (*mig_cb)(int step, int total, const char *what);

int mig_survey(mig_survey_t *s);
int mig_run(const mig_survey_t *s, int target, mig_cb cb, mig_result_t *r);
int mig_finish(const mig_survey_t *s, mig_cb cb, mig_result_t *r);
const char *mig_strerror(int rc);

#endif
