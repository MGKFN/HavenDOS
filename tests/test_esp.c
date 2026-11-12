/* Installing into Windows' EFI System Partition without breaking Windows.
 *
 * This is the suite for the half of a dual-boot install that worries me
 * most. A partition-table bug is loud: a tool says the table is wrong, and
 * 34 sectors of backup put it back. A bug in here is silent. The GPT stays
 * perfect, every partition stays exactly where it was, and the machine
 * just stops booting Windows — because one directory sector in a FAT32
 * volume got a read-modify-write wrong and \EFI\Microsoft\Boot\bootmgfw.efi
 * now points at the wrong cluster.
 *
 * So the central claim is not "the install succeeded". It is:
 *
 *     every file Windows put on the ESP is byte-for-byte identical
 *     afterwards, checked with mtools — which is not our code — and
 *     fsck.vfat calls the volume clean.
 *
 * The fixture is formatted by mkfs.vfat and populated by mcopy, so it is
 * somebody else's FAT32, and 4.6 MB of it is laid out the way a Windows 11
 * ESP is laid out. Writes go to a scratch copy; tests/esp.img is never
 * written to.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* memset, memcpy and memcmp are deliberately NOT defined here. fs/fat32.c
   includes the kernel's include/string.h, which declares them with a
   size_t length, and a uint32_t definition beside that declaration does
   not compile. The host libc supplies them and the signatures agree --
   the arrangement tests/test_fat32.c uses. tests/test_gptwrite.c does the
   opposite, because fs/gpt.c declares them itself and includes no
   string.h; the difference is in the kernel sources, not the tests. */
static int streq(const char *a, const char *b){
    while(*a && *a==*b){a++;b++;} return *a==*b;
}

/* the block layer, backed by a file */
static FILE *g_img;
static int img_read(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(fseeko(g_img,(off_t)lba*512,SEEK_SET)) return 0;
    return fread(buf,1,512,g_img)==512;
}
/* A disk that stops writing but keeps saying yes.
 *
 * This is here because the whole verify-after-write path is dead code on a
 * disk that works: if every write lands, reading it back always agrees and
 * removing the check changes nothing. The injection harness proved exactly
 * that -- four separate bugs in verify_file() went unnoticed.
 *
 * A dying SSD does not return an error. It acknowledges the write and
 * keeps the old sector, which is the single most dangerous failure a
 * bootloader install can meet: the installer says "done", the firmware
 * loads half a file, and the machine is unbootable. After
 * g_fail_after_write writes, this does the same thing. */
/* Two fault modes, because they break different halves of the check.
 *
 *   g_fail_after_write  -- every later write is acknowledged and dropped.
 *                          The file ends up the WRONG SIZE, which a size
 *                          comparison alone would catch.
 *   g_corrupt_write     -- one write lands with a byte flipped. The file
 *                          is the RIGHT SIZE and the wrong contents, which
 *                          only a byte comparison catches -- and only one
 *                          that reads past the first sector.
 *
 * Without the second mode, three separate bugs in verify_file() went
 * unnoticed: comparing only the size, comparing only the first sector,
 * and ignoring the recorded size. A corrupt sector deep inside an 811 KB
 * loader is the realistic version of all three. */
static int g_fail_after_write = -1;
static int g_corrupt_write = -1;
static int g_write_count = 0;
static int img_write(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(g_fail_after_write >= 0 && g_write_count >= g_fail_after_write){
        g_write_count++;
        return 1;                    /* "yes" -- and nothing written */
    }
    if(g_corrupt_write >= 0 && g_write_count == g_corrupt_write){
        uint8_t bad[512];
        for(int i=0;i<512;i++) bad[i] = buf[i];
        bad[7] ^= 0xFF;              /* one byte, deep inside the file */
        g_write_count++;
        if(fseeko(g_img,(off_t)lba*512,SEEK_SET)) return 0;
        if(fwrite(bad,1,512,g_img)!=512) return 0;
        fflush(g_img);
        return 1;
    }
    g_write_count++;
    if(fseeko(g_img,(off_t)lba*512,SEEK_SET)) return 0;
    if(fwrite(buf,1,512,g_img)!=512) return 0;
    fflush(g_img);
    return 1;
}

/* fat32_init() probes these; not the path under test, but it must link. */
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_ready(void){ return 0; }
int ata_detected(void){ return 0; }
int ata_lba48(void){ return 0; }
int ata_read_sector   (uint32_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
/* uint64_t, which is what drivers/ata.c actually takes. fs/fat32.c used
   to declare these as uint32_t -- its own local extern, wrong, and never
   noticed because this stub was written to match the declaration rather
   than the driver. The LBA48 top-8-bit truncation is a bug this project
   has already shipped once, so the type is worth getting right even in a
   stub that returns 0. fs/blkdev.c declares them correctly now and the
   compiler compares the two. */
int ata_read_sector48 (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }

/* ── the buses this suite does not simulate ───────────────────────────
   v0.8.6: fs/fat32.c no longer carries its own two-bus switch, so
   blk_scan() asks every bus whether it is there. These say no. An ESP
   test driving an NVMe controller would be testing two things at once. */
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

/* the PMM, for the fallback-backup copy buffer */
void *pmm_alloc_contig(uint32_t nblocks){ return malloc((size_t)nblocks*4096); }
void  pmm_free_contig(void *p, uint32_t nblocks){ (void)nblocks; free(p); }

/* A loader image standing in for grub-mkimage's output. The real one is
   811 KB of PE32+; what this suite mostly cares about is that a large file
   lands intact beside Windows' files, not what is inside it.

   It does have to be PE32+-shaped, though, because fs/esp.c refuses to
   install anything that is not -- and that refusal is the reason it has to.
   The tree was built once with HAVE_GRUB_EFI_EMBED undefined, so esp.c
   compiled against its zero-byte stub and the installer would have written
   an empty BOOTX64.EFI over the machine's real one and called it a
   success. A suite whose loader is 811 KB of arbitrary bytes would have
   passed that build, which is exactly what it did. */
#define FAKE_LOADER_SZ  811008u
static uint8_t g_loader[FAKE_LOADER_SZ];

/* Writes a minimal-but-real DOS stub + PE signature + COFF machine field
   into a buffer. Little-endian, byte at a time, same as the checker. */
static void shape_as_pe(uint8_t *p, unsigned long sz, unsigned pe_off,
                        unsigned machine)
{
    unsigned i;
    for(i = 0; i < sz; i++)
        p[i] = (uint8_t)((i*7u + (i>>9)*13u) & 0xFF);
    p[0] = 'M'; p[1] = 'Z';
    p[0x3C] = (uint8_t)(pe_off      ); p[0x3D] = (uint8_t)(pe_off >>  8);
    p[0x3E] = (uint8_t)(pe_off >> 16); p[0x3F] = (uint8_t)(pe_off >> 24);
    if(pe_off + 6 <= sz){
        p[pe_off+0]='P'; p[pe_off+1]='E'; p[pe_off+2]=0; p[pe_off+3]=0;
        p[pe_off+4] = (uint8_t)(machine     );
        p[pe_off+5] = (uint8_t)(machine >> 8);
    }
}
static uint32_t g_loader_sz = FAKE_LOADER_SZ;
#define ESP_LOADER_PROVIDED 1
#define grub_efi_img       g_loader
#define grub_efi_img_size  g_loader_sz

#include "blkbus.inc"
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

/* v0.9.3: fat32_init() reads a GPT, so fs/gpt.c is linked, not stubbed. */
#include "../fs/gpt.c"
#include "../fs/fat32.c"
#define ESP_NO_KERNEL_GLUE 1
#include "../fs/esp.c"
#include "esp_fixture.inc"

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}

static unsigned crc32b(const void *p, unsigned n){
    const unsigned char *d=p; unsigned c=0xFFFFFFFFu;
    for(unsigned i=0;i<n;i++){ c^=d[i];
        for(int k=0;k<8;k++) c=(c>>1)^(0xEDB88320u&(unsigned)(-(int)(c&1))); }
    return c^0xFFFFFFFFu;
}

static int open_img(const char *p, const char *mode){
    if(g_img){ fclose(g_img); g_img=NULL; }
    g_img = fopen(p, mode);
    return g_img != NULL;
}

/* ── mtools and fsck: the opinions that are not ours ─────────────────── */
static int sh(const char *cmd){ return system(cmd); }

static int fsck_clean(const char *img, const char *why)
{
    char cmd[640];
    if(g_img){ fclose(g_img); g_img=NULL; }
    snprintf(cmd,sizeof cmd,"fsck.vfat -n %s > %s.fsck 2>&1", img, img);
    int rc = sh(cmd);
    total++;
    if(rc != 0){
        fails++;
        printf("    FAIL fsck.vfat reports errors after %s\n", why);
        snprintf(cmd,sizeof cmd,"sed -n '1,20p' %s.fsck | sed 's/^/           /'",img);
        if(sh(cmd)){}
        return 0;
    }
    return 1;
}

/* Read a file out of the image with mtools and compare it to what it
   should be. This is the check that matters: our own driver agreeing with
   our own writer proves nothing about whether Windows can still read it. */
static int mtools_file_matches(const char *img, const char *path,
                               unsigned size, unsigned crc)
{
    char cmd[768], out[512];
    snprintf(out,sizeof out,"/tmp/hd_esp_x.bin");
    if(g_img){ fclose(g_img); g_img=NULL; }
    snprintf(cmd,sizeof cmd,
             "MTOOLS_SKIP_CHECK=1 mcopy -n -i %s ::%s %s >/dev/null 2>&1",
             img, path+1, out);
    if(sh(cmd) != 0) return 0;
    FILE *f = fopen(out,"rb");
    if(!f) return 0;
    static unsigned char buf[2*1024*1024];
    size_t n = fread(buf,1,sizeof buf,f);
    fclose(f);
    if(n != size) return 0;
    return crc32b(buf,(unsigned)n) == crc;
}

static int lst_n;
static void lst_cb(const char *name,int d,uint32_t s){ (void)name;(void)d;(void)s; lst_n++; }

int main(void)
{
    char scratch[256], cmd[640];
    esp_report_t rep;

    printf("Installing onto Windows' EFI partition, without breaking Windows\n");
    printf("================================================================\n");

    /* 0x80 is where a real grub-mkimage image puts its PE header. */
    shape_as_pe(g_loader, FAKE_LOADER_SZ, 0x80, 0x8664);

    head("the fixture is a Windows-shaped ESP");
    if(!open_img(ESP_IMG,"rb")){
        printf("    FAIL %s missing -- run tools/make_esp_fixture.py\n", ESP_IMG);
        printf("\n0/1 passed\n"); return 1;
    }
    ck("it opens", 1, 0);
    ck("it mounts as FAT32",
       fat32_mount_dev(img_read, 0, 0, 0) == 1, 0);
    ck("Windows' boot manager is on it", fat32_exists(P_WINDOWS), 0);
    ck("so is the BCD", fat32_exists("/EFI/Microsoft/Boot/BCD"), 0);
    ck("and every file the manifest lists", ({
        int ok=1;
        for(int i=0;i<ESP_NFILES;i++)
            if(!fat32_exists(ESP_FILES[i].path)) ok=0;
        ok; }), 0);
    ck("there is no HavenDOS on it yet", !fat32_exists(P_LOADER), 0);
    fat32_unmount();

    head("the survey reads the volume and writes nothing");
    {
        snprintf(scratch,sizeof scratch,"%s.scratch",ESP_IMG);
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        ck("a scratch copy is made", sh(cmd)==0, 0);
        ck("it mounts read-only",
           open_img(scratch,"r+b") && fat32_mount_dev(img_read,0,0,0), 0);
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 1);     /* dry run */
        ck("the dry run says it would succeed", r == ESP_OK, r);
        ck("it saw Windows", rep.has_microsoft, 0);
        ck("it found no existing fallback loader", !rep.has_fallback, 0);
        /* Two copies of an 811 KB loader is 1584 KB, plus the config and
           eight clusters of slack: about 1589 KB. Asserted as a range
           rather than the exact number so a change to the slack does not
           fail the suite, but tight enough that forgetting the SECOND
           copy -- which is half the total -- cannot pass. */
        ck("it needs room for two copies of the loader, not one",
           rep.need_kb > 1500 && rep.need_kb < 2048, (long)rep.need_kb);
        ck("and there is far more than that free",
           rep.free_kb > rep.need_kb * 4, (long)rep.free_kb);
        ck("nothing was created by the dry run", !fat32_exists(P_LOADER), 0);
        fat32_unmount();
    }
    ck("and fsck agrees the dry run changed nothing",
       fsck_clean(scratch,"a dry run"), 0);

    head("the install");
    {
        ck("remount read-write",
           open_img(scratch,"r+b") && fat32_mount_dev(img_read,img_write,0,0), 0);
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 0);
        ck("it succeeds", r == ESP_OK, r);
        ck("the loader is there at full size",
           fat32_file_size(P_LOADER) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_LOADER));
        ck("the fallback loader is there too",
           fat32_file_size(P_FALLBACK) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_FALLBACK));
        ck("so is a grub.cfg", fat32_file_size(P_CFG) > 100, 0);
        ck("nothing was backed up, because nothing was in the way",
           !rep.would_back_up, 0);
        fat32_unmount();
    }

    head("THE CLAIM: every Windows file is byte-identical, per mtools");
    {
        int bad = 0;
        for(int i=0;i<ESP_NFILES;i++){
            if(!mtools_file_matches(scratch, ESP_FILES[i].path,
                                    ESP_FILES[i].size, ESP_FILES[i].crc)){
                if(bad < 4) printf("      %s DIFFERS\n", ESP_FILES[i].path);
                bad++;
            }
        }
        ck("all nine of Windows' files read back identical through mtools",
           bad == 0, bad);
    }
    ck("and fsck.vfat calls the volume clean", fsck_clean(scratch,"the install"), 0);

    head("and what we wrote is really what we meant to write");
    {
        ck("remount", open_img(scratch,"r+b") && fat32_mount_dev(img_read,0,0,0), 0);
        static char buf[4096];
        int n = fat32_read_at(P_LOADER, 0, buf, sizeof buf);
        ck("the loader's first 4 KB match the source",
           n == (int)sizeof buf && memcmp(buf, g_loader, sizeof buf) == 0, n);
        n = fat32_read_at(P_LOADER, FAKE_LOADER_SZ - 2048, buf, 2048);
        ck("and so does its last 2 KB",
           n == 2048 && memcmp(buf, g_loader + FAKE_LOADER_SZ - 2048, 2048) == 0, n);
        n = fat32_read(P_CFG, buf, sizeof buf);
        ck("the menu offers Windows, not just HavenDOS", ({
            buf[n>0?n:0] = 0;
            int has = 0;
            for(int i=0;i+20<n;i++)
                if(memcmp(buf+i,"bootmgfw.efi",12)==0) has=1;
            has; }), 0);
        ck("and it carries the real version, not a typed one", ({
            buf[n>0?n:0]=0; int has=0;
            for(int i=0;i+8<n;i++)
                if(memcmp(buf+i,HAVENDOS_VER,4)==0) has=1;
            has; }), 0);
        fat32_unmount();
    }

    head("Windows' own directory is untouched");
    {
        ck("remount", open_img(scratch,"r+b") && fat32_mount_dev(img_read,0,0,0), 0);
        lst_n = 0; fat32_list_dir("/EFI/Microsoft/Boot", lst_cb);
        ck("the Boot directory still holds its seven entries", lst_n == 7, lst_n);
        lst_n = 0; fat32_list_dir("/EFI", lst_cb);
        ck("and /EFI now holds Microsoft, HAVENDOS and BOOT", lst_n == 3, lst_n);
        ck("Microsoft is still a directory", fat32_is_dir("/EFI/Microsoft"), 0);
        fat32_unmount();
    }

    head("an existing fallback loader is copied aside, never overwritten");
    {
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        ck("fresh scratch copy", sh(cmd)==0, 0);
        /* put something at the fallback path first, as a machine that has
           already had another OS installed would have */
        static uint8_t other[64000];
        for(unsigned i=0;i<sizeof other;i++) other[i]=(uint8_t)(i^0x5A);
        ck("mount rw", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        ck("plant someone else's BOOTX64.EFI",
           fat32_mkdir(P_DIR_EFI) && fat32_mkdir(P_DIR_BOOT)
           && fat32_write(P_FALLBACK,(const char*)other,sizeof other), 0);
        unsigned other_crc = crc32b(other,sizeof other);

        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 1);
        ck("the dry run notices it", r == ESP_OK && rep.has_fallback, r);
        ck("and says it would back it up", rep.would_back_up, 0);
        ck("under a name it states up front",
           streq(rep.backup_name, P_FALLBACK_BAK), 0);

        memset(&rep,0,sizeof rep);
        r = esp_install_mounted(&rep, 1, 0);
        ck("the install succeeds", r == ESP_OK, r);
        ck("the previous loader is still on the volume, intact", ({
            static char rb[70000];
            int n = fat32_read(P_FALLBACK_BAK, rb, sizeof rb);
            n == (int)sizeof other && crc32b(rb,(unsigned)n) == other_crc; }), 0);
        ck("and ours is now at the fallback path",
           fat32_file_size(P_FALLBACK) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_FALLBACK));
        fat32_unmount();
        ck("fsck is clean", fsck_clean(scratch,"backing up a fallback"), 0);

        /* ── a SECOND install into the same gap ──────────────────────
           Until v0.8.4 this returned ESP_E_BACKUP, and reinstalling meant
           restoring BOOTX64.EFI by hand first.  The rule it enforced --
           never overwrite a backup -- is right on a first install and
           exactly backwards on a second: by then the file at the fallback
           path is OUR loader, and backing it up would write our loader
           over BOOTX64.HDB and destroy the only copy of the one that was
           there before HavenDOS.

           So the claim is stronger than "it is refused". It is: the
           install goes through, and the original loader is still in the
           backup afterwards, byte for byte. */
        ck("remount", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        memset(&rep,0,sizeof rep);
        r = esp_install_mounted(&rep, 1, 1);
        ck("a second dry run is allowed", r == ESP_OK, r);
        ck("and recognises the fallback as ours", rep.fallback_is_ours, 0);
        ck("so it does not plan another backup", !rep.would_back_up, 0);

        memset(&rep,0,sizeof rep);
        r = esp_install_mounted(&rep, 1, 0);
        ck("the second install succeeds", r == ESP_OK, r);
        ck("THE ORIGINAL LOADER IS STILL IN THE BACKUP, BYTE FOR BYTE", ({
            static char rb[70000];
            int n = fat32_read(P_FALLBACK_BAK, rb, sizeof rb);
            n == (int)sizeof other && crc32b(rb,(unsigned)n) == other_crc; }), 0);
        ck("and ours is still at the fallback path",
           fat32_file_size(P_FALLBACK) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_FALLBACK));
        fat32_unmount();
        ck("fsck is clean after reinstalling", fsck_clean(scratch,"second install"), 0);

        /* ── but a stranger's loader at the fallback path, with the backup
              name already taken, is still refused ──────────────────────
           The fix must not turn into "overwrite whatever is there". This
           is the case the old rule existed for, and it still holds. */
        ck("remount", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        {
            /* EXACTLY the size of our own loader, and none of the same
               bytes. A comparison that looks at the length only would
               call this ours and overwrite it -- which is the whole
               failure the byte comparison exists to prevent, and the
               injection harness MISSED it while this was 8 KB. */
            static unsigned char stranger[FAKE_LOADER_SZ];
            unsigned i;
            for(i = 0; i < sizeof stranger; i++)
                stranger[i] = (unsigned char)(i * 7u + 3u);
            ck("plant a stranger's loader of exactly our loader's size",
               fat32_delete(P_FALLBACK)
               && fat32_write(P_FALLBACK,(const char*)stranger,sizeof stranger), 0);
            ck("it really is the same size",
               fat32_file_size(P_FALLBACK) == FAKE_LOADER_SZ,
               (long)fat32_file_size(P_FALLBACK));
        }
        memset(&rep,0,sizeof rep);
        r = esp_install_mounted(&rep, 1, 1);
        ck("that is NOT ours", !rep.fallback_is_ours, 0);
        ck("and with the backup name taken it is still refused",
           r == ESP_E_BACKUP, r);
        ck("and the original loader is STILL in the backup", ({
            static char rb[70000];
            int n = fat32_read(P_FALLBACK_BAK, rb, sizeof rb);
            n == (int)sizeof other && crc32b(rb,(unsigned)n) == other_crc; }), 0);
        fat32_unmount();
    }

    head("a full EFI partition is refused before anything is written");
    {
        char full[256];
        snprintf(full,sizeof full,"%s.scratch",ESP_IMG_FULL);
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG_FULL,full);
        ck("scratch copy of the full one", sh(cmd)==0, 0);
        ck("it mounts", open_img(full,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 1);
        ck("the dry run refuses for want of space", r == ESP_E_SPACE, r);
        ck("it says how much it needed", rep.need_kb > 1500, (long)rep.need_kb);
        ck("and how little there was", rep.free_kb < rep.need_kb, (long)rep.free_kb);
        r = esp_install_mounted(&rep, 1, 0);
        ck("the real install refuses the same way", r == ESP_E_SPACE, r);
        ck("nothing was created", !fat32_exists(P_LOADER), 0);
        ck("Windows' loader is still there",
           fat32_file_size(P_WINDOWS) == 1720320,
           (long)fat32_file_size(P_WINDOWS));
        fat32_unmount();
        ck("and fsck is clean on it", fsck_clean(full,"a refused install"), 0);
        /* esp_full.img's bootmgfw is built from the same seed as esp.img's,
           so the manifest's CRC is the right expectation for both. */
        ck("Windows' file survives per mtools too",
           mtools_file_matches(full, "/EFI/Microsoft/Boot/bootmgfw.efi",
                               ESP_FILES[0].size, ESP_FILES[0].crc), 0);
        snprintf(cmd,sizeof cmd,"rm -f %s %s.fsck",full,full);
        if(sh(cmd)){}
    }

    head("a disk that stops writing but keeps saying yes");
    {
        /* The failure a verify-after-write exists for, and the one that
           cannot be caught any other way: every call returns success and
           the bytes are not there. */
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        if(sh(cmd)){}
        ck("mount rw", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        g_write_count = 0; g_fail_after_write = 200;
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 0);
        g_fail_after_write = -1;
        ck("the install does NOT report success", r != ESP_OK, r);
        ck("it reports the file read back wrong, not a write error",
           r == ESP_E_VERIFY, r);
        fat32_unmount();

        /* and the same during the backup copy, where believing a bad
           write means trusting a corrupt copy of somebody's bootloader */
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        if(sh(cmd)){}
        static uint8_t other2[64000];
        for(unsigned i=0;i<sizeof other2;i++) other2[i]=(uint8_t)(i^0x33);
        ck("mount rw again", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        ck("plant another loader at the fallback path",
           fat32_mkdir(P_DIR_EFI) && fat32_mkdir(P_DIR_BOOT)
           && fat32_write(P_FALLBACK,(const char*)other2,sizeof other2), 0);
        g_write_count = 0; g_corrupt_write = 20;   /* inside the backup copy */
        memset(&rep,0,sizeof rep);
        r = esp_install_mounted(&rep, 1, 0);
        g_corrupt_write = -1;
        ck("a backup that landed corrupt is refused, not trusted",
           r == ESP_E_BACKUP || r == ESP_E_VERIFY, r);
        ck("and ours was never written over theirs", ({
            static char rb[70000];
            int n = fat32_read(P_FALLBACK, rb, sizeof rb);
            n == (int)sizeof other2; }), 0);
        fat32_unmount();
    }

    head("one corrupt sector deep inside the loader");
    {
        /* Right size, wrong contents. The only check that finds this is a
           byte comparison that reads the whole file -- which is why the
           loader is verified end to end and not by its length. */
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        if(sh(cmd)){}
        ck("mount rw", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        g_write_count = 0; g_corrupt_write = 800;   /* inside the loader */
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 0);
        g_corrupt_write = -1;
        ck("the install reports the file read back wrong", r == ESP_E_VERIFY, r);
        ck("even though the file is exactly the right length",
           fat32_file_size(P_LOADER) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_LOADER));
        fat32_unmount();
    }

    head("a kernel built without the loader refuses, and touches nothing");
    {
        /* This section exists because the build did it. CFLAGS carried
           -DHAVE_GRUB_EMBED and -DHAVE_KERNEL_ELF but not
           -DHAVE_GRUB_EFI_EMBED, so fs/esp.c took its #else branch -- a
           one-byte stub -- and the resulting kernel would have moved the
           machine's real BOOTX64.EFI aside, written nothing in its place,
           verified that nothing read back as nothing, and said "installed".

           Every case below is a build or packaging mistake, not a disk
           fault: an undefined macro, a truncated embed, a rule pointed at
           one of the two i386-pc images in include/. None of them should
           reach the disk, so the refusal has to come from survey(), which
           is also what makes `install --dry-run` report it. */
        unsigned char pristine[4096];
        unsigned       pcrc;

        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        if(sh(cmd)){}
        ck("mount rw", open_img(scratch,"r+b")
           && fat32_mount_dev(img_read,img_write,0,0), 0);
        /* the first 8 sectors: boot sector, FSInfo, backups */
        { int i; for(i=0;i<8;i++) img_read(0,(uint64_t)i,pristine+512*i); }
        pcrc = crc32b(pristine,sizeof pristine);

        struct { const char *what; uint32_t sz; unsigned off, mach; int pe; }
        bad[] = {
          { "the zero-byte stub the broken build produced",   0, 0x80, 0x8664, 1 },
          { "a one-byte stub",                                1, 0x80, 0x8664, 1 },
          { "a truncated embed, under the 64 KB floor",   32768u, 0x80, 0x8664, 1 },
          { "no MZ at all -- a raw blob, not an image", FAKE_LOADER_SZ, 0x80, 0x8664, 0 },
          { "an i386-pc image, which 64-bit firmware will not run",
                                        FAKE_LOADER_SZ, 0x80, 0x014C, 1 },
          { "MZ, but no PE signature where the stub points",
                                        FAKE_LOADER_SZ, 0x80, 0x8664, 2 },
          { "a PE offset pointing past the end of the image",
                                        FAKE_LOADER_SZ, 0x7FFFFFFFu, 0x8664, 1 },
        };
        unsigned bi;
        for(bi = 0; bi < sizeof bad / sizeof bad[0]; bi++){
            char lbl[160];
            g_loader_sz = bad[bi].sz;
            if(bad[bi].pe == 0){            /* valid PE, then break the MZ */
                shape_as_pe(g_loader, FAKE_LOADER_SZ, bad[bi].off, bad[bi].mach);
                g_loader[0] = 'Z'; g_loader[1] = 'M';
            } else if(bad[bi].pe == 2){     /* keep MZ, erase the PE sig   */
                shape_as_pe(g_loader, FAKE_LOADER_SZ, bad[bi].off, bad[bi].mach);
                g_loader[bad[bi].off] = 'X';
            } else {
                shape_as_pe(g_loader, FAKE_LOADER_SZ, bad[bi].off, bad[bi].mach);
            }

            /* called once and the result kept: a `ck` that invokes the
               installer twice would write twice the day one of these
               stopped refusing, which is the day it matters. */
            memset(&rep,0,sizeof rep);
            int rl = esp_install_mounted(&rep,1,0);
            snprintf(lbl,sizeof lbl,"refused: %s", bad[bi].what);
            ck(lbl, rl == ESP_E_LOADER, rl);

            memset(&rep,0,sizeof rep);
            rl = esp_install_mounted(&rep,1,1);
            ck("  and the dry run says so too, not \"ok\"",
               rl == ESP_E_LOADER, rl);
        }

        /* put it back, and prove the refusals were free of charge */
        g_loader_sz = FAKE_LOADER_SZ;
        shape_as_pe(g_loader, FAKE_LOADER_SZ, 0x80, 0x8664);

        ck("nothing was created under /EFI/HAVENDOS", !fat32_exists(P_LOADER), 0);
        ck("nor at the fallback path", !fat32_exists(P_FALLBACK), 0);
        ck("nor a backup of the file that was there",
           !fat32_exists(P_FALLBACK_BAK), 0);
        ck("Windows' boot manager still there", fat32_exists(P_WINDOWS), 0);
        { int i; unsigned char now[4096];
          for(i=0;i<8;i++) img_read(0,(uint64_t)i,now+512*i);
          ck("and the volume's own first 8 sectors are byte-identical",
             crc32b(now,sizeof now) == pcrc, 0); }

        ck("the good loader installs on the very next call",
           esp_install_mounted(&rep,1,0) == ESP_OK, 0);
        ck("at its full length",
           fat32_file_size(P_LOADER) == FAKE_LOADER_SZ,
           (long)fat32_file_size(P_LOADER));
        fat32_unmount();
        ck("fsck clean after seven refusals and one install",
           fsck_clean(scratch,"refusals then an install"), 0);
    }

    head("the menu it writes boots a desktop, not the installer");
    {
        /* harness/uefi_dualboot.py booted the installed system under OVMF
           and got the INSTALLER, announcing it had booted from the install
           ISO, on a machine with no ISO in it. The multiboot line carried
           no kernel command line, and with none the kernel comes up in
           install mode.

           Nothing native could have caught that -- every suite was green
           and the bytes on the ESP were exactly the bytes intended. So the
           content is asserted here, and the end-to-end harness is still
           what proves the machine boots. */
        int has_mode = 0, has_gfx = 0, has_elf = 0, n = (int)cstrlen(CFG_TEXT);
        int i;
        for(i = 0; i + 12 <= n; i++){
            if(memcmp(CFG_TEXT + i, "mode=desktop", 12) == 0) has_mode = 1;
            if(memcmp(CFG_TEXT + i, "gfx=text", 8) == 0)      has_gfx  = 1;
            if(memcmp(CFG_TEXT + i, "/boot/havendos.elf", 18) == 0) has_elf = 1;
        }
        ck("it loads /boot/havendos.elf", has_elf, 0);
        ck("with mode=desktop, or the installed system boots the installer",
           has_mode, 0);
        ck("and gfx=text, as fs/install.c's own grub.cfg does", has_gfx, 0);

        /* install=1 is the ISO's entry, not an installed system's. */
        {
            int has_install = 0;
            for(i = 0; i + 9 <= n; i++)
                if(memcmp(CFG_TEXT + i, "install=1", 9) == 0) has_install = 1;
            ck("and NOT install=1, which is the ISO's entry",
               !has_install, 0);
        }
    }

    head("a read-only mount cannot install, however it is asked");
    {
        snprintf(cmd,sizeof cmd,"cp -f %s %s",ESP_IMG,scratch);
        if(sh(cmd)){}
        ck("mount with no write function",
           open_img(scratch,"r+b") && fat32_mount_dev(img_read,0,0,0), 0);
        memset(&rep,0,sizeof rep);
        int r = esp_install_mounted(&rep, 1, 0);
        ck("the install fails rather than half-writing", r != ESP_OK, r);
        ck("and created nothing", !fat32_exists(P_LOADER), 0);
        fat32_unmount();
        ck("fsck still clean", fsck_clean(scratch,"a refused read-only install"), 0);
    }

    if(g_img){ fclose(g_img); g_img=NULL; }
    snprintf(cmd,sizeof cmd,"rm -f %s %s.fsck /tmp/hd_esp_x.bin",scratch,scratch);
    if(sh(cmd)){}

    printf("\n%d/%d passed\n", total-fails, total);
    return fails ? 1 : 0;
}
