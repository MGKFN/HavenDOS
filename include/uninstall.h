/* ================================================================
   include/uninstall.h -- HavenDOS taking itself back off a disk
   ================================================================
   v0.8.8.8.  Asked for directly: "give instructions to remove
   HavenDOS fully of PC and let HavenDOS do everything we did
   manually".

   The manual procedure is four steps in an Administrator command
   prompt -- mountvol, restore BOOTX64.EFI from BOOTX64.HDB, rmdir
   \EFI\HAVENDOS, then diskpart with `delete partition override`.
   Every one of them is a step where the wrong keystroke takes a
   partition that was not ours, and `delete partition override` does
   not ask twice.

   So this does it instead, and it knows exactly which partition is
   ours because it is the one with our type GUID in it.

   ── WHAT IT WILL NOT DO ─────────────────────────────────────────────

   It never removes \EFI\BOOT\BOOTX64.EFI unless that file is OURS.
   fs/esp.c already distinguishes: our loader is byte-for-byte
   \EFI\HAVENDOS\grubx64.efi. Anything else at that path belongs to
   somebody else -- a Linux distribution's shim, a firmware updater,
   a rescue tool -- and taking it away would unboot a machine on the
   way to uninstalling something else.

   It removes an EFI System Partition only if HAVENDOS MADE IT: the
   volume label is HAVENBOOT, which only fs/espmake.c writes, AND
   there is no \EFI\Microsoft on it. Both, not either.

   It does not erase the data in the HavenDOS partition. Clearing the
   table entry is what makes the space free; writing over 8 GB of
   somebody's files to be tidy is a different operation with a
   different risk, and they can format it in thirty seconds if they
   want it gone.

   ── ORDER ───────────────────────────────────────────────────────────

   Boot loader first, then our files, then the partition, then our
   ESP if we made one. Every step is reversible by the one after it
   not having run: a machine that loses power half way through has
   its ORIGINAL loader back and a HavenDOS partition still in the
   table, which boots Windows and can be finished later. The reverse
   order would leave a machine with no loader and no HavenDOS.
   ================================================================ */
#ifndef UNINSTALL_H
#define UNINSTALL_H
#include "types.h"
#include "esp.h"

#define UNINST_OK            0
#define UNINST_E_NOGPT     (-1)
#define UNINST_E_NOESP     (-2)
#define UNINST_E_MOUNT     (-3)
#define UNINST_E_NOTHING   (-4)   /* no HavenDOS on this disk at all     */
#define UNINST_E_READONLY  (-5)
#define UNINST_E_LOADER    (-6)   /* could not restore the boot loader   */
#define UNINST_E_FILES     (-7)   /* could not remove \EFI\HAVENDOS      */
#define UNINST_E_PART      (-8)   /* could not clear the table entry     */
#define UNINST_E_PARAM     (-9)

typedef struct {
    /* ── what the survey found ──────────────────────────────────── */
    int      esp_rc;             /* esp_find()'s verdict                 */
    uint64_t esp_lba, esp_sectors;

    int      has_havendos_dir;   /* \EFI\HAVENDOS is there               */
    int      has_our_loader;     /* ...and grubx64.efi is in it          */
    int      has_backup;         /* \EFI\BOOT\BOOTX64.HDB is there       */
    int      fallback_is_ours;   /* \EFI\BOOT\BOOTX64.EFI is our loader  */
    int      fallback_foreign;   /* ...or is somebody else's, so LEFT    */

    int      part_index;         /* our partition's GPT slot, -1 if none */
    uint64_t part_first, part_sectors;

    int      esp_is_ours;        /* label HAVENBOOT and no \EFI\Microsoft */
    int      esp_index;          /* its GPT slot, -1 if not ours          */

    /* ── what the run actually did ──────────────────────────────── */
    int      did_restore_loader;
    int      did_delete_backup;
    int      did_remove_files;
    int      did_remove_part;
    int      did_remove_esp;
    int      n_sectors_written;
} uninst_report_t;

/* Reads only. Safe on anybody's disk. */
int uninst_survey(int disk, uninst_report_t *r);

/* dry_run != 0 runs every check and withholds every write function,
   so it is the same code path and cannot put a byte on the disk. */
int uninst_run(int disk, int dry_run, uninst_report_t *r);

const char *uninst_strerror(int code);

#endif /* UNINSTALL_H */
