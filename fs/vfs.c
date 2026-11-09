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
#include "../include/fat16.h"

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
/* include/fat16.h declares this correctly now (v0.8.6.1); the local
   extern that used to be here was a workaround for a header that said
   something else, and a workaround nobody could see. */
extern int  fat16_exists(const char*);
extern int  fat16_rename(const char*, const char*);
extern int  fat16_mkdir(const char*);
/* v0.7.12: path-aware variants — see the routing note above vfs_write() */
extern int  fat16_write_path (const char*, const char*, uint32_t);
extern int  fat16_read_path  (const char*, char*, uint32_t);
extern int  fat16_exists_path(const char*);
extern int  fat16_delete_path(const char*);

/* FAT32 API (Phase A).  Every entry point is path-aware, so there is no
   flat/_path pair here — that pair is what let vfs_write() hand
   "boot/grub.cfg" to name_to_83() and create a root-directory file with a
   slash in its name. */
extern int  fat32_init(void);
extern int  fat32_detected(void);
extern int  fat32_write(const char*, const char*, uint32_t);
extern int  fat32_read(const char*, char*, uint32_t);
extern int  fat32_delete(const char*);
extern void fat32_list(void(*)(const char*,int,uint32_t));
extern void fat32_list_dir(const char*, void(*)(const char*,int,uint32_t));
extern int  fat32_exists(const char*);
extern int  fat32_is_dir(const char*);
extern int  fat32_mkdir(const char*);

/* Does this name carry a directory component? */
static int has_dir(const char *n){
    if(!n) return 0;
    if(*n=='/') n++;              /* a leading slash alone is still flat */
    for(; *n; n++) if(*n=='/') return 1;
    return 0;
}

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

/* ── while `fat16` is looking at somebody else's volume ──────────────
 * v0.8.6.1. fat16_borrow() points fs/fat16.c at a volume on another
 * disk -- a USB stick -- and fat16_return() puts the system's own back.
 * `using_fat16` is a flag cached by vfs_init(), not re-evaluated per
 * read, so for the length of a borrow every route below would reach the
 * STICK while its caller believed it was reading the system volume.
 *
 * Nothing can get there today: the shell is single-threaded, and
 * listing or reading a borrowed volume does no VFS I/O of its own. But
 * "unreachable by construction" is what the three bus-dispatch switches
 * v0.8.6 deleted were also true by, and reading the wrong disk is not a
 * failure that announces itself.
 *
 * So the routes refuse while a borrow is held. Refusing is honest; the
 * alternative is a read that succeeds against the wrong device. */
static int fs_borrowed(void){ return fat16_borrowed(); }
static int using_fat12 = 0;
static int using_fat32 = 0;

void vfs_init(void) {
    /* Live media mounts nothing at all.
       Not "mounts read-only" -- mounts NOTHING: using_fat16, using_fat12 and
       using_fat32 all stay zero, so every vfs_write/_delete/_rename in the
       system routes to the RAM filesystem and no code path anywhere can
       reach a real partition.  That is the point: a machine booted from the
       ISO is not allowed to touch whatever else is installed on it, and
       relying on "the kernel has no NVMe driver yet" is relying on an
       accident.

       ADDING FAT32 MAKES THIS MORE IMPORTANT, NOT LESS.  Section 13b of the
       handoff names the exact danger: an EFI System Partition is FAT, sits
       first on the disk, and is what a careless probe lands on.  Until this
       release HavenDOS could not read one even if it tried.  It can now, so
       the only thing keeping a live boot off a UEFI machine's ESP is this
       early return -- which is why fat32_init() is below it and not above,
       and why no caller anywhere else in the tree calls fat32_init(). */
    extern int boot_is_live(void);
    if (boot_is_live()) {
        using_fat16 = 0;
        using_fat12 = 0;
        using_fat32 = 0;
        return;
    }

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
        using_fat32 = 0;
        return;
    }
    /* FAT32 comes AFTER FAT16 and that order is deliberate.  An installed
       HavenDOS keeps .accounts, DESKTOP.CFG, the theme and the shell history
       on its own FAT16 partition; a machine that has both that and a FAT32
       partition must mount the FAT16 one or every setting the user has ever
       saved disappears.  Putting FAT32 second also means this release cannot
       change the behaviour of any disk that works today: on every one of
       them fat16_detected() is true and we have already returned. */
    fat32_init();
    if (fat32_detected()) {
        using_fat16 = 0;
        using_fat12 = 0;
        using_fat32 = 1;
        return;
    }
    /* Fallback: FAT12 on ATA (legacy path, kept for compatibility) */
    fat12_init();
    if (fat12_detected()) {
        using_fat16 = 0;
        using_fat12 = 1;
        using_fat32 = 0;
        return;
    }
    using_fat16 = 0;
    using_fat12 = 0;
    using_fat32 = 0;
}

int vfs_using_disk(void) { return using_fat16 || using_fat12 || using_fat32; }
int vfs_using_fat16(void) { return using_fat16; }
int vfs_using_fat32(void) { return using_fat32; }

/* Re-mount the FAT32 volume this VFS is using.
 *
 * fs/fat32.c holds ONE mounted volume, so anything that mounts another one
 * — `fat32 ls` inspecting somebody's ESP, say — displaces it, and every
 * later vfs_read() would be talking to a volume that is no longer there.
 * Whoever borrows the driver calls this when they are done.
 *
 * It does nothing unless the VFS was already routing to FAT32, so it cannot
 * mount a disk on a system that had deliberately mounted nothing: a live
 * boot leaves using_fat32 at zero and this stays a no-op, which keeps the
 * guarantee in vfs_init() true for this path too. */
void vfs_refresh_fat32(void) {
    if (!using_fat32) return;
    fat32_init();
    using_fat32 = fat32_detected() ? 1 : 0;
}

/* v0.7.12 FIX — the VFS never routed to the path-aware FAT16 entry points.
 * fat16_write_path/read_path/exists_path/delete_path have existed since
 * v0.5.9, but every vfs_* call went to the flat root-directory version, which
 * hands the whole string to name_to_83().  "boot/grub.cfg" therefore became
 * the 8.3 name "BOOT/GRU" in the ROOT directory — a file that is not the one
 * anybody meant, with a slash in its name, which no other FAT implementation
 * will ever find.  Anything that produced a path rather than a bare filename
 * hit this: `cd boot` followed by any write, wget/curl with a subdirectory
 * target, bpkg installing into /apps.  It looked like a write that "worked"
 * and a read that came back empty. */
int vfs_write(const char *name, const char *data, uint32_t size) {
    if (using_fat32) return fat32_write(name, data, size);
    if (using_fat16 && fs_borrowed()) return -1;
    if (using_fat16) return has_dir(name) ? fat16_write_path(name, data, size)
                                          : fat16_write(name, data, size);
    if (using_fat12) return fat12_write(name, data, size);
    return ramfs_write(name, data, size);
}

int vfs_read(const char *name, char *buf, uint32_t bufsz) {
    if (using_fat32) return fat32_read(name, buf, bufsz);
    if (using_fat16 && fs_borrowed()) return -1;
    if (using_fat16) return has_dir(name) ? fat16_read_path(name, buf, bufsz)
                                          : fat16_read(name, buf, bufsz);
    if (using_fat12) return fat12_read(name, buf, bufsz);
    return ramfs_read(name, buf, bufsz);
}

int vfs_delete(const char *name) {
    if (using_fat32) return fat32_delete(name);
    if (using_fat16 && fs_borrowed()) return -1;
    if (using_fat16) return has_dir(name) ? fat16_delete_path(name)
                                          : fat16_delete(name);
    if (using_fat12) return fat12_delete(name);
    return ramfs_delete(name);
}

void vfs_list(void (*cb)(const char*, int, uint32_t)) {
    if (using_fat32)      fat32_list(cb);
    else if (using_fat16 && fs_borrowed()) { /* see fs_borrowed() */ }
    else if (using_fat16) fat16_list(cb);
    else if (using_fat12) fat12_list(cb);
    else                  ramfs_list(cb);
}

int vfs_exists(const char *name) {
    if (using_fat32) return fat32_exists(name);
    if (using_fat16 && fs_borrowed()) return 0;
    if (using_fat16) return has_dir(name) ? fat16_exists_path(name)
                                          : fat16_exists(name);
    if (using_fat12) return fat12_exists(name);
    return ramfs_exists(name);
}

int vfs_rename(const char *old, const char *newname) {
    /* FAT32 has no rename of its own yet: read, write under the new name,
       delete the old.  Stated rather than silently routed to a filesystem
       that is not mounted, which is what returning fat16_rename() here
       would do. */
    if (using_fat32) return 0;
    if (using_fat16 && fs_borrowed()) return -1;
    if (using_fat16) return fat16_rename(old, newname);
    if (using_fat12) return fat12_rename(old, newname);
    return ramfs_rename(old, newname);
}

int vfs_mkdir(const char *name) {
    if (using_fat32) return fat32_mkdir(name);
    if (using_fat16 && fs_borrowed()) return -1;
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
    if (using_fat32) return fat32_is_dir(name);
    return ramfs_is_dir(name);
}

void vfs_list_dir(const char *dir, void (*cb)(const char*, int, uint32_t)) {
    /* Route through same priority as vfs_list so File Manager
       sees the same files as Notepad writes to.
       FAT32 is the only one of the three that can actually list a
       subdirectory; FAT16 and FAT12 ignore `dir` and list their root, which
       is the behaviour this release leaves exactly as it found it. */
    if (using_fat32)      fat32_list_dir(dir, cb);
    else if (using_fat16 && fs_borrowed()) { /* see fs_borrowed() */ }
    else if (using_fat16) fat16_list(cb);
    else if (using_fat12) fat12_list(cb);
    else                  ramfs_list_dir(dir, cb);
}

/* Called by login.c after successful login */
void vfs_set_user(const char *name, int is_admin) {
    extern void ramfs_set_user(const char*, int);
    ramfs_set_user(name, is_admin);
}
