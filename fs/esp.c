/* ================================================================
   fs/esp.c -- putting HavenDOS's boot files on Windows' ESP
   ================================================================
   See include/esp.h for the rules. They are stricter than anywhere
   else in the tree because this is the one place where a quiet bug
   does not announce itself: the partition table stays perfect, every
   partition stays where it was, and the machine simply stops booting
   Windows.

   There is no unlink in this file. There is no overwrite without a
   copy. Nothing is written until the free space has been counted, and
   nothing is believed written until it has been read back.
   ================================================================ */

#include "types.h"
#include "string.h"
#include "fat32.h"
#include "esp.h"
#include "version.h"

/* memset and memcpy come from include/string.h above; declaring them again
   here with a uint32_t length collides with that declaration. */

/* The loader, built at image time by grub-mkimage -O x86_64-efi.
   ESP_LOADER_PROVIDED lets a test supply its own image instead of the
   real 811 KB one, so the suite does not depend on a GRUB build being
   available wherever `make test` runs. */
#ifndef ESP_LOADER_PROVIDED
# ifdef HAVE_GRUB_EFI_EMBED
#  include "grub_efi_embed.h"
# else
static const uint8_t  grub_efi_img[] = { 0 };
static const uint32_t grub_efi_img_size = 0;
# endif
#endif

#define P_DIR_EFI       "/EFI"
#define P_DIR_HAVENDOS  "/EFI/HAVENDOS"
#define P_DIR_BOOT      "/EFI/BOOT"
#define P_LOADER        "/EFI/HAVENDOS/grubx64.efi"
#define P_CFG           "/EFI/HAVENDOS/grub.cfg"
#define P_FALLBACK      "/EFI/BOOT/BOOTX64.EFI"
#define P_FALLBACK_BAK  "/EFI/BOOT/BOOTX64.HDB"
#define P_WINDOWS       "/EFI/Microsoft/Boot/bootmgfw.efi"

/* ── the menu ───────────────────────────────────────────────────────
   Written with the version pulled from include/version.h, never typed:
   tools/make_version.py --check fails the build on a version literal
   anywhere else, and a boot menu that lies about which release it loads
   is exactly the drift that check exists to stop.

   The Windows entry matters as much as ours. An installer that adds
   itself to the boot path and does not offer the operating system that
   was already there has not produced a dual boot, it has produced a
   machine whose owner has to go into the firmware menu to get their work
   back. `search --file` finds the ESP wherever it is rather than assuming
   a disk number. */
static const char *CFG_TEXT =
    "# Written by HavenDOS " HAVENDOS_VER " at install time.\n"
    "# Both entries are here on purpose: the second one is how you get\n"
    "# Windows back, and it is first in the list for a reason.\n"
    "set timeout=8\n"
    "set default=1\n"
    "\n"
    /* The kernel command line is not optional and leaving it off is not a
       cosmetic difference. With no arguments the kernel comes up in
       INSTALLER mode and announces "You have booted from the HavenDOS
       install ISO" -- on a machine booting from its own disk, with no ISO
       anywhere. harness/uefi_dualboot.py found this: every native suite
       was green, the partition was correct, the loader ran, GRUB ran, the
       kernel ran, and what the owner got was the installer.

       These are the same arguments fs/install.c writes into the grub.cfg
       on the HavenDOS partition, and tests/test_esp.c now asserts the two
       agree. */
    "menuentry \"HavenDOS " HAVENDOS_VER " " HAVENDOS_CHANNEL "\" {\n"
    "    search --no-floppy --set=root --label HAVENDOS\n"
    "    multiboot /boot/havendos.elf mode=desktop gfx=text\n"
    "    boot\n"
    "}\n"
    "\n"
    "menuentry \"Windows Boot Manager\" {\n"
    "    search --no-floppy --set=root --file /EFI/Microsoft/Boot/bootmgfw.efi\n"
    "    chainloader /EFI/Microsoft/Boot/bootmgfw.efi\n"
    "}\n"
    "\n"
    "menuentry \"Firmware setup\" {\n"
    "    fwsetup\n"
    "}\n";

static uint32_t cstrlen(const char *s){
    uint32_t n = 0; while(s && s[n]) n++; return n;
}

/* ── verify: read it back and compare, in sector-sized bites ──────────
   "the write returned success" is not the same statement as "the bytes
   are there", and on this partition the difference is somebody's Windows
   install. Nothing here trusts a return code. */
static int verify_file(const char *path, const uint8_t *want, uint32_t len)
{
    static char chunk[512];
    uint32_t off = 0;
    if(fat32_file_size(path) != len) return 0;
    while(off < len){
        uint32_t n = len - off;
        if(n > sizeof chunk) n = sizeof chunk;
        int got = fat32_read_at(path, off, chunk, n);
        if(got != (int)n) return 0;
        for(uint32_t i = 0; i < n; i++)
            if((uint8_t)chunk[i] != want[off + i]) return 0;
        off += n;
    }
    return 1;
}

/* Write, then immediately read back and compare. */
static int write_verified(const char *path, const uint8_t *data, uint32_t len)
{
    if(!fat32_write(path, (const char *)data, len)) return ESP_E_WRITE;
    if(!verify_file(path, data, len))               return ESP_E_VERIFY;
    return ESP_OK;
}

/* ── the survey, which writes nothing ───────────────────────────────── */
/* --- is the embedded loader actually a loader? ----------------------
   The build generates include/grub_efi_embed.h from grub-mkimage output.
   If that rule does not run, or runs against the wrong image, this file
   still compiles -- the #else branch below supplies a one-byte stub -- and
   the install then writes a zero-byte \EFI\BOOT\BOOTX64.EFI over the one
   the machine was using, takes a backup of it, and reports success. The
   partition table is perfect, every file is "verified" (zero bytes read
   back as zero bytes), and the machine does not boot what was installed.

   So the bytes are checked for being a PE32+ image for x86_64 before the
   disk is touched at all, and the check lives in survey() so that a dry
   run reports it too: finding out from the preview is the entire point of
   having a preview.

   Multi-byte fields are composed a byte at a time. Casting a buffer to a
   header struct faults on a strict-alignment host, and the same rule is
   already written into fs/fat32.c and fs/gpt_write.c. */
#define ESP_LOADER_MIN  (64u * 1024u)   /* a real x86_64-efi GRUB is ~811 KB */

static int loader_is_pe32plus(const uint8_t *img, uint32_t len)
{
    uint32_t e_lfanew;

    if(len < ESP_LOADER_MIN)            return 0;
    if(img[0] != 'M' || img[1] != 'Z')  return 0;

    e_lfanew  = (uint32_t)img[0x3C];
    e_lfanew |= (uint32_t)img[0x3D] << 8;
    e_lfanew |= (uint32_t)img[0x3E] << 16;
    e_lfanew |= (uint32_t)img[0x3F] << 24;

    /* The size floor above is not only a plausibility check: it is what
       makes this subtraction safe. At len 0 or 1, len - 6u wraps to near
       4 G, every offset is "in bounds", and a one-byte stub whose
       neighbouring memory happens to spell PE passes. */
    if(e_lfanew > len - 6u)             return 0;
    if(img[e_lfanew + 0] != 'P' || img[e_lfanew + 1] != 'E'
       || img[e_lfanew + 2] != 0 || img[e_lfanew + 3] != 0) return 0;

    /* Machine type, little-endian: 0x8664 is x86_64. 0x014C is i386, which
       is what include/grub_embed.h and grub_disk_embed.h are built as, and
       what a Makefile rule pointed at the wrong file would hand us. No
       64-bit firmware will execute that. */
    if(img[e_lfanew + 4] != 0x64 || img[e_lfanew + 5] != 0x86) return 0;

    return 1;
}

/* ── is the thing at the fallback path already ours? ─────────────────
 *
 * v0.8.4.  A second install into the same gap used to stop with
 * ESP_E_BACKUP, and the reason was a rule that is right once and wrong
 * every time after that.
 *
 * The rule: \EFI\BOOT\BOOTX64.EFI is replaced only after a copy is made,
 * and if the copy's name is taken we refuse rather than decide which of
 * two previous backups matters.  On a FIRST install that is exactly
 * right -- BOOTX64.HDB holds whatever the machine booted before HavenDOS
 * existed, and on the test laptop that is Windows' own fallback loader.
 *
 * On a SECOND install the same rule does the opposite of what it is for.
 * BOOTX64.EFI is now OUR loader, because we wrote it last time.  Backing
 * it up would write our loader over BOOTX64.HDB and destroy the only copy
 * of the one that matters.  Refusing was the safe half of a choice that
 * should not have been a choice: there is nothing there worth saving.
 *
 * So the question asked here is not "is a backup name taken" but "is the
 * file at the fallback path one we put there".  Two ways it can be:
 *
 *   - it is byte-for-byte the loader THIS kernel carries, or
 *   - it is byte-for-byte \EFI\HAVENDOS\grubx64.efi, which is where a
 *     previous HavenDOS put its own copy -- and that covers the upgrade
 *     case, where the loader embedded in the new release differs from
 *     the one embedded in the old one.
 *
 * Anything else at that path belongs to somebody else, and if the backup
 * name is also taken we still refuse, exactly as before.
 *
 * Both comparisons read through fs/fat32.c in 512-byte pieces rather than
 * staging whole files in memory: survey() runs on the preview screen,
 * before the user has agreed to anything, and a preview that needs a
 * megabyte of contiguous physical memory to draw is a preview that can
 * fail for reasons the user cannot act on.
 */
static uint8_t g_cmp_a[512];
static uint8_t g_cmp_b[512];

static int file_eq_mem(const char *path, const uint8_t *mem, uint32_t len)
{
    uint32_t off = 0;
    if(len == 0) return 0;
    if(fat32_file_size(path) != len) return 0;
    while(off < len){
        uint32_t n = len - off, i;
        if(n > sizeof g_cmp_a) n = sizeof g_cmp_a;
        if(fat32_read_at(path, off, (char *)g_cmp_a, n) != (int)n) return 0;
        for(i = 0; i < n; i++)
            if(g_cmp_a[i] != mem[off + i]) return 0;
        off += n;
    }
    return 1;
}

static int file_eq_file(const char *a, const char *b)
{
    uint32_t sa = fat32_file_size(a);
    uint32_t off = 0;
    if(sa == 0) return 0;
    if(fat32_file_size(b) != sa) return 0;
    while(off < sa){
        uint32_t n = sa - off, i;
        if(n > sizeof g_cmp_a) n = sizeof g_cmp_a;
        if(fat32_read_at(a, off, (char *)g_cmp_a, n) != (int)n) return 0;
        if(fat32_read_at(b, off, (char *)g_cmp_b, n) != (int)n) return 0;
        for(i = 0; i < n; i++)
            if(g_cmp_a[i] != g_cmp_b[i]) return 0;
        off += n;
    }
    return 1;
}

/* The one function that decides what happens to the existing fallback.
   survey() asks it and esp_install_mounted() acts on what survey() wrote,
   so the preview screen and the write cannot disagree about it -- which
   is the same discipline the bus label got in 0.8.3.1 after four screens
   worked it out separately and one of them was wrong. */
static int fallback_is_ours(void)
{
    if(file_eq_mem(P_FALLBACK, grub_efi_img, grub_efi_img_size)) return 1;
    if(fat32_exists(P_LOADER) && file_eq_file(P_FALLBACK, P_LOADER)) return 1;
    return 0;
}

static int survey(esp_report_t *rep)
{
    uint32_t cl;
    uint32_t free_cl;

    /* Before the disk is read, never mind written. */
    if(!loader_is_pe32plus(grub_efi_img, grub_efi_img_size))
        return ESP_E_LOADER;

    cl = fat32_cluster_bytes();
    free_cl = fat32_free_clusters();

    rep->has_microsoft = fat32_exists(P_WINDOWS)
                      || fat32_is_dir("/EFI/Microsoft");
    rep->has_fallback  = fat32_exists(P_FALLBACK);
    rep->would_back_up = 0;
    rep->backup_name[0] = 0;

    rep->fallback_is_ours = 0;

    if(rep->has_fallback && fallback_is_ours()){
        /* v0.8.4 -- the reinstall case. What is at the fallback path is a
           loader we wrote, so there is nothing there worth preserving and
           BOOTX64.HDB, if it exists, already holds the one that matters.
           Leave it alone. */
        rep->fallback_is_ours = 1;
    } else if(rep->has_fallback){
        /* Something that is not ours is at the fallback path. It is
           replaced only after a copy is made, and if the copy's name is
           itself taken we stop rather than decide which of two previous
           backups matters. */
        if(fat32_exists(P_FALLBACK_BAK)) return ESP_E_BACKUP;
        rep->would_back_up = 1;
        {
            const char *s = P_FALLBACK_BAK; int i = 0;
            while(s[i] && i < (int)sizeof rep->backup_name - 1){
                rep->backup_name[i] = s[i]; i++;
            }
            rep->backup_name[i] = 0;
        }
    }

    /* What it needs: the loader twice (ours and the fallback), the
       config, and a cluster of slack per file for the directory entries.
       Rounded up to whole clusters, because that is how a filesystem
       spends space. */
    {
        uint32_t need = 0;
        uint32_t lsz = grub_efi_img_size;
        uint32_t csz = cstrlen(CFG_TEXT);
        if(!cl) cl = 512;
        need += ((lsz + cl - 1) / cl) * cl;          /* /EFI/HAVENDOS/grubx64.efi */
        need += ((csz + cl - 1) / cl) * cl;          /* its grub.cfg              */
        need += ((lsz + cl - 1) / cl) * cl;          /* the fallback copy         */
        if(rep->would_back_up){
            uint32_t ex = fat32_file_size(P_FALLBACK);
            need += ((ex + cl - 1) / cl) * cl;       /* and a copy of what is there */
        }
        need += cl * 8;                              /* directories and slack     */
        rep->need_kb = need / 1024u;
    }

    if(free_cl == 0xFFFFFFFFu){
        /* FSInfo does not know. Rather than guess, treat it as unknown and
           let the caller decide; the install itself will still fail safely
           if it runs out, because every write is verified. */
        rep->free_kb = 0xFFFFFFFFu;
    } else {
        rep->free_kb = (uint32_t)(((uint64_t)free_cl * cl) / 1024u);
        if(rep->free_kb < rep->need_kb) return ESP_E_SPACE;
    }
    return ESP_OK;
}

/* ── the install ────────────────────────────────────────────────────── */
/* Takes an already-mounted ESP. The kernel wrapper below mounts it; the
   test calls this directly against an image file, which is why the mount
   and the work are separate functions. */
int esp_install_mounted(esp_report_t *rep, int havendos_part, int dry_run)
{
    int r;
    (void)havendos_part;        /* the menu finds the partition by label */

    if(!rep) return ESP_E_PARAM;
    r = survey(rep);
    if(r != ESP_OK) return r;

    if(dry_run) return ESP_OK;

    /* Directories first. mkdir on something that already exists succeeds
       quietly, so Windows' own /EFI is never disturbed. */
    if(!fat32_mkdir(P_DIR_EFI))       return ESP_E_WRITE;
    if(!fat32_mkdir(P_DIR_HAVENDOS))  return ESP_E_WRITE;
    if(!fat32_mkdir(P_DIR_BOOT))      return ESP_E_WRITE;

    /* If something that is NOT ours is at the fallback path, copy it aside
       BEFORE writing anything over it, and verify the copy before trusting
       it.  survey() ran at the top of this function and set would_back_up,
       so this acts on the same decision the preview screen showed rather
       than working it out a second time. */
    if(rep->would_back_up){
        extern void *pmm_alloc_contig(uint32_t nblocks);
        extern void  pmm_free_contig(void *p, uint32_t nblocks);
        uint32_t sz = fat32_file_size(P_FALLBACK);
        if(sz == 0 || sz > 16u*1024u*1024u) return ESP_E_BACKUP;
        uint32_t blocks = (sz + 4095u) / 4096u;
        uint8_t *buf = (uint8_t *)pmm_alloc_contig(blocks);
        if(!buf) return ESP_E_BACKUP;
        int ok = (fat32_read_at(P_FALLBACK, 0, (char *)buf, sz) == (int)sz);
        if(ok) ok = (write_verified(P_FALLBACK_BAK, buf, sz) == ESP_OK);
        pmm_free_contig(buf, blocks);
        if(!ok) return ESP_E_BACKUP;
        rep->would_back_up = 1;
    }

    r = write_verified(P_LOADER, grub_efi_img, grub_efi_img_size);
    if(r != ESP_OK) return r;

    r = write_verified(P_CFG, (const uint8_t *)CFG_TEXT, cstrlen(CFG_TEXT));
    if(r != ESP_OK) return r;

    r = write_verified(P_FALLBACK, grub_efi_img, grub_efi_img_size);
    if(r != ESP_OK) return r;

    if(!fat32_flush()) return ESP_E_WRITE;

    /* Last: confirm Windows' loader is still readable at the size it was.
       Not a proof of correctness, but the single cheapest check that the
       thing we were most afraid of breaking is still there. */
    if(rep->has_microsoft && fat32_exists(P_WINDOWS)){
        if(fat32_file_size(P_WINDOWS) == 0) return ESP_E_VERIFY;
    }
    return ESP_OK;
}

const char *esp_strerror(int code)
{
    switch(code){
    case ESP_OK:           return "ok";
    case ESP_E_LOADER:     return "the built-in UEFI loader is missing or not a\n                                   PE32+ x86_64 image -- this kernel was built\n                                   without include/grub_efi_embed.h";
    case ESP_E_NOGPT:      return "this disk has no GPT";
    case ESP_E_NOESP:      return "no EFI System Partition on this disk";
    case ESP_E_MOUNT:      return "the EFI System Partition would not mount";
    case ESP_E_SPACE:      return "not enough free space on the EFI partition";
    case ESP_E_BACKUP:     return "a boot file is already backed up there; refusing";
    case ESP_E_WRITE:      return "a write to the EFI partition failed";
    case ESP_E_VERIFY:     return "a file read back wrong after writing";
    case ESP_E_READONLY:   return "that disk cannot be written to";
    case ESP_E_PARAM:      return "bad parameters";
    default:               return "unknown error";
    }
}

/* ── the kernel entry points ────────────────────────────────────────── */
/* Separated from the work above so the suite can drive the work against an
   image file without a disk table, a GPT or an installer behind it. */
#ifndef ESP_NO_KERNEL_GLUE

#include "gpt.h"

extern int      inst_disk_read (int i, uint64_t lba, uint8_t *b);
extern int      inst_disk_write(int i, uint64_t lba, const uint8_t *b);
extern int      inst_disk_is_readonly(int i);
extern int      inst_disk_gpt_parts(int i, gpt_part_t *out, int max);
extern int      inst_disk_has_gpt(int i);

static int g_disk;

static int esp_rd(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx; return inst_disk_read(g_disk, lba, buf);
}
static int esp_wr(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx; return inst_disk_write(g_disk, lba, buf);
}

static int mount_esp(int disk, esp_report_t *rep, int want_write)
{
    gpt_part_t p[GPT_MAX_PARTS];
    if(!rep) return ESP_E_PARAM;
    memset(rep, 0, sizeof *rep);
    rep->disk = disk;
    g_disk = disk;

    if(!inst_disk_has_gpt(disk)) return ESP_E_NOGPT;
    int n = inst_disk_gpt_parts(disk, p, GPT_MAX_PARTS);
    if(n <= 0) return ESP_E_NOGPT;

    for(int i = 0; i < n; i++){
        if(!p[i].is_esp) continue;
        if(!fat32_mount_dev(esp_rd, want_write ? esp_wr : 0, 0, p[i].first_lba))
            return ESP_E_MOUNT;
        rep->found   = 1;
        rep->lba     = p[i].first_lba;
        rep->sectors = p[i].last_lba - p[i].first_lba + 1;
        return ESP_OK;
    }
    return ESP_E_NOESP;
}

int esp_find(int disk, esp_report_t *rep)
{
    /* Read-only: mounted with no write function, so nothing reachable from
       here can put a byte on somebody's boot partition. */
    int r = mount_esp(disk, rep, 0);
    if(r != ESP_OK) return r;
    r = survey(rep);
    fat32_unmount();
    return r;
}

int esp_install(int disk, esp_report_t *rep, int havendos_part, int dry_run)
{
    int r;
    if(!rep) return ESP_E_PARAM;
    if(!dry_run && inst_disk_is_readonly(disk)) return ESP_E_READONLY;
    r = mount_esp(disk, rep, dry_run ? 0 : 1);
    if(r != ESP_OK) return r;
    r = esp_install_mounted(rep, havendos_part, dry_run);
    fat32_unmount();
    return r;
}

#endif /* ESP_NO_KERNEL_GLUE */
