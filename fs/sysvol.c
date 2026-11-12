/* fs/sysvol.c -- see include/sysvol.h */
#include "../include/types.h"
#include "../include/sysvol.h"
#include "../include/fat32.h"

/* Declared here rather than by including include/fat16.h: that header
   pulls the kernel's string.h, and tests/test_sysvol.c links this file
   against stubs for exactly these four. */
extern int fat16_detected(void);
extern int fat16_exists_path(const char *path);
extern int fat16_read_path  (const char *path, char *buf, uint32_t maxlen);
extern int fat16_write_path (const char *path, const char *data, uint32_t len);
extern int fat16_delete_path(const char *path);
extern int vfs_using_fat16(void);
extern int vfs_using_fat32(void);

int sysvol_kind(void)
{
    if(vfs_using_fat16() && fat16_detected())       return SYSVOL_FAT16;
    if(vfs_using_fat32() && fat32_system_mounted()) return SYSVOL_FAT32;
    return SYSVOL_NONE;
}

const char *sysvol_name(void)
{
    switch(sysvol_kind()){
    case SYSVOL_FAT16: return "FAT16";
    case SYSVOL_FAT32: return "FAT32";
    }
    return "none";
}

int sysvol_exists(const char *path)
{
    int r;
    switch(sysvol_kind()){
    case SYSVOL_FAT16: return fat16_exists_path(path) ? 1 : 0;
    case SYSVOL_FAT32:
        if(!fat32_sys_begin()) return 0;
        r = fat32_exists(path) && !fat32_is_dir(path);
        fat32_sys_end();
        return r ? 1 : 0;
    }
    return 0;
}

int sysvol_read(const char *path, char *buf, uint32_t max)
{
    int r;
    switch(sysvol_kind()){
    case SYSVOL_FAT16: return fat16_read_path(path, buf, max);
    case SYSVOL_FAT32:
        if(!fat32_sys_begin()) return -1;
        /* Up to `max` bytes, as fat16_read_path() does -- a file larger
           than the buffer is TRUNCATED, not refused. The first version of
           this refused it, on the reasoning that a short kernel read is a
           bad backup; harness/esp_update.py then showed what it broke:
           fs/update.c's verify step reads FOUR bytes of the kernel it has
           just written, to check the ELF magic, and every update on a
           FAT32 system failed its own verify and rolled back. A caller
           that needs the whole file sizes its buffer, as fat16.h says. */
        if(!fat32_exists(path) || fat32_is_dir(path)){ fat32_sys_end(); return -1; }
        r = fat32_read(path, buf, max);
        fat32_sys_end();
        return r;
    }
    return -1;
}

int sysvol_write(const char *path, const char *buf, uint32_t len)
{
    int r;
    switch(sysvol_kind()){
    case SYSVOL_FAT16: return fat16_write_path(path, buf, len);
    case SYSVOL_FAT32:
        if(!fat32_sys_begin()) return -1;
        r = fat32_write(path, buf, len) == 1 && fat32_file_size(path) == len;
        fat32_sys_end();
        return r ? (int)len : -1;
    }
    return -1;
}

int sysvol_delete(const char *path)
{
    int r;
    switch(sysvol_kind()){
    case SYSVOL_FAT16: return fat16_delete_path(path) ? 1 : 0;
    case SYSVOL_FAT32:
        if(!fat32_sys_begin()) return 0;
        r = fat32_delete(path);
        fat32_sys_end();
        return r ? 1 : 0;
    }
    return 0;
}
