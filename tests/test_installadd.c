/* ================================================================
   tests/test_installadd.c -- "will I lose Windows?", as a test
   ================================================================
   The whole additive install, run against a disk shaped like a 512 GB
   Windows 11 laptop with a shrunk partition, with one claim checked
   above all others:

       EVERY SECTOR on the disk that is not inside the new partition
       and not on the EFI System Partition is BYTE-IDENTICAL after the
       install, except four, whose LBAs the installer printed BEFORE
       it wrote anything.

   Not "the partitions still look right". Byte-identical, every sector
   the disk model has ever been given, compared one at a time.

   The dry run is checked the same way and more strictly: zero writes.
   Not "no writes we care about" -- the disk model counts every call.

   Headers: -Iinclude makes <string.h> resolve to the kernel's own, so
   this file includes the kernel one (via the fs/*.c it pulls in) and
   declares nothing from the host's. tests/test_gptwrite.c does the
   opposite and both say why.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* The kernel image the installer would copy. Supplied here so the suite
   does not need a GRUB build; 300 KB is enough to span many clusters and
   exercise the chaining, and it starts with a real ELF magic because
   havendos_install_image_available() checks for one. */
#define FAKE_KERNEL_SZ  307200u
static uint8_t  g_kernel[FAKE_KERNEL_SZ];
static uint32_t kernel_elf_size = FAKE_KERNEL_SZ;
#define kernel_elf_data g_kernel
#define INSTALL_KERNEL_PROVIDED 1

/* The UEFI loader esp.c would write. Same reasoning; it has to be
   PE32+-shaped because fs/esp.c refuses anything that is not. */
#define FAKE_LOADER_SZ  811008u
static uint8_t  g_loader[FAKE_LOADER_SZ];
static uint32_t g_loader_sz = FAKE_LOADER_SZ;
#define ESP_LOADER_PROVIDED 1
#define grub_efi_img       g_loader
#define grub_efi_img_size  g_loader_sz

/* The disk model's sizes. A whole install touches far more sectors than
   adding one partition does: two copies of an 811 KB loader onto the ESP,
   a 256-sector FAT written twice, a 300 KB kernel, plus the FAT32 ESP that
   mkfs.vfat built. */
#define MAXSEC   98304
#define MAXWLOG  65536

#include <unistd.h>
#include <sys/types.h>
#include "types.h"
#include "gpt.h"
#include "gpt_write.h"
#include "fat16fmt.h"

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/gpt.c"
#include "../fs/gpt_write.c"
#include "gptdisk.inc"
#include "../fs/fat16fmt.c"
/* v0.8.8.8: fs/install.c reserves space for an EFI System Partition
   and creates one when the disk has none, so the installer now
   depends on these two. They come in here rather than being stubbed
   because a stub would let the reservation arithmetic be tested
   against something that is not the code that runs. */
#include "../fs/fat32fmt.c"
#include "../fs/espmake.c"
#include "../fs/uninstall.c"

/* ── the installer's disk API, onto the model above ─────────────────── */
static int g_ro = 0;            /* pretend there is no write path       */
static int g_live = 0;          /* pretend this is a live session       */
/* Two fault modes, because they catch different bugs.

   g_wfail_at: every write from the Nth onward refuses -- a disk that has
   stopped accepting writes. Good for "does the installer stop", bad at
   pinpointing WHICH unchecked write let it carry on, because the next
   checked write fails too and reports the same error.

   g_wfail_once: exactly one write refuses and everything after it works --
   a single bad sector. This is the mode that catches an ignored return
   value, because the install then runs to the end, reports success, and
   has a truncated kernel on the disk. */
static int g_wfail_at   = -1;
/* v0.9.3: a write to this LBA reports success and is thrown away -- a
   disk that lies. The read-back at the end of the install is the only
   thing that can notice, and until this existed nothing showed that it
   does: deleting it passed the suite. */
static long long g_wdrop_lba = -1;
static int g_wfail_once = -1;
static int g_wcount = 0;

/* THE MODEL DISK IS INDEX 1, NOT 0, on purpose.

   Disk 0 exists in the table and every access to it fails. Anything in the
   installer that reaches for disk 0 instead of the disk it was handed --
   and there was such a thing: esp_install() used to take the disk out of
   the report struct, which a caller that zeroed the struct left as 0 --
   fails here instead of silently working because the only disk happened to
   be the right one. */
#define MODEL_DISK 1

int inst_disk_count(void){ return 2; }
uint64_t inst_disk_sectors(int i){
    return i == MODEL_DISK ? g_disk_sectors : 0;
}
int inst_disk_read(int i, uint64_t lba, uint8_t *b){
    if(i != MODEL_DISK) return 0;
    return d_read(0, lba, b) == 0;
}
int inst_disk_write(int i, uint64_t lba, const uint8_t *b){
    if(i != MODEL_DISK) return 0;
    if(g_ro) return 0;
    if(g_wfail_at >= 0 && g_wcount >= g_wfail_at){ g_wcount++; return 0; }
    if(g_wfail_once >= 0 && g_wcount == g_wfail_once){ g_wcount++; return 0; }
    g_wcount++;
    if(g_wdrop_lba >= 0 && (long long)lba == g_wdrop_lba) return 1;
    return d_write(0, lba, b) == 0;
}
int inst_disk_is_readonly(int i){ return i == MODEL_DISK ? g_ro : 1; }
int boot_is_live(void){ return g_live; }
int vfs_exists(const char *p){ (void)p; return 0; }

/* 0 on success, like fs/gpt.c wants; d_read already uses that convention,
   so this passes it through rather than inverting it. */
static int gptrd(void *ctx, uint64_t lba, void *buf){
    (void)ctx; return d_read(0, lba, buf);
}
int inst_disk_has_gpt(int i){
    return i == MODEL_DISK && gpt_present(gptrd, 0);
}
int inst_disk_gpt_parts(int i, gpt_part_t *out, int max){
    if(i != MODEL_DISK) return 0;
    return gpt_scan(gptrd, 0, out, max);
}

/* fs/fat32.c's own device back ends. It can mount through a supplied
   read/write pair -- which is what everything here uses -- but it still
   references the two built-in ones, so they have to resolve. They are
   never called: every mount in this suite passes functions explicitly. */
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_ready(void){ return 0; }
int ata_detected(void){ return 0; }
int ata_lba48(void){ return 0; }
int ata_read_sector   (uint32_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
/* uint64_t, which is what drivers/ata.c takes. fs/fat32.c used to
   declare these as uint32_t in its own local extern and this stub was
   written to match the declaration rather than the driver; the LBA48
   top-8-bit truncation is a bug this project has shipped once already.
   fs/blkdev.c declares them correctly and the compiler compares them. */
int ata_read_sector48 (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
void ata_init(void){}
uint32_t ata_sectors(void){ return 0; }
int virtio_scan_drives(void){ return 0; }
uint64_t virtio_drive_sectors(int i){ (void)i; return 0; }
uint8_t virtio_drive_bus(int i){ (void)i; return 0; }
uint8_t virtio_drive_dev(int i){ (void)i; return 0; }
int virtio_drive_read(int i, uint64_t l, uint8_t *b){ (void)i;(void)l;(void)b; return 0; }
int virtio_drive_write(int i, uint64_t l, const uint8_t *b){ (void)i;(void)l;(void)b; return 0; }

/* ── v0.8.6: the rest of what fs/blkdev.c's blk_scan() asks ───────────
   fs/fat32.c no longer carries a two-bus switch of its own, so every bus
   is asked whether it is there. All of them say no: this suite's disks
   are model GPT disks reached through functions it passes explicitly,
   and a real controller answering here would be a second disk nobody
   asked for. */
int nvme_count(void){ return 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
uint64_t nvme_sectors(int i){ (void)i; return 0; }
int      nvme_writable(int i){ (void)i; return 0; }
int      ahci_init(void){ return 0; }
int      ahci_count(void){ return 0; }
int      ahci_is_atapi(int i){ (void)i; return 0; }
uint64_t ahci_sectors(int i){ (void)i; return 0; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
int      msc_count(void){ return 0; }
uint64_t msc_sectors(int i){ (void)i; return 0; }
int      msc_readonly(int i){ (void)i; return 1; }
int msc_read_sector (int i, uint64_t l, uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
int msc_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }

#include "../fs/blkdev.c"

/* the PMM, for esp.c's backup buffer */
void *pmm_alloc_contig(uint32_t n){ return malloc((size_t)n * 4096); }
void  pmm_free_contig(void *p, uint32_t n){ (void)n; free(p); }

#include "../fs/fat32.c"
#define ESP_NO_KERNEL_GLUE 1
#include "../fs/esp.c"

/* esp.c's kernel glue is excluded above because it mounts through the
   installer's disk table; these are the two entry points install.c calls,
   wired to the model instead. Mirrors fs/esp.c's own mount_esp(). */
static int g_espdisk = 0;
static int esp_rd2(void *ctx, uint64_t lba, uint8_t *b){
    (void)ctx; return d_read(0, lba, b) == 0;
}
static int esp_wr2(void *ctx, uint64_t lba, const uint8_t *b){
    (void)ctx; return inst_disk_write(g_espdisk, lba, b);
}
static int mount_the_esp(int disk, esp_report_t *rep, int want_write)
{
    gpt_part_t p[GPT_MAX_PARTS];
    int n, i;
    if(!rep) return ESP_E_PARAM;
    memset(rep, 0, sizeof *rep);
    rep->disk = disk;
    g_espdisk = disk;
    if(!inst_disk_has_gpt(disk)) return ESP_E_NOGPT;
    n = inst_disk_gpt_parts(disk, p, GPT_MAX_PARTS);
    if(n <= 0) return ESP_E_NOGPT;
    for(i = 0; i < n; i++){
        if(!p[i].is_esp) continue;
        if(!fat32_mount_dev(esp_rd2, want_write ? esp_wr2 : 0, 0, p[i].first_lba))
            return ESP_E_MOUNT;
        rep->found = 1;
        rep->lba = p[i].first_lba;
        rep->sectors = p[i].last_lba - p[i].first_lba + 1;
        return ESP_OK;
    }
    return ESP_E_NOESP;
}
int esp_find(int disk, esp_report_t *rep){
    int r = mount_the_esp(disk, rep, 0);
    if(r != ESP_OK) return r;
    r = survey(rep);
    fat32_unmount();
    return r;
}
int esp_install(int disk, esp_report_t *rep, int part, int dry){
    int r;
    if(!rep) return ESP_E_PARAM;
    if(!dry && g_ro) return ESP_E_READONLY;
    r = mount_the_esp(disk, rep, dry ? 0 : 1);
    if(r != ESP_OK) return r;
    r = esp_install_mounted(rep, part, dry);
    fat32_unmount();
    return r;
}

#include "install_add.h"
#include "uninstall.h"
/* v0.9.4: the system volume, for the self-hosting install. NONE by
   default -- the ISO case; a section below stands one up with a kernel
   on it and the embedded image switched off. */
#include "sysvol.h"
static int         g_sys_kind = SYSVOL_NONE;
static const uint8_t *g_sys_elf;
static uint32_t    g_sys_elf_n;
static int         g_sys_reads;
static int         g_boot_disk = -1;
int sysvol_kind(void){ return g_sys_kind; }
int sysvol_exists(const char *p){
    return g_sys_kind && g_sys_elf && strcmp(p, "/boot/havendos.elf") == 0;
}
int sysvol_read(const char *p, char *b, uint32_t m){
    uint32_t n;
    if(!sysvol_exists(p)) return -1;
    g_sys_reads++;
    n = g_sys_elf_n < m ? g_sys_elf_n : m;
    memcpy(b, g_sys_elf, n);
    return (int)n;
}
int inst_disk_is_boot(int i){ return i == g_boot_disk; }

#include "../fs/instsize.c"
#include "../fs/install.c"

/* ── a real FAT32 ESP inside the model disk ─────────────────────────── */
/* The gptdisk fixture declares where the ESP is but puts no filesystem in
   it. esp.c needs a mountable FAT32 with Windows' files on it, so one is
   formatted here, with mkfs.vfat and mcopy -- outside tools, so the thing
   under test is not also the thing that built the fixture. */
/* v0.8.8.8: when set, the fixture ESP carries a fallback loader that
   is NOT ours, so the install has something to back up and the
   uninstall has something to put back. Without it `would_back_up` is
   false and the restore path -- the one that decides whether a
   machine still boots after an uninstall -- is never executed. */
static int g_esp_foreign_loader = 0;

static int build_esp_into_model(uint64_t first_lba, uint64_t sectors)
{
    char cmd[640];
    FILE *f;
    uint64_t i;
    static uint8_t sec[512];
    uint64_t n = sectors > 204800u ? 204800u : sectors;   /* cap at 100 MB */

    snprintf(cmd, sizeof cmd,
             "rm -f /tmp/hd_ia_esp.img && "
             "dd if=/dev/zero of=/tmp/hd_ia_esp.img bs=512 count=%llu "
             "status=none && mkfs.vfat -F 32 -n EFI /tmp/hd_ia_esp.img "
             "> /dev/null 2>&1",
             (unsigned long long)n);
    if(system(cmd)) return 0;

    /* Windows' own files, so there is something to not break. */
    if(system("mmd -i /tmp/hd_ia_esp.img ::/EFI > /dev/null 2>&1")) return 0;
    if(system("mmd -i /tmp/hd_ia_esp.img ::/EFI/Microsoft > /dev/null 2>&1")) return 0;
    if(system("mmd -i /tmp/hd_ia_esp.img ::/EFI/Microsoft/Boot > /dev/null 2>&1")) return 0;
    if(system("mmd -i /tmp/hd_ia_esp.img ::/EFI/BOOT > /dev/null 2>&1")) return 0;
    if(system("head -c 1800000 /dev/urandom > /tmp/hd_ia_bootmgfw.bin")) return 0;
    if(system("head -c 262144 /dev/urandom > /tmp/hd_ia_bcd.bin")) return 0;
    if(system("mcopy -i /tmp/hd_ia_esp.img /tmp/hd_ia_bootmgfw.bin "
              "::/EFI/Microsoft/Boot/bootmgfw.efi > /dev/null 2>&1")) return 0;
    if(system("mcopy -i /tmp/hd_ia_esp.img /tmp/hd_ia_bcd.bin "
              "::/EFI/Microsoft/Boot/BCD > /dev/null 2>&1")) return 0;
    if(g_esp_foreign_loader){
        if(system("head -c 740000 /dev/urandom > /tmp/hd_ia_foreign.bin")) return 0;
        if(system("mcopy -i /tmp/hd_ia_esp.img /tmp/hd_ia_foreign.bin "
                  "::/EFI/BOOT/BOOTX64.EFI > /dev/null 2>&1")) return 0;
    }

    f = fopen("/tmp/hd_ia_esp.img", "rb");
    if(!f) return 0;
    for(i = 0; i < n; i++){
        if(fread(sec, 1, 512, f) != 512) break;
        {   /* only non-zero sectors, to keep the sparse model small */
            int nz = 0, k;
            for(k = 0; k < 512; k++) if(sec[k]){ nz = 1; break; }
            if(!nz) continue;
        }
        if(d_write(0, first_lba + i, sec) != 0){ fclose(f); return 0; }
    }
    fclose(f);
    return 1;
}

/* mtools' opinion of a file inside the ESP, read back out of the model. */
static int esp_file_matches(uint64_t first_lba, const char *path,
                            const char *hostfile)
{
    char cmd[640];
    FILE *f;
    uint64_t i;
    static uint8_t sec[512];
    uint64_t n = 204800u;

    f = fopen("/tmp/hd_ia_check.img", "wb");
    if(!f) return 0;
    for(i = 0; i < n; i++){
        if(d_read(0, first_lba + i, sec) != 0) memset(sec, 0, 512);
        if(fwrite(sec, 1, 512, f) != 512){ fclose(f); return 0; }
    }
    fclose(f);

    snprintf(cmd, sizeof cmd,
             "mcopy -n -i /tmp/hd_ia_check.img %s /tmp/hd_ia_out.bin "
             "> /dev/null 2>&1 && cmp -s /tmp/hd_ia_out.bin %s",
             path, hostfile);
    return system(cmd) == 0;
}
/* esp_file_matches() above leaves /tmp/hd_ia_check.img holding the ESP as
   it currently stands on the model disk, so this runs straight after one
   of those and checks the filesystem the install just wrote into. */
static int esp_fsck_clean(void)
{
    return system("fsck.vfat -n /tmp/hd_ia_check.img > /dev/null 2>&1") == 0;
}

/* Rebuild the model's ESP filesystem after a build_disk*. The fixture puts
   a partition table on the disk but no filesystem inside the EFI System
   Partition, and fs/esp.c needs a mountable FAT32 with Windows' files on
   it. Returns the ESP's extent as well, which several sections need. */
static int reset_esp(uint64_t *lo, uint64_t *hi)
{
    gpt_part_t p[GPT_MAX_PARTS];
    int n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS), k;
    if(lo) *lo = 0;
    if(hi) *hi = 0;
    for(k = 0; k < (n < 0 ? 0 : n); k++) if(p[k].is_esp){
        if(lo) *lo = p[k].first_lba;
        if(hi) *hi = p[k].last_lba;
        return build_esp_into_model(p[k].first_lba,
                                    p[k].last_lba - p[k].first_lba + 1);
    }
    return 0;
}

/* v0.9.3: a partition out of the model as a SPARSE file -- a 3 GB FAT32
   volume is a few thousand non-zero sectors and three billion zero bytes,
   and writing the zeroes would make this suite a disk benchmark. */
static int extract_sparse(uint64_t first, uint64_t n, const char *path)
{
    FILE *f = fopen(path, "wb");
    int k;
    if(!f) return 0;
    for(k = 0; k < g_ndsec; k++){
        uint64_t l = g_dsec[k].lba;
        if(l < first || l >= first + n) continue;
        if(fseeko(f, (off_t)((l - first) * 512u), SEEK_SET) != 0
           || fwrite(g_dsec[k].b, 1, 512, f) != 512){ fclose(f); return 0; }
    }
    fclose(f);
    return truncate(path, (off_t)(n * 512u)) == 0;
}

/* fs/fat32.c's read callback, over the model: 1 for success. */
static int f32_model_rd(void *ctx, uint64_t lba, uint8_t *buf)
{
    (void)ctx; return d_read(0, lba, buf) == 0;
}

/* ── the comparison that matters ────────────────────────────────────── */
/* Every sector the model holds, excluding a range and an allowed list. */
static int identical_outside(uint64_t lo, uint64_t hi,
                             uint64_t esp_lo, uint64_t esp_hi,
                             const uint64_t *allowed, int n_allowed,
                             uint64_t *first_bad)
{
    int bad = 0, i, j;
    for(i = 0; i < g_ndsec; i++){
        uint64_t lba = g_dsec[i].lba;
        int skip = 0;
        if(lba >= lo && lba <= hi) continue;
        if(lba >= esp_lo && lba <= esp_hi) continue;
        for(j = 0; j < n_allowed; j++) if(allowed[j] == lba){ skip = 1; break; }
        if(skip) continue;
        /* find it in the snapshot */
        {
            int found = -1;
            for(j = 0; j < g_nsnap; j++)
                if(g_snap[j].lba == lba){ found = j; break; }
            if(found < 0){
                if(!bad && first_bad) *first_bad = lba;
                bad++;                      /* a sector that did not exist */
                continue;
            }
            if(memcmp(g_snap[found].b, g_dsec[i].b, 512) != 0){
                if(!bad && first_bad) *first_bad = lba;
                bad++;
            }
        }
    }
    return bad;
}

int main(void)
{
    inst_add_t rep;
    unsigned i;

    printf("The additive install, against a Windows-shaped 512 GB disk\n");
    printf("==========================================================\n");

    for(i = 0; i < FAKE_KERNEL_SZ; i++)
        g_kernel[i] = (uint8_t)((i * 11u + (i >> 8) * 7u) & 0xFF);
    g_kernel[0] = 0x7F; g_kernel[1] = 'E'; g_kernel[2] = 'L'; g_kernel[3] = 'F';

    for(i = 0; i < FAKE_LOADER_SZ; i++)
        g_loader[i] = (uint8_t)((i * 7u + (i >> 9) * 13u) & 0xFF);
    g_loader[0] = 'M'; g_loader[1] = 'Z';
    g_loader[0x3C] = 0x80; g_loader[0x3D] = 0; g_loader[0x3E] = 0; g_loader[0x3F] = 0;
    g_loader[0x80] = 'P'; g_loader[0x81] = 'E'; g_loader[0x82] = 0; g_loader[0x83] = 0;
    g_loader[0x84] = 0x64; g_loader[0x85] = 0x86;

    if(system("which mkfs.vfat mcopy mmd fsck.vfat > /dev/null 2>&1") != 0){
        printf("\n  mkfs.vfat/mtools/fsck.vfat are not installed. This suite\n"
               "  needs them: they are the outside opinion, and without one\n"
               "  the test is our own code agreeing with itself.\n");
        printf("\n0/0 passed\n");
        return 1;
    }

    head("the disk: 512 GB, Windows 11 layout, 1 GB shrunk free");
    build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);   /* 512 GB */
    ck("the table verifies the way firmware checks it", table_verifies(), 0);
    ck("gpt_present agrees", inst_disk_has_gpt(MODEL_DISK), 0);
    ck("and there is free space in it",
       WIN_FREE_LAST > WIN_FREE_FIRST, 0);
    printf("      free space: LBA %llu..%llu (%llu MB)\n",
           (unsigned long long)WIN_FREE_FIRST,
           (unsigned long long)WIN_FREE_LAST,
           (unsigned long long)((WIN_FREE_LAST - WIN_FREE_FIRST + 1) / 2048));

    head("a real FAT32 EFI System Partition, built by mkfs.vfat");
    {
        gpt_part_t p[GPT_MAX_PARTS];
        int n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS), k, found = 0;
        uint64_t elo = 0, esec = 0;
        n = n < 0 ? 0 : n;
        for(k = 0; k < n; k++) if(p[k].is_esp){
            elo = p[k].first_lba; esec = p[k].last_lba - p[k].first_lba + 1;
            found = 1; break;
        }
        ck("the layout has an ESP", found, 0);
        ck("mkfs.vfat and mtools populate it",
           found && build_esp_into_model(elo, esec), 0);
        ck("and Windows' boot manager is readable from it",
           esp_file_matches(elo, "::/EFI/Microsoft/Boot/bootmgfw.efi",
                            "/tmp/hd_ia_bootmgfw.bin"), 0);
    }

    head("THE DRY RUN WRITES NOTHING AT ALL");
    {
        int r;
        g_wcount = 0;
        g_logging = 1; g_nwlog = 0;
        g_live = 1;              /* a live session may still preview */
        r = havendos_install_additive(MODEL_DISK, 1, &rep, 0);
        g_logging = 0;
        g_live = 0;

        ck("the preview succeeds", r == INSTADD_OK, r);
        ck("ZERO sectors were written", g_nwlog == 0, g_nwlog);
        ck("and the disk API was never asked to write",
           g_wcount == 0, g_wcount);
        ck("a plan was formed", rep.plan_ok, 0);

        ck("it names four sectors outside the new partition",
           rep.n_outside == 4, rep.n_outside);
        printf("      would write, outside the partition: ");
        for(i = 0; i < (unsigned)rep.n_outside; i++)
            printf("%llu%s", (unsigned long long)rep.outside[i],
                   i + 1 < (unsigned)rep.n_outside ? ", " : "\n");

        ck("the partition starts on a 1 MB boundary",
           rep.part_first % INSTADD_ALIGN == 0,
           (long)(rep.part_first % INSTADD_ALIGN));
        ck("it sits inside the free space",
           rep.part_first >= WIN_FREE_FIRST
           && rep.part_last <= WIN_FREE_LAST, 0);
        /* last - first + 1 == sectors. Trivial, and it was missing: an
           off-by-one here gives the GPT entry a partition one sector
           longer than the volume formatted inside it, which overlaps
           whatever begins where the gap ends. */
        ck("first, last and the count agree with each other",
           rep.part_last - rep.part_first + 1 == rep.part_sectors,
           (long)(rep.part_last - rep.part_first + 1));
        ck("and the count is a whole number of megabytes",
           rep.part_sectors % INSTADD_ALIGN == 0,
           (long)(rep.part_sectors % INSTADD_ALIGN));
        ck("the geometry is a valid FAT16",
           rep.geom.clusters >= FAT16_MIN_CLUSTERS
           && rep.geom.clusters <= FAT16_MAX_CLUSTERS,
           (long)rep.geom.clusters);
        ck("and the volume can hold the kernel",
           rep.vol_need_bytes <= rep.vol_free_bytes, 0);
        ck("the ESP survey succeeded", rep.esp_rc == ESP_OK, rep.esp_rc);
        ck("it found Windows there",  rep.esp.has_microsoft, 0);
        printf("      partition: LBA %llu..%llu (%llu MB), "
               "%u sectors/cluster, %u clusters\n",
               (unsigned long long)rep.part_first,
               (unsigned long long)rep.part_last,
               (unsigned long long)(rep.part_sectors / 2048),
               rep.geom.sects_per_clust, rep.geom.clusters);
    }

    head("a live session may preview but may not write");
    {
        inst_add_t r2;
        int r;
        g_live = 1;
        g_wcount = 0; g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        g_live = 0;
        ck("the real install is refused", r == INSTADD_E_LIVE, r);
        ck("and nothing was written", g_nwlog == 0, g_nwlog);
    }

    head("a disk with no write path is refused, not attempted");
    {
        inst_add_t r2;
        int r;
        g_ro = 1;
        g_wcount = 0; g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        g_ro = 0;
        ck("refused", r == INSTADD_E_READONLY, r);
        ck("and nothing was written", g_nwlog == 0, g_nwlog);
    }

    head("THE CLAIM: the install, then every other sector byte for byte");
    {
        uint64_t allowed[4], first_bad = 0;
        uint64_t plo, phi, elo = rep.esp.lba, ehi;
        int r, n_out;
        gpt_part_t p[GPT_MAX_PARTS];
        int n, k;

        /* the ESP's extent, from the table rather than from the report */
        n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS);
        ehi = elo;
        for(k = 0; k < (n < 0 ? 0 : n); k++) if(p[k].is_esp){
            elo = p[k].first_lba; ehi = p[k].last_lba; break;
        }

        snapshot();
        ck("snapshot taken of every sector the disk holds", g_nsnap > 0, g_nsnap);

        g_wcount = 0;
        g_logging = 1; g_nwlog = 0;     /* the log is how order is checked */
        r = havendos_install_additive(MODEL_DISK, 0, &rep, 0);
        g_logging = 0;
        ck("the install succeeds", r == INSTADD_OK, r);
        printf("      %d sectors written in all\n", g_nwlog);
        if(r != INSTADD_OK)
            printf("      %s\n", havendos_install_add_strerror(r));

        plo = rep.part_first; phi = rep.part_last;
        n_out = rep.n_outside;
        for(i = 0; i < (unsigned)n_out && i < 4; i++) allowed[i] = rep.outside[i];

        ck("exactly four sectors outside the partition were permitted",
           n_out == 4, n_out);

        {
            int bad = identical_outside(plo, phi, elo, ehi,
                                        allowed, n_out, &first_bad);
            ck("EVERY OTHER SECTOR ON THE DISK IS BYTE-IDENTICAL",
               bad == 0, bad);
            if(bad)
                printf("      first differing sector: LBA %llu\n",
                       (unsigned long long)first_bad);
        }

        ck("the table still verifies end to end", table_verifies(), 0);

        /* The type GUID is load-bearing, not decoration. Microsoft Basic
           Data would make Windows enumerate this partition, give it a
           drive letter, and -- on a filesystem it half-recognises -- offer
           to repair or reformat it. An unknown type GUID is left alone. */
        {
            uint8_t ent[512];
            uint64_t elba;
            uint32_t esz;
            uint8_t h[512];
            int k, same = 1;
            ck("read the primary header", d_read(0, 1, h) == 0, 0);
            elba = g64(h + 72);
            esz  = g32(h + 84);
            ck("read the entry sector holding the new partition",
               d_read(0, elba + (uint64_t)(rep.gpt_entry * esz) / 512, ent) == 0,
               0);
            {
                uint32_t off = (uint32_t)(rep.gpt_entry * esz) % 512u;
                /* Written out, not compared against ADD_TYPE_GUID: the
                   first version of this check referenced the constant it
                   was checking, so changing that constant changed the
                   expectation with it and the check could not fail. */
                static const char want_guid[17] = "HAVENDOS-PART-01";
                for(k = 0; k < 16; k++)
                    if(ent[off + k] != (uint8_t)want_guid[k]) same = 0;
                ck("its type GUID is the ASCII HAVENDOS-PART-01",
                   same, 0);
                {
                    /* and specifically NOT Microsoft Basic Data, which is
                       what makes Windows claim a partition */
                    static const uint8_t MSDATA[16] = {
                        0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,
                        0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
                    int msd = 1;
                    for(k = 0; k < 16; k++)
                        if(ent[off + k] != MSDATA[k]) msd = 0;
                    ck("which is not Microsoft Basic Data", !msd, 0);
                }
                ck("and its first LBA is the one that was reported",
                   g64(ent + off + 32) == rep.part_first, 0);
                ck("and its last LBA likewise",
                   g64(ent + off + 40) == rep.part_last, 0);
            }
        }

        /* and the four really are the four it said they would be */
        {
            int in_log = 0;
            /* bounded by the array as well as by the count: g_nwlog keeps
               counting after g_wlog stops recording */
            unsigned lim = (unsigned)g_nwlog;
            long last_part = -1, first_esp = -1;
            if(lim > MAXWLOG) lim = MAXWLOG;
            for(i = 0; i < lim; i++){
                uint64_t l = g_wlog[i];
                if(l >= plo && l <= phi){ last_part = (long)i; continue; }
                if(l >= elo && l <= ehi){
                    if(first_esp < 0) first_esp = (long)i;
                    continue;
                }
                in_log++;
            }
            ck("the write log holds nothing but the partition, the ESP "
               "and the four", in_log == 4, in_log);

            /* THE ORDER MATTERS AND IS NOT VISIBLE IN THE END STATE.
               The boot files go on the ESP last, so that until they do the
               machine boots exactly as it did before: there is a new
               partition with an operating system in it that nothing points
               at yet. Installed the other way round, a power cut between
               the two leaves the firmware chainloading an empty volume.
               Only the write log can tell the two apart. */
            ck("the partition is filled before the ESP is touched",
               last_part >= 0 && first_esp >= 0 && first_esp > last_part,
               first_esp);
        }
    }

    head("Windows is still bootable: its files, per mtools");
    {
        uint64_t elo = rep.esp.lba;
        ck("bootmgfw.efi is byte-identical",
           esp_file_matches(elo, "::/EFI/Microsoft/Boot/bootmgfw.efi",
                            "/tmp/hd_ia_bootmgfw.bin"), 0);
        ck("the BCD is byte-identical",
           esp_file_matches(elo, "::/EFI/Microsoft/Boot/BCD",
                            "/tmp/hd_ia_bcd.bin"), 0);
        ck("the EFI partition still fsck's clean", esp_fsck_clean(), 0);

        /* The first version of this was `... == 0 || 1`, which is true
           whatever happens -- a check that cannot fail, in the suite whose
           whole purpose is checks that can. Written out properly: the
           loader has to be there, at the right length, byte for byte. */
        {
            FILE *w = fopen("/tmp/hd_ia_loader.bin", "wb");
            if(w){ fwrite(g_loader, 1, FAKE_LOADER_SZ, w); fclose(w); }
        }
        ck("HavenDOS' loader is on it, byte-identical",
           esp_file_matches(elo, "::/EFI/HAVENDOS/grubx64.efi",
                            "/tmp/hd_ia_loader.bin"), 0);
        ck("and so is the removable-media fallback it also writes",
           esp_file_matches(elo, "::/EFI/BOOT/BOOTX64.EFI",
                            "/tmp/hd_ia_loader.bin"), 0);
        ck("the menu on the ESP offers Windows back",
           system("mcopy -n -i /tmp/hd_ia_check.img ::/EFI/HAVENDOS/grub.cfg "
                  "/tmp/hd_ia_espcfg.txt > /dev/null 2>&1") == 0
           && system("grep -q bootmgfw /tmp/hd_ia_espcfg.txt") == 0, 0);
        ck("and it is not the only entry",
           system("grep -q HavenDOS /tmp/hd_ia_espcfg.txt") == 0, 0);
    }

    head("the new partition holds a mountable volume with the kernel in it");
    {
        char cmd[512];
        FILE *f;
        uint64_t i2;
        static uint8_t sec[512];

        f = fopen("/tmp/hd_ia_part.img", "wb");
        ck("extract the new partition", f != NULL, 0);
        if(f){
            for(i2 = 0; i2 < rep.part_sectors; i2++){
                if(d_read(0, rep.part_first + i2, sec) != 0)
                    memset(sec, 0, 512);
                if(fwrite(sec, 1, 512, f) != 512) break;
            }
            fclose(f);
        }
        ck("fsck.vfat accepts the volume we created",
           system("fsck.vfat -n /tmp/hd_ia_part.img > /dev/null 2>&1") == 0, 0);
        ck("its label is HAVENDOS, which is what GRUB searches for",
           system("mlabel -i /tmp/hd_ia_part.img -s :: 2>/dev/null "
                  "| grep -q HAVENDOS") == 0, 0);
        snprintf(cmd, sizeof cmd,
                 "mcopy -n -i /tmp/hd_ia_part.img ::/boot/havendos.elf "
                 "/tmp/hd_ia_kout.bin > /dev/null 2>&1");
        ck("the kernel is there, readable by mtools", system(cmd) == 0, 0);
        {
            FILE *k = fopen("/tmp/hd_ia_kout.bin", "rb");
            long sz = 0;
            if(k){ fseek(k, 0, SEEK_END); sz = ftell(k); fclose(k); }
            ck("at the right length", sz == (long)FAKE_KERNEL_SZ, sz);
        }
        if(system("head -c 307200 /tmp/hd_ia_kernel.bin > /dev/null 2>&1")){}
        {
            FILE *w = fopen("/tmp/hd_ia_kernel.bin", "wb");
            if(w){ fwrite(g_kernel, 1, FAKE_KERNEL_SZ, w); fclose(w); }
        }
        ck("and byte-identical to what was asked for",
           system("cmp -s /tmp/hd_ia_kout.bin /tmp/hd_ia_kernel.bin") == 0, 0);
        ck("the menu is there too",
           system("mcopy -n -i /tmp/hd_ia_part.img ::/boot/grub/grub.cfg "
                  "/tmp/hd_ia_cfg.txt > /dev/null 2>&1") == 0, 0);
    }

    head("a second install finds no room and refuses without writing");
    {
        inst_add_t r2;
        int r;
        g_wcount = 0; g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        g_logging = 0;
        /* The gap is gone -- it became the HavenDOS partition. So "no free
           space" is a TRUE answer, and it is the wrong one: it reads as
           though the disk has been damaged, and it sent someone looking
           for a fault in a machine that had just worked. The specific
           answer is the useful one. */
        ck("the preview says HavenDOS is already there",
           r == INSTADD_E_ALREADY, r);
        ck("not that the disk is full",
           r != INSTADD_E_NOSPACE && r != INSTADD_E_TOOSMALL, r);
        ck("and wrote nothing finding out", g_nwlog == 0, g_nwlog);
        ck("the existing partition is found by type GUID",
           havendos_find_existing(MODEL_DISK) >= 0,
           havendos_find_existing(MODEL_DISK));
        ck("a real install refuses for the same reason",
           havendos_install_additive(MODEL_DISK, 0, &r2, 0)
           == INSTADD_E_ALREADY, 0);
        ck("the message names the disk's state, not its capacity",
           havendos_install_add_strerror(INSTADD_E_ALREADY) != 0
           && havendos_install_add_shortstr(INSTADD_E_ALREADY) != 0, 0);
    }

    head("a disk with HavenDOS on it and room to spare is still refused");
    {
        /* Not a space question at all. Two partitions labelled HAVENDOS
           would make the boot menu's `search --label` pick whichever it
           found first, which is a machine that boots a different release
           depending on partition order. */
        inst_add_t r2;
        int r;
        uint64_t elo, ehi;
        /* 100 GB free: v0.9.3 takes a gap up to 32 GB whole, so the
           8 GB this used to be is now used up entirely and "room to
           spare" needs more room. */
        build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128,
                       100ull * 1024 * 1024 * 1024 / 512);
        ck("rebuild with plenty of free space", reset_esp(&elo, &ehi), 0);
        ck("the first install succeeds",
           havendos_install_additive(MODEL_DISK, 0, &r2, 0) == INSTADD_OK, 0);
        ck("there is still free space afterwards",
           r2.gap_sectors > r2.part_sectors * 2, 0);
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        g_logging = 0;
        ck("and the second is refused as already installed",
           r == INSTADD_E_ALREADY, r);
        ck("not for want of room", r != INSTADD_E_NOSPACE, r);
        ck("having written nothing", g_nwlog == 0, g_nwlog);
    }

    head("an install that fails halfway still does not touch Windows");
    {
        /* The disk stops accepting writes partway through the kernel copy.
           Two things have to be true afterwards: the installer says so
           rather than reporting success, and everything outside the new
           partition is still byte-identical -- because a failure is when
           a half-finished installer is most likely to flail. */
        inst_add_t r2;
        int r, bad;
        uint64_t allowed[4], first_bad = 0, elo, ehi;
        gpt_part_t p2[GPT_MAX_PARTS];
        int n, k;

        (void)p2; (void)n; (void)k;
        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        ck("rebuild the disk and its ESP", reset_esp(&elo, &ehi), 0);
        snapshot();

        /* 4 for the table, 545 for the format, ~70 for the directories and
           the menu: failing from 700 lands inside the kernel copy. */
        g_wcount = 0; g_wfail_at = 700;
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0; g_wfail_at = -1;

        ck("the install reports the failure", r == INSTADD_E_FILES, r);
        ck("rather than claiming success", r != INSTADD_OK, r);

        for(k = 0; k < r2.n_outside && k < 4; k++) allowed[k] = r2.outside[k];
        bad = identical_outside(r2.part_first, r2.part_last, elo, ehi,
                                allowed, r2.n_outside, &first_bad);
        ck("and EVERY SECTOR OUTSIDE THE NEW PARTITION IS STILL IDENTICAL",
           bad == 0, bad);
        if(bad)
            printf("      first differing sector: LBA %llu\n",
                   (unsigned long long)first_bad);
        ck("the partition table still verifies", table_verifies(), 0);
    }

    head("one bad sector in the kernel is not reported as a success");
    {
        /* A single refused write, deep in the kernel copy, with everything
           after it working. Nothing else fails, so if the kernel's write
           result were not checked the install would run to the end and say
           "complete" with a short kernel on the disk -- which boots to a
           GRUB error, on a machine whose owner has just been told the
           install worked. */
        inst_add_t r2;
        int r;
        uint64_t elo, ehi;

        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        ck("rebuild the disk and its ESP", reset_esp(&elo, &ehi), 0);

        g_wcount = 0; g_wfail_once = 700;      /* inside the kernel copy */
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_wfail_once = -1;

        ck("the install reports a file write failure", r == INSTADD_E_FILES, r);
        ck("and does not report success", r != INSTADD_OK, r);

        /* If it ever did report success, the kernel on the disk would be
           short. Asserted so the claim is about the disk and not only
           about the return value. */
        if(r == INSTADD_OK){
            FILE *f = fopen("/tmp/hd_ia_badpart.img", "wb");
            uint64_t i3;
            static uint8_t sec[512];
            if(f){
                for(i3 = 0; i3 < r2.part_sectors; i3++){
                    if(d_read(0, r2.part_first + i3, sec) != 0)
                        memset(sec, 0, 512);
                    if(fwrite(sec, 1, 512, f) != 512) break;
                }
                fclose(f);
            }
            ck("the kernel on disk is NOT intact, proving the point",
               system("mcopy -n -i /tmp/hd_ia_badpart.img ::/boot/havendos.elf "
                      "/tmp/hd_ia_badk.bin > /dev/null 2>&1") != 0
               || system("cmp -s /tmp/hd_ia_badk.bin /tmp/hd_ia_kernel.bin")
                  != 0, 0);
        }
    }

    head("a disk that drops the boot sector is caught by the read-back");
    {
        /* Every write "succeeds". One of them -- the volume's boot sector
           -- never reaches the platter. Nothing in the install reads that
           sector back except the verify at the end, so this is the test
           that the verify exists and looks at the right sector. */
        inst_add_t r2;
        int r;
        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        ck("rebuild the disk and its ESP", reset_esp(0, 0), 0);
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        ck("plan it first", r == INSTADD_OK, r);
        g_wdrop_lba = (long long)r2.part_first;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_wdrop_lba = -1;
        ck("the install reports it as a verify failure",
           r == INSTADD_E_VERIFY, r);
    }

    head("a disk with no GPT is refused, not guessed at");
    {
        inst_add_t r2;
        int r;
        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        memset(find(1)->b, 0, 512);                    /* no primary header */
        memset(find(g_disk_sectors - 1)->b, 0, 512);   /* nor a backup      */
        g_wcount = 0; g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        g_logging = 0;
        ck("the preview refuses", r == INSTADD_E_NOGPT, r);
        ck("and read-only, so nothing was written", g_nwlog == 0, g_nwlog);
        ck("the message says what to do instead",
           havendos_install_add_strerror(r) != 0, 0);
    }

    head("a gap far larger than 32 GB is capped, not refused");
    {
        inst_add_t r2;
        int r;
        /* 400 GB free. v0.9.3 lifted the 2047 MB FAT16 cap: the volume is
           FAT32 now, and capped at 32 GB -- the largest FAT32 Windows'
           own format makes, and a bound on how long formatting takes.
           The rest of the gap is left free. */
        build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128,
                       400ull * 1024 * 1024 * 1024 / 512);
        ck("an ESP with Windows on it, as before", reset_esp(0, 0), 0);
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        ck("the preview succeeds", r == INSTADD_OK, r);
        ck("the partition is capped at 32 GB",
           r2.part_sectors == INSTADD_MAX_SECTORS, (long)r2.part_sectors);
        ck("which is not the old 2047 MB",
           r2.part_sectors > INSTADD_FAT16_MAX_SECTORS, (long)r2.part_sectors);
        ck("the gap it came from is much larger",
           r2.gap_sectors > r2.part_sectors * 8, (long)(r2.gap_sectors >> 20));
        ck("the preview says FAT32", r2.fs_type == 32, r2.fs_type);
        ck("the geometry is a valid FAT32",
           r2.geom32.clusters >= FAT32_MIN_CLUSTERS
           && r2.geom32.part_sectors == r2.part_sectors,
           (long)r2.geom32.clusters);
        ck("and the FAT16 one is left empty", r2.geom.clusters == 0,
           (long)r2.geom.clusters);
        ck("free space is reported past 4 GB without wrapping",
           r2.vol_free_bytes > 0xFFFFFFFFull, (long)(r2.vol_free_bytes >> 20));
        ck("and the leftover is left alone",
           r2.part_last < r2.gap_last, 0);
        printf("      %llu MB gap -> a %llu MB FAT32 partition, %llu MB left free\n",
               (unsigned long long)(r2.gap_sectors / 2048),
               (unsigned long long)(r2.part_sectors / 2048),
               (unsigned long long)((r2.gap_last - r2.part_last) / 2048));
    }

    head("GAIA: a 3 GB gap is FAT32, used whole, and Windows is untouched");
    {
        /* The thing 0.9.3 is for, done for real and judged from outside:
           fsck.fat and mtools read the volume, the bytes outside it are
           compared one by one, and the volume is then mounted with the
           driver the installed system will mount it with. */
        inst_add_t r2;
        int r, bad;
        uint64_t allowed[4], first_bad = 0, elo, ehi;
        build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128,
                       3ull * 1024 * 1024 * 1024 / 512);
        ck("rebuild with a 3 GB gap", reset_esp(&elo, &ehi), 0);
        snapshot();
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        ck("the install succeeds", r == INSTADD_OK, r);
        if(r != INSTADD_OK)
            printf("      %s\n", havendos_install_add_strerror(r));
        ck("as FAT32", r2.fs_type == 32, r2.fs_type);
        ck("over the whole gap, less alignment",
           r2.part_sectors > INSTADD_FAT16_MAX_SECTORS
           && r2.part_sectors + 2 * INSTADD_ALIGN > r2.gap_sectors,
           (long)(r2.part_sectors >> 11));

        for(i = 0; i < (unsigned)r2.n_outside && i < 4; i++)
            allowed[i] = r2.outside[i];
        bad = identical_outside(r2.part_first, r2.part_last, elo, ehi,
                                allowed, r2.n_outside, &first_bad);
        ck("EVERY SECTOR OUTSIDE IT AND THE ESP IS IDENTICAL", bad == 0, bad);
        if(bad) printf("      first differing sector: LBA %llu\n",
                       (unsigned long long)first_bad);
        {
            /* and the write log agrees: the volume, the ESP, the four */
            unsigned lim = (unsigned)g_nwlog, k, out = 0;
            if(lim > MAXWLOG) lim = MAXWLOG;
            for(k = 0; k < lim; k++){
                uint64_t l = g_wlog[k];
                if(l >= r2.part_first && l <= r2.part_last) continue;
                if(l >= elo && l <= ehi) continue;
                out++;
            }
            ck("the write log holds the partition, the ESP and the four",
               out == 4, (long)out);
        }

        ck("extract the volume (sparse)",
           extract_sparse(r2.part_first, r2.part_sectors,
                          "/tmp/hd_ia_f32.img"), 0);
        ck("fsck.fat accepts it",
           system("fsck.fat -n /tmp/hd_ia_f32.img > /dev/null 2>&1") == 0, 0);
        ck("file(1)-level: the BPB says FAT32",
           system("dd if=/tmp/hd_ia_f32.img bs=1 skip=82 count=5 status=none "
                  "| grep -q FAT32") == 0, 0);
        ck("its label is HAVENDOS, which is what GRUB searches for",
           system("mlabel -i /tmp/hd_ia_f32.img -s :: 2>/dev/null "
                  "| grep -q HAVENDOS") == 0, 0);
        ck("the kernel is there, readable by mtools",
           system("mcopy -n -i /tmp/hd_ia_f32.img ::/boot/havendos.elf "
                  "/tmp/hd_ia_k32.bin > /dev/null 2>&1") == 0, 0);
        {
            FILE *w = fopen("/tmp/hd_ia_kernel.bin", "wb");
            if(w){ fwrite(g_kernel, 1, FAKE_KERNEL_SZ, w); fclose(w); }
        }
        ck("and byte-identical to the image",
           system("cmp -s /tmp/hd_ia_k32.bin /tmp/hd_ia_kernel.bin") == 0, 0);
        ck("the menu is there",
           system("mcopy -n -i /tmp/hd_ia_f32.img ::/boot/grub/grub.cfg "
                  "/tmp/hd_ia_cfg32.txt > /dev/null 2>&1") == 0
           && system("grep -q 'search --no-floppy --set=root --label HAVENDOS' "
                     "/tmp/hd_ia_cfg32.txt") == 0, 0);
        ck("and the marker",
           system("mcopy -n -i /tmp/hd_ia_f32.img ::/havendos.txt "
                  "/tmp/hd_ia_m32.txt > /dev/null 2>&1") == 0, 0);

        /* And with the driver the installed system mounts it with. */
        ck("fs/fat32.c mounts it",
           fat32_mount_dev(f32_model_rd, 0, 0, r2.part_first) == 1, 0);
        {
            static char kb[FAKE_KERNEL_SZ + 16];
            int n = fat32_read("/boot/havendos.elf", kb, sizeof kb);
            ck("and reads the kernel back, every byte",
               n == (int)FAKE_KERNEL_SZ
               && memcmp(kb, g_kernel, FAKE_KERNEL_SZ) == 0, n);
        }
        ck("FSInfo is believed, not 'unknown'",
           fat32_free_clusters() != 0xFFFFFFFFu, 0);
        fat32_unmount();

        /* The bound under the driver: fs/install.c's v32_wr() refuses any
           sector outside the volume being installed. Asked directly --
           this suite includes fs/install.c, so the static is in reach --
           because no correct driver ever exercises it, and a bound that
           nothing exercises is a bound nobody knows works. Both edges,
           and the volume's last sector, which must be ALLOWED: a bound
           one sector short would fail the install that fills the disk. */
        {
            static uint8_t sb[512];
            d_read(0, r2.part_last, sb);
            ck("the bound allows the volume's last sector",
               v32_wr(0, r2.part_last, sb) == 1, 0);
            d_read(0, r2.part_first, sb);
            ck("and its first",
               v32_wr(0, r2.part_first, sb) == 1, 0);
            ck("and refuses the sector after it",
               v32_wr(0, r2.part_last + 1, sb) == 0, 0);
            ck("and the sector before it",
               v32_wr(0, r2.part_first - 1, sb) == 0, 0);
            ck("and LBA 1, the GPT header",
               v32_wr(0, 1, sb) == 0, 0);
        }
    }

    head("PROMETHEUS: an installed system installs its own /boot/havendos.elf");
    {
        /* The running kernel is pass 1 and carries no image: the embedded
           one is switched off. The system volume holds a kernel of a
           different size and pattern, so the bytes on the new volume can
           only have come from there. */
        static uint8_t sysk[400000];
        inst_add_t r2;
        int r;
        uint32_t k;
        uint64_t elo, ehi;
        for(k = 0; k < sizeof sysk; k++) sysk[k] = (uint8_t)(k * 13u + 5u);
        sysk[0] = 0x7F; sysk[1] = 'E'; sysk[2] = 'L'; sysk[3] = 'F';
        kernel_elf_size = 0;                       /* a pass-1 kernel */
        g_sys_elf = sysk; g_sys_elf_n = sizeof sysk;
        havendos_install_image_release();

        build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128,
                       3ull * 1024 * 1024 * 1024 / 512);
        ck("rebuild with a 3 GB gap", reset_esp(&elo, &ehi), 0);

        g_sys_kind = SYSVOL_NONE;
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        ck("from live media with no image: refused as NOIMAGE",
           r == INSTADD_E_NOIMAGE, r);
        ck("having written nothing", g_nwlog == 0, g_nwlog);
        ck("and the source says none",
           havendos_install_image_source() == HAVENDOS_IMG_NONE, 0);

        g_sys_kind = SYSVOL_FAT32; g_boot_disk = 0; g_sys_reads = 0;
        ck("on an installed system the source is the system volume",
           havendos_install_image_source() == HAVENDOS_IMG_SYSTEM, 0);
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        ck("the preview succeeds", r == INSTADD_OK, r);
        ck("and says the image comes from the system", r2.image_from_system, 0);
        ck("at the system kernel's size", r2.kernel_bytes == sizeof sysk,
           (long)r2.kernel_bytes);
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        ck("the install succeeds", r == INSTADD_OK, r);
        ck("the kernel was read off the system volume once, not per call",
           g_sys_reads == 1, g_sys_reads);
        ck("extract the new volume",
           extract_sparse(r2.part_first, r2.part_sectors, "/tmp/hd_ia_p4.img"), 0);
        {
            FILE *w = fopen("/tmp/hd_ia_sysk.bin", "wb");
            if(w){ fwrite(sysk, 1, sizeof sysk, w); fclose(w); }
        }
        ck("its kernel is byte-for-byte the system's",
           system("mcopy -n -i /tmp/hd_ia_p4.img ::/boot/havendos.elf "
                  "/tmp/hd_ia_k4.bin > /dev/null 2>&1") == 0
           && system("cmp -s /tmp/hd_ia_k4.bin /tmp/hd_ia_sysk.bin") == 0, 0);
        ck("fsck.fat accepts the volume",
           system("fsck.fat -n /tmp/hd_ia_p4.img > /dev/null 2>&1") == 0, 0);

        /* not over itself */
        build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128,
                       3ull * 1024 * 1024 * 1024 / 512);
        reset_esp(0, 0);
        g_boot_disk = MODEL_DISK;
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        ck("onto the disk it is running from: refused as SELF, even to preview",
           r == INSTADD_E_SELF, r);
        r = havendos_install_additive(MODEL_DISK, 0, &r2, 0);
        g_logging = 0;
        ck("and for real", r == INSTADD_E_SELF, r);
        ck("having written nothing", g_nwlog == 0, g_nwlog);
        ck("the refusal says so",
           strstr(havendos_install_add_strerror(INSTADD_E_SELF), "running") != 0, 0);
        ck("whole-disk too, before step 1",
           havendos_install_to_drive(MODEL_DISK, 0) == -16, 0);

        /* a system file that is not a kernel */
        g_boot_disk = 0;
        havendos_install_image_release();
        sysk[0] = 'M';
        ck("a /boot/havendos.elf without ELF magic is no image",
           havendos_install_image_source() == HAVENDOS_IMG_NONE, 0);
        sysk[0] = 0x7F;

        havendos_install_image_release();
        g_sys_kind = SYSVOL_NONE; g_sys_elf = 0; g_boot_disk = -1;
        kernel_elf_size = FAKE_KERNEL_SZ;
        ck("and with the embedded image back, the source is embedded",
           havendos_install_image_source() == HAVENDOS_IMG_EMBEDDED, 0);
    }

    head("a gap that alignment shrinks below the floor is refused");
    {
        /* gpt_find_free() only returns gaps of at least the minimum, so the
           floor looks unreachable -- until alignment. Aligning the start up
           to a megabyte and the length down to one can cost 4094 sectors
           between them, which is enough to take a gap just over 16 MB
           under it. Rather than assert which gap size does that -- that
           would mean recomputing the installer's own arithmetic here and
           asserting the two agree -- this searches the range and requires
           that at least one such size exists and is refused with the right
           code. */
        inst_add_t r2;
        uint64_t g2;
        int found_small = 0, wrote = 0, wrong_code = 0;
        for(g2 = INSTADD_MIN_SECTORS; g2 < INSTADD_MIN_SECTORS + 4096; g2 += 1){
            int r;
            build_disk_gap(1024ull * 1024 * 1024 * 1024 / 512 / 2, 128, g2);
            g_logging = 1; g_nwlog = 0;
            r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
            g_logging = 0;
            if(g_nwlog) wrote = 1;
            if(r == INSTADD_E_TOOSMALL){ found_small = 1; continue; }
            /* INSTADD_E_ESP is expected here and is not a failure: this
               loop rebuilds the disk four thousand times and does not
               format an EFI System Partition into each one, which would
               take minutes. The floor is checked long before the ESP is
               looked at, so a gap that should be refused as too small
               still is. */
            if(r != INSTADD_OK && r != INSTADD_E_VOLSPACE
               && r != INSTADD_E_GEOM && r != INSTADD_E_ESP) wrong_code = 1;
        }
        ck("some gap in that range is refused as too small", found_small, 0);
        ck("every other answer in the range is a plan or a known refusal",
           !wrong_code, 0);
        ck("and the preview never wrote while finding out", !wrote, g_nwlog);
    }

    head("a kernel too big for the partition is refused before formatting");
    {
        inst_add_t r2;
        int r;
        uint32_t save = kernel_elf_size;
        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        ck("an ESP, so a refusal here is about the volume", reset_esp(0, 0), 0);
        /* The check is on the SIZE, and it refuses before a byte is read
           from the image, so an inflated size is a safe way to ask. */
        kernel_elf_size = 1800u * 1024u * 1024u;       /* 1800 MB */
        g_logging = 1; g_nwlog = 0;
        r = havendos_install_additive(MODEL_DISK, 1, &r2, 0);
        g_logging = 0;
        kernel_elf_size = save;
        ck("the preview refuses for want of room in the volume",
           r == INSTADD_E_VOLSPACE, r);
        ck("and wrote nothing", g_nwlog == 0, g_nwlog);
        ck("the report says how much it needed and how much there was",
           r2.vol_need_bytes > r2.vol_free_bytes
           && r2.vol_free_bytes > 0, 0);
    }

    if(system("rm -f /tmp/hd_ia_*.img /tmp/hd_ia_*.bin /tmp/hd_ia_*.txt")){}

    /* ════════════════════════════════════════════════════════════════
       v0.8.8.8: A DISK WITH NO EFI SYSTEM PARTITION

       Until this release the install needed one to already exist,
       which on a real machine meant it needed Windows. Now the
       installer reserves room for a boot partition at the FRONT of
       the gap and makes one.

       This is the wiring, not the mechanism: tests/test_espmake.c
       already puts espmk_create_at() in front of sgdisk and
       fsck.fat. What is in question here is the arithmetic that
       splits one gap into two partitions without them overlapping,
       and whether the rest of the install still works on top of an
       ESP we made rather than one Windows made.
       ════════════════════════════════════════════════════════════════ */
    head("a disk with NO EFI System Partition: one is created");
    {
        inst_add_t r3;
        int r;
        gpt_part_t p[GPT_MAX_PARTS];
        int n, k, n_esp = 0;

        build_disk_noesp(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        ck("the fixture has no ESP on it",
           inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS) > 0, 0);
        n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS);
        for(k = 0; k < n; k++) if(p[k].is_esp) n_esp++;
        ck("...confirmed: zero ESPs", n_esp == 0, n_esp);

        /* The preview must say what it is going to do BEFORE it does
           it. A person about to have two partitions added to their
           disk should see two. */
        r = havendos_install_additive(MODEL_DISK, 1 /* dry run */, &r3, 0);
        ck("the preview succeeds", r == INSTADD_OK, r);
        ck("the preview says a boot partition will be created",
           r3.esp_new_sectors > 0, (long)r3.esp_new_sectors);
        ck("...and says where", r3.esp_new_first >= WIN_FREE_FIRST,
           (long)r3.esp_new_first);
        ck("the preview did NOT create it yet", r3.esp_created == 0,
           r3.esp_created);
        ck("the two partitions do not overlap",
           r3.esp_new_first + r3.esp_new_sectors <= r3.part_first,
           (long)(r3.part_first - (r3.esp_new_first + r3.esp_new_sectors)));
        ck("the boot partition is 1 MB aligned",
           (r3.esp_new_first % 2048ull) == 0, (long)r3.esp_new_first);
        ck("HavenDOS's partition is 1 MB aligned",
           (r3.part_first % 2048ull) == 0, (long)r3.part_first);
        ck("both fit inside the gap",
           r3.part_first + r3.part_sectors - 1 <= WIN_FREE_LAST, 0);

        n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS);
        n_esp = 0;
        for(k = 0; k < n; k++) if(p[k].is_esp) n_esp++;
        ck("...and a dry run wrote nothing: still zero ESPs", n_esp == 0, n_esp);

        /* For real. */
        r = havendos_install_additive(MODEL_DISK, 0, &r3, 0);
        ck("the install succeeds on a disk that had no ESP", r == INSTADD_OK, r);
        if(r != INSTADD_OK)
            printf("      espmk: %s\n", espmk_strerror(r3.espmk_rc));
        ck("it reports that it created the boot partition",
           r3.esp_created == 1, r3.esp_created);

        ck("the table still verifies the way firmware checks it",
           table_verifies(), 0);

        n = inst_disk_gpt_parts(MODEL_DISK, p, GPT_MAX_PARTS);
        n_esp = 0;
        for(k = 0; k < n; k++) if(p[k].is_esp) n_esp++;
        ck("there is now exactly ONE ESP on the disk", n_esp == 1, n_esp);

        for(k = 0; k < n; k++) if(p[k].is_esp){
            ck("it is where the preview said it would be",
               p[k].first_lba == r3.esp_new_first,
               (long)(p[k].first_lba - r3.esp_new_first));
            ck("and the size the preview said",
               p[k].last_lba - p[k].first_lba + 1 == r3.esp_new_sectors,
               (long)(p[k].last_lba - p[k].first_lba + 1));
        }

        /* The strongest check in this block: fs/fat32.c mounting the
           volume fs/fat32fmt.c created, and fs/esp.c having written
           the loader into it. Three files that have never met. */
        {
            esp_report_t er;
            memset(&er, 0, sizeof er);
            ck("fs/esp.c can find the ESP we made",
               esp_find(MODEL_DISK, &er) == ESP_OK, 0);
            ck("...and it reports our loader is in it", er.has_fallback, 0);
        }

        /* And hand the whole partition to fsck.fat, which has no
           stake in any of it. */
        {
            FILE *f = fopen("/tmp/hd_ia_newesp.img", "wb");
            uint64_t i4;
            static uint8_t sec[512];
            int wrote = 0;
            if(f){
                for(i4 = 0; i4 < r3.esp_new_sectors; i4++){
                    if(d_read(0, r3.esp_new_first + i4, sec) != 0)
                        memset(sec, 0, 512);
                    if(fwrite(sec, 1, 512, f) != 512) break;
                }
                wrote = 1;
                fclose(f);
            }
            ck("the new ESP is extracted", wrote, 0);
            ck("fsck.fat accepts the ESP HavenDOS created",
               system("fsck.fat -n /tmp/hd_ia_newesp.img > /dev/null 2>&1") == 0,
               0);
            ck("mdir can list the loader in it",
               system("mdir -i /tmp/hd_ia_newesp.img ::/EFI/BOOT 2>/dev/null "
                      "| grep -qi BOOTX64") == 0, 0);
        }
    }


    /* ════════════════════════════════════════════════════════════════
       v0.8.8.8: THE ROUND TRIP

       Install, then uninstall, then ask whether the disk is where it
       started. This is the test that matters, because it is literally
       what Kyan is about to do to his laptop: remove HavenDOS, then
       install it again from the ISO.

       The ESP here carries a FOREIGN \EFI\BOOT\BOOTX64.EFI -- 740 KB
       of something that is not ours -- so the install has to back it
       up and the uninstall has to put it back. That restore is the
       step that decides whether the machine still boots afterwards,
       and without a foreign loader in the fixture it never runs.
       ════════════════════════════════════════════════════════════════ */
    head("install then uninstall: the disk goes back where it started");
    {
        inst_add_t r4;
        uninst_report_t u;
        gpt_part_t p4[GPT_MAX_PARTS];
        int r, n_before, n_after;

        g_esp_foreign_loader = 1;
        build_disk(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        /* build_disk() lays out the TABLE; the ESP's filesystem is a
           separate step. Without this the install fails
           INSTADD_E_ESP (-10) because there is nothing to mount --
           which is what the first run of this block spent all of its
           checks discovering. */
        ck("the fixture ESP is formatted with a foreign loader in it",
           reset_esp(0, 0), 0);
        g_esp_foreign_loader = 0;

        n_before = inst_disk_gpt_parts(MODEL_DISK, p4, GPT_MAX_PARTS);
        ck("the fixture disk has partitions", n_before > 0, n_before);

        ck("uninstall on a clean disk reports there is nothing to do",
           uninst_survey(MODEL_DISK, &u) == UNINST_E_NOTHING, 0);

        r = havendos_install_additive(MODEL_DISK, 0, &r4, 0);
        ck("HavenDOS installs", r == INSTADD_OK, r);
        n_after = inst_disk_gpt_parts(MODEL_DISK, p4, GPT_MAX_PARTS);
        ck("one partition was added", n_after == n_before + 1, n_after);

        /* ── the survey sees exactly what it should ─────────────── */
        ck("the survey finds the install", uninst_survey(MODEL_DISK, &u)
           == UNINST_OK, 0);
        ck("it found OUR partition by type GUID, not by size",
           u.part_index >= 0, u.part_index);
        ck("...at the LBA the installer reported",
           u.part_first == r4.part_first, (long)u.part_first);
        ck("it sees \\EFI\\HAVENDOS", u.has_havendos_dir, 0);
        ck("it sees the backup of the original loader", u.has_backup, 0);
        ck("it knows the fallback loader is now OURS", u.fallback_is_ours, 0);
        ck("it does NOT think this ESP is one we made",
           !u.esp_is_ours, u.esp_is_ours);

        /* ── a dry run must not touch a thing ───────────────────── */
        {
            uint64_t i5;
            static uint8_t a[512], b[512];
            int diff = 0;
            ck("the dry run succeeds", uninst_run(MODEL_DISK, 1, &u)
               == UNINST_OK, 0);
            ck("it says it would restore the loader", u.did_restore_loader, 0);
            ck("it says it would remove the partition", u.did_remove_part, 0);
            ck("the partition is still in the table",
               inst_disk_gpt_parts(MODEL_DISK, p4, GPT_MAX_PARTS)
                   == n_before + 1, 0);
            /* and the ESP still has our files in it */
            ck("the survey still finds everything after the dry run",
               uninst_survey(MODEL_DISK, &u) == UNINST_OK
               && u.has_havendos_dir && u.part_index >= 0, 0);
            (void)i5; (void)a; (void)b; (void)diff;
        }

        /* ── for real ───────────────────────────────────────────── */
        r = uninst_run(MODEL_DISK, 0, &u);
        ck("the uninstall succeeds", r == UNINST_OK, r);
        if(r != UNINST_OK) printf("      %s\n", uninst_strerror(r));
        ck("it restored the loader", u.did_restore_loader, 0);
        ck("it deleted the backup", u.did_delete_backup, 0);
        ck("it removed our files", u.did_remove_files, 0);
        ck("it removed our partition", u.did_remove_part, 0);
        ck("it did NOT remove the ESP (we did not make it)",
           !u.did_remove_esp, u.did_remove_esp);

        ck("the table verifies the way firmware checks it",
           table_verifies(), 0);
        n_after = inst_disk_gpt_parts(MODEL_DISK, p4, GPT_MAX_PARTS);
        ck("the partition count is back to what it was",
           n_after == n_before, n_after);
        {
            int k5, still = 0;
            for(k5 = 0; k5 < n_after; k5++){
                int m5 = 1, j5;
                for(j5 = 0; j5 < 16; j5++)
                    if(p4[k5].type_guid[j5] != GPT_TYPE_HAVENDOS[j5]){ m5 = 0; break; }
                if(m5) still = 1;
            }
            ck("no HavenDOS partition remains", !still, still);
        }
        ck("a second uninstall reports there is nothing left to do",
           uninst_survey(MODEL_DISK, &u) == UNINST_E_NOTHING, 0);

        /* ── and the ESP, judged by mtools and byte comparison ──── */
        {
            FILE *f = fopen("/tmp/hd_ia_after.img", "wb");
            uint64_t i6, esp_n = 204800ull;      /* the fixture's 100 MB */
            static uint8_t sec[512];
            ck("the ESP is extracted", f != NULL, 0);
            if(f){
                for(i6 = 0; i6 < esp_n; i6++){
                    if(d_read(0, 2048 + i6, sec) != 0) memset(sec, 0, 512);
                    if(fwrite(sec, 1, 512, f) != 512) break;
                }
                fclose(f);
            }
            ck("fsck.fat still accepts the ESP",
               system("fsck.fat -n /tmp/hd_ia_after.img > /dev/null 2>&1") == 0, 0);
            ck("\\EFI\\HAVENDOS is gone",
               system("mdir -i /tmp/hd_ia_after.img ::/EFI 2>/dev/null "
                      "| grep -qi HAVENDOS") != 0, 0);
            ck("BOOTX64.HDB is gone",
               system("mdir -i /tmp/hd_ia_after.img ::/EFI/BOOT 2>/dev/null "
                      "| grep -qi HDB") != 0, 0);
            ck("BOOTX64.EFI is back",
               system("mdir -i /tmp/hd_ia_after.img ::/EFI/BOOT 2>/dev/null "
                      "| grep -qi BOOTX64") == 0, 0);
            ck("...and it is BYTE-FOR-BYTE the loader that was there "
               "before HavenDOS",
               system("mcopy -n -i /tmp/hd_ia_after.img "
                      "::/EFI/BOOT/BOOTX64.EFI /tmp/hd_ia_back.bin "
                      "> /dev/null 2>&1") == 0
               && system("cmp -s /tmp/hd_ia_foreign.bin /tmp/hd_ia_back.bin") == 0,
               0);
            ck("Windows' own loader is untouched",
               system("mcopy -n -i /tmp/hd_ia_after.img "
                      "::/EFI/Microsoft/Boot/bootmgfw.efi /tmp/hd_ia_wb.bin "
                      "> /dev/null 2>&1") == 0
               && system("cmp -s /tmp/hd_ia_bootmgfw.bin /tmp/hd_ia_wb.bin") == 0,
               0);
        }

        /* ── and it can be installed again afterwards ───────────── */
        r = havendos_install_additive(MODEL_DISK, 0, &r4, 0);
        ck("HavenDOS installs again onto the cleaned disk",
           r == INSTADD_OK, r);
        ck("the table verifies afterwards", table_verifies(), 0);
    }

    head("an ESP WE made is removed too; one we did not is kept");
    {
        inst_add_t r5;
        uninst_report_t u;
        gpt_part_t p5[GPT_MAX_PARTS];
        int r, n_before;

        build_disk_noesp(1024ull * 1024 * 1024 * 1024 / 512 / 2);
        n_before = inst_disk_gpt_parts(MODEL_DISK, p5, GPT_MAX_PARTS);
        /* No reset_esp() here on purpose: there is no ESP to format.
           The install has to make one, which is the point. */

        r = havendos_install_additive(MODEL_DISK, 0, &r5, 0);
        ck("the install creates its own ESP", r == INSTADD_OK && r5.esp_created,
           r);
        ck("two partitions were added",
           inst_disk_gpt_parts(MODEL_DISK, p5, GPT_MAX_PARTS) == n_before + 2,
           inst_disk_gpt_parts(MODEL_DISK, p5, GPT_MAX_PARTS));

        ck("the survey recognises the ESP as ours",
           uninst_survey(MODEL_DISK, &u) == UNINST_OK && u.esp_is_ours,
           u.esp_is_ours);

        r = uninst_run(MODEL_DISK, 0, &u);
        ck("the uninstall succeeds", r == UNINST_OK, r);
        if(r != UNINST_OK) printf("      %s\n", uninst_strerror(r));
        ck("it removed the ESP it made", u.did_remove_esp, 0);
        ck("BOTH partitions are gone",
           inst_disk_gpt_parts(MODEL_DISK, p5, GPT_MAX_PARTS) == n_before,
           inst_disk_gpt_parts(MODEL_DISK, p5, GPT_MAX_PARTS));
        ck("the table verifies", table_verifies(), 0);
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
