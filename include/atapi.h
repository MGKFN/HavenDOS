#pragma once
#include "types.h"
int  atapi_init(void);
int  atapi_read_sector(uint32_t lba, void *buf);
int  atapi_read_sectors(uint32_t lba, uint32_t count, void *buf);
