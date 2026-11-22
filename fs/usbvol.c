/* ════════════════════════════════════════════════════════════════════
   fs/usbvol.c -- a USB stick as the folder /USB.  v0.9.7.3.

   Kyan, 22 Nov: "the ability to save something to USB" -- and asked
   where: "Everywhere". Until now a stick could be read by the commands
   that knew about it (`fat32`, `update`, Retro PC) and written by one
   (`fat16 cp`), and by nothing a person uses. Now the first stick
   plugged in is a folder, /USB, in Files, in the editor's Save As, in
   `cp`, `ls`, `cat`, `mkdir` -- every path that goes through the VFS.

   WHAT IS WRITTEN, AND WHAT IS NOT
     - Only a REMOVABLE disk, as fs/blkdev.c says, ever: the same test
       fs/update_src.c uses. A fixed disk is never /USB, so nothing here
       can write to the internal drive.
     - FAT32 sticks are read and written. exFAT sticks (most of 64 GB and
       up) are read: fs/exfat.c has no write path, and writes to one are
       refused, by name. FAT16 sticks are not mounted here: fs/fat16.c is
       the system's own driver on an installed FAT16 HavenDOS, and its
       writable borrow belongs to `fat16 cp`.

   ONE DRIVER, ONE VOLUME
   fs/fat32.c holds one volume and parks the system's while it is lent
   out (fat32_sys_begin/end). Every call here mounts the stick, does one
   thing, and unmounts, so the system volume is back in the slot the
   moment it returns and no call elsewhere can find the stick mounted.
   It costs a boot-sector read per call; usb/msc.c's read-ahead makes
   that cheap.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/fat32vol.h"
#include "../include/fat32.h"
#include "../include/exfat.h"
#include "../include/blkdev.h"
#include "../include/klog.h"
#include "../include/usbvol.h"

extern int inst_disk_read (int i, uint64_t lba, uint8_t *b);
extern int inst_disk_write(int i, uint64_t lba, const uint8_t *b);

static fat32_vol_t g_vol;
static int         g_have;          /* 1 FAT32, 2 exFAT, 0 none */

static int v_rd(void *ctx, uint64_t lba, uint8_t *buf){ return inst_disk_read((int)(long)ctx, lba, buf); }
static int v_wr(void *ctx, uint64_t lba, const uint8_t *buf){
    int d = (int)(long)ctx;
    if(!blk_removable(d)) return 0;              /* never, whatever asked */
    return inst_disk_write(d, lba, buf);
}

/* Find the first FAT32, else exFAT, volume on a removable disk. */
static int find(void){
    fat32_vol_t v[FAT32_MAX_VOLS];
    int n;
    g_have = 0;
    n = fat32_scan_volumes(v, FAT32_MAX_VOLS);
    for(int i = 0; i < n; i++)
        if(blk_removable(v[i].disk)){ g_vol = v[i]; g_have = 1; return 1; }
    n = exfat_scan_volumes(v, FAT32_MAX_VOLS);
    for(int i = 0; i < n; i++)
        if(blk_removable(v[i].disk)){ g_vol = v[i]; g_have = 2; return 1; }
    return 0;
}

/* Mount it for one call. A cached volume that no longer mounts (the
   stick was swapped or pulled) is looked for again, once. */
static int begin(int write){
    for(int tries = 0; tries < 2; tries++){
        if(!g_have || tries) find();
        if(!g_have) return 0;
        if(g_have == 1){
            if(fat32_mount_dev(v_rd, write ? v_wr : 0, (void *)(long)g_vol.disk, g_vol.lba)) return 1;
        } else {
            if(write) return 0;                  /* exFAT: read only */
            if(exfat_vol_mount_ro(&g_vol)) return 1;
        }
        g_have = 0;
    }
    return 0;
}
static void end(void){
    if(g_have == 2) exfat_unmount();
    else fat32_unmount();                        /* flushes, returns the system's */
}

/* Looked for every time: a stick can be pulled or swapped between two
   calls, and reading the partition tables again is a handful of sectors. */
int usbv_present(void){ return find(); }
int usbv_writable(void){ return usbv_present() && g_have == 1; }

/* "/USB", "/USB/a/b", "usb/a" -> 1 with *rest "" / "/a/b" / "/a" */
int usbv_path(const char *p, const char **rest){
    if(!p) return 0;
    while(*p == '/') p++;
    if((p[0] == 'U' || p[0] == 'u') && (p[1] == 'S' || p[1] == 's') &&
       (p[2] == 'B' || p[2] == 'b') && (p[3] == 0 || p[3] == '/')){
        *rest = p + 3;
        return 1;
    }
    return 0;
}

static void parents(const char *path){
    char buf[128];
    int i;
    for(i = 0; path[i] && i < (int)sizeof buf - 1; i++){
        buf[i] = path[i];
        if(path[i] == '/' && i > 0){
            buf[i] = 0;
            if(!fat32_exists(buf)) fat32_mkdir(buf);
            buf[i] = '/';
        }
    }
}

int usbv_read(const char *rest, char *buf, uint32_t max){
    int r;
    if(!begin(0)) return -1;
    if(g_have == 2){
        exfat_dirent_t st;
        r = (exfat_stat(rest, &st) == EXFAT_OK && !st.is_dir)
            ? exfat_read(rest, 0, buf, (uint32_t)(st.size < max ? st.size : max)) : -1;
    } else r = fat32_read(rest, buf, max);
    end();
    return r < 0 ? -1 : r;
}

int usbv_write(const char *rest, const char *data, uint32_t len){
    int ok;
    if(!begin(1)){
        if(g_have == 2) klog(KLOG_WARN, "/USB: the stick is exFAT, which HavenDOS reads but cannot write");
        return -1;
    }
    parents(rest);
    ok = fat32_write(rest, data, len) == 1;
    end();
    return ok ? (int)len : -1;
}

int usbv_exists(const char *rest){
    int r;
    if(!*rest || !strcmp(rest, "/")) return usbv_present();
    if(!begin(0)) return 0;
    if(g_have == 2){ exfat_dirent_t st; r = exfat_stat(rest, &st) == EXFAT_OK; }
    else r = fat32_exists(rest) ? 1 : 0;
    end();
    return r;
}

int usbv_is_dir(const char *rest){
    int r;
    if(!*rest || !strcmp(rest, "/")) return usbv_present();
    if(!begin(0)) return 0;
    if(g_have == 2){ exfat_dirent_t st; r = exfat_stat(rest, &st) == EXFAT_OK && st.is_dir; }
    else r = fat32_is_dir(rest) ? 1 : 0;
    end();
    return r;
}

/* fs/fat32.c calls back with its sector buffer in use, so the names are
   collected first and handed on after the stick is let go -- a caller
   that reads a file from inside its callback (Files does) must not find
   the driver mid-walk. */
#define LIST_MAX 128
static char     l_name[LIST_MAX][64];
static int      l_dir[LIST_MAX];
static uint32_t l_size[LIST_MAX];
static int      l_n;
static void collect(const char *n, int d, uint32_t sz){
    if(l_n >= LIST_MAX) return;
    strncpy(l_name[l_n], n, 63); l_name[l_n][63] = 0;
    l_dir[l_n] = d; l_size[l_n] = sz; l_n++;
}
void usbv_list_dir(const char *rest, void (*cb)(const char *, int, uint32_t)){
    l_n = 0;
    if(!begin(0)) return;
    if(g_have == 2){
        exfat_dir_t d; exfat_dirent_t e;
        if(exfat_opendir(*rest ? rest : "/", &d) == EXFAT_OK)
            while(exfat_readdir(&d, &e) == 1)
                collect(e.name, e.is_dir, e.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e.size);
    } else fat32_list_dir(*rest ? rest : "/", collect);
    end();
    for(int i = 0; i < l_n; i++)
        if(strcmp(l_name[i], ".") && strcmp(l_name[i], "..")) cb(l_name[i], l_dir[i], l_size[i]);
}

int usbv_mkdir(const char *rest){
    int ok;
    if(!begin(1)) return -1;
    parents(rest);
    ok = fat32_exists(rest) ? fat32_is_dir(rest) : fat32_mkdir(rest) == 1;
    end();
    return ok ? 0 : -1;
}

int usbv_delete(const char *rest){
    int ok;
    if(!begin(1)) return -1;
    ok = fat32_is_dir(rest) ? fat32_rmdir(rest) == 1 : fat32_delete(rest) == 1;
    end();
    return ok ? 0 : -1;
}

int usbv_rename(const char *from, const char *to){
    int ok;
    if(!begin(1)) return -1;
    ok = fat32_rename(from, to) == 1;
    end();
    return ok ? 0 : -1;
}

/* the volume's label, or "USB", for Files' title */
const char *usbv_label(void){
    static char l[12];
    int k = 0;
    if(!usbv_present()) return "";
    for(int i = 0; i < 11 && g_vol.label[i]; i++) l[k++] = g_vol.label[i];
    while(k > 0 && l[k - 1] == ' ') k--;
    l[k] = 0;
    /* bus-label-ok: the folder's name, /USB, when the stick has no label */
    return k ? l : "USB";
}
