/* ================================================================
   fs/uninstall.c -- see include/uninstall.h for the rules.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "uninstall.h"
#include "esp.h"
#include "gpt.h"
#include "gpt_write.h"
#include "fat32.h"

extern int inst_disk_read(int idx, uint64_t lba, uint8_t *buf);
extern int inst_disk_write(int idx, uint64_t lba, const uint8_t *buf);
extern int inst_disk_is_readonly(int i);
extern int inst_disk_gpt_parts(int i, gpt_part_t *out, int max);
extern int inst_disk_has_gpt(int i);

#define P_DIR_HAVENDOS  "/EFI/HAVENDOS"
#define P_LOADER        "/EFI/HAVENDOS/grubx64.efi"
#define P_CFG           "/EFI/HAVENDOS/grub.cfg"
#define P_FALLBACK      "/EFI/BOOT/BOOTX64.EFI"
#define P_FALLBACK_BAK  "/EFI/BOOT/BOOTX64.HDB"
#define P_WINDOWS       "/EFI/Microsoft/Boot/bootmgfw.efi"

/* The label fs/espmake.c writes, and nothing else does. */
#define OUR_ESP_LABEL   "HAVENBOOT"

static int s_un_disk;

static int un_rd(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx; return inst_disk_read(s_un_disk, lba, buf);
}
static int un_wr(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx; return inst_disk_write(s_un_disk, lba, buf);
}
/* gpt_write.c's callbacks return 0 for success; the installer's disk
   layer returns 1. Converting here rather than at each call site,
   because the one place this was written out by hand in fs/espmake.c
   is the kind of line that gets copied with its sense inverted. */
static int gw_rd(void *ctx, uint64_t lba, void *buf){
    (void)ctx; return inst_disk_read(s_un_disk, lba, (uint8_t *)buf) ? 0 : -1;
}
static int gw_wr(void *ctx, uint64_t lba, const void *buf){
    (void)ctx; return inst_disk_write(s_un_disk, lba, (const uint8_t *)buf) ? 0 : -1;
}

/* ── is this ESP one we made? ────────────────────────────────────
   Two independent facts, both required. The FAT32 volume label is at
   offset 0x47 of the boot sector and only fs/espmake.c ever writes
   HAVENBOOT there; \EFI\Microsoft is Windows saying the partition is
   its own. Either alone would be too thin a reason to delete
   somebody's boot partition. */
static int esp_is_ours(uint64_t esp_lba)
{
    uint8_t s[512];
    int i;
    if(fat32_exists(P_WINDOWS)) return 0;
    if(!inst_disk_read(s_un_disk, esp_lba, s)) return 0;
    for(i = 0; OUR_ESP_LABEL[i]; i++)
        if(s[0x47 + i] != (uint8_t)OUR_ESP_LABEL[i]) return 0;
    return 1;
}

static int mount_for(int disk, uninst_report_t *r, int want_write,
                     uint64_t *esp_lba_out, int *esp_idx_out)
{
    gpt_part_t p[GPT_MAX_PARTS];
    int n, i;

    s_un_disk = disk;
    if(!inst_disk_has_gpt(disk)) return UNINST_E_NOGPT;
    n = inst_disk_gpt_parts(disk, p, GPT_MAX_PARTS);
    if(n <= 0) return UNINST_E_NOGPT;

    /* Our own partition, by type GUID. This is the whole reason the
       uninstall is safer than diskpart: there is no size to squint at
       and no partition number to mistype. */
    r->part_index = -1;
    for(i = 0; i < n; i++){
        int k, match = 1;
        for(k = 0; k < 16; k++)
            if(p[i].type_guid[k] != GPT_TYPE_HAVENDOS[k]){ match = 0; break; }
        if(match){
            r->part_index   = i;
            r->part_first   = p[i].first_lba;
            r->part_sectors = p[i].last_lba - p[i].first_lba + 1;
            break;
        }
    }

    for(i = 0; i < n; i++){
        if(!p[i].is_esp) continue;
        if(!fat32_mount_dev(un_rd, want_write ? un_wr : 0, 0, p[i].first_lba))
            return UNINST_E_MOUNT;
        r->esp_lba     = p[i].first_lba;
        r->esp_sectors = p[i].last_lba - p[i].first_lba + 1;
        if(esp_lba_out) *esp_lba_out = p[i].first_lba;
        if(esp_idx_out) *esp_idx_out = i;
        return UNINST_OK;
    }
    return UNINST_E_NOESP;
}

int uninst_survey(int disk, uninst_report_t *r)
{
    int rc, esp_idx = -1;
    uint64_t esp_lba = 0;

    if(!r) return UNINST_E_PARAM;
    memset(r, 0, sizeof *r);
    r->part_index = -1;
    r->esp_index  = -1;

    /* Read-only: mounted with no write function, so nothing reachable
       from here can put a byte on somebody's boot partition. */
    rc = mount_for(disk, r, 0, &esp_lba, &esp_idx);
    r->esp_rc = rc;
    if(rc != UNINST_OK){
        /* No ESP does not mean nothing to do -- the partition may
           still be in the table, and mount_for() found it before it
           looked for an ESP. */
        return (r->part_index >= 0) ? UNINST_OK : rc;
    }

    r->has_havendos_dir = fat32_exists(P_DIR_HAVENDOS);
    r->has_our_loader   = fat32_exists(P_LOADER);
    r->has_backup       = fat32_exists(P_FALLBACK_BAK);

    if(fat32_exists(P_FALLBACK)){
        /* Ours, or somebody else's? The test fs/esp.c uses: our
           loader is byte-for-byte \EFI\HAVENDOS\grubx64.efi. Without
           that file to compare against we cannot claim it is ours,
           and the safe answer to "cannot claim" is "leave it". */
        if(r->has_our_loader){
            uint32_t a = fat32_file_size(P_FALLBACK);
            uint32_t b = fat32_file_size(P_LOADER);
            r->fallback_is_ours = (a == b && a != 0);
        }
        r->fallback_foreign = !r->fallback_is_ours;
    }

    if(esp_is_ours(esp_lba)){
        r->esp_is_ours = 1;
        r->esp_index   = esp_idx;
    }

    fat32_unmount();

    if(!r->has_havendos_dir && !r->has_backup && !r->fallback_is_ours
       && r->part_index < 0 && !r->esp_is_ours)
        return UNINST_E_NOTHING;
    return UNINST_OK;
}

/* Copy a file within the mounted volume, and VERIFY it before the
   caller is allowed to delete the original.

   Whole-file through a PMM allocation rather than streamed in chunks,
   because fs/fat32.c has no append -- fat32_write() replaces -- and
   because this is exactly what fs/esp.c does to make the backup in
   the first place. The same shape in both directions is one fewer
   thing to reason about.

   The verify is not ceremony. This is the step that puts the
   machine's original boot loader back; the next thing the caller
   does is delete the only other copy of it. */
static int copy_verified(const char *from, const char *to)
{
    extern void *pmm_alloc_contig(uint32_t nblocks);
    extern void  pmm_free_contig(void *p, uint32_t nblocks);
    uint32_t sz = fat32_file_size(from);
    uint32_t blocks;
    uint8_t *buf, *back;
    int ok;

    if(sz == 0 || sz > 16u * 1024u * 1024u) return 0;
    blocks = (sz + 4095u) / 4096u;

    buf = (uint8_t *)pmm_alloc_contig(blocks);
    if(!buf) return 0;
    ok = (fat32_read_at(from, 0, (char *)buf, sz) == (int)sz);
    if(ok) ok = fat32_write(to, (const char *)buf, sz);

    if(ok){
        back = (uint8_t *)pmm_alloc_contig(blocks);
        if(!back){ ok = 0; }
        else {
            ok = (fat32_file_size(to) == sz)
              && (fat32_read_at(to, 0, (char *)back, sz) == (int)sz)
              && (memcmp(buf, back, sz) == 0);
            pmm_free_contig(back, blocks);
        }
    }

    pmm_free_contig(buf, blocks);
    return ok;
}

int uninst_run(int disk, int dry_run, uninst_report_t *r)
{
    int rc, esp_idx = -1;
    uint64_t esp_lba = 0;
    uninst_report_t sv;

    if(!r) return UNINST_E_PARAM;
    if(!dry_run && inst_disk_is_readonly(disk)) return UNINST_E_READONLY;

    rc = uninst_survey(disk, &sv);
    if(rc != UNINST_OK) return rc;
    *r = sv;

    /* ── 1 and 2: the boot loader and our files, on the ESP ─────── */
    if(sv.esp_rc == UNINST_OK){
        rc = mount_for(disk, r, !dry_run, &esp_lba, &esp_idx);
        if(rc != UNINST_OK) return rc;

        /* THE LOADER COMES FIRST. A machine that loses power after
           this step boots the way it did before HavenDOS existed. */
        if(sv.has_backup){
            if(!dry_run){
                if(!copy_verified(P_FALLBACK_BAK, P_FALLBACK)){
                    fat32_unmount(); return UNINST_E_LOADER;
                }
                if(!fat32_delete(P_FALLBACK_BAK)){
                    fat32_unmount(); return UNINST_E_LOADER;
                }
            }
            r->did_restore_loader = 1;
            r->did_delete_backup  = 1;
        } else if(sv.fallback_is_ours){
            /* No backup means there was no fallback loader before us,
               so the right end state is no fallback loader at all --
               not somebody else's copied into place. */
            if(!dry_run && !fat32_delete(P_FALLBACK)){
                fat32_unmount(); return UNINST_E_LOADER;
            }
            r->did_restore_loader = 1;
        }
        /* sv.fallback_foreign: left exactly where it is, on purpose. */

        if(sv.has_havendos_dir){
            if(!dry_run){
                if(fat32_exists(P_CFG))    (void)fat32_delete(P_CFG);
                if(fat32_exists(P_LOADER)) (void)fat32_delete(P_LOADER);
                /* rmdir refuses a directory with anything left in it,
                   so a failure here means something we did not put
                   there is inside -- which is a reason to stop and
                   say so, not to force it. */
                if(!fat32_rmdir(P_DIR_HAVENDOS)){
                    fat32_unmount(); return UNINST_E_FILES;
                }
            }
            r->did_remove_files = 1;
        }
        fat32_unmount();
    }

    /* ── 3: our partition's table entry ─────────────────────────── */
    if(sv.part_index >= 0){
        s_un_disk = disk;
        rc = gpt_remove_partition(gw_rd, dry_run ? 0 : gw_wr, 0,
                                  sv.part_index, GPT_TYPE_HAVENDOS,
                                  sv.part_first, 0, 0);
        if(rc != GPTW_OK) return UNINST_E_PART;
        r->did_remove_part = 1;
        r->n_sectors_written += 4;
    }

    /* ── 4: the ESP, but only if we made it ─────────────────────── */
    if(sv.esp_is_ours && sv.esp_index >= 0){
        gpt_part_t p[GPT_MAX_PARTS];
        int n = inst_disk_gpt_parts(disk, p, GPT_MAX_PARTS);
        /* Re-read the table: step 3 just changed it, and acting on a
           stale index is exactly what gpt_remove_partition's
           three-way match exists to catch. Finding it again by its
           start LBA costs nothing and cannot be stale. */
        int i, idx = -1;
        for(i = 0; i < n; i++)
            if(p[i].is_esp && p[i].first_lba == sv.esp_lba){ idx = i; break; }
        if(idx >= 0){
            uint8_t t[16];
            for(i = 0; i < 16; i++) t[i] = p[idx].type_guid[i];
            s_un_disk = disk;
            rc = gpt_remove_partition(gw_rd, dry_run ? 0 : gw_wr, 0,
                                      idx, t, sv.esp_lba, 0, 0);
            if(rc != GPTW_OK) return UNINST_E_PART;
            r->did_remove_esp = 1;
            r->n_sectors_written += 4;
        }
    }

    return UNINST_OK;
}

const char *uninst_strerror(int code)
{
    switch(code){
    case UNINST_OK:         return "ok";
    case UNINST_E_NOGPT:    return "this disk has no GPT";
    case UNINST_E_NOESP:    return "no EFI System Partition on this disk";
    case UNINST_E_MOUNT:    return "the EFI System Partition would not mount";
    case UNINST_E_NOTHING:  return "there is no HavenDOS on this disk";
    case UNINST_E_READONLY: return "this disk is read-only";
    case UNINST_E_LOADER:   return "the original boot loader could not be restored";
    case UNINST_E_FILES:    return "\\EFI\\HAVENDOS could not be emptied -- something else is in it";
    case UNINST_E_PART:     return "the partition table entry could not be cleared";
    case UNINST_E_PARAM:    return "bad parameters";
    default:                return "unknown error";
    }
}
