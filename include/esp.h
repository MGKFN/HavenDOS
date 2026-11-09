#pragma once
#include "types.h"

/* Installing HavenDOS's boot files into the EFI System Partition that is
 * ALREADY on the disk -- the one Windows boots from.
 *
 * ── WHY THIS IS THE DANGEROUS HALF ───────────────────────────────────
 *
 * fs/gpt_write.c writes four sectors of partition table, and a mistake
 * there is visible immediately and recoverable from a backup of 34
 * sectors. This is worse. The ESP holds \EFI\Microsoft\Boot\bootmgfw.efi
 * and the BCD, which is Windows' entire boot path, and it is written
 * through fs/fat32.c -- a filesystem driver that is two weeks old. A
 * read-modify-write bug in a FAT directory sector does not announce
 * itself: the partition table stays perfect, every partition stays where
 * it was, and the machine simply stops booting Windows.
 *
 * So the rules here are stricter than anywhere else in the tree:
 *
 *   NOTHING IS EVER DELETED.  There is no unlink in this file.
 *
 *   NOTHING IS EVER OVERWRITTEN without a copy being made first, under a
 *   name that says where it came from. If the copy would itself overwrite
 *   something, the install REFUSES rather than guessing which of the two
 *   is wanted.
 *
 *   FREE SPACE IS CHECKED BEFORE THE FIRST BYTE.  A FAT32 volume that
 *   runs out of clusters mid-write leaves a truncated file and a dirty
 *   FSInfo, and on an ESP that truncated file could be the one the
 *   firmware loads.
 *
 *   EVERY FILE IS READ BACK AND COMPARED after it is written. Not "the
 *   write returned success" -- the bytes are read through the same
 *   driver and checked against what they were supposed to be.
 *
 *   AND IT CAN BE ASKED WITHOUT BEING TOLD.  esp_find() and a dry-run
 *   esp_install() do every check and every refusal and cannot write.
 *
 * ── WHAT IT PUTS THERE ───────────────────────────────────────────────
 *
 *   \EFI\HAVENDOS\grubx64.efi    the loader
 *   \EFI\HAVENDOS\grub.cfg       its menu
 *   \EFI\BOOT\BOOTX64.EFI        the removable-media fallback path, which
 *                                is how a machine finds it with no NVRAM
 *                                entry -- created only if absent, or
 *                                after the existing one is copied aside
 *
 * Windows' own files live under \EFI\Microsoft\ and are not touched, read
 * or looked at beyond confirming they are still there afterwards.
 */

#define ESP_OK             0
#define ESP_E_NOGPT      (-1)   /* the disk has no GPT                     */
#define ESP_E_NOESP      (-2)   /* no EFI System Partition on it           */
#define ESP_E_MOUNT      (-3)   /* the ESP would not mount as FAT32        */
#define ESP_E_SPACE      (-4)   /* not enough free space                   */
#define ESP_E_BACKUP     (-5)   /* a backup name is taken; refusing        */
#define ESP_E_WRITE      (-6)   /* a write failed                          */
#define ESP_E_VERIFY     (-7)   /* a file read back wrong -- the bad one   */
#define ESP_E_READONLY   (-8)   /* the disk is not writable                */
#define ESP_E_PARAM      (-9)
#define ESP_E_LOADER    (-10)   /* the embedded loader is not a loader    */

/* What esp_find() learned, and what esp_install() would do. Filled in by
 * both, so the installer UI can show it before anything happens. */
typedef struct {
    int      found;           /* an ESP was located                        */
    int      disk;            /* index in the installer's disk table       */
    uint64_t lba;             /* its first sector                          */
    uint64_t sectors;         /* its size                                  */
    uint32_t free_kb;         /* free space on it right now                */
    uint32_t need_kb;         /* what the install needs                    */
    int      has_microsoft;   /* \EFI\Microsoft exists -- Windows lives here */
    int      has_fallback;    /* \EFI\BOOT\BOOTX64.EFI already exists      */
    int      would_back_up;   /* ...and we would copy it aside first       */
    int      fallback_is_ours;/* ...or it is a loader we wrote, and a
                                 reinstall must NOT back it up over the
                                 copy of the one that was there first     */
    char     backup_name[32]; /* where the existing fallback would go      */
} esp_report_t;

/* Find the ESP on disk `disk` and fill in `rep`. Reads only. */
int esp_find(int disk, esp_report_t *rep);

/* Install into the ESP described by `rep`.
 *
 * dry_run != 0 runs every check and every refusal and cannot write: it is
 * the same code path with the write function withheld, not a separate one
 * that might drift from it.
 *
 * `havendos_part` is the GPT partition number HavenDOS was installed to,
 * which the generated grub.cfg points at. */
/* The disk is a parameter and not read back out of *rep. It used to be
   taken from rep->disk, which esp_find() sets -- so a caller that filled
   the report itself, or zeroed it, silently targeted disk 0. On a machine
   whose target is not disk 0 that is a survey of one disk and a write to
   another, and nothing in the types would have objected. */
int esp_install(int disk, esp_report_t *rep, int havendos_part, int dry_run);

const char *esp_strerror(int code);
