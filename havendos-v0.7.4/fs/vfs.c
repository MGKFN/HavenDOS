/*
 * vfs.c — Virtual Filesystem Switch for BOOT OS
 *
 * Routes all filesystem calls to FAT12 (if disk detected)
 * or RAM filesystem (fallback). Transparent to all callers.
 *
 * Boot sequence:
 *   1. ata_init()    → detect disk
 *   2. fat12_init()  → format/mount if disk present
 *   3. vfs_init()    → sets routing
 *
 * All shell/desktop code calls vfs_* functions only.
 */

#include "../include/types.h"
#include "../include/string.h"

/* FAT12 API */
extern int  fat12_init(void);
extern int  fat12_detected(void);
extern int  fat12_write(const char*, const char*, uint32_t);
extern int  fat12_read(const char*, char*, uint32_t);
extern int  fat12_delete(const char*);
extern void fat12_list(void(*)(const char*,int,uint32_t));
extern int  fat12_exists(const char*);
extern int  fat12_rename(const char*, const char*);

/* FAT16 API (VirtIO-backed, v0.5.9.12) */
extern int  fat16_init(void);
extern int  fat16_detected(void);
extern int  fat16_write(const char*, const char*, uint32_t);
extern int  fat16_read(const char*, char*, uint32_t);
extern int  fat16_delete(const char*);
extern void fat16_list(void(*)(const char*,int,uint32_t));
extern int  fat16_exists(const char*);
extern int  fat16_rename(const char*, const char*);
extern int  fat16_mkdir(const char*);

/* RAM filesystem API */
extern void ramfs_init(void);
extern int  ramfs_write(const char*, const char*, uint32_t);
extern int  ramfs_read(const char*, char*, uint32_t);
extern int  ramfs_delete(const char*);
extern void ramfs_list(void(*)(const char*,int,uint32_t));
extern int  ramfs_exists(const char*);
extern int  ramfs_rename(const char*, const char*);
extern int  ramfs_mkdir(const char*);

static int using_fat16 = 0;
static int using_fat12 = 0;

void vfs_init(void) {
    /* fat16_init() now probes VirtIO then ATA internally.
       On QEMU/UTM SE: VirtIO path taken.
       On real PC with HavenDOS installed on SATA: ATA path taken.
       fat16_detected() returns 1 in both cases if a valid partition
       was found, so the boot decision in main.c works identically
       on both platforms — no VirtIO assumption anywhere.            */
    fat16_init();
    if (fat16_detected()) {
        using_fat16 = 1;
        using_fat12 = 0;
        return;
    }
    /* Fallback: FAT12 on ATA (legacy path, kept for compatibility) */
    fat12_init();
    if (fat12_detected()) {
        using_fat16 = 0;
        using_fat12 = 1;
        return;
    }
    using_fat16 = 0;
    using_fat12 = 0;
}

int vfs_using_disk(void) { return using_fat16 || using_fat12; }
int vfs_using_fat16(void) { return using_fat16; }

int vfs_write(const char *name, const char *data, uint32_t size) {
    if (using_fat16) return fat16_write(name, data, size);
    if (using_fat12) return fat12_write(name, data, size);
    return ramfs_write(name, data, size);
}

int vfs_read(const char *name, char *buf, uint32_t bufsz) {
    if (using_fat16) return fat16_read(name, buf, bufsz);
    if (using_fat12) return fat12_read(name, buf, bufsz);
    return ramfs_read(name, buf, bufsz);
}

int vfs_delete(const char *name) {
    if (using_fat16) return fat16_delete(name);
    if (using_fat12) return fat12_delete(name);
    return ramfs_delete(name);
}

void vfs_list(void (*cb)(const char*, int, uint32_t)) {
    if (using_fat16)      fat16_list(cb);
    else if (using_fat12) fat12_list(cb);
    else                  ramfs_list(cb);
}

int vfs_exists(const char *name) {
    if (using_fat16) return fat16_exists(name);
    if (using_fat12) return fat12_exists(name);
    return ramfs_exists(name);
}

int vfs_rename(const char *old, const char *newname) {
    if (using_fat16) return fat16_rename(old, newname);
    if (using_fat12) return fat12_rename(old, newname);
    return ramfs_rename(old, newname);
}

int vfs_mkdir(const char *name) {
    if (using_fat16) return fat16_mkdir(name);
    if (using_fat12) return 0; /* FAT12 mkdir not implemented */
    return ramfs_mkdir(name);
}

/* v0.5.2 permission extensions */
extern int         ramfs_chmod(const char*, int);
extern int         ramfs_chown(const char*, const char*);
extern const char *ramfs_get_owner(const char*);
extern int         ramfs_get_admin_only(const char*);

int vfs_chmod(const char *name, int admin_only) {
    /* FAT12 permissions not implemented yet */
    return ramfs_chmod(name, admin_only);
}
int vfs_chown(const char *name, const char *owner) {
    return ramfs_chown(name, owner);
}

/* Directory helpers */
extern int  ramfs_is_dir(const char*);
extern void ramfs_list_dir(const char*, void(*)(const char*,int,uint32_t));

int vfs_is_dir(const char *name) {
    return ramfs_is_dir(name);
}

void vfs_list_dir(const char *dir, void (*cb)(const char*, int, uint32_t)) {
    /* Route through same priority as vfs_list so File Manager
       sees the same files as Notepad writes to */
    if (using_fat16)      fat16_list(cb);
    else if (using_fat12) fat12_list(cb);
    else                  ramfs_list_dir(dir, cb);
}

/* Called by login.c after successful login */
void vfs_set_user(const char *name, int is_admin) {
    extern void ramfs_set_user(const char*, int);
    ramfs_set_user(name, is_admin);
}
