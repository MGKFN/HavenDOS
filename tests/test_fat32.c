/* FAT32, compiled natively and run against an image this project did not
 * format.
 *
 * The image is made by mkfs.vfat and populated by mcopy, both of which are
 * somebody else's FAT32 implementation, and tools/make_fat32_fixture.py
 * emits the manifest below from the same run that writes it.  That is the
 * whole design: a FAT32 driver checked only against volumes it created
 * itself proves that it is self-consistent, which is not the claim anybody
 * needs.  The claim that matters is that it agrees with everything else.
 *
 * The suite FAILS rather than skips when the image is missing.  tests/
 * test_fat16path.c skips unless BOOTDISK names an installer-produced image,
 * no such image was ever in the tree, and a `tail -1` in the Makefile ate
 * the skip notice -- so it sat in `make test` output with no result for
 * three releases.  The fixture here is a Makefile prerequisite and its
 * absence is an error.
 *
 * Writes go to a SCRATCH COPY.  One run of test_fat16path.c against a real
 * image replaced a 631 KB /boot/havendos.elf with a 2 KB test file and the
 * install stopped booting.  Nothing here touches tests/fat32.img.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── the block layer fs/fat32.c expects, backed by a file ───────────── */
static FILE *g_img;

static int img_read(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, g_img) == 512;
}
static int img_write(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx;
    if(!g_img) return 0;
    if(fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    if(fwrite(buf, 1, 512, g_img) != 512) return 0;
    fflush(g_img);
    return 1;
}

/* fat32_init() probes these; it is not the path under test here, but the
   symbols have to exist for the file to link. */
int virtio_blk_read (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_write(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
int virtio_blk_ready(void){ return 0; }
int ata_detected(void){ return 0; }
int ata_lba48(void){ return 0; }
int ata_read_sector   (uint32_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector  (uint32_t l, const uint8_t *b){ (void)l;(void)b; return 0; }
/* uint64_t, which is what drivers/ata.c takes. fs/fat32.c declared
   these as uint32_t in a local extern of its own and this stub matched
   the declaration rather than the driver -- the LBA48 top-8-bit
   truncation is a bug this project has shipped once. fs/blkdev.c
   declares them correctly and the compiler compares the two. */
int ata_read_sector48 (uint64_t l, uint8_t *b){ (void)l;(void)b; return 0; }
int ata_write_sector48(uint64_t l, const uint8_t *b){ (void)l;(void)b; return 0; }

/* v0.8.6: fs/fat32.c's two-bus switch is gone, so blk_scan() asks all
   five buses. These say no -- this suite mounts an mkfs.vfat image
   through functions it passes explicitly. */
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

/* v0.8.6: the bus selection fs/fat16.c and fs/fat32.c used to do
   themselves is fs/blkdev.c's now -- one block layer under what were
   three switches holding four, four and TWO buses. tests/blkbus.inc
   supplies the remaining back-end entry points blk_scan() calls, in
   terms of the stubs above, and pulls in the real fs/blkdev.c. */
#include "blkbus.inc"

/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/fat32.c"
#include "fat32_fixture.inc"

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-56s got %ld\n", what, got); }
}
static void cks(const char *what, const char *got, const char *want){
    total++;
    if(!got || strcmp(got, want) != 0){
        fails++;
        printf("    FAIL %-56s got \"%s\" want \"%s\"\n",
               what, got ? got : "(null)", want);
    }
}

/* CRC-32, the polynomial GPT and zlib share, so the manifest's numbers and
   these agree without the fixture having to ship file bodies. */
static unsigned crc32b(const void *p, unsigned n){
    const unsigned char *d = (const unsigned char *)p;
    unsigned c = 0xFFFFFFFFu;
    for(unsigned i = 0; i < n; i++){
        c ^= d[i];
        for(int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (unsigned)(-(int)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* Look a fixture file up by path rather than by index: the manifest is
   generated, so its order is not something a test should depend on. */
static unsigned fix_crc(const char *path){
    for(int i = 0; i < FIX_NFILES; i++)
        if(strcmp(FIX_FILES[i].path, path) == 0) return FIX_FILES[i].crc;
    return 0;
}

/* Count live directory entries with a BLANK name field, read raw off the
   disk rather than through the renderer that wrote them -- our own renderer
   agrees with our own writer by construction and so proves nothing.  A
   blank name field is what fs/fat16.c's name_to_83() produces for every
   dotfile, and the reason it matters is that the next implementation to
   read the volume does not have to agree with us about what it means. */
static int blank_name_entries(uint32_t dir_clus, int *live_out){
    f32_cur_t c; uint8_t d[DIRENT_SZ];
    int blanks = 0, live = 0, guard = 0;
    cur_start(&c, dir_clus);
    for(;;){
        if(++guard > 40000) break;
        if(!cur_read(&c, d)) break;
        if(d[0] == DE_END) break;
        if(d[0] != DE_FREE && (d[DE_ATTR] & ATTR_LFN) != ATTR_LFN){
            live++;
            if(d[DE_NAME] == ' ') blanks++;
        }
        if(cur_step(&c) <= 0) break;
    }
    if(live_out) *live_out = live;
    return blanks;
}

static int open_img(const char *path, const char *mode){
    if(g_img){ fclose(g_img); g_img = NULL; }
    g_img = fopen(path, mode);
    return g_img != NULL;
}

/* ── a synthetic boot sector, for the type-detection boundaries ─────── */
static uint8_t bs[512];
static void bs_build(uint32_t clusters, uint32_t spc, int fat32_shaped)
{
    memset(bs, 0, sizeof bs);
    bs[510] = 0x55; bs[511] = 0xAA;
    st16(bs + 11, 512);                 /* bytes per sector               */
    bs[13] = (uint8_t)spc;
    st16(bs + 14, fat32_shaped ? 32 : 1);
    bs[16] = 2;                         /* two FATs                       */
    uint32_t rootsects, rsvd = fat32_shaped ? 32 : 1, fsz = 100;
    if(fat32_shaped){
        st16(bs + 17, 0);               /* RootEntCnt zero                */
        st16(bs + 22, 0);               /* FATSz16 zero                   */
        st32(bs + 36, fsz);             /* FATSz32                        */
        st32(bs + 44, 2);               /* RootClus                       */
        rootsects = 0;
    } else {
        st16(bs + 17, 512);
        st16(bs + 22, (uint16_t)fsz);
        rootsects = (512 * 32 + 511) / 512;
    }
    uint32_t datastart = rsvd + 2 * fsz + rootsects;
    uint32_t tot = datastart + clusters * spc;
    st16(bs + 19, 0);
    st32(bs + 32, tot);
}

/* ── fsck, which is the only opinion that counts on a written volume ── */
static int fsck_clean(const char *img, const char *why)
{
    char cmd[1024];
    if(g_img){ fclose(g_img); g_img = NULL; }
    snprintf(cmd, sizeof cmd,
             "dd if=%s of=%s.part bs=512 skip=%d status=none && "
             "fsck.vfat -n %s.part > %s.fsck 2>&1",
             img, img, FIX_PART_LBA, img, img);
    int rc = system(cmd);
    total++;
    if(rc != 0){
        fails++;
        printf("    FAIL fsck.vfat reports errors after %s\n", why);
        snprintf(cmd, sizeof cmd, "sed -n '1,25p' %s.fsck | sed 's/^/           /'", img);
        if(system(cmd)) { /* best effort */ }
        return 0;
    }
    return 1;
}

/* ── listing callback ───────────────────────────────────────────────── */
static int   lst_n;
static int   lst_dirs;
static int   lst_bad;          /* names no directory should ever yield */
static char  lst_last[260];
static int   lst_found;
static const char *lst_want;
static void lst_cb(const char *name, int isdir, uint32_t size){
    (void)size;
    lst_n++;
    if(isdir) lst_dirs++;
    /* A deleted entry's first byte is 0xE5.  A scan that forgets to skip
       one hands it straight back as a file name, which is how you find out
       months later that `ls` has been showing the ghosts of deleted files. */
    if(!name[0]) lst_bad++;
    for(const char *p = name; *p; p++)
        if((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F
           || (unsigned char)*p == 0xE5) lst_bad++;
    snprintf(lst_last, sizeof lst_last, "%s", name);
    if(lst_want && strcmp(name, lst_want) == 0) lst_found = 1;
}
static void lst_reset(const char *want){
    lst_n = 0; lst_dirs = 0; lst_bad = 0; lst_found = 0;
    lst_want = want; lst_last[0] = 0;
}

int main(void)
{
    static char buf[128 * 1024];

    printf("FAT32 -- against an image made by mkfs.vfat, not by this driver\n");
    printf("==============================================================\n");

    /* ---------------------------------------------------------------- */
    head("the fixture exists (a suite whose fixture does not is decoration)");
    if(!open_img(FIX_IMG, "rb")){
        printf("    FAIL %s is missing -- run tools/make_fat32_fixture.py\n",
               FIX_IMG);
        printf("\n0/1 passed\n");
        return 1;
    }
    ck("tests/fat32.img opens", 1, 0);

    /* ---------------------------------------------------------------- */
    head("A2: the FAT type is the CLUSTER COUNT, not the label or type byte");
    {
        uint8_t s[512];
        img_read(0, FIX_PART_LBA, s);
        ck("the 64 MB fixture reads as FAT32",
           fat_probe_type(s) == FATTYPE_32, fat_probe_type(s));
    }
    {
        FILE *save = g_img;
        if(open_img(FIX_IMG16, "rb")){
            uint8_t s[512];
            img_read(0, FIX_PART_LBA, s);
            ck("the 16 MB fixture reads as FAT16",
               fat_probe_type(s) == FATTYPE_16, fat_probe_type(s));
        } else ck("fat16.img opens", 0, 0);
        if(open_img(FIX_IMG12, "rb")){
            uint8_t s[512];
            img_read(0, FIX_PART_LBA, s);
            ck("the 2 MB fixture reads as FAT12",
               fat_probe_type(s) == FATTYPE_12, fat_probe_type(s));
        } else ck("fat12.img opens", 0, 0);
        (void)save;
        open_img(FIX_IMG, "rb");
    }
    /* The two boundaries, from both sides.  These are the numbers the spec
       gives and they are off-by-one in the direction everybody gets wrong:
       the comparison is <, not <=, on both. */
    bs_build(4084, 1, 0);
    ck("4084 clusters is FAT12", fat_probe_type(bs) == FATTYPE_12, fat_probe_type(bs));
    bs_build(4085, 1, 0);
    ck("4085 clusters is FAT16", fat_probe_type(bs) == FATTYPE_16, fat_probe_type(bs));
    bs_build(65524, 1, 0);
    ck("65524 clusters is FAT16", fat_probe_type(bs) == FATTYPE_16, fat_probe_type(bs));
    bs_build(65525, 1, 1);
    ck("65525 clusters is FAT32", fat_probe_type(bs) == FATTYPE_32, fat_probe_type(bs));
    /* A volume that SAYS FAT32 and is not one.  Real media does this. */
    bs_build(5000, 1, 0);
    memcpy(bs + 54, "FAT32   ", 8);
    memcpy(bs + 82, "FAT32   ", 8);
    ck("a volume labelled FAT32 with 5000 clusters is still FAT16",
       fat_probe_type(bs) == FATTYPE_16, fat_probe_type(bs));
    bs_build(200000, 8, 1);
    memcpy(bs + 54, "FAT16   ", 8);
    ck("a volume labelled FAT16 with 200000 clusters is still FAT32",
       fat_probe_type(bs) == FATTYPE_32, fat_probe_type(bs));
    /* Nonsense that must not be mounted rather than mounted wrongly. */
    bs_build(100000, 1, 1); bs[510] = 0;
    ck("no 0x55AA signature is not a filesystem",
       fat_probe_type(bs) == FATTYPE_NONE, fat_probe_type(bs));
    bs_build(100000, 1, 1); st16(bs + 11, 2048);
    ck("a 2048-byte sector volume is refused, not misparsed",
       fat_probe_type(bs) == FATTYPE_NONE, fat_probe_type(bs));
    bs_build(100000, 1, 1); bs[13] = 3;
    ck("sectors-per-cluster must be a power of two",
       fat_probe_type(bs) == FATTYPE_NONE, fat_probe_type(bs));
    bs_build(100000, 1, 1); bs[16] = 0;
    ck("zero FATs is refused", fat_probe_type(bs) == FATTYPE_NONE, fat_probe_type(bs));

    /* ---------------------------------------------------------------- */
    head("A1: the geometry parsed is the geometry mkfs.vfat wrote");
    ck("the volume mounts",
       fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA) == 1, 0);
    ck("sectors per cluster",  g_sects_per_clust == FIX_SECTS_PER_CLUST, g_sects_per_clust);
    ck("reserved sectors",     g_rsvd_sects      == FIX_RSVD_SECTS,      g_rsvd_sects);
    ck("number of FATs",       g_num_fats        == FIX_NUM_FATS,        g_num_fats);
    ck("FAT size in sectors",  g_fat_sz          == FIX_FAT_SZ,          g_fat_sz);
    ck("total sectors",        g_total_sects     == FIX_TOTAL_SECTS,     g_total_sects);
    ck("first data sector",    g_data_start      == FIX_DATA_START,      g_data_start);
    ck("root cluster",         g_root_clus       == FIX_ROOT_CLUS,       g_root_clus);
    ck("FSInfo sector",        g_fsinfo_sect     == FIX_FSINFO_SECT,     g_fsinfo_sect);
    ck("count of data clusters", g_clusters      == FIX_CLUSTERS,        g_clusters);
    ck("RootEntCnt is zero on FAT32 and the root is a chain",
       FIX_ROOT_ENT_CNT == 0, FIX_ROOT_ENT_CNT);
    ck("fat32_detected() agrees", fat32_detected() == 1, fat32_detected());

    head("the FAT is 32 bits of which only the low 28 are the cluster");
    {
        uint32_t v = 0;
        ck("the end-of-chain test is >= 0x0FFFFFF8",
           CLUS_EOC == 0x0FFFFFF8u, (long)CLUS_EOC);
        /* A cluster past the end of the volume is not a cluster. */
        ck("a cluster past the last one is refused",
           fat_get(g_clusters + 2, &v) == 0, 0);
        ck("clus_valid rejects 0 and 1", !clus_valid(0) && !clus_valid(1), 0);
        ck("clus_valid accepts the last real cluster",
           clus_valid(g_clusters + 1), 0);
    }

    head("the volume a FAT32 driver must refuse to mount");
    /* fat32_mount_dev() is gated on fat_probe_type(), and that gate is what
       makes the rest of the mount code safe to write the way it is: a FAT32
       volume has BPB_FATSz16 == 0 by definition, so the field-order trap
       cannot reach the mount path as long as nothing else gets through it.
       Check the gate rather than trusting it. */
    {
        fat32_unmount();
        ck("a FAT16 volume is refused a FAT32 mount",
           open_img(FIX_IMG16, "rb")
           && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA) == 0, 0);
        ck("a FAT12 volume is refused a FAT32 mount",
           open_img(FIX_IMG12, "rb")
           && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA) == 0, 0);
        ck("and nothing is left mounted after a refusal",
           !fat32_detected(), 0);
        ck("the MBR sector is not a boot sector either",
           open_img(FIX_IMG, "rb")
           && fat32_mount_dev(img_read, img_write, 0, 0) == 0, 0);
        ck("the fixture remounts", open_img(FIX_IMG, "rb")
           && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
    }

    /* ---------------------------------------------------------------- */
    head("every file in the fixture reads back byte for byte");
    for(int i = 0; i < FIX_NFILES; i++){
        const fix_file_t *f = &FIX_FILES[i];
        char what[160];
        uint32_t sz = fat32_file_size(f->path);
        snprintf(what, sizeof what, "%s: size", f->path);
        ck(what, sz == f->size, (long)sz);

        int n = fat32_read(f->path, buf, sizeof buf);
        snprintf(what, sizeof what, "%s: bytes read", f->path);
        ck(what, n == (int)f->size, n);

        if(n == (int)f->size){
            snprintf(what, sizeof what, "%s: contents", f->path);
            if(f->bytes) ck(what, memcmp(buf, f->bytes, f->size) == 0, 0);
            else         ck(what, crc32b(buf, f->size) == f->crc,
                            (long)crc32b(buf, f->size));
        }
    }

    head("the cases every cluster-chain walk gets wrong");
    ck("a file of exactly one cluster reads fully",
       fat32_read("/EXACT.BIN", buf, sizeof buf) == (int)FIX_CLUSTER_BYTES,
       fat32_read("/EXACT.BIN", buf, sizeof buf));
    ck("a file of one cluster plus one byte reads fully",
       fat32_read("/PLUSONE.BIN", buf, sizeof buf) == 513,
       fat32_read("/PLUSONE.BIN", buf, sizeof buf));
    ck("an empty file reads zero bytes and does not walk a chain",
       fat32_read("/EMPTY.TXT", buf, sizeof buf) == 0,
       fat32_read("/EMPTY.TXT", buf, sizeof buf));
    ck("a 64 KB file is a 128-link chain at this cluster size",
       fat32_read("/BIG.BIN", buf, sizeof buf) == 65536,
       fat32_read("/BIG.BIN", buf, sizeof buf));

    head("reading at an offset -- what the streaming installer needs");
    {
        static char whole[128 * 1024];
        int n = fat32_read("/BIG.BIN", whole, sizeof whole);
        ck("the whole file first", n == 65536, n);
        struct { uint32_t off, len; const char *w; } T[] = {
            {    0,  10, "from zero" },
            {  511,   2, "across a sector boundary" },
            {  512, 512, "a whole aligned cluster" },
            { 1000, 300, "unaligned, spanning clusters" },
            {65535,   1, "the last byte" },
            {65530,  99, "a read that runs past the end is clipped" },
        };
        for(unsigned t = 0; t < sizeof T / sizeof T[0]; t++){
            char what[120];
            int got = fat32_read_at("/BIG.BIN", T[t].off, buf, T[t].len);
            uint32_t want = T[t].len;
            if(T[t].off + want > 65536) want = 65536 - T[t].off;
            snprintf(what, sizeof what, "read_at %s: length", T[t].w);
            ck(what, got == (int)want, got);
            snprintf(what, sizeof what, "read_at %s: contents", T[t].w);
            ck(what, got > 0 && memcmp(buf, whole + T[t].off, (size_t)got) == 0, 0);
        }
        ck("an offset past the end returns nothing, not garbage",
           fat32_read_at("/BIG.BIN", 65536, buf, 10) == 0,
           fat32_read_at("/BIG.BIN", 65536, buf, 10));
    }

    /* ---------------------------------------------------------------- */
    head("A4: long names, including the shape FAT16 gets wrong");
    ck("a long name with spaces is found by its long name",
       fat32_exists("/A Long File Name.txt"), 0);
    ck("and by the mangled short name beside it",
       fat32_exists("/ALONGF~1.TXT"), 0);
    ck("matching is case-insensitive",
       fat32_exists("/a long file name.TXT"), 0);
    /* fs/fat16.c's name_to_83() turns ".config" into a BLANK basename with
       extension CON.  Every dotfile HavenDOS writes to a FAT16 volume today
       is a directory entry with an empty name field.  This driver does not
       inherit it. */
    ck("a dotfile is a real entry, not a blank basename",
       fat32_exists("/.config"), 0);
    ck("and reads back its own contents",
       fat32_read("/.config", buf, sizeof buf) == 34, 0);
    ck("a second dotfile sharing its first three characters is distinct",
       fat32_exists("/.shell_history"), 0);
    {
        int a = fat32_read("/.config", buf, sizeof buf);
        static char other[512];
        int b = fat32_read("/.shell_history", other, sizeof other);
        ck("the two dotfiles are not the same file",
           a > 0 && b > 0 && a != b, 0);
    }
    /* mkfs/mcopy store an all-lower-case 8.3 name with the NTRes case flags
       and NO long name at all.  A driver that ignores NTRes reports this as
       README.MD and never matches what the user typed. */
    ck("a lower-case 8.3 name is rendered through the NTRes case flags",
       fat32_exists("/readme.md"), 0);
    ck("two names colliding on their first six characters stay distinct",
       fat32_exists("/configuration-one.cfg")
       && fat32_exists("/configuration-two.cfg"), 0);
    {
        static char one[256], two[256];
        int a = fat32_read("/configuration-one.cfg", one, sizeof one);
        int b = fat32_read("/configuration-two.cfg", two, sizeof two);
        ck("and read back different contents",
           a > 0 && b > 0 && memcmp(one, two, (size_t)(a < b ? a : b)) != 0, 0);
    }
    ck("a name that is not there is not found",
       !fat32_exists("/NOSUCH.TXT"), 0);
    ck("a path through a file rather than a directory fails",
       !fat32_exists("/HELLO.TXT/inner"), 0);

    head("subdirectories, and the ESP shape Phase B writes into");
    ck("/EFI is a directory", fat32_is_dir("/EFI"), 0);
    ck("/EFI/BOOT is a directory", fat32_is_dir("/EFI/BOOT"), 0);
    ck("a nested file reads", fat32_read("/EFI/BOOT/BOOTX64.EFI", buf, sizeof buf) == 9000,
       fat32_read("/EFI/BOOT/BOOTX64.EFI", buf, sizeof buf));
    ck("a leading slash is optional",
       fat32_exists("EFI/HAVENDOS/README.TXT"), 0);
    ck("doubled slashes are tolerated",
       fat32_exists("//EFI//BOOT//BOOTX64.EFI"), 0);
    ck("a file is not a directory", !fat32_is_dir("/HELLO.TXT"), 0);

    head("the root is a CHAIN, which is the thing FAT16 roots are not");
    lst_reset("EFI");
    fat32_list(lst_cb);
    ck("the root lists every entry", lst_n == 13, lst_n);
    ck("including its subdirectories", lst_found && lst_dirs == 2, lst_dirs);
    /* 120 long-named files is several clusters of directory.  A driver that
       stops at the first cluster finds about seven of them. */
    lst_reset("many-0120.txt");
    fat32_list_dir("/MANY", lst_cb);
    ck("a directory spanning many clusters lists all of it",
       lst_n == FIX_MANY, lst_n);
    ck("including the last entry in the last cluster", lst_found, 0);
    {
        int bad = 0;
        for(int i = 1; i <= FIX_MANY; i++){
            char p[64], want[64];
            snprintf(p, sizeof p, "/MANY/many-%04d.txt", i);
            snprintf(want, sizeof want, "entry %d of %d\n", i, FIX_MANY);
            int n = fat32_read(p, buf, sizeof buf);
            if(n != (int)strlen(want) || memcmp(buf, want, (size_t)n)) bad++;
        }
        ck("and every one of them reads back correctly", bad == 0, bad);
    }
    {
        /* . and .. exist on disk and must not be listed as files. */
        lst_reset(".");
        fat32_list_dir("/EFI/BOOT", lst_cb);
        ck("'.' and '..' are not listed as entries", lst_n == 1 && !lst_found, lst_n);
    }

    /* ---------------------------------------------------------------- */
    /* Everything from here writes, and writes to a COPY. */
    head("A3: writing -- to a scratch copy, never to the fixture");
    fat32_unmount();
    if(g_img){ fclose(g_img); g_img = NULL; }
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cp -f %s %s.scratch", FIX_IMG, FIX_IMG);
        if(system(cmd) != 0){
            printf("    FAIL could not copy the fixture to a scratch image\n");
            printf("\n%d/%d passed\n", total - fails, total + 1);
            return 1;
        }
    }
    {
        char scratch[256];
        snprintf(scratch, sizeof scratch, "%s.scratch", FIX_IMG);
        ck("the scratch copy opens read-write", open_img(scratch, "r+b"), 0);
        ck("and mounts", fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);

        uint32_t free0 = fat32_free_clusters();
        ck("FSInfo carries a believable free count",
           free0 != 0xFFFFFFFFu && free0 <= g_clusters, (long)free0);

        /* an 8.3 name: one directory slot, no long name */
        static const char msg[] = "written by HavenDOS\n";
        ck("write an 8.3 name", fat32_write("/NEW.TXT", msg, sizeof msg - 1), 0);
        ck("it exists", fat32_exists("/NEW.TXT"), 0);
        ck("its size is right", fat32_file_size("/NEW.TXT") == sizeof msg - 1,
           (long)fat32_file_size("/NEW.TXT"));
        ck("it reads back byte for byte",
           fat32_read("/NEW.TXT", buf, sizeof buf) == (int)(sizeof msg - 1)
           && memcmp(buf, msg, sizeof msg - 1) == 0, 0);

        /* a long name: LFN slots written as well, and read back through the
           same reassembly that reads mkfs's */
        static const char lmsg[] = "a long name written by HavenDOS\n";
        ck("write a long name",
           fat32_write("/Written By HavenDOS.txt", lmsg, sizeof lmsg - 1), 0);
        ck("it is found by its long name",
           fat32_exists("/Written By HavenDOS.txt"), 0);
        ck("it reads back byte for byte",
           fat32_read("/Written By HavenDOS.txt", buf, sizeof buf)
             == (int)(sizeof lmsg - 1)
           && memcmp(buf, lmsg, sizeof lmsg - 1) == 0, 0);
        lst_reset("Written By HavenDOS.txt");
        fat32_list(lst_cb);
        ck("and it lists under that name, not the mangled one", lst_found, 0);

        /* a dotfile written by us -- the FAT16 bug, proved absent on write
           as well as on read */
        static const char dmsg[] = "pins and themes live in files like this\n";
        ck("write a dotfile", fat32_write("/.havendos_config", dmsg, sizeof dmsg - 1), 0);
        ck("it reads back under the name it was written with",
           fat32_read("/.havendos_config", buf, sizeof buf) == (int)(sizeof dmsg - 1)
           && memcmp(buf, dmsg, sizeof dmsg - 1) == 0, 0);
        lst_reset("/.havendos_config");
        lst_want = ".havendos_config";
        fat32_list(lst_cb);
        ck("and it lists with its leading dot and full name", lst_found, 0);
        ck("a second dotfile with the same first three characters is distinct",
           fat32_write("/.havendos_theme", "dark\n", 5)
           && fat32_read("/.havendos_config", buf, sizeof buf)
                == (int)(sizeof dmsg - 1), 0);
        /* The dotfile whose tail is short enough to pass for an extension.
           ".cfg" is the one that gets through a careless 8.3 check: basename
           empty, extension CFG, eleven bytes of "        CFG".  That entry
           round-trips inside one implementation, which is exactly why the
           FAT16 version of it has survived nine dotfiles and three releases
           -- the damage shows up in the next implementation to read the
           volume, not in this one. */
        ck("write a dotfile whose tail looks like an extension",
           fat32_write("/.cfg", "short tail\n", 11), 0);
        ck("it reads back", fat32_read("/.cfg", buf, sizeof buf) == 11, 0);
        lst_reset(".cfg");
        fat32_list(lst_cb);
        ck("and lists as .cfg", lst_found, 0);
        {
            int live = 0;
            int blanks = blank_name_entries(g_root_clus, &live);
            ck("the root holds live entries to check", live > 15, live);
            ck("not one of them has a blank name field", blanks == 0, blanks);
        }

        /* a file long enough to be a real chain */
        {
            static char big[40000];
            for(unsigned i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + (i >> 9));
            ck("write a file spanning many clusters",
               fat32_write("/BIGNEW.BIN", big, sizeof big), 0);
            ck("it reads back byte for byte",
               fat32_read("/BIGNEW.BIN", buf, sizeof buf) == (int)sizeof big
               && memcmp(buf, big, sizeof big) == 0, 0);

            /* overwrite smaller, then larger: the chain has to shrink and
               grow, and the old clusters have to come back */
            ck("overwrite it with something smaller",
               fat32_write("/BIGNEW.BIN", "small\n", 6), 0);
            ck("and the smaller contents are what is there",
               fat32_read("/BIGNEW.BIN", buf, sizeof buf) == 6
               && memcmp(buf, "small\n", 6) == 0, 0);
            ck("overwrite it with something larger again",
               fat32_write("/BIGNEW.BIN", big, sizeof big), 0);
            ck("and the larger contents are what is there",
               fat32_read("/BIGNEW.BIN", buf, sizeof buf) == (int)sizeof big
               && memcmp(buf, big, sizeof big) == 0, 0);
        }

        /* an empty file is first cluster ZERO, which is not cluster 0 */
        ck("write an empty file", fat32_write("/ZERO.TXT", "", 0), 0);
        ck("it exists with size zero",
           fat32_exists("/ZERO.TXT") && fat32_file_size("/ZERO.TXT") == 0, 0);

        /* into a subdirectory, then a new one */
        ck("write into an existing subdirectory",
           fat32_write("/EFI/HAVENDOS/grub.cfg", "set timeout=0\n", 14), 0);
        ck("and it replaced rather than duplicated",
           fat32_read("/EFI/HAVENDOS/grub.cfg", buf, sizeof buf) == 14, 0);
        lst_reset("grub.cfg");
        fat32_list_dir("/EFI/HAVENDOS", lst_cb);
        ck("the directory still holds two entries, not three", lst_n == 2, lst_n);

        ck("mkdir", fat32_mkdir("/EFI/HavenDOS Boot"), 0);
        ck("the new directory is one", fat32_is_dir("/EFI/HavenDOS Boot"), 0);
        ck("write into it",
           fat32_write("/EFI/HavenDOS Boot/BOOTX64.EFI", "stub\n", 5), 0);
        ck("and read it back",
           fat32_read("/EFI/HavenDOS Boot/BOOTX64.EFI", buf, sizeof buf) == 5, 0);
        ck("mkdir on something that already exists succeeds quietly",
           fat32_mkdir("/EFI/BOOT"), 0);

        /* growing the root past its first cluster */
        {
            int ok = 1;
            for(int i = 0; i < 40; i++){
                char p[64];
                snprintf(p, sizeof p, "/grow-%03d.txt", i);
                if(!fat32_write(p, "x\n", 2)) { ok = 0; break; }
            }
            ck("40 more files grow the root directory's chain", ok, 0);
            lst_reset("grow-039.txt");
            fat32_list(lst_cb);
            ck("and all of them are listed", lst_found, lst_n);
        }

        /* delete, and the clusters coming back */
        {
            uint32_t before = fat32_free_clusters();
            lst_reset("BIGNEW.BIN");
            fat32_list(lst_cb);
            int n_before = lst_n;
            ck("the file is listed before it is deleted", lst_found, 0);

            ck("delete a file", fat32_delete("/BIGNEW.BIN"), 0);
            ck("it is gone", !fat32_exists("/BIGNEW.BIN"), 0);
            uint32_t after = fat32_free_clusters();
            ck("and its clusters came back to the free count",
               after > before, (long)(after - before));

            /* A deleted entry is marked 0xE5 and stays where it is.  A scan
               that does not skip it keeps handing the name back -- under a
               mangled spelling, because 0xE5 replaced the first character. */
            lst_reset("BIGNEW.BIN");
            fat32_list(lst_cb);
            ck("the listing is exactly one shorter", lst_n == n_before - 1, lst_n);
            ck("the deleted name is not listed any more", !lst_found, 0);
            ck("and no ghost entry appeared in its place", lst_bad == 0, lst_bad);

            /* The long-named one too: its LFN slots must go as well, or the
               orphaned run attaches itself to whatever entry lands next. */
            ck("delete a long-named file",
               fat32_delete("/Written By HavenDOS.txt"), 0);
            lst_reset("Written By HavenDOS.txt");
            fat32_list(lst_cb);
            ck("it is not listed", !lst_found, 0);
            ck("and its LFN slots left no debris", lst_bad == 0, lst_bad);
            ck("the slots it freed are reusable",
               fat32_write("/Reused Long Name.txt", "reused\n", 7)
               && fat32_read("/Reused Long Name.txt", buf, sizeof buf) == 7, 0);

            ck("deleting something that is not there fails",
               !fat32_delete("/NOSUCH.TXT"), 0);
            ck("deleting a directory through fat32_delete is refused",
               !fat32_delete("/EFI"), 0);
        }

        head("the reserved top four bits of a FAT entry are not ours");
        {
            /* Every FAT32 entry mkfs writes has a zero top nibble, so a
               driver that never masks and never preserves reads correctly on
               any volume it has only ever seen.  Set the bits by hand and
               both halves become observable. */
            f32_ent_t be;
            ck("find a file to borrow a cluster from",
               path_walk("/BIG.BIN", &be, 0, 0, 0) == 1, 0);
            uint32_t c = be.clus;
            uint32_t fsect = FIX_PART_LBA + g_fat_start + c / FAT_PER_SECT;
            uint32_t foff  = (c % FAT_PER_SECT) * 4;
            uint8_t  raw[512];

            fat32_flush();
            ck("read the FAT sector raw", img_read(0, fsect, raw), 0);
            uint32_t orig = ld32(raw + foff);
            st32(raw + foff, orig | 0xF0000000u);
            ck("set the reserved nibble on disk", img_write(0, fsect, raw), 0);

            /* Remount so nothing is cached from before the change. */
            fat32_unmount();
            ck("remount", fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
            uint32_t v = 0;
            ck("fat_get masks the reserved bits off",
               fat_get(c, &v) && (v & ~CLUS_MASK) == 0, (long)v);
            ck("and the chain still walks, so the file still reads",
               fat32_read("/BIG.BIN", buf, sizeof buf) == 65536
               && crc32b(buf, 65536) == fix_crc("/BIG.BIN"), 0);

            /* Now write through that entry and check the bits survived. */
            ck("rewrite the same entry through fat_set",
               fat_set(c, v) && fat32_flush(), 0);
            ck("re-read the FAT sector", img_read(0, fsect, raw), 0);
            ck("the reserved nibble is still there",
               (ld32(raw + foff) & 0xF0000000u) == 0xF0000000u,
               (long)(ld32(raw + foff) >> 28));
            ck("and the cluster value is unchanged",
               (ld32(raw + foff) & CLUS_MASK) == (orig & CLUS_MASK), 0);

            /* Put it back -- in EVERY FAT.  The poke above went into FAT 1
               only, and fat32_flush() then mirrored the modified window into
               both, so restoring one copy leaves them disagreeing and fsck
               says "FATs differ but appear to be intact".  Which is fsck
               doing its job on a mess this test made, and is worth leaving
               written down: a raw write behind the driver's back has to
               maintain by hand everything the driver maintains for itself. */
            st32(raw + foff, orig);
            {
                int okall = 1;
                for(uint32_t fi = 0; fi < g_num_fats; fi++)
                    if(!img_write(0, FIX_PART_LBA + g_fat_start + fi * g_fat_sz
                                     + c / FAT_PER_SECT, raw)) okall = 0;
                ck("restore the entry in every FAT copy", okall, 0);
            }
            fat32_unmount();
            ck("remount after restoring",
               fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
        }

        head("a cluster number above 65535 needs both halves of the dirent");
        {
            /* This fixture has 127006 clusters and mkfs put everything in
               the first few hundred of them, so every cluster number in it
               fits in sixteen bits -- and a driver that drops the high word
               reads the whole volume correctly.  Push an allocation past
               65535 and the other half of the field starts mattering. */
            static char payload[4096];
            for(unsigned i = 0; i < sizeof payload; i++)
                payload[i] = (char)(i * 13 + 5);
            g_next_free = 70000;
            ck("write a file out past cluster 65535",
               fat32_write("/HIGHCLUS.BIN", payload, sizeof payload), 0);

            f32_ent_t he;
            ck("find it again", path_walk("/HIGHCLUS.BIN", &he, 0, 0, 0) == 1, 0);
            ck("its first cluster really is above 65535",
               he.clus > 65535, (long)he.clus);
            ck("it reads back byte for byte",
               fat32_read("/HIGHCLUS.BIN", buf, sizeof buf) == (int)sizeof payload
               && memcmp(buf, payload, sizeof payload) == 0, 0);

            fat32_flush();
            fat32_unmount();
            ck("and survives a remount",
               fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA)
               && fat32_read("/HIGHCLUS.BIN", buf, sizeof buf) == (int)sizeof payload
               && memcmp(buf, payload, sizeof payload) == 0, 0);
            ck("a directory out there works too",
               fat32_mkdir("/HighDir") && fat32_is_dir("/HighDir"), 0);
        }

        head("FSInfo is maintained, not left stale");
        {
            uint32_t before = fat32_free_clusters();
            static char k[8192];
            memset(k, 'k', sizeof k);
            ck("write 8 KB", fat32_write("/FSI.BIN", k, sizeof k), 0);
            uint32_t after = fat32_free_clusters();
            ck("the free count fell by exactly the clusters used",
               before - after == sizeof k / FIX_CLUSTER_BYTES,
               (long)(before - after));
            /* and the number actually reached the disk */
            fat32_flush();
            uint8_t fsi[512];
            img_read(0, FIX_PART_LBA + FIX_FSINFO_SECT, fsi);
            ck("FSInfo's signatures are intact", ld32(fsi + 0) == 0x41615252u
               && ld32(fsi + 484) == 0x61417272u
               && ld32(fsi + 508) == 0xAA550000u, 0);
            ck("and the free count on disk matches the one in memory",
               ld32(fsi + 488) == after, (long)ld32(fsi + 488));
            ck("the next-free hint on disk is a real cluster",
               clus_valid(ld32(fsi + 492)), (long)ld32(fsi + 492));
        }

        {   /* and again over everything written since, including the forty
               files that grew the root past its first cluster */
            int live = 0;
            int blanks = blank_name_entries(g_root_clus, &live);
            ck("after every write, the root is bigger still", live > 50, live);
            ck("and still not one blank name field", blanks == 0, blanks);
            f32_ent_t ed;
            if(path_walk("/EFI", &ed, 0, 0, 0) == 1){
                int dlive = 0;
                int dblanks = blank_name_entries(ed.clus, &dlive);
                ck("nor in a subdirectory we wrote into",
                   dblanks == 0 && dlive > 0, dblanks);
            } else ck("/EFI is still walkable", 0, 0);
        }

        head("a read-only mount refuses rather than half-succeeding");
        fat32_flush();
        fat32_unmount();
        ck("mounts with no write function",
           fat32_mount_dev(img_read, 0, 0, FIX_PART_LBA), 0);
        ck("reads still work", fat32_read("/HELLO.TXT", buf, sizeof buf) == 22, 0);
        ck("write is refused", !fat32_write("/RO.TXT", "no", 2), 0);
        ck("delete is refused", !fat32_delete("/HELLO.TXT"), 0);
        ck("mkdir is refused", !fat32_mkdir("/RO"), 0);
        ck("and the refused file was not created",
           !fat32_exists("/RO.TXT"), 0);
        fat32_unmount();

        head("fsck.vfat, which is the only opinion that counts");
        fsck_clean(scratch, "writing files, directories and long names");

        /* Reopen and re-verify: everything written survives a remount, and
           the files that were there before are untouched. */
        ck("the volume remounts after all of that",
           open_img(scratch, "r+b")
           && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
        ck("a file written before the remount is still there",
           fat32_read("/Reused Long Name.txt", buf, sizeof buf) == 7, 0);
        ck("so is the 8.3-named one",
           fat32_read("/NEW.TXT", buf, sizeof buf) == (int)(sizeof msg - 1), 0);
        ck("and the one out past cluster 65535",
           fat32_file_size("/HIGHCLUS.BIN") == 4096,
           (long)fat32_file_size("/HIGHCLUS.BIN"));
        ck("and one mkfs put there is unchanged",
           fat32_read("/BIG.BIN", buf, sizeof buf) == 65536
           && crc32b(buf, 65536) == fix_crc("/BIG.BIN"), 0);
        fat32_unmount();
    }

    /* ---------------------------------------------------------------- */
    /* A long name is only a long name when the run of slots carrying it
       agrees with the 8.3 entry it precedes.  On a healthy volume it always
       does, so the two checks that enforce it are dead weight until
       something has gone wrong -- which is exactly when they matter, and
       exactly why they have to be tested against a volume where it HAS.
       Its own copy, because it is deliberately corrupt and fsck would be
       right to complain about it. */
    head("an LFN run that does not belong to the entry after it is ignored");
    {
        char corrupt[256];
        char cmd[640];
        snprintf(corrupt, sizeof corrupt, "%s.corrupt", FIX_IMG);
        snprintf(cmd, sizeof cmd, "cp -f %s %s", FIX_IMG, corrupt);
        if(system(cmd) != 0){
            ck("could not make a copy to corrupt", 0, 0);
        } else {
            f32_ent_t e;
            uint8_t slot[DIRENT_SZ];

            /* checksum mismatch: the run is an orphan left by somebody else */
            ck("mount the copy", open_img(corrupt, "r+b")
               && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
            ck("the long name is there to start with",
               path_walk("/A Long File Name.txt", &e, 0, 0, 0) == 1, 0);
            ck("and it has more than one slot", e.nslots > 1, e.nslots);
            /* Change the checksum in EVERY slot of the run, not just the
               first.  Breaking one slot makes the run disagree with ITSELF,
               which the sequence check catches first -- so the final
               comparison against the 8.3 entry never gets a say and an
               injected bug in it goes unnoticed.  A run that is internally
               consistent and belongs to nothing is the case that check is
               for, and it is the case a half-written rename leaves behind. */
            {
                f32_cur_t c3 = e.first;
                int wrote = 0, isl = 1;
                for(int s = 0; s < e.nslots - 1; s++){
                    if(!cur_read(&c3, slot)) break;
                    if((slot[DE_ATTR] & ATTR_LFN) != ATTR_LFN){ isl = 0; break; }
                    slot[DE_CRT_TENTH] ^= 0xFF;
                    if(!cur_write(&c3, slot)) break;
                    wrote++;
                    if(cur_step(&c3) <= 0) break;
                }
                ck("every slot in the run is an LFN slot", isl, 0);
                ck("rewrite the whole run with a consistent wrong checksum",
                   wrote == e.nslots - 1, wrote);
            }
            fat32_unmount();
            ck("remount", fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
            ck("the long name is no longer claimed",
               !fat32_exists("/A Long File Name.txt"), 0);
            ck("but the file is still reachable by its 8.3 name",
               fat32_exists("/ALONGF~1.TXT"), 0);
            ck("and still reads correctly",
               fat32_read("/ALONGF~1.TXT", buf, sizeof buf) == 36, 0);
            lst_reset("ALONGF~1.TXT");
            fat32_list(lst_cb);
            ck("it lists under the short name instead", lst_found, 0);
            ck("and no debris is listed", lst_bad == 0, lst_bad);

            /* out-of-order sequence numbers: the same fallback */
            fat32_unmount();
            if(system(cmd) == 0 && open_img(corrupt, "r+b")
               && fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA)
               && path_walk("/A Long File Name.txt", &e, 0, 0, 0) == 1){
                f32_cur_t c2 = e.first;
                ck("step to the second LFN slot", cur_step(&c2) > 0, 0);
                ck("read it", cur_read(&c2, slot), 0);
                slot[0] = (uint8_t)((slot[0] & 0xC0) | 0x05);   /* wrong ordinal */
                ck("write a wrong sequence number", cur_write(&c2, slot), 0);
                fat32_unmount();
                ck("remount",
                   fat32_mount_dev(img_read, img_write, 0, FIX_PART_LBA), 0);
                ck("an out-of-order run is not used as a name",
                   !fat32_exists("/A Long File Name.txt"), 0);
                ck("and the short name still finds the file",
                   fat32_exists("/ALONGF~1.TXT"), 0);
                /* Not-found is the weaker half of the claim.  A run accepted
                   out of order assembles characters at the offsets its wrong
                   sequence numbers point at and leaves a hole at the front,
                   so the entry comes back with an EMPTY name -- which no
                   lookup matches either.  The listing is where the difference
                   between "rejected" and "accepted as nonsense" shows. */
                lst_reset("ALONGF~1.TXT");
                fat32_list(lst_cb);
                ck("it lists under its short name", lst_found, 0);
                ck("and not as a nameless entry", lst_bad == 0, lst_bad);
                fat32_unmount();
            } else {
                ck("could not set up the out-of-order case", 0, 0);
            }
            if(g_img){ fclose(g_img); g_img = NULL; }
            snprintf(cmd, sizeof cmd, "rm -f %s", corrupt);
            if(system(cmd)) { /* best effort */ }
        }
    }

    if(g_img){ fclose(g_img); g_img = NULL; }
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
