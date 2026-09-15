#pragma once
#include "types.h"
int  fat16_init(void);
int  fat16_read(const char *path, char *buf, uint32_t maxlen);
int  fat16_write(const char *path, const char *buf, uint32_t len);
int  fat16_delete(const char *path);
int  fat16_exists(const char *path);
int  fat16_is_dir(const char *path);
int  fat16_mkdir(const char *path);
int  fat16_list(const char *dir, char *out, uint32_t maxlen);
uint32_t fat16_file_size(const char *path);
