/* ================================================================
   include/sysvol.h -- the SYSTEM volume, whichever FAT it is
   ================================================================
   v0.9.3 "Gaia". Until this release an installed HavenDOS was FAT16,
   always, and fs/update.c said so in every line: fat16_read_path,
   fat16_write_path, fat16_detected. An install larger than 2047 MB is
   FAT32 now, and on one of those `update` would have printed "No FAT16
   partition found. You must be running from a disk install" -- on a
   disk install.

   This is the one place that knows which filesystem the machine's own
   volume is. It asks the VFS (which decided at boot), and for FAT32 it
   takes the system volume's turn in fs/fat32.c's slot around each call,
   so it reaches the system volume even while an update source is
   mounted from the ESP or a stick. See "THE SYSTEM VOLUME IS PARKED"
   in fs/fat32.c.

   RETURN CONVENTIONS ARE fs/fat16.c's, deliberately, because the one
   caller that matters was written against them and was wrong about
   them once already (fs/update.c, v0.7.19):
     sysvol_read   bytes read, < 0 on failure; a file longer than the
                   buffer is truncated to it, exactly as fat16_read_path()
                   does -- fs/update.c's verify reads 4 bytes on purpose
     sysvol_write  bytes written (== len) on success, <= 0 on failure
     sysvol_exists / sysvol_delete   1 or 0
   fat32_write() returns 1 for success. Handing that straight to a
   caller testing `<= 0` would work; handing it to one testing `== len`
   would report every kernel write as a failure. The translation is
   here and nowhere else.
   ================================================================ */
#ifndef SYSVOL_H
#define SYSVOL_H
#include "types.h"

#define SYSVOL_NONE   0
#define SYSVOL_FAT16 16
#define SYSVOL_FAT32 32

int         sysvol_kind(void);        /* SYSVOL_NONE / _FAT16 / _FAT32 */
const char *sysvol_name(void);        /* "FAT16", "FAT32", "none"      */

int sysvol_exists(const char *path);
int sysvol_read  (const char *path, char *buf, uint32_t max);
int sysvol_write (const char *path, const char *buf, uint32_t len);
int sysvol_delete(const char *path);

#endif
