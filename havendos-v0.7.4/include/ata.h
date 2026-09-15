#pragma once
#include "types.h"

void        ata_init(void);
int         ata_detected(void);
uint32_t    ata_sectors(void);
int         ata_lba48(void);
const char *ata_get_debug(void);

/* LBA28 — used for drives <= 128 GB */
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);

/* LBA48 — used for SATA SSDs / large drives; auto-selected by lba28 fns */
int ata_read_sector48(uint32_t lba, uint8_t *buf);
int ata_write_sector48(uint32_t lba, const uint8_t *buf);
