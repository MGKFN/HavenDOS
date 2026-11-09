/*
 * ramfs.c — RAM filesystem for BOOT OS v0.5.4
 * Added: owner field, admin-only flag, hidden flag (. prefix)
 */
#include "../include/types.h"
#include "../include/string.h"

#define MAX_FILES 64
#define MAX_NAME  48
#define MAX_DATA  2048

typedef struct {
    char     name[MAX_NAME];
    char     data[MAX_DATA];
    uint32_t size;
    int      used;
    int      is_dir;
    char     owner[16];   /* username of creator */
    int      admin_only;  /* 1 = only admin can read/write/delete */
} ramfs_file_t;

static ramfs_file_t files[MAX_FILES];

/* Current user context — set by login.c */
static const char *ramfs_current_user  = "bootos";
static int         ramfs_current_admin = 1;

void ramfs_set_user(const char *name, int is_admin) {
    ramfs_current_user  = name;
    ramfs_current_admin = is_admin;
}

void ramfs_init(void) { memset(files, 0, sizeof(files)); }

static int ramfs_find(const char *name) {
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0) return i;
    return -1;
}
static int ramfs_alloc(void) {
    for (int i = 0; i < MAX_FILES; i++) if (!files[i].used) return i;
    return -1;
}

/* Permission check — returns 1 if current user can access */
static int ramfs_can_access(int idx) {
    if (ramfs_current_admin) return 1;
    if (files[idx].admin_only) return 0;
    return 1;
}

int ramfs_mkdir(const char *name) {
    if (ramfs_find(name) >= 0) return -1;
    int i = ramfs_alloc(); if (i < 0) return -1;
    files[i].used = 1; files[i].is_dir = 1; files[i].size = 0;
    strncpy(files[i].name,  name,                MAX_NAME-1);
    strncpy(files[i].owner, ramfs_current_user,  15);
    files[i].admin_only = 0;
    return 0;
}

int ramfs_write(const char *name, const char *data, uint32_t size) {
    int i = ramfs_find(name);
    if (i >= 0 && !ramfs_can_access(i)) return -2; /* permission denied */
    if (i < 0) {
        i = ramfs_alloc(); if (i < 0) return -1;
        files[i].used = 1;
        strncpy(files[i].name,  name,               MAX_NAME-1);
        strncpy(files[i].owner, ramfs_current_user, 15);
        files[i].admin_only = 0;
    }
    if (size > MAX_DATA) size = MAX_DATA;
    memcpy(files[i].data, data, size);
    files[i].size   = size;
    files[i].is_dir = 0;
    return (int)size;
}

int ramfs_read(const char *name, char *buf, uint32_t bufsz) {
    int i = ramfs_find(name); if (i < 0) return -1;
    if (!ramfs_can_access(i)) return -2;
    uint32_t sz = files[i].size < bufsz ? files[i].size : bufsz;
    memcpy(buf, files[i].data, sz);
    return (int)sz;
}

int ramfs_delete(const char *name) {
    int i = ramfs_find(name); if (i < 0) return -1;
    if (!ramfs_can_access(i)) return -2;
    memset(&files[i], 0, sizeof(ramfs_file_t));
    return 0;
}

void ramfs_list(void (*cb)(const char*, int, uint32_t)) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used) continue;
        /* Hide admin_only files from non-admin users */
        if (files[i].admin_only && !ramfs_current_admin) continue;
        cb(files[i].name, files[i].is_dir, files[i].size);
    }
}

int ramfs_exists(const char *name)  { return ramfs_find(name) >= 0; }

int ramfs_rename(const char *old, const char *newname) {
    int i = ramfs_find(old); if (i < 0) return -1;
    if (!ramfs_can_access(i)) return -2;
    strncpy(files[i].name, newname, MAX_NAME-1);
    return 0;
}

/* Admin-only: set permissions */
int ramfs_chmod(const char *name, int admin_only) {
    if (!ramfs_current_admin) return -2;
    int i = ramfs_find(name); if (i < 0) return -1;
    files[i].admin_only = admin_only;
    return 0;
}

int ramfs_chown(const char *name, const char *newowner) {
    if (!ramfs_current_admin) return -2;
    int i = ramfs_find(name); if (i < 0) return -1;
    strncpy(files[i].owner, newowner, 15);
    return 0;
}

/* Get file owner for display */
const char *ramfs_get_owner(const char *name) {
    int i = ramfs_find(name); if (i < 0) return "";
    return files[i].owner;
}

int ramfs_get_admin_only(const char *name) {
    int i = ramfs_find(name); if (i < 0) return 0;
    return files[i].admin_only;
}

/* Check if a name is a directory */
int ramfs_is_dir(const char *name) {
    int i = ramfs_find(name); if (i < 0) return 0;
    return files[i].is_dir;
}

/* List entries whose names start with prefix "dir/" */
void ramfs_list_dir(const char *dir, void (*cb)(const char*, int, uint32_t)) {
    int dlen = (int)strlen(dir);
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (files[i].admin_only && !ramfs_current_admin) continue;
        const char *n = files[i].name;
        /* Match "dir/something" — strip the prefix, show only the leaf */
        if (dlen > 0) {
            if (strncmp(n, dir, dlen) != 0) continue;
            if (n[dlen] != '/') continue;
            const char *leaf = n + dlen + 1;
            if (!leaf[0]) continue;
            /* Don't recurse into deeper subdirs — only one level */
            int has_slash = 0;
            for (int j = 0; leaf[j]; j++) if (leaf[j]=='/') { has_slash=1; break; }
            if (has_slash) continue;
            cb(leaf, files[i].is_dir, files[i].size);
        } else {
            /* Root: only show top-level entries (no slash in name) */
            int has_slash = 0;
            for (int j = 0; n[j]; j++) if (n[j]=='/') { has_slash=1; break; }
            if (has_slash) continue;
            cb(n, files[i].is_dir, files[i].size);
        }
    }
}
