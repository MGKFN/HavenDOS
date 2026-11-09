#ifndef ISO9660_H
#define ISO9660_H
#include "types.h"

int iso9660_init(void);
int iso9660_detected(void);
int iso9660_find(const char *path, uint32_t *out_lba, uint32_t *out_size);
int iso9660_read_file(const char *path, uint8_t *buf, uint32_t buf_size);

#endif /* ISO9660_H */
