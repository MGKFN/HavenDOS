/* ================================================================
   include/install_add.h -- installing WITHOUT replacing anything
   ================================================================
   havendos_install_to_drive() takes a whole disk. Its first act is to
   zero LBA 0..63 and the last 33 sectors, which on a GPT disk is the
   primary header, the entry array and the backup header -- the entire
   partition table, both copies. That is why it refuses a GPT disk
   outright (-14): on a dual-boot machine those sectors are somebody
   else's operating system.

   This is the other installer. It adds a partition to a table that
   already exists and writes nothing else outside it. The guarantee it
   is built to make, and the one worth stating in exactly these terms:

       OUTSIDE the new partition, FOUR SECTORS are written. Their
       absolute LBAs are in the report below, before anything is
       written, and they are the two GPT headers and the one sector of
       each entry array that holds the new entry. No other sector
       outside the new partition is read-modify-written, zeroed, or
       touched in any way.

       INSIDE the new partition, everything is written -- it is free
       space, which is the point.

       ON THE EFI SYSTEM PARTITION, files are added beside the ones
       already there. Nothing is deleted. Nothing is overwritten
       without a copy being taken first. See include/esp.h.

   A dry run performs every check, fills in the whole report, and
   cannot write: the write function handed to fs/gpt_write.c is NULL
   and every other write is gated at one place. The preview and the
   real thing are the same code path, which is the only way the
   preview is worth reading.
   ================================================================ */
#ifndef INSTALL_ADD_H
#define INSTALL_ADD_H
#include "types.h"
#include "fat16fmt.h"
#include "esp.h"

/* Partitions are aligned to this, in sectors: 1 MB, which is what every
   partitioning tool made since about 2010 uses. An unaligned partition
   works and is slower on every SSD, and looks wrong in Windows' own disk
   manager, which is its own kind of alarming. */
#define INSTADD_ALIGN   2048u

/* The largest volume this installer will create, in sectors. FAT16 with
   32 KB clusters tops out just under 2 GB; see include/fat16fmt.h for why
   64 KB clusters are not used to reach further. A gap larger than this is
   not refused -- this much is taken from the start of it and the rest is
   left free. */
#define INSTADD_MAX_SECTORS  4192256u      /* 2047 MB */

/* And the smallest. FAT16 needs 4085 clusters to be FAT16 at all, and the
   kernel is well over a megabyte. */
#define INSTADD_MIN_SECTORS  32768u        /* 16 MB */

typedef struct {
    int      plan_ok;          /* a complete plan was formed             */
    int      disk;

    /* the partition that would be created */
    uint64_t part_first;       /* absolute LBA, aligned                  */
    uint64_t part_last;        /* absolute LBA, inclusive                */
    uint64_t part_sectors;
    int      gpt_entry;        /* which entry slot it would claim        */

    /* the gap it was taken from, which may be larger */
    uint64_t gap_first, gap_last, gap_sectors;

    /* the volume inside it */
    fat16_geom_t geom;
    uint32_t vol_free_bytes;   /* what the volume would hold             */
    uint32_t vol_need_bytes;   /* what we would put in it                */

    /* THE SECTORS OUTSIDE THE NEW PARTITION THAT WOULD BE WRITTEN.
       Four, always, in the order fs/gpt_write.c writes them: backup
       entries, backup header, primary entries, primary header. */
    uint64_t outside[4];
    int      n_outside;

    /* the EFI System Partition */
    int          esp_rc;       /* ESP_OK, or why not                     */
    esp_report_t esp;

    /* ── v0.8.8.8: WE MAY HAVE TO MAKE THE ESP OURSELVES ──────────
       Until now the install required an EFI System Partition that
       already existed, which in practice meant it required Windows.
       On a disk with none, esp_find() returned ESP_E_NOESP and the
       install stopped.

       When there is no ESP, one is carved off the FRONT of the same
       gap HavenDOS is going into and the HavenDOS partition starts
       after it. These fields say whether that happened and where, so
       the preview can show it before anything is written -- a person
       about to have their partition table changed should see both
       partitions, not one. */
    int          esp_created;      /* we made it, it was not there    */
    uint64_t     esp_new_first;    /* ...here                          */
    uint64_t     esp_new_sectors;
    int          esp_new_entry;    /* its GPT entry index              */
    int          espmk_rc;         /* ESPMK_OK, or why not             */

    /* What else lives on this disk, from espmk_survey(). Reported so
       the installer can say "this disk has Windows on it" out loud
       rather than the person finding out afterwards. */
    int          disk_has_windows;
    int          disk_has_linux;
    int          disk_n_unused;    /* unknown, empty partitions seen   */

    uint32_t kernel_bytes;
} inst_add_t;

#define INSTADD_OK            0
#define INSTADD_E_DISK      (-1)   /* no such disk                        */
#define INSTADD_E_NOGPT     (-2)   /* no GPT -- use the whole-disk path   */
#define INSTADD_E_NOSPACE   (-3)   /* no unallocated gap big enough       */
#define INSTADD_E_TOOSMALL  (-4)   /* the gap is under INSTADD_MIN_SECTORS*/
#define INSTADD_E_GEOM      (-5)   /* no FAT16 geometry fits it           */
#define INSTADD_E_ENTRY     (-6)   /* every GPT entry slot is in use      */
#define INSTADD_E_GPTWRITE  (-7)   /* the table write failed              */
#define INSTADD_E_FORMAT    (-8)   /* the format failed                   */
#define INSTADD_E_FILES     (-9)   /* a file write failed                 */
#define INSTADD_E_ESP      (-10)   /* the ESP step failed; see esp_rc     */
#define INSTADD_E_NOIMAGE  (-11)   /* this kernel carries no kernel image */
#define INSTADD_E_READONLY (-12)   /* no write path to this disk (NVMe)   */
#define INSTADD_E_LIVE     (-13)   /* a live session may preview, not write */
#define INSTADD_E_VERIFY   (-14)   /* written, read back, did not match   */
#define INSTADD_E_VOLSPACE (-15)   /* the volume cannot hold the kernel   */
/* A HavenDOS partition is already on this disk.
   
   This used to come out as INSTADD_E_NOSPACE, because after a successful
   install the gap really is gone -- and "no free space on this disk --
   shrink a partition in Windows first" is a true sentence that reads like
   the disk has been damaged. It sent someone looking for a fault in a
   machine that had just worked. The specific answer is the useful one. */
#define INSTADD_E_ALREADY  (-16)

/* v0.8.8.8. The disk has no EFI System Partition and one could not
   be made -- distinct from INSTADD_E_ESP, which means an ESP exists
   and the install into it was refused. Two different problems with
   two different answers, so two codes. */
#define INSTADD_E_ESPMAKE  (-17)

/* dry_run != 0: every check, the whole report, not one byte written.
   rep may not be NULL -- the report is the deliverable, not a courtesy. */
/* Is a HavenDOS partition already on this disk? Returns its entry index,
   or -1. Used by the installer and by the drive picker, which shows it as
   information rather than as a refusal. */
int havendos_find_existing(int disk);

int havendos_install_additive(int disk, int dry_run, inst_add_t *rep,
                              void (*cb)(int,int,const char*));

const char *havendos_install_add_strerror(int code);

/* The same codes, at most 30 characters, for the Notes column of the drive
   picker -- which begins at column 42 of an 80-column screen. The long
   forms run to 72 characters and would be written off the edge, so there
   are two sets rather than one set that happens to fit the narrower place.
   tools/check_screen_width.py is what noticed. */
#define INSTADD_SHORT_MAX 30
const char *havendos_install_add_shortstr(int code);

#endif /* INSTALL_ADD_H */
