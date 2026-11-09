/* Where an update comes from, and whether it is the one it claims to be.
 *
 * fs/update.c could only ever read a new kernel from an ATAPI CD-ROM. The
 * machine this project is aimed at has no optical drive, no USB mass
 * storage in this kernel and no NIC anything here drives, so `update`
 * could not run on it at all -- and every release reached it by running
 * the additive installer beside a live Windows partition instead. That is
 * a write to somebody's Windows disk, once per release, because of where
 * a file was.
 *
 * v0.8.4 puts an EFI System Partition source beside the CD-ROM one. This
 * suite is about the two things that makes newly possible to get wrong:
 *
 *   1. THE MANIFEST. A file on an ESP is typed by a person at a Windows
 *      prompt. It can be truncated, half-written, CRLF-mangled, or point
 *      at the wrong file. Every one of those must be refused by its own
 *      name, before a byte of the running kernel is touched.
 *
 *   2. THE CHOICE. Two sources now exist and exactly one function may
 *      decide between them. 0.8.3.1 shipped four screens that each worked
 *      a bus label out separately, and one of them was wrong: the picker
 *      said NVMe and the screen gated behind typing YES said VirtIO.
 *
 * The ESP half runs against tests/update_esp.img, which mkfs.vfat
 * formatted and mtools populated, so it is somebody else's FAT32. The CRC
 * in its manifest was computed by python3's zlib, and the suite asserts
 * that gpt_crc32() -- the function the kernel actually uses -- agrees with
 * it on the real staged image. Our code agreeing with itself is not
 * evidence.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "update_fixture.inc"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}
static int streq(const char *a, const char *b){
    while(*a && *a==*b){a++;b++;} return *a==*b;
}

/* fat32_init() probes these; not the path under test, but it must link. */
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_ready(void){ return 0; }
int ata_detected(void){ return 0; }
int ata_lba48(void){ return 0; }
int ata_read_sector   (uint32_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
/* uint64_t, which is what drivers/ata.c takes. fs/fat32.c used to
   declare these as uint32_t in a local extern and the stub matched the
   declaration rather than the driver. fs/blkdev.c declares them
   correctly now and the compiler compares the two. */
int ata_read_sector48 (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }

/* ── v0.8.6: every other bus, saying no ───────────────────────────────
   fs/fat32.c's own two-bus switch is gone, so blk_scan() asks all five.
   This suite's volumes are files reached through functions it passes
   explicitly; a controller answering here would be a disk nobody asked
   for. */
int nvme_count(void){ return 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int ahci_count(void){ return 0; }
int ahci_is_atapi(int i){ (void)i; return 0; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i;(void)l;(void)b; return 0; }
#include "blkbus.inc"

/* ── the block layer, backed by a file ─────────────────────────────── */
static FILE *g_img;
static int img_read(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(fseeko(g_img,(off_t)lba*512,SEEK_SET)) return 0;
    return fread(buf,1,512,g_img)==512;
}
static int img_write(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(fseeko(g_img,(off_t)lba*512,SEEK_SET)) return 0;
    return fwrite(buf,1,512,g_img)==512;
}
static int open_img(const char *p, const char *mode){
    if(g_img){ fclose(g_img); g_img=NULL; }
    g_img = fopen(p, mode);
    return g_img != NULL;
}

/* fs/fat32.c is the real driver. fs/update_src.c is the code under test.
   fs/gpt.c is linked as a second translation unit rather than included,
   because it declares memcpy and memset itself and cannot share a unit
   with anything that includes the kernel's <string.h> -- the same split
   tests/test_esp.c documents. */
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/fat32.c"
#include "../include/fat32vol.h"
#include "../include/exfat.h"
/* exFAT is LINKED, not included and not stubbed. fs/fat32.c above is
   #included because this suite reaches its statics; nothing here
   needs exfat's, and including both in one translation unit makes
   each redefine the other's SECT_SZ and ATTR_DIRECTORY. The Makefile
   and tools/check_update_suite.py both name fs/exfat.c on the
   compiler line -- and check_harnesses.py is the thing that notices
   when only one of them does. */

/* ── the three fat32vol entry points, stubbed ───────────────────────
   The real ones live in shell/fat32cmd.c and run the installer's own disk
   scan. What matters here is the CONTRACT they have with update_src.c:
   a volume is handed over, and it is mounted with a NULL write function
   so the refusal lives in the driver. Both of those are reproduced
   exactly, including the read-only part -- which the suite then proves by
   trying to write. */
static int g_have_vol   = 1;   /* is there a volume at all              */
static int g_vol_is_esp = 1;   /* is it flagged as the ESP              */
static int g_mount_ok   = 1;   /* does mounting it succeed              */
static int g_mounted    = 0;
static const char *g_vol_path = NULL;

/* ── v0.8.6.3: a REAL disk in fs/blkdev.c's table ─────────────────────
   The only thing separating the USB source from the ESP source is
   blk_removable(), so that call has to be answered by the real block
   layer about a real registered disk rather than by an empty table that
   says 0 to everything -- which is what made the USB source
   untestable, and silently absent, in the first version of this. */
static int sim_rd(int u, uint64_t l, uint8_t *b){
    (void)u;(void)l;(void)b; return 0;
}
static const blk_ops_t OPS_FIXED = { "NVMe", sim_rd, 0, 0, 0 };
static const blk_ops_t OPS_STICK = { "USB",  sim_rd, 0, 0, 1 };
static void the_disk_is(int removable){
    blk_reset();
    blk_register(removable ? &OPS_STICK : &OPS_FIXED,
                 removable ? BLK_BUS_USB : BLK_BUS_NVME,
                 0, 1000, 0, 0);
}

int fat32_scan_volumes(fat32_vol_t *out, int max){
    if(max < 1 || !g_have_vol) return 0;
    memset(&out[0], 0, sizeof out[0]);
    out[0].disk = 0; out[0].lba = 0; out[0].sectors = 48u*1024u*2u;
    out[0].from_gpt = 1; out[0].is_esp = g_vol_is_esp;
    return 1;
}
int fat32_vol_mount_ro(const fat32_vol_t *v){
    (void)v;
    if(!g_mount_ok) return 0;
    if(!open_img(g_vol_path, "rb")) return 0;
    if(!fat32_mount_dev(img_read, 0, 0, 0)){   /* NULL write == read only */
        fclose(g_img); g_img = NULL; return 0;
    }
    g_mounted = 1;
    return 1;
}
void fat32_vol_release(void){
    if(!g_mounted) return;
    fat32_unmount();
    if(g_img){ fclose(g_img); g_img = NULL; }
    g_mounted = 0;
}

/* ── THE exFAT SIDE OF THE SAME FAKE VOLUME LAYER, v0.8.8.2 ──────────
   fs/update_src.c tries FAT32 and then exFAT, because a USB stick of
   32 GB or more is formatted exFAT by default and the source read
   FAT32 only until this release.

   THESE EXECUTE. They mount tests/update_stick.img through
   fs/exfat.c, which is linked into this suite -- the same image
   mkfs.exfat wrote, the FUSE driver populated and fsck.exfat called
   clean, carrying the SAME manifest and the SAME kernel bytes as the
   FAT32 one. A stub that returned 1 would prove the dispatch compiles
   and nothing else; "a stub is not the function", v0.8.5.1.

   g_have_xvol is separate from g_have_vol on purpose: the interesting
   cases are a stick with ONLY an exFAT volume on it (which is the
   real-world one) and a stick with both. */
static int g_have_xvol  = 0;   /* is there an exFAT volume at all       */
static int g_xmount_ok  = 1;   /* does mounting it succeed              */
static FILE *g_ximg     = NULL;
/* There is deliberately NO g_xmounted here. The first version had one,
   set by the mount stub -- and nothing ever cleared it, because the
   code under test unmounts by calling the REAL exfat_unmount(), which
   knows nothing about this file's bookkeeping. The check that asked
   "is exFAT mounted now" therefore read a flag that was only ever
   written, and said yes forever. exfat_mounted() is the driver's own
   answer and cannot drift from it. */

static int ximg_read(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx;
    if(!g_ximg) return 0;
    if(fseek(g_ximg, (long)(lba * 512u), SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, g_ximg) == 512;
}

int exfat_scan_volumes(fat32_vol_t *out, int max){
    if(max < 1 || !g_have_xvol) return 0;
    memset(&out[0], 0, sizeof out[0]);
    out[0].disk = 0; out[0].lba = 0; out[0].sectors = 48u*1024u*2u;
    out[0].from_gpt = 1; out[0].is_esp = 0;
    return 1;
}
int exfat_vol_mount_ro(const fat32_vol_t *v){
    (void)v;
    /* Close any handle a previous mount left open. The code under test
       releases through exfat_unmount(), which cannot close this FILE*,
       so without this every probe leaks one and a second mount reads
       through a stale descriptor. */
    if(g_ximg){ fclose(g_ximg); g_ximg = NULL; }
    if(!g_xmount_ok) return 0;
    g_ximg = fopen(UPD_FX_STICK, "rb");
    if(!g_ximg) return 0;
    if(!exfat_mount_dev(ximg_read, 0, 0)){
        fclose(g_ximg); g_ximg = NULL; return 0;
    }
    return 1;
}

/* ── a simulated ISO 9660 disc that executes reads ──────────────────
   Not a mock that returns success: atapi_read_sector() really copies out
   of an image laid out in 2048-byte sectors, so a driver that reads the
   wrong LBA gets the wrong bytes rather than a pass. */
#define ISO_KERNEL_LBA  40u
static int             g_cd_present = 0;
static unsigned char  *g_iso_kernel = NULL;
static unsigned        g_iso_kernel_sz = 0;
static const char     *g_iso_version  = NULL;   /* NULL == no /version.txt */
static const char     *g_iso_manifest = NULL;   /* NULL == no /update.inf  */

int atapi_init(void){ return g_cd_present; }
int atapi_read_sector(uint32_t lba, void *buf){
    unsigned off;
    if(!g_cd_present || lba < ISO_KERNEL_LBA) return 0;
    off = (unsigned)(lba - ISO_KERNEL_LBA) * 2048u;
    if(off >= g_iso_kernel_sz) return 0;
    memset(buf, 0, 2048);
    {
        unsigned n = g_iso_kernel_sz - off;
        if(n > 2048u) n = 2048u;
        memcpy(buf, g_iso_kernel + off, n);
    }
    return 1;
}
int atapi_read_sectors(uint32_t lba, uint32_t count, void *buf){
    uint8_t *p = (uint8_t*)buf;
    for(uint32_t i=0;i<count;i++){ if(!atapi_read_sector(lba+i,p+i*2048)) return 0; }
    return 1;
}
int iso9660_init(void){ return g_cd_present; }
int iso9660_detected(void){ return g_cd_present; }
int iso9660_find(const char *path, uint32_t *lba, uint32_t *size){
    if(!g_cd_present) return 0;
    if(!streq(path,"/boot/havendos.elf")) return 0;   /* upper case spelling misses */
    *lba = ISO_KERNEL_LBA; *size = g_iso_kernel_sz;
    return 1;
}
int iso9660_read_file(const char *path, uint8_t *buf, uint32_t max){
    const char *src = NULL;
    unsigned n;
    if(!g_cd_present) return 0;
    if(streq(path,"/version.txt")) src = g_iso_version;
    if(streq(path,"/update.inf"))  src = g_iso_manifest;
    if(!src) return 0;
    n = (unsigned)strlen(src);
    if(n > max) n = max;
    memcpy(buf, src, n);
    return (int)n;
}

#include "../fs/update_src.c"

/* ── helpers ────────────────────────────────────────────────────────── */
static int sh(const char *c){ return system(c); }

static int parse(const char *text, upd_manifest_t *m){
    return upd_manifest_parse(text, (uint32_t)strlen(text), m);
}

static unsigned g_prog_calls, g_prog_last, g_prog_total;
static void prog(uint32_t done, uint32_t total_){
    g_prog_calls++; g_prog_last = done; g_prog_total = total_;
}

int main(void){
    char scratch[256], cmd[512];
    upd_manifest_t m;
    int r;

    snprintf(scratch,sizeof scratch,"%s.scratch",UPD_FX_IMG);

    /* ══════════════════════════════════════════════════════════════
       1. The manifest parser
       ══════════════════════════════════════════════════════════════ */
    head("a manifest written on Windows, CRLF and all");
    r = parse(UPD_FX_TEXT, &m);
    ck("it parses", r == UPDM_OK, r);
    ck("version", streq(m.version, UPD_FX_VERSION), 0);
    ck("kernel name", streq(m.kernel, UPD_FX_NAME), 0);
    ck("size", m.size == UPD_FX_SIZE, (long)m.size);
    ck("crc", m.crc == UPD_FX_CRC, (long)m.crc);
    ck("every field reported present",
       m.have_version && m.have_kernel && m.have_size && m.have_crc, 0);

    head("the first line has to say what the file is");
    ck("a text file that is not a manifest",
       parse("hello\nversion=1\n", &m) == UPDM_E_MAGIC, 0);
    ck("empty", parse("", &m) == UPDM_E_MAGIC, 0);
    ck("the magic alone, truncated",
       parse("HAVENDOS-UPD", &m) == UPDM_E_MAGIC, 0);
    ck("the magic with no format number",
       parse("HAVENDOS-UPDATE\nversion=1\n", &m) == UPDM_E_MAGIC, 0);
    /* Found by tools/check_update_suite.py. Removing the byte-by-byte
       comparison of the magic was MISSED by every case above, because
       each of them happens to fail for a different reason -- the string
       is too short, or has no format number after it. The case that
       matters is a first line of exactly the right SHAPE and the wrong
       text, which is what a manifest for some other program looks like. */
    ck("a first line one letter off is not the magic",
       parse("HAVENDOS-UPDATF 1\nversion=1\nsize=8\nkernel=k\n", &m)
       == UPDM_E_MAGIC, 0);
    ck("nor is fifteen arbitrary bytes followed by a 1",
       parse("AAAAAAAAAAAAAAA 1\nversion=1\nsize=8\nkernel=k\n", &m)
       == UPDM_E_MAGIC, 0);
    ck("nor a first line that is the magic with a letter missing",
       parse("HAVENDOS-UPDAT 1\nversion=1\nsize=8\nkernel=k\n", &m)
       == UPDM_E_MAGIC, 0);
    ck("rubbish after the format number",
       parse("HAVENDOS-UPDATE 1 and then some\nversion=1\n", &m)
       == UPDM_E_MAGIC, 0);
    ck("a format from a later HavenDOS is refused BY NUMBER",
       parse("HAVENDOS-UPDATE 2\nversion=9\nsize=1\nkernel=k\n", &m)
       == UPDM_E_FORMAT, 0);
    ck("and not misread field by field", m.have_version == 0, 0);

    head("the required fields are required");
    ck("no version",
       parse("HAVENDOS-UPDATE 1\nsize=10\nkernel=k\n", &m) == UPDM_E_FIELD, 0);
    ck("no size",
       parse("HAVENDOS-UPDATE 1\nversion=1\nkernel=k\n", &m) == UPDM_E_FIELD, 0);
    ck("no kernel",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=10\n", &m) == UPDM_E_FIELD, 0);
    ck("crc32 is NOT required -- an old ISO has none",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=10\nkernel=k\n", &m)
       == UPDM_OK, 0);
    ck("and says so rather than reporting a zero checksum",
       m.have_crc == 0 && m.crc == 0, 0);

    head("values that will not parse are refused, one reason each");
    ck("size=0",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=0\nkernel=k\n", &m)
       == UPDM_E_VALUE, 0);
    ck("size with letters in it",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=12x4\nkernel=k\n", &m)
       == UPDM_E_VALUE, 0);
    ck("size empty",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=\nkernel=k\n", &m)
       == UPDM_E_VALUE, 0);
    ck("a size that would overflow 32 bits is refused, not wrapped",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=99999999999\nkernel=k\n", &m)
       == UPDM_E_VALUE, 0);
    ck("crc with a non-hex digit",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\nkernel=k\ncrc32=12g4\n", &m)
       == UPDM_E_VALUE, 0);
    ck("crc wider than 32 bits",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\nkernel=k\ncrc32=1234567890\n", &m)
       == UPDM_E_VALUE, 0);
    ck("crc empty",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\nkernel=k\ncrc32=\n", &m)
       == UPDM_E_VALUE, 0);
    ck("version empty",
       parse("HAVENDOS-UPDATE 1\nversion=\nsize=1\nkernel=k\n", &m)
       == UPDM_E_VALUE, 0);

    head("a kernel= that tries to leave the update directory");
    ck("a forward slash",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\nkernel=../x\n", &m)
       == UPDM_E_VALUE, 0);
    ck("a backslash -- which is what Windows types",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\n"
             "kernel=..\\Microsoft\\Boot\\bootmgfw.efi\n", &m)
       == UPDM_E_VALUE, 0);
    ck("a drive letter",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\nkernel=C:x\n", &m)
       == UPDM_E_VALUE, 0);
    ck("a name longer than the field",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=1\n"
             "kernel=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", &m)
       == UPDM_E_VALUE, 0);

    head("the shapes a real file comes in");
    ck("CRLF throughout",
       parse("HAVENDOS-UPDATE 1\r\nversion=1.2\r\nsize=8\r\nkernel=k\r\n", &m)
       == UPDM_OK, 0);
    ck("and the CR is not left on the value", streq(m.version,"1.2"), 0);
    ck("no newline on the last line",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=8\nkernel=k", &m)
       == UPDM_OK, 0);
    ck("spaces around the equals",
       parse("HAVENDOS-UPDATE 1\nversion = 1.3 \nsize = 8 \nkernel = k \n", &m)
       == UPDM_OK, 0);
    ck("and they are not part of the value", streq(m.version,"1.3"), 0);
    ck("blank lines and comments",
       parse("HAVENDOS-UPDATE 1\n\n# a note\n\nversion=1\nsize=8\nkernel=k\n", &m)
       == UPDM_OK, 0);
    ck("a key this release has never heard of is ignored",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=8\nkernel=k\n"
             "signature=deadbeef\nchannel=Beta\n", &m) == UPDM_OK, 0);
    ck("a line with no equals on it is skipped, not fatal",
       parse("HAVENDOS-UPDATE 1\n[section]\nversion=1\nsize=8\nkernel=k\n", &m)
       == UPDM_OK, 0);
    ck("0x on the crc",
       parse("HAVENDOS-UPDATE 1\nversion=1\nsize=8\nkernel=k\ncrc32=0xAbCd12\n", &m)
       == UPDM_OK && m.crc == 0xABCD12u, (long)m.crc);

    head("the buffer is not a C string, and is not read as one");
    {
        /* Read off a FAT32 volume into a buffer with other bytes after it.
           A parser that walked to a NUL would read the lot. */
        char buf[256];
        const char *ok = "HAVENDOS-UPDATE 1\nversion=2.0\nsize=8\nkernel=k\n";
        unsigned n = (unsigned)strlen(ok);
        memset(buf, 'Z', sizeof buf);
        memcpy(buf, ok, n);
        r = upd_manifest_parse(buf, n, &m);
        ck("it stops at the length it was given", r == UPDM_OK, r);
        ck("and the version is not polluted", streq(m.version,"2.0"), 0);
    }
    {
        /* Truncated mid-key: the commonest shape of a half-written file. */
        char buf[64];
        const char *t = "HAVENDOS-UPDATE 1\nversion=3.0\nsiz";
        memcpy(buf, t, strlen(t));
        r = upd_manifest_parse(buf, (uint32_t)strlen(t), &m);
        ck("a half-written manifest is refused", r == UPDM_E_FIELD, r);
    }

    head("out is fully initialised even when the parse is refused");
    {
        upd_manifest_t a, b;
        memset(&a, 0xAA, sizeof a);
        memset(&b, 0x55, sizeof b);
        parse("not a manifest", &a);
        parse("not a manifest", &b);
        ck("two refusals leave identical structs",
           memcmp(&a,&b,sizeof a) == 0, 0);
        ck("and no version string survives in it", a.version[0] == 0, 0);
    }

    head("every refusal has its own sentence");
    {
        const int codes[] = { UPDM_OK, UPDM_E_MAGIC, UPDM_E_FORMAT,
                              UPDM_E_FIELD, UPDM_E_VALUE, UPDM_E_READ,
                              UPDM_E_NONE };
        int i, j, dup = 0;
        for(i=0;i<7;i++) for(j=i+1;j<7;j++)
            if(streq(upd_manifest_strerror(codes[i]),
                     upd_manifest_strerror(codes[j]))) dup = 1;
        ck("no two codes share a message", !dup, 0);
        ck("and an unknown code still says something",
           upd_manifest_strerror(-999)[0] != 0, 0);
    }

    /* ══════════════════════════════════════════════════════════════
       2. The EFI System Partition source, against a real FAT32 volume
       ══════════════════════════════════════════════════════════════ */
    snprintf(cmd,sizeof cmd,"cp -f %s %s",UPD_FX_IMG,scratch);
    if(sh(cmd)){ printf("  cannot copy fixture\n"); return 1; }
    g_vol_path = scratch;

    head("an update staged on the ESP, as if copied there from Windows");
    ck("the source probes", esp_src_probe() == 1, 0);
    ck("and it left the volume mounted", g_mounted == 1, 0);
    r = f32_src_manifest(&m);
    ck("the manifest reads off FAT32", r == UPDM_OK, r);
    ck("and agrees with the copy the fixture emitted",
       streq(m.version,UPD_FX_VERSION) && m.size == UPD_FX_SIZE
       && m.crc == UPD_FX_CRC, 0);

    head("the volume is mounted READ ONLY, and the driver enforces it");
    ck("a write to it is refused",
       fat32_write("/EFI/HAVENDOS/update/x.bin","x",1) <= 0, 0);
    ck("so is a delete", !fat32_delete(UPD_FX_MANIFEST), 0);
    ck("and the manifest is still there", fat32_exists(UPD_FX_MANIFEST), 0);

    head("reading the staged kernel back");
    {
        static unsigned char got[UPD_FX_SIZE + 4096];
        static unsigned char want[UPD_FX_SIZE];
        g_prog_calls = g_prog_last = g_prog_total = 0;
        memset(got, 0, sizeof got);
        ck("it reads", f32_src_read(&m, got, sizeof got, prog) == 1, 0);
        upd_fx_body(UPD_FX_SEED, want, UPD_FX_SIZE);
        ck("BYTE FOR BYTE what mtools put there",
           memcmp(got, want, UPD_FX_SIZE) == 0, 0);
        ck("it looks like a kernel",
           got[0]==0x7F && got[1]=='E' && got[2]=='L' && got[3]=='F', 0);
        ck("progress was reported", g_prog_calls > 0, (long)g_prog_calls);
        ck("and it ended at the real size",
           g_prog_last == UPD_FX_SIZE && g_prog_total == UPD_FX_SIZE,
           (long)g_prog_last);

        head("the checksum the manifest carries is the one zlib computed");
        ck("gpt_crc32 agrees with python3",
           upd_image_crc(got, UPD_FX_SIZE) == UPD_FX_CRC,
           (long)upd_image_crc(got, UPD_FX_SIZE));
        ck("and one flipped byte changes it", ({
            got[UPD_FX_SIZE/2] ^= 0x01;
            unsigned c = upd_image_crc(got, UPD_FX_SIZE);
            got[UPD_FX_SIZE/2] ^= 0x01;
            c != UPD_FX_CRC; }), 0);

        head("a buffer too small for the image is refused before it is read");
        ck("refused", f32_src_read(&m, got, UPD_FX_SIZE - 1, prog) == 0, 0);
    }

    head("a manifest that disagrees with the file beside it");
    {
        upd_manifest_t bad = m;
        bad.size = UPD_FX_SIZE - 512u;      /* the file is bigger than this */
        static unsigned char got[UPD_FX_SIZE];
        ck("the size is checked against the directory entry, "
           "before a byte is read",
           f32_src_read(&bad, got, sizeof got, prog) == 0, 0);
    }
    head("a manifest naming a kernel that is not there");
    {
        upd_manifest_t bad = m;
        static unsigned char got[UPD_FX_SIZE];
        memcpy(bad.kernel, "nosuch.elf", 11);
        ck("refused", f32_src_read(&bad, got, sizeof got, prog) == 0, 0);
    }
    f32_src_release();
    ck("release unmounts the volume it borrowed", g_mounted == 0, 0);

    /* ── the volume in states that are not an update ────────────────── */
    head("an ESP with no update staged on it is not a source");
    {
        snprintf(cmd,sizeof cmd,
                 "MTOOLS_SKIP_CHECK=1 mdel -i %s ::EFI/HAVENDOS/update/update.inf",
                 scratch);
        ck("remove the manifest with mtools", sh(cmd) == 0, 0);
        ck("probe says no", esp_src_probe() == 0, 0);
        ck("and nothing was left mounted", g_mounted == 0, 0);
    }
    head("a volume that is not flagged as the ESP is not looked at");
    {
        snprintf(cmd,sizeof cmd,"cp -f %s %s",UPD_FX_IMG,scratch);
        if(sh(cmd)){}
        g_vol_is_esp = 0;
        ck("probe says no", esp_src_probe() == 0, 0);
        g_vol_is_esp = 1;
        ck("and with the flag back, yes", esp_src_probe() == 1, 0);
        f32_src_release();
    }
    head("no volume at all, and a volume that will not mount");
    {
        g_have_vol = 0;
        ck("no volume", esp_src_probe() == 0, 0);
        g_have_vol = 1; g_mount_ok = 0;
        ck("a volume that will not mount", esp_src_probe() == 0, 0);
        g_mount_ok = 1;
    }

    /* ══════════════════════════════════════════════════════════════
       2b. The USB source -- v0.8.6.3
       ══════════════════════════════════════════════════════════════

       Staging an update meant booting Windows and mounting the EFI
       System Partition with mountvol. A stick is the same manifest on a
       volume found a different way, so it is the same reader: what
       differs is one call to blk_removable(), and these check that it
       is the thing that differs. */
    head("a FAT32 volume on a removable disk is an update source");
    {
        snprintf(cmd,sizeof cmd,"cp -f %s %s",UPD_FX_IMG,scratch);
        if(sh(cmd)){}
        the_disk_is(1);
        ck("the disk is removable", blk_removable(0), 0);
        ck("the USB source probes", usb_src_probe() == 1, 0);
        {
            upd_manifest_t m;
            /* The SAME reader the ESP source uses -- not a second copy
               of it, which is what this check is really asserting. */
            ck("and the shared reader reads its manifest",
               f32_src_manifest(&m) == UPDM_OK, 0);
            ck("...with the version off the stick",
               streq(m.version, UPD_FX_VERSION), 0);
        }
        f32_src_release();
    }

    head("...and it does NOT have to be flagged as an EFI System Partition");
    /* The whole point. A stick formatted by Windows has no ESP type
       GUID, so the ESP source would never look at it. */
    {
        the_disk_is(1);
        g_vol_is_esp = 0;
        ck("the ESP source will not look at it", esp_src_probe() == 0, 0);
        ck("the USB source will", usb_src_probe() == 1, 0);
        f32_src_release();
        g_vol_is_esp = 1;
    }

    head("a FIXED disk is not a USB source, whatever is on it");
    {
        the_disk_is(0);
        ck("the disk is not removable", !blk_removable(0), 0);
        ck("the USB source says no", usb_src_probe() == 0, 0);
        ck("and nothing was left mounted", g_mounted == 0, 0);
        /* ...while the ESP source still finds it, because that one asks
           about the partition type and not about the disk. */
        ck("but the ESP source still finds it", esp_src_probe() == 1, 0);
        f32_src_release();
    }

    head("a stick with nothing staged on it is not a source");
    {
        the_disk_is(1);
        snprintf(cmd,sizeof cmd,
                 "MTOOLS_SKIP_CHECK=1 mdel -i %s ::EFI/HAVENDOS/update/update.inf",
                 scratch);
        ck("remove the manifest with mtools", sh(cmd) == 0, 0);
        ck("the USB source says no", usb_src_probe() == 0, 0);
        ck("...and PUT THE VOLUME BACK", g_mounted == 0, 0);
        snprintf(cmd,sizeof cmd,"cp -f %s %s",UPD_FX_IMG,scratch);
        if(sh(cmd)){}
    }

    head("A STICK IS CHOSEN BEFORE THE ESP, and that is deliberate");
    /* fs/update.c does not delete a staged update after applying it, so
       an ESP that has been updated from once keeps its manifest for
       ever. Probed first, that leftover would beat a stick somebody
       plugged in two minutes ago, every time, and `update` would answer
       "already installed". This volume satisfies BOTH probes, so the
       only thing deciding is the order in upd_source_select(). */
    {
        const upd_source_t *s;
        the_disk_is(1);
        g_vol_is_esp = 1;
        g_cd_present = 0;
        ck("the ESP source would also take this volume",
           esp_src_probe() == 1, 0);
        f32_src_release();
        s = upd_source_select();
        ck("but the chosen source is the stick",
           s && s->kind == UPD_SRC_USB, s ? s->kind : -1);
        ck("and it is NAMED as one, on the screen before anything is "
           "touched", s && streq(upd_source_label(s), "USB stick"), 0);
        if(s && s->release) s->release();
    }

    head("and when the stick is NOT used, the probe says what it saw");
    /* "update only found the ESP" is four different failures wearing
       one face. Each arm is driven here, because a diagnostic that is
       wrong is worse than none -- it sends somebody to reformat a stick
       that was never enumerated. */
    {
        upd_usb_why_t w;

        /* 1. nothing removable on the bus at all */
        the_disk_is(0);
        ck("the USB probe says no", usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("no removable disk is counted", w.removable == 0, w.removable);
        ck("...and no volumes either",     w.volumes == 0, w.volumes);

        /* 2. a stick, but its volume is not one fat32 reports */
        the_disk_is(1);
        g_have_vol = 0;
        ck("the USB probe says no", usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("the removable disk IS counted", w.removable == 1, w.removable);
        ck("...but no FAT32 volume on it",  w.volumes == 0, w.volumes);
        g_have_vol = 1;

        /* 3. a FAT32 volume that will not mount */
        the_disk_is(1);
        g_mount_ok = 0;
        ck("the USB probe says no", usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("the volume is counted",   w.volumes == 1, w.volumes);
        ck("...and it did not mount", w.mounted == 0, w.mounted);
        g_mount_ok = 1;

        /* 4. it mounts, and carries no manifest */
        the_disk_is(1);
        snprintf(cmd,sizeof cmd,
                 "MTOOLS_SKIP_CHECK=1 mdel -i %s ::EFI/HAVENDOS/update/update.inf",
                 scratch);
        ck("remove the manifest with mtools", sh(cmd) == 0, 0);
        ck("the USB probe says no", usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("it mounted",                w.mounted == 1, w.mounted);
        ck("...and had no manifest",    w.nomanifest == 1, w.nomanifest);
        snprintf(cmd,sizeof cmd,"cp -f %s %s",UPD_FX_IMG,scratch);
        if(sh(cmd)){}

        /* ...and the arms are distinguishable, which is the whole
           point: a diagnostic that cannot tell them apart would send
           somebody to reformat a stick that was never enumerated. */
        the_disk_is(1);
        ck("a staged stick is found again", usb_src_probe() == 1, 0);
        usb_src_release();
    }

    head("an exFAT stick, which is what a 32 GB one actually is");
    /* ── WHY THIS BLOCK EXISTS ────────────────────────────────────────
       A USB stick of 32 GB or more is formatted exFAT by default, and
       until v0.8.8.2 fs/update_src.c read FAT32 only. "USB discovery
       for 32 GB drives still broken for updating" was reported four
       releases running and was this every time -- a refusal by
       design, printed in small print nobody read.

       tests/update_stick.img is formatted by mkfs.exfat, populated
       through the FUSE driver and called clean by fsck.exfat, and it
       carries THE SAME manifest and THE SAME kernel bytes as the
       FAT32 image. So the real claim can be tested: not "the exFAT
       arm returns 1" but "the update produced from an exFAT stick is
       the same one produced from a FAT32 stick". */
    {
        upd_usb_why_t w;
        upd_manifest_t m;
        static uint8_t f32_bytes[UPD_FX_SIZE];
        static uint8_t xfa_bytes[UPD_FX_SIZE];
        int i, differ;

        /* First the FAT32 stick, and keep what it produced. */
        the_disk_is(1);
        g_have_vol = 1; g_have_xvol = 0;
        ck("the FAT32 stick is the source", usb_src_probe() == 1, 0);
        ck("  ...its manifest parses",
           usb_src_manifest(&m) == UPDM_OK, 0);
        ck("  ...and the image reads",
           usb_src_read(&m, f32_bytes, sizeof f32_bytes, 0) == 1, 0);
        usb_src_release();

        /* Now a stick with ONLY an exFAT volume on it -- the case
           that was unreachable before this release. */
        the_disk_is(1);
        g_have_vol = 0; g_have_xvol = 1;
        ck("a stick with only an exFAT volume IS a source",
           usb_src_probe() == 1, 0);
        upd_usb_why(&w);
        ck("  ...no FAT32 volume was counted", w.volumes == 0, w.volumes);
        ck("  ...the exFAT volume was",       w.xvolumes == 1, w.xvolumes);
        ck("  ...and it mounted",             w.xmounted == 1, w.xmounted);

        ck("its manifest parses", usb_src_manifest(&m) == UPDM_OK, 0);
        ck("  ...to the same version",  streq(m.version, UPD_FX_VERSION), 0);
        ck("  ...the same kernel name", streq(m.kernel, UPD_FX_NAME), 0);
        ck("  ...the same size",        m.size == UPD_FX_SIZE, (long)m.size);
        ck("  ...and the same crc",     m.crc == UPD_FX_CRC, (long)m.crc);

        ck("the image reads off exFAT",
           usb_src_read(&m, xfa_bytes, sizeof xfa_bytes, 0) == 1, 0);

        /* THE CHECK THAT MATTERS. Not "it returned 1": the same
           bytes. A cluster walk that is wrong on a fragmented file
           returns the right COUNT of bytes from the wrong clusters,
           which is a pass for any check that only measures length. */
        differ = 0;
        for(i = 0; i < (int)UPD_FX_SIZE; i++)
            if(f32_bytes[i] != xfa_bytes[i]){ differ = i + 1; break; }
        ck("  ...byte for byte the same image the FAT32 stick gave",
           differ == 0, differ - 1);

        /* And the checksum the manifest declares, computed over what
           came off exFAT -- so a reader that is self-consistently
           wrong still fails here. */
        ck("  ...and it matches the manifest's crc32",
           upd_image_crc(xfa_bytes, m.size) == m.crc,
           (long)upd_image_crc(xfa_bytes, m.size));
        usb_src_release();

        /* An exFAT volume that will not mount is its own arm, because
           "no volume we can read" and "an exFAT volume that refused"
           send somebody to two different places. */
        the_disk_is(1);
        g_have_vol = 0; g_have_xvol = 1; g_xmount_ok = 0;
        ck("an exFAT volume that will not mount is not a source",
           usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("  ...the volume is still counted", w.xvolumes == 1, w.xvolumes);
        ck("  ...and it did not mount",        w.xmounted == 0, w.xmounted);
        g_xmount_ok = 1;

        /* A manifest whose size disagrees with the file, refused
           BEFORE a byte is read. Without this a short file is read as
           m->size bytes and the tail of the buffer is whatever was
           there -- and the checksum then runs over it. The FAT32 arm
           has had this check since 0.8.6; the exFAT arm is new and a
           copy is a thing that can be forgotten. */
        the_disk_is(1);
        g_have_vol = 0; g_have_xvol = 1;
        ck("the exFAT stick probes", usb_src_probe() == 1, 0);
        ck("  ...and its manifest parses",
           usb_src_manifest(&m) == UPDM_OK, 0);
        m.size -= 1;
        ck("  ...a manifest one byte short of the file is refused",
           usb_src_read(&m, xfa_bytes, sizeof xfa_bytes, 0) == 0, 0);
        m.size += 1;

        /* And a DIRECTORY pointed at as a kernel. exfat_stat()
           succeeds for one -- tests/update_stick.img carries
           /EFI/HAVENDOS/update/notakernel so this is reachable. */
        {
            int k;
            for(k = 0; k < (int)sizeof m.kernel - 1 && "notakernel"[k]; k++)
                m.kernel[k] = "notakernel"[k];
            m.kernel[k] = 0;
        }
        ck("  ...and a directory is not read as a kernel",
           usb_src_read(&m, xfa_bytes, sizeof xfa_bytes, 0) == 0, 0);
        usb_src_release();

        /* A FIXED disk with an exFAT volume on it is NOT a stick.
           The removable test is the only thing separating this
           source from the ESP one, and it went into the exFAT arm as
           a copy of the FAT32 line -- so it gets its own check
           rather than relying on the copy being right. */
        the_disk_is(0);
        g_have_vol = 0; g_have_xvol = 1;
        ck("an exFAT volume on a FIXED disk is not a USB source",
           usb_src_probe() == 0, 0);
        upd_usb_why(&w);
        ck("  ...and is not counted as one", w.xvolumes == 0, w.xvolumes);

        /* FAT32 FIRST when both are present: the stager scripts have
           always produced FAT32 and a stick that worked before must
           keep taking the path it took before. */
        the_disk_is(1);
        g_have_vol = 1; g_have_xvol = 1;
        ck("with both, the source probes", usb_src_probe() == 1, 0);
        ck("  ...and it is the FAT32 one, not the exFAT one",
           g_mounted == 1 && exfat_mounted() == 0, exfat_mounted());
        usb_src_release();

        g_have_xvol = 0;
    }

    head("with no stick, the ESP is still chosen as it always was");
    {
        const upd_source_t *s;
        the_disk_is(0);
        g_cd_present = 0;
        s = upd_source_select();
        ck("the ESP wins", s && s->kind == UPD_SRC_ESP, s ? s->kind : -1);
        ck("...and is named for what it is",
           s && streq(upd_source_label(s), "EFI System Partition"), 0);
        if(s && s->release) s->release();
    }

    head("the three sources are not named the same thing");
    ck("USB and ESP differ",
       !streq(upd_source_label(&SRC_USB), upd_source_label(&SRC_ESP)), 0);
    ck("USB and CD-ROM differ",
       !streq(upd_source_label(&SRC_USB), upd_source_label(&SRC_CD)), 0);

    /* ══════════════════════════════════════════════════════════════
       3. Choosing a source, and naming it
       ══════════════════════════════════════════════════════════════ */
    the_disk_is(0);          /* back to a fixed disk for section 3 */
    head("exactly one function chooses, and exactly one names it");
    {
        const upd_source_t *s;

        g_cd_present = 0;
        s = upd_source_select();
        ck("with an ESP and no disc, the ESP wins", s && s->kind == UPD_SRC_ESP, 0);
        ck("and is named for what it is",
           streq(upd_source_label(s), "EFI System Partition"), 0);
        if(s && s->release) s->release();

        /* Both present. On a desktop with an optical drive and a UEFI
           install, the ESP is the one somebody just copied a file onto
           from Windows and the disc is whatever was left in the tray. */
        g_iso_kernel_sz = 8192;
        g_iso_kernel = malloc(g_iso_kernel_sz);
        upd_fx_body(5, g_iso_kernel, g_iso_kernel_sz);
        g_iso_version = "v0.8.0\n";
        g_cd_present = 1;
        s = upd_source_select();
        ck("with BOTH an ESP update and a disc, the ESP wins",
           s && s->kind == UPD_SRC_ESP, s ? s->kind : -1);
        ck("and the two sources are not named the same thing",
           !streq(upd_source_label(&SRC_ESP), upd_source_label(&SRC_CD)), 0);
        if(s && s->release) s->release();
        free(g_iso_kernel); g_iso_kernel = NULL;
        g_cd_present = 0;

        /* An ESP with nothing staged, and a disc in the tray. */
        snprintf(cmd,sizeof cmd,
                 "MTOOLS_SKIP_CHECK=1 mdel -i %s ::EFI/HAVENDOS/update/update.inf",
                 scratch);
        if(sh(cmd)){}
        g_iso_kernel_sz = 8192;
        g_iso_kernel = malloc(g_iso_kernel_sz);
        upd_fx_body(3, g_iso_kernel, g_iso_kernel_sz);
        g_iso_version = "v0.8.1\n";
        g_cd_present = 1;
        s = upd_source_select();
        ck("with no update on the ESP, the disc is used",
           s && s->kind == UPD_SRC_CDROM, 0);
        ck("and is named for what it is",
           streq(upd_source_label(s), "CD-ROM"), 0);
        if(s && s->release) s->release();

        g_cd_present = 0;
        s = upd_source_select();
        ck("with neither, nothing is chosen", s == NULL, 0);
        ck("and the label says so rather than crashing",
           streq(upd_source_label(0), "nowhere"), 0);
    }

    /* ══════════════════════════════════════════════════════════════
       4. The CD-ROM source still behaves as it did
       ══════════════════════════════════════════════════════════════ */
    head("an ISO built before 0.8.4 carries no manifest");
    {
        g_cd_present = 1; g_iso_manifest = NULL;
        g_iso_version = "v0.7.20\n";
        ck("it probes", cd_src_probe() == 1, 0);
        r = cd_src_manifest(&m);
        ck("a manifest is synthesised", r == UPDM_OK, r);
        /* A release that has already shipped, standing in for what an
           old ISO would be carrying. */
        ck("the version comes off /version.txt",
           streq(m.version,"v0.7.20"), 0);  /* version-literal-ok */
        ck("the size comes off the directory entry",
           m.size == g_iso_kernel_sz, (long)m.size);
        ck("AND IT SAYS THERE IS NO CHECKSUM rather than reporting zero",
           m.have_crc == 0, 0);
    }
    head("an ISO with no /version.txt either");
    {
        g_iso_version = NULL;
        ck("probes", cd_src_probe() == 1, 0);
        ck("still parses", cd_src_manifest(&m) == UPDM_OK, 0);
        ck("and says the version is unknown rather than inventing one",
           streq(m.version,"unknown"), 0);
    }
    head("an ISO built by 0.8.4 or later carries a real one");
    {
        static char t[256];
        unsigned char *k = g_iso_kernel;
        unsigned crc;
        g_iso_version = "v0.8.4\n";
        /* the CRC of the simulated disc's own kernel, by the kernel's
           own function -- which the first section already proved agrees
           with zlib on the real fixture */
        crc = upd_image_crc(k, g_iso_kernel_sz);
        snprintf(t,sizeof t,
                 "HAVENDOS-UPDATE 1\r\nversion=0.8.4\r\nsize=%u\r\n"
                 "crc32=%08x\r\nkernel=havendos.elf\r\n",
                 g_iso_kernel_sz, crc);
        g_iso_manifest = t;
        ck("it parses", cd_src_manifest(&m) == UPDM_OK, 0);
        ck("and the disc gets a checksum too", m.have_crc == 1, 0);
        ck("which matches the bytes on it",
           m.crc == crc, (long)m.crc);

        head("a manifest that disagrees with the disc it sits on");
        {
            static char bad[256];
            snprintf(bad,sizeof bad,
                     "HAVENDOS-UPDATE 1\nversion=0.8.4\nsize=%u\n"
                     "crc32=%08x\nkernel=havendos.elf\n",
                     g_iso_kernel_sz + 2048u, crc);
            g_iso_manifest = bad;
            ck("the ISO's own directory entry wins, and it is refused",
               cd_src_manifest(&m) == UPDM_E_VALUE, 0);
            g_iso_manifest = t;
        }

        head("the disc is read through a controller that executes reads");
        {
            static unsigned char got[8192 + 4096];
            ck("manifest", cd_src_manifest(&m) == UPDM_OK, 0);
            memset(got, 0, sizeof got);
            ck("it reads", cd_src_read(&m, got, sizeof got, prog) == 1, 0);
            ck("byte for byte what is on the disc",
               memcmp(got, k, g_iso_kernel_sz) == 0, 0);
            ck("and its checksum matches the manifest",
               upd_image_crc(got, m.size) == m.crc, 0);
        }
        head("a disc with no kernel on it is not a source");
        {
            unsigned keep = g_iso_kernel_sz;
            g_iso_kernel_sz = 0;
            ck("probe refuses it", cd_src_probe() == 0, 0);
            g_iso_kernel_sz = keep;
        }
    }

    if(g_img){ fclose(g_img); g_img=NULL; }
    free(g_iso_kernel);
    snprintf(cmd,sizeof cmd,"rm -f %s",scratch);
    if(sh(cmd)){}

    printf("\n%d/%d passed\n", total-fails, total);
    return fails ? 1 : 0;
}
