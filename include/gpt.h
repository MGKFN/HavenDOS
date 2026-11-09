#pragma once
#include "types.h"

/* GUID Partition Table, read-only.
 *
 * This exists primarily so the installer can REFUSE.  fs/install.c zeroes
 * LBA 0..63 and the last 33 sectors, which is both GPT headers and the
 * partition entries; on any UEFI machine that is the whole partition table,
 * and on a dual-boot machine it is somebody's other operating system.  Until
 * there is an installer that adds a partition rather than replacing the
 * table, recognising a GPT disk is how that disk gets left alone.
 *
 * The secondary use is honest reporting: `disk` can say what is actually on
 * the drive instead of guessing from a protective MBR.
 */

#define GPT_MAX_PARTS 16

typedef struct {
    uint64_t first_lba, last_lba;
    char     name[37];        /* UTF-16LE name, folded to ASCII */
    uint8_t  type_guid[16];
    int      is_esp;          /* the EFI System Partition          */
    int      is_msft;         /* a Microsoft data/reserved/recovery partition */
} gpt_part_t;

/* read_sector returns 0 on success.  Returns the number of partitions found,
 * 0 if the disk is not GPT, or -1 on a read error. */
int gpt_scan(int (*read_sector)(void *ctx, uint64_t lba, void *buf), void *ctx,
             gpt_part_t *out, int max);

/* True if LBA 1 carries a GPT header signature.  Cheap, and the only thing
 * the installer actually needs to decide to stop. */
int gpt_present(int (*read_sector)(void *ctx, uint64_t lba, void *buf), void *ctx);

/* CRC-32 as the GPT spec defines it; exposed for tests. */
uint32_t gpt_crc32(const void *data, uint32_t len);
