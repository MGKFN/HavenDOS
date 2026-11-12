/* fs/instsize.c -- how big, and which FAT. See include/install_add.h.
   v0.9.3 "Gaia". No I/O and no state: two answers that both installers,
   both installer screens and tests/test_wholedisk_size.c must agree on,
   so there is one copy of each. */
#include "../include/types.h"
#include "../include/install_add.h"

#define WHOLE_PART_START 2048ull   /* fs/install.c's PART_START_SECT */

int instadd_fs_for(uint64_t sectors)
{
    return sectors > INSTADD_FAT16_MAX_SECTORS ? 32 : 16;
}

uint32_t havendos_wholedisk_sectors(uint64_t disk_sectors)
{
    uint64_t n;
    if(disk_sectors <= WHOLE_PART_START) return 0;
    n = disk_sectors - WHOLE_PART_START;
    if(n > INSTADD_MAX_SECTORS) n = INSTADD_MAX_SECTORS;
    return (uint32_t)(n / INSTADD_ALIGN * INSTADD_ALIGN);
}
