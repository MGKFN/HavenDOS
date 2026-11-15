/* fs/migrate.c -- v0.9.6: the kernel half of `migrate`.
 *
 * WHAT IT DOES, IN THE ORDER IT DOES IT
 *
 *   mig_run(), on a system running from FAT16:
 *     0. asks every question first: the target is still what the survey
 *        saw, the ESP's menu can be written (a dry run of the real write),
 *        the kernel image can be read, a copy buffer exists. Nothing has
 *        been written when any of these says no.
 *     1. the table: a gap gets a new HavenDOS partition; an empty partition
 *        is retyped to HavenDOS (removed and added again over exactly the
 *        same sectors -- fs/gpt_write.c has no "retype", and two narrow,
 *        tested calls are better than a third, wider one).
 *     2. the volume: FAT32, with the kernel this system booted and a menu,
 *        by fs/install.c's own writers.
 *     3. the files: everything on the FAT16 volume but /boot, each read
 *        back by size; the names FAT16 cannot hold under their real names.
 *     4. the ESP's menu: the new install first and named by its serial,
 *        Windows second and still the default, the old install third.
 *   Until step 4 the machine boots exactly as it did, and step 4 adds an
 *   entry; it removes none. Nothing on the FAT16 volume is written at all,
 *   except that reading a pre-0.9.3 dotfile renames its entry, which the
 *   driver does on any read.
 *
 *   mig_finish(), on the NEW system, booted by its own menu entry:
 *     1. the ESP's menu without the old entry;
 *     2. the old volume's boot sector zeroed, so nothing can mount it;
 *     3. the old partition's table entry removed (slot, type and start
 *        must all match, as for uninstall).
 *   The order is the safe one: a power cut between any two steps leaves a
 *   menu that boots, and at worst an entry or an unmountable partition
 *   that the next `migrate finish` finishes.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/migrate.h"
#include "../include/instdisk.h"
#include "../include/gpt.h"
#include "../include/gpt_write.h"
#include "../include/fat16.h"
#include "../include/fat32.h"
#include "../include/esp.h"
#include "../include/install_add.h"
#include "../include/blkdev.h"
#include "../include/klog.h"

extern int  vfs_using_fat16(void);
extern int  vfs_using_fat32(void);
extern void *pmm_alloc_contig(uint32_t nblocks);
extern void  pmm_free_contig(void *p, uint32_t nblocks);

static const uint8_t T_HAVEN[16] = { 'H','A','V','E','N','D','O','S',
                                     '-','P','A','R','T','-','0','1' };
static int geq(const uint8_t *a, const uint8_t *b){
    for(int i = 0; i < 16; i++) if(a[i] != b[i]) return 0;
    return 1;
}
static uint32_t ld32(const uint8_t *p){
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* gpt_write's callbacks: 0 is success there, 1 is success here. */
static int g_disk = -1;
static int gw_rd(void *c, uint64_t l, void *b){ (void)c; return inst_disk_read(g_disk, l, (uint8_t *)b) ? 0 : -1; }
static int gw_wr(void *c, uint64_t l, const void *b){ (void)c; return inst_disk_write(g_disk, l, (const uint8_t *)b) ? 0 : -1; }

/* ── the survey ────────────────────────────────────────────────────── */
static void refuse(mig_survey_t *s, int slot, uint64_t sectors, const char *why){
    if(s->n_refused >= MIG_MAX_TARGETS) return;
    s->refused[s->n_refused].slot = slot;
    s->refused[s->n_refused].sectors = sectors;
    strncpy(s->refused[s->n_refused].why, why, 39);
    s->refused[s->n_refused].why[39] = 0;
    s->n_refused++;
}

static int is_fat16_haven(const uint8_t *b){
    return b[510] == 0x55 && b[511] == 0xAA
        && b[3] == 'H' && b[4] == 'A' && b[5] == 'V' && b[6] == 'E' && b[7] == 'N'
        && (b[22] | (b[23] << 8)) != 0;
}

int mig_survey(mig_survey_t *s)
{
    gpt_part_t p[GPT_MAX_PARTS];
    gpt_gap_t  gaps[GPT_MAX_PARTS + 1];
    uint8_t    bs[512];
    int n, i, ng;

    memset(s, 0, sizeof *s);
    s->disk = -1; s->sys_slot = -1; s->old_slot = -1;

    if(vfs_using_fat16() && fat16_dev() >= 0 && fat16_part_lba()){
        s->disk = fat16_dev(); s->sys_fs = 16; s->sys_first = fat16_part_lba();
    } else if(vfs_using_fat32() && fat32_system_part_lba()){
        s->disk = blk_boot_dev(); s->sys_fs = 32; s->sys_first = fat32_system_part_lba();
    } else return MIG_E_NOSYS;
    if(s->disk < 0) return MIG_E_NOSYS;
    g_disk = s->disk;

    if(!inst_disk_has_gpt(s->disk)) return MIG_E_NOGPT;
    n = inst_disk_gpt_parts(s->disk, p, GPT_MAX_PARTS);
    if(n <= 0) return MIG_E_NOGPT;

    if(inst_disk_read(s->disk, s->sys_first, bs))
        s->sys_serial = ld32(bs + (s->sys_fs == 32 ? 67 : 39));
    {
        uint32_t want;
        s->booted_by_serial = blk_sysvol_serial(&want) && want == s->sys_serial;
    }

    for(i = 0; i < n; i++){
        uint64_t sz = p[i].last_lba - p[i].first_lba + 1;
        if(p[i].is_esp){ s->has_esp = 1; continue; }
        if(p[i].first_lba == s->sys_first){
            s->sys_slot = p[i].slot; s->sys_sectors = sz;
            continue;
        }
        if(geq(p[i].type_guid, T_HAVEN)){
            /* another HavenDOS partition: the old one, if it is FAT16 */
            if(inst_disk_read(s->disk, p[i].first_lba, bs) && is_fat16_haven(bs)){
                s->n_old++;
                s->old_slot = p[i].slot; s->old_first = p[i].first_lba;
                s->old_sectors = sz; s->old_serial = ld32(bs + 39);
            } else s->n_moved++;
            continue;
        }
        if(s->sys_fs != 16) continue;               /* targets only matter there */

        if(!mig_type_takeable(p[i].type_guid)){
            if(sz >= MIG_MIN_SECTORS) refuse(s, p[i].slot, sz, mig_type_name(p[i].type_guid));
            continue;
        }
        if(sz < MIG_MIN_SECTORS){ refuse(s, p[i].slot, sz, "smaller than 512 MB"); continue; }
        {
            static uint8_t s0[512], s2[512], s7[512], s128[512];
            uint64_t f = p[i].first_lba;
            const char *sig = mig_fs_signature(
                inst_disk_read(s->disk, f,       s0)   ? s0   : 0,
                inst_disk_read(s->disk, f + 2,   s2)   ? s2   : 0,
                inst_disk_read(s->disk, f + 7,   s7)   ? s7   : 0,
                inst_disk_read(s->disk, f + 128, s128) ? s128 : 0);
            if(sig){
                char why[40] = "holds ";
                strncat(why, sig, sizeof why - strlen(why) - 1);
                refuse(s, p[i].slot, sz, why);
                continue;
            }
        }
        if(s->n_targets < MIG_MAX_TARGETS){
            mig_target_t *t = &s->targets[s->n_targets++];
            t->kind = MIG_T_PART;
            t->first = p[i].first_lba; t->last = p[i].last_lba; t->sectors = sz;
            t->slot = p[i].slot;
            memcpy(t->type_guid, p[i].type_guid, 16);
            memcpy(t->name, p[i].name, sizeof t->name);
            t->blank = 1;
            for(uint64_t k = 0; k < 64 && t->blank; k++){
                if(!inst_disk_read(s->disk, p[i].first_lba + k, bs)){ t->blank = 0; break; }
                for(int j = 0; j < 512; j++) if(bs[j]){ t->blank = 0; break; }
            }
        }
    }
    if(s->sys_slot < 0) return MIG_E_NOSYS;      /* the table does not know us */
    if(!s->has_esp)     return MIG_E_NOESP;

    if(s->sys_fs == 16){
        ng = gpt_find_gaps(gw_rd, 0, gaps, GPT_MAX_PARTS + 1);
        for(i = 0; i < ng && s->n_targets < MIG_MAX_TARGETS; i++){
            uint64_t f = (gaps[i].first_lba + MIG_ALIGN - 1) / MIG_ALIGN * MIG_ALIGN;
            if(f > gaps[i].last_lba) continue;
            uint64_t sz = (gaps[i].last_lba - f + 1) / MIG_ALIGN * MIG_ALIGN;
            if(sz < MIG_MIN_SECTORS) continue;
            mig_target_t *t = &s->targets[s->n_targets++];
            t->kind = MIG_T_GAP; t->first = f; t->sectors = sz;
            t->last = f + sz - 1; t->slot = -1;
        }
    }
    return MIG_OK;
}

/* ── the copy ──────────────────────────────────────────────────────────
   Names first, files after: fat16_list_path() calls back with the
   driver's sector buffer in use, so nothing may read the volume from
   inside the callback. Each directory's listing is collected into this
   table, then walked. */
#define MIG_MAX_ENT   1024
#define MIG_PATH      96
typedef struct { char name[16]; int dir; uint32_t size; } ent_t;
static ent_t g_ent[MIG_MAX_ENT];
static int   g_nent, g_over;
static void collect(const char *n, int d, uint32_t sz){
    if(g_nent >= MIG_MAX_ENT){ g_over = 1; return; }
    strncpy(g_ent[g_nent].name, n, 15); g_ent[g_nent].name[15] = 0;
    g_ent[g_nent].dir = d; g_ent[g_nent].size = sz;
    g_nent++;
}

/* the new volume, through a writer that cannot leave it */
static uint64_t g_v_first, g_v_sectors;
static int v_rd(void *c, uint64_t l, uint8_t *b){ (void)c; return inst_disk_read(g_disk, l, b); }
static int v_wr(void *c, uint64_t l, const uint8_t *b){
    (void)c;
    if(l < g_v_first || l >= g_v_first + g_v_sectors) return 0;
    return inst_disk_write(g_disk, l, b);
}

static char    *g_buf;
static uint32_t g_cap, g_blocks;
static mig_result_t *g_r;

static void skipped(const char *path){
    if(g_r->n_skipped < 4){
        strncpy(g_r->skipped[g_r->n_skipped], path, 47);
        g_r->skipped[g_r->n_skipped][47] = 0;
    }
    g_r->n_skipped++;
}

static int copy_one(const char *src, const char *dst, int root, uint32_t size){
    int n;
    if(size >= g_cap){ skipped(src); return 1; }       /* reported, not fatal */
    n = root ? fat16_read(src, g_buf, g_cap) : fat16_read_path(src, g_buf, g_cap);
    if(n < 0 || (uint32_t)n != size) return 0;
    if(fat32_write(dst, g_buf, (uint32_t)n) != 1) return 0;
    if(fat32_file_size(dst) != (uint32_t)n) return 0;
    g_r->n_files++; g_r->bytes += (uint32_t)n;
    return 1;
}

static int is_root_skip(const char *n){
    /* what the new install already wrote, from this very system */
    return !strcmp(n, "boot") || !strcmp(n, "havendos.txt");
}

/* Depth-first over the FAT16 volume. `dir` is "" for the root. */
static int copy_tree(const char *dir, int depth){
    int first = g_nent, count, i;
    if(depth > 8) return 1;                          /* nothing nests that deep */
    if(!fat16_list_path(dir, collect)) return 0;
    if(g_over) return 0;
    count = g_nent - first;
    for(i = 0; i < count; i++){
        ent_t e = g_ent[first + i];
        char src[MIG_PATH], dst[MIG_PATH];
        if(!dir[0] && (is_root_skip(e.name) || mig_is_mapped_name(e.name))) continue;
        if(strlen(dir) + strlen(e.name) + 3 >= MIG_PATH){ skipped(e.name); continue; }
        if(dir[0]){ strcpy(src, dir); strcat(src, "/"); strcat(src, e.name); }
        else strcpy(src, e.name);
        dst[0] = '/'; strcpy(dst + 1, src);
        if(e.dir){
            if(!fat32_is_dir(dst) && !fat32_mkdir(dst)) return 0;
            g_r->n_dirs++;
            if(!copy_tree(src, depth + 1)) return 0;
        } else if(!copy_one(src, dst, !dir[0], e.size)) return 0;
    }
    g_nent = first;                                  /* give the slots back */
    return 1;
}

static int copy_long_names(void){
    for(int i = 0; MIG_LONG_NAMES[i]; i++){
        const char *nm = MIG_LONG_NAMES[i];
        char dst[24];
        int n = fat16_read(nm, g_buf, g_cap);
        if(n < 0) continue;                          /* this system never made it */
        if((uint32_t)n >= g_cap){ skipped(nm); continue; }
        dst[0] = '/'; strcpy(dst + 1, nm);
        if(fat32_write(dst, g_buf, (uint32_t)n) != 1) return 0;
        if(fat32_file_size(dst) != (uint32_t)n) return 0;
        g_r->n_files++; g_r->bytes += (uint32_t)n;
    }
    return 1;
}

static int buf_get(void){
    static const uint32_t try_mb[] = { 64, 32, 16, 8, 4 };
    for(unsigned i = 0; i < sizeof try_mb / sizeof try_mb[0]; i++){
        uint32_t blocks = try_mb[i] * 256u;
        g_buf = (char *)pmm_alloc_contig(blocks);
        if(g_buf){ g_blocks = blocks; g_cap = try_mb[i] * 1024u * 1024u; return 1; }
    }
    return 0;
}
static void buf_put(void){
    if(g_buf) pmm_free_contig(g_buf, g_blocks);
    g_buf = 0; g_cap = 0; g_blocks = 0;
}

/* ── the ESP menu ──────────────────────────────────────────────────── */
static char g_menu[2048];
static int write_menu(int disk, uint32_t new_ser, uint32_t old_ser, int dry){
    esp_report_t rep;
    int rc;
    if(mig_esp_menu(g_menu, sizeof g_menu, new_ser, old_ser) < 0) return -100;
    esp_set_menu(g_menu);
    rc = esp_install(disk, &rep, 0, dry);
    esp_set_menu(0);
    return rc;
}

/* ── undoing steps 1-3 ─────────────────────────────────────────────────
   If the volume or the copy fails, the menu has not been touched and the
   machine boots its FAT16 install exactly as before -- but a half-made
   partition left in the table would sit there, and the next `migrate`
   would not offer that space again. So it goes: the boot sector first
   (nothing mounts it), then the entry, then, for a partition that was
   somebody's empty one, the entry it had, over the same sectors. */
static void rollback(const mig_target_t *t, int entry){
    uint8_t z[512];
    memset(z, 0, sizeof z);
    (void)inst_disk_write(g_disk, t->first, z);
    if(entry >= 0)
        (void)gpt_remove_partition(gw_rd, gw_wr, 0, entry, T_HAVEN, t->first, 0, 0);
    if(t->kind == MIG_T_PART)
        (void)gpt_add_partition(gw_rd, gw_wr, 0, t->first, t->last, t->type_guid,
                                t->name[0] ? t->name : "Basic data partition", 0, 0, 0);
}

/* ── mig_run ───────────────────────────────────────────────────────── */
int mig_run(const mig_survey_t *s, int ti, mig_cb cb, mig_result_t *r)
{
    static char part_cfg[512];
    mig_survey_t now;
    const mig_target_t *t;
    int rc, total = 6, entry = -1;

    memset(r, 0, sizeof *r);
    g_r = r;
    if(s->sys_fs == 32 && s->n_old)            return r->rc = MIG_E_TWO;
    if(s->sys_fs != 16)                        return r->rc = MIG_E_ALREADY32;
    if(ti < 0 || ti >= s->n_targets)           return r->rc = MIG_E_TARGET;

    /* The disk has not changed under us: survey again and find the same
       target, the same range. */
    rc = mig_survey(&now);
    if(rc != MIG_OK)                           return r->rc = rc;
    t = 0;
    for(int i = 0; i < now.n_targets; i++)
        if(now.targets[i].kind == s->targets[ti].kind
           && now.targets[i].first == s->targets[ti].first
           && now.targets[i].sectors == s->targets[ti].sectors){ t = &now.targets[i]; break; }
    if(!t)                                     return r->rc = MIG_E_TARGET;
    if(now.n_old || now.n_moved)               return r->rc = MIG_E_TWO;
    g_disk = now.disk;
    if(inst_disk_is_readonly(g_disk))          return r->rc = MIG_E_READONLY;

    r->first = t->first; r->sectors = t->sectors;
    r->serial = havendos_vol_serial(t->first);
    if(r->serial == now.sys_serial) r->serial ^= 0x00010001u;  /* never the same */

    /* ── 0: every question before any write ─────────────────────── */
    r->step = 0;
    if(cb) cb(0, total, "Checking the boot menu can be written...");
    r->esp_rc = write_menu(g_disk, r->serial, now.sys_serial, 1);
    if(r->esp_rc != ESP_OK)                    return r->rc = MIG_E_ESPPLAN;
    if(mig_part_menu(part_cfg, sizeof part_cfg, r->serial) < 0)
                                               return r->rc = MIG_E_INSTALL;
    if(t->kind == MIG_T_PART){
        rc = gpt_remove_partition(gw_rd, 0, 0, t->slot, t->type_guid, t->first, 0, 0);
        if(rc != GPTW_OK)                      return r->rc = MIG_E_TABLE;
    } else {
        rc = gpt_add_partition(gw_rd, 0, 0, t->first, t->last, T_HAVEN, "HavenDOS",
                               &entry, 0, 0);
        if(rc != GPTW_OK)                      return r->rc = MIG_E_TABLE;
    }
    if(!buf_get())                             return r->rc = MIG_E_NOMEM;


    /* ── 1: the table ───────────────────────────────────────────── */
    r->step = 1;
    if(cb) cb(1, total, t->kind == MIG_T_PART ? "Claiming the empty partition..."
                                              : "Adding a partition in the free space...");
    if(t->kind == MIG_T_PART){
        rc = gpt_remove_partition(gw_rd, gw_wr, 0, t->slot, t->type_guid, t->first, 0, 0);
        if(rc != GPTW_OK){ rc = MIG_E_TABLE; goto out; }
    }
    rc = gpt_add_partition(gw_rd, gw_wr, 0, t->first, t->last, T_HAVEN, "HavenDOS",
                           &entry, 0, 0);
    if(rc != GPTW_OK){ rc = MIG_E_TABLE; goto out; }
    r->slot = entry;

    /* ── 2: the volume ──────────────────────────────────────────── */
    r->step = 2;
    if(cb) cb(2, total, "Formatting FAT32 and copying the kernel...");
    rc = havendos_install_volume32(g_disk, t->first, t->sectors, r->serial, part_cfg);
    havendos_install_image_release();      /* the copy wants the memory */
    if(rc != INSTADD_OK){
        klog(KLOG_WARN, "migrate: volume32 rc=%d", rc);
        rollback(t, entry); rc = MIG_E_INSTALL; goto out;
    }

    /* ── 3: the files ───────────────────────────────────────────── */
    r->step = 3;
    if(cb) cb(3, total, "Copying your accounts, settings and files...");
    g_v_first = t->first; g_v_sectors = t->sectors;
    if(!fat32_mount_dev(v_rd, v_wr, 0, t->first)){ rollback(t, entry); rc = MIG_E_COPY; goto out; }
    g_nent = 0; g_over = 0;
    if(!copy_long_names() || !copy_tree("", 0)){
        fat32_unmount(); rollback(t, entry); rc = MIG_E_COPY; goto out;
    }
    fat32_flush();
    fat32_unmount();

    /* ── 4: the boot menu ───────────────────────────────────────── */
    r->step = 4;
    if(cb) cb(4, total, "Adding the new install to the boot menu...");
    r->esp_rc = write_menu(g_disk, r->serial, now.sys_serial, 0);
    if(r->esp_rc != ESP_OK){ rc = MIG_E_ESP; goto out; }

    r->step = 5;
    if(cb) cb(total, total, "Done.");
    rc = MIG_OK;
out:
    buf_put();
    return r->rc = rc;
}

/* ── mig_finish ────────────────────────────────────────────────────── */
int mig_finish(const mig_survey_t *s, mig_cb cb, mig_result_t *r)
{
    mig_survey_t now;
    uint8_t z[512];
    int rc;

    memset(r, 0, sizeof *r);
    (void)s;
    rc = mig_survey(&now);
    if(rc != MIG_OK)                           return r->rc = rc;
    if(now.sys_fs != 32)                       return r->rc = MIG_E_NOT32;
    if(!now.booted_by_serial)                  return r->rc = MIG_E_NOTBOOTED;
    if(now.n_old == 0)                         return r->rc = MIG_E_NOOLD;
    if(now.n_old > 1)                          return r->rc = MIG_E_MANYOLD;
    g_disk = now.disk;
    if(inst_disk_is_readonly(g_disk))          return r->rc = MIG_E_READONLY;

    /* all three asked before any is done */
    r->esp_rc = write_menu(g_disk, now.sys_serial, 0, 1);
    if(r->esp_rc != ESP_OK)                    return r->rc = MIG_E_ESPPLAN;
    if(gpt_remove_partition(gw_rd, 0, 0, now.old_slot, T_HAVEN, now.old_first, 0, 0) != GPTW_OK)
                                               return r->rc = MIG_E_TABLE;

    r->step = 1;
    if(cb) cb(1, 3, "Taking the old install out of the boot menu...");
    r->esp_rc = write_menu(g_disk, now.sys_serial, 0, 0);
    if(r->esp_rc != ESP_OK){ rc = MIG_E_ESP; goto out; }

    r->step = 2;
    if(cb) cb(2, 3, "Erasing the old volume's boot sector...");
    memset(z, 0, sizeof z);
    if(!inst_disk_write(g_disk, now.old_first, z)){ rc = MIG_E_TABLE; goto out; }

    r->step = 3;
    if(cb) cb(3, 3, "Removing the old partition from the table...");
    if(gpt_remove_partition(gw_rd, gw_wr, 0, now.old_slot, T_HAVEN, now.old_first, 0, 0) != GPTW_OK){
        rc = MIG_E_TABLE; goto out;
    }
    r->slot = now.old_slot;
    r->first = now.old_first; r->sectors = now.old_sectors;
    r->freed_mb = (uint32_t)(now.old_sectors / 2048u);
    rc = MIG_OK;
out:
    return r->rc = rc;
}

const char *mig_strerror(int rc)
{
    switch(rc){
    case MIG_OK:          return "done";
    case MIG_E_NOSYS:     return "this is not an installed HavenDOS running from its own disk";
    case MIG_E_NOGPT:     return "the system disk has no GPT -- migrate is for UEFI installs";
    case MIG_E_NOESP:     return "no EFI System Partition on the system disk";
    case MIG_E_ALREADY32: return "this system is already FAT32";
    case MIG_E_TARGET:    return "that place is not free any more -- run 'migrate' again";
    case MIG_E_ESPPLAN:   return "the boot menu on the EFI partition could not be written (nothing changed)";
    case MIG_E_TABLE:     return "the partition table change failed";
    case MIG_E_INSTALL:   return "formatting the new partition or copying the kernel failed";
    case MIG_E_COPY:      return "a file did not copy -- the new partition was taken back out; your FAT16 install is untouched";
    case MIG_E_ESP:       return "the boot menu write failed";
    case MIG_E_NOMEM:     return "no memory for the copy buffer (nothing changed)";
    case MIG_E_NOT32:     return "run this from the NEW FAT32 install, not the old one";
    case MIG_E_NOTBOOTED: return "this boot did not come from the new menu entry -- reboot, pick the first HavenDOS entry";
    case MIG_E_NOOLD:     return "there is no old FAT16 install left on this disk";
    case MIG_E_MANYOLD:   return "more than one old FAT16 install -- refusing to pick one";
    case MIG_E_READONLY:  return "this disk has no write path";
    case MIG_E_TWO:       return "a move is already done: boot the new install, then 'migrate finish'";
    }
    return "unknown error";
}
