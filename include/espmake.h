/* ================================================================
   include/espmake.h -- HavenDOS making its OWN boot partition
   ================================================================
   v0.8.8.8.  Until now the install path was: find the EFI System
   Partition somebody else made -- in practice Windows' -- and write
   the loader into it.  With no ESP, esp_find() returns ESP_E_NOESP
   and that is the end of the install.  HavenDOS could not be put on
   a disk that had never had another operating system on it.

   Asked for directly: "without anything on disk detect an unused
   partition and use that as free space and detect windows or linux
   and then make its own grub or UEFI bios boot entry without needing
   other OS like BCDEdit stuff".

   ── WHAT THIS DOES AND DOES NOT DECIDE ──────────────────────────────

   It SURVEYS and it CREATES, and they are separate calls on purpose.
   The survey reads and nothing else; it is safe to run on anybody's
   disk and its whole job is to let the installer -- and the person
   in front of it -- see what is there before a sector is written.

   It does NOT reuse an existing partition on its own.  Kyan's ask
   includes "detect an unused partition and use that as free space",
   and the detection is here: a partition whose type GUID we do not
   recognise and whose first sector holds no filesystem signature is
   reported as `unused`.  But "we did not recognise it" is not the
   same fact as "it is empty", and the difference is somebody's data.
   An unused partition is offered; it is never taken.  Free space is
   taken, because free space belongs to nobody by definition.

   ── NO BCDEDIT, AND NO NVRAM EITHER ─────────────────────────────────

   The boot entry this produces is \\EFI\\BOOT\\BOOTX64.EFI on an ESP:
   the removable-media fallback path, which every UEFI firmware boots
   when no NVRAM entry claims the machine first.  That needs no
   Windows, no bcdedit and no efibootmgr.

   What it is NOT is a named "HavenDOS" entry in the firmware's own
   boot menu.  That is an NVRAM variable, SetVariable() is a UEFI
   RUNTIME SERVICE, and a multiboot 1 kernel never receives the EFI
   system table -- GRUB has already called ExitBootServices by the
   time HavenDOS's first instruction runs.  Adding one means moving
   HavenDOS to the multiboot 2 EFI handoff and calling
   ExitBootServices itself, which is a boot-path rewrite and not a
   feature.  Saying so here because the alternative is somebody
   spending a day looking for the call.
   ================================================================ */
#ifndef ESPMAKE_H
#define ESPMAKE_H
#include "types.h"
#include "gpt.h"
#include "gpt_write.h"

#define ESPMK_OK            0
#define ESPMK_E_READ      (-1)
#define ESPMK_E_NOTGPT    (-2)   /* no GPT: we do not create one here     */
#define ESPMK_E_HAVEESP   (-3)   /* there already is one; use esp.c       */
#define ESPMK_E_NOSPACE   (-4)   /* no gap big enough for an ESP          */
#define ESPMK_E_GPT       (-5)   /* gpt_add_partition refused; see detail */
#define ESPMK_E_GEOM      (-6)   /* the gap will not make a legal FAT32   */
#define ESPMK_E_FORMAT    (-7)   /* a format write failed                 */
#define ESPMK_E_VERIFY    (-8)   /* it did not read back as FAT32         */
#define ESPMK_E_PARAM     (-9)

/* The smallest ESP worth making. FAT32 needs 65,525 clusters, which
   at one sector per cluster is about 33 MB; 100 MB is what Windows
   makes and leaves room for a second loader beside ours. */
#define ESPMK_MIN_MB       48u
#define ESPMK_WANT_MB     100u

#define ESPMK_MAX_UNUSED    4

typedef struct {
    int      has_gpt;
    int      n_parts;

    int      esp_index;          /* -1 when there is none                 */
    uint64_t esp_lba, esp_sectors;

    /* What else lives here. Reported so the installer can say "this
       disk has Windows on it" before it touches the table, and so a
       grub.cfg can offer the right other entries. */
    int      has_windows;        /* MS basic data / reserved / recovery   */
    int      has_linux;          /* Linux filesystem / swap / LVM / RAID  */
    int      has_bootmgfw;       /* Windows' loader is on the ESP         */

    /* Free space, largest first. */
    int      n_gaps;
    uint64_t best_gap_lba, best_gap_sectors;

    /* Partitions we could not identify AND that carry no filesystem
       signature in their first sector. Candidates, never choices. */
    int      n_unused;
    uint64_t unused_lba[ESPMK_MAX_UNUSED];
    uint64_t unused_sectors[ESPMK_MAX_UNUSED];
    int      unused_index[ESPMK_MAX_UNUSED];
} espmk_survey_t;

/* Reads only. Returns ESPMK_OK or a negative code. */
int espmk_survey(int disk, espmk_survey_t *s);

/* Create an EFI System Partition in free space on `disk` and format
 * it FAT32, ready for esp_install() to write the loader into.
 *
 * dry_run != 0 runs every check and withholds BOTH write functions,
 * so it is the same code path and cannot put a byte on the disk.
 *
 * On success the new partition's geometry comes back in *lba and
 * *sectors, and its GPT entry index in *part_index. */
int espmk_create(int disk, uint32_t want_mb, int dry_run,
                 uint64_t *lba, uint64_t *sectors, int *part_index);

/* The same thing at a range the caller has already chosen.
 *
 * fs/install.c needs this because it and this file would otherwise
 * both call gpt_find_free() and both pick the same gap: the installer
 * takes the whole of it for HavenDOS, and then there is nowhere left
 * for the boot partition it just decided it needed. The installer
 * carves the ESP off the front of the gap and tells us where.
 *
 * first_lba must already be aligned; this does not move it, because
 * the caller has planned the rest of the gap around exactly this
 * range and shifting it would silently overlap what comes after. */
int espmk_create_at(int disk, uint64_t first_lba, uint64_t sectors,
                    int dry_run, int *part_index);

const char *espmk_strerror(int code);

/* The EFI System Partition type GUID, C12A7328-F81F-11D2-BA4B-
   00A0C93EC93B, in on-disk mixed-endian form. */
extern const uint8_t ESPMK_TYPE_EFI[16];

#endif /* ESPMAKE_H */
