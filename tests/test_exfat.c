/* ════════════════════════════════════════════════════════════════════
   test_exfat.c -- exFAT, against a volume somebody else wrote.

   A USB stick over 32 GB ships formatted exFAT. Until v0.8.8.1
   HavenDOS could name one and nothing else, which from the outside is
   indistinguishable from not seeing the drive -- "USB drives still
   not always recognised", 31 October.

   ── WHAT THIS SUITE RUNS AGAINST ────────────────────────────────────

   tests/exfat.img, written by mkfs.exfat and populated through the
   FUSE exFAT driver, then declared clean by fsck.exfat before
   tools/make_exfat_fixture.py will emit a manifest for it. Two other
   people's implementations wrote the volume and a third judged it.

   THAT IS THE WHOLE POINT. exFAT's directory entries are a SET -- a
   0x85 file entry, a 0xC0 stream extension and as many 0xC1 name
   entries as the name needs -- and a fixture built from the same
   reading of the specification as the driver agrees with the driver
   perfectly. tests/test_usbmux.c passed 79 of 79 against a
   sixteen-byte header the phone has never used, because the fixture
   and the code under test were written by the same person on the same
   afternoon from the same guess.

   ── THE ONE THAT CATCHES A READER OUT ───────────────────────────────

   A file's clusters are not always a FAT chain. The stream extension
   entry's NoFatChain flag says the allocation is one contiguous run
   and the FAT holds nothing for it, and writers set it for any file
   laid down in one go. A reader that follows the FAT works on a
   fragmented volume and walks garbage on a fresh one; one that
   assumes contiguity does the opposite. Both are wrong half the time
   in a way that looks like a corrupt disk.

   So the checks below assert WHICH FORM each file on the fixture
   actually uses, read off the volume -- and if the fixture ever stops
   containing one of each, the suite says so rather than quietly
   testing one walk twice.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── CRC-32, ours, on purpose ────────────────────────────────────────
   The manifest's CRCs are computed by zlib in
   tools/make_exfat_fixture.py. This is a second, independent
   implementation of the same standard algorithm, so a match is two
   implementations agreeing rather than one agreeing with itself --
   and linking zlib here would make it the same code on both sides.
   (It also keeps the link line to source files only, which is what
   tools/inject.py's build expects.) */
static unsigned long crc32x(const unsigned char *p, unsigned long n)
{
    unsigned long c = 0xFFFFFFFFul;
    for(unsigned long i = 0; i < n; i++){
        c ^= p[i];
        for(int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320ul & (unsigned long)(-(long)(c & 1)));
    }
    return c ^ 0xFFFFFFFFul;
}

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

#include "exfat_fixture.inc"
#include "../include/exfat.h"

/* ── the block device: the image, through a read function with the
   shape fs/exfat.c takes ───────────────────────────────────────────
   It COUNTS, and it refuses a read past the end of the image. A
   fixture that answers any LBA would let a driver with wrong
   arithmetic read zeroes for ever and call it an empty directory --
   the mock-that-cannot-refuse mistake include/usb.h has warned about
   since v0.8.5. */
static FILE *g_img;
static long  g_img_sectors;
static long  g_reads, g_refused;

static int img_read(void *ctx, uint64_t lba, uint8_t *buf)
{
    (void)ctx;
    g_reads++;
    if((long)lba >= g_img_sectors){ g_refused++; return 0; }
    if(fseek(g_img, (long)lba * 512, SEEK_SET) != 0){ g_refused++; return 0; }
    if(fread(buf, 1, 512, g_img) != 512){ g_refused++; return 0; }
    return 1;
}

static const exfix_file_t *fixture(const char *path)
{
    for(int i = 0; i < EXFIX_NFILES; i++)
        if(!strcmp(EXFIX_FILES[i].path, path)) return &EXFIX_FILES[i];
    return 0;
}

int main(void)
{
    printf("\ntest_exfat -- exFAT, against a volume mkfs.exfat wrote\n");

    g_img = fopen("tests/exfat.img", "rb");
    if(!g_img){
        printf("\n  tests/exfat.img is missing -- run\n"
               "      python3 tools/make_exfat_fixture.py tests\n");
        printf("\n0/1 passed\n");
        return 1;
    }
    fseek(g_img, 0, SEEK_END);
    g_img_sectors = ftell(g_img) / 512;
    fseek(g_img, 0, SEEK_SET);

    /* ════════════════════════════════════════════════════════════════
       1. MOUNT, AND THE GEOMETRY AS mkfs WROTE IT
       Every number here came off the image in the generator, not from
       anything this suite or the driver computed.
       ════════════════════════════════════════════════════════════════ */
    head("the volume mounts, and the geometry is exfatprogs'");
    ck("it mounts", exfat_mount_dev(img_read, 0, 0) == 1, exfat_why());
    ck("  ... and says so", exfat_mounted() == 1, 0);
    ck("  ... with no refusal recorded", exfat_why() == EXFAT_OK,
       exfat_why());
    ck("the cluster size is what mkfs chose",
       exfat_cluster_bytes() == (1u << EXFIX_SPC_SHIFT) * 512u,
       exfat_cluster_bytes());
    ck("the cluster count is what mkfs chose",
       exfat_cluster_count() == EXFIX_CLUSTER_COUNT,
       exfat_cluster_count());
    ck("the root cluster is what mkfs chose",
       exfat_root_cluster() == EXFIX_ROOT_CLUSTER, exfat_root_cluster());
    ck("the volume length is what mkfs chose",
       exfat_volume_bytes() == (uint64_t)EXFIX_VOL_LENGTH * 512u,
       (long)(exfat_volume_bytes() / 512));
    /* The driver really went to the disk for all that. */
    ck("and it actually read the device", g_reads > 0, g_reads);
    ck("  ... without reading past the end of it", g_refused == 0,
       g_refused);

    /* ════════════════════════════════════════════════════════════════
       2. THE REFUSALS, EACH BY NAME
       v0.8.7.7's lesson, applied before anybody has to debug it: "this
       is not exFAT" and "this is exFAT and I will not read it" send a
       reader to two different places.
       ════════════════════════════════════════════════════════════════ */
    head("a volume this reader will not take, refused by name");
    {
        /* Not exFAT at all: a FAT32 boot sector has an OEM name at
           offset 3 and no EXFAT signature. */
        static uint8_t fake[512];
        static uint8_t *cur;
        memset(fake, 0, sizeof fake);
        memcpy(fake + 3, "MSDOS5.0", 8);
        fake[510] = 0x55; fake[511] = 0xAA;
        cur = fake;

        /* A read function that serves one sector, so the refusal is
           about the CONTENT rather than about I/O. */
        struct { uint8_t *p; } box = { cur };
        (void)box;
        int fake_read(void *ctx, uint64_t lba, uint8_t *buf);
        /* defined below main via a file-scope helper */
        extern uint8_t *g_fake;
        g_fake = fake;
        ck("a FAT32 boot sector is refused",
           exfat_mount_dev(fake_read, 0, 0) == 0, 0);
        ck("  ... as 'not an exFAT volume'",
           exfat_why() == EXFAT_ENOTEXFAT, exfat_why());
        ck("  ... and nothing is left mounted",
           exfat_mounted() == 0, exfat_mounted());

        /* ── AND A REFUSAL AFTER A GOOD MOUNT ─────────────────────
           Which is the realistic sequence: a stick is mounted and
           another one is put in. With nothing mounted beforehand the
           flag is already 0 and an injection that stops a refusal
           unmounting is invisible -- which is exactly what happened.
           A half-mounted volume whose geometry was REJECTED still
           answering exfat_mounted() means every read after it uses
           the fields that were rejected. */
        /* And specifically a NOT-EXFAT one: `fake` holds whatever the
           last case above left in it, and each refusal path has its
           own unmount. Testing the geometry path here left the
           signature path's unmount unexercised, which is how the
           injection into it was MISSED. */
        memset(fake, 0, sizeof fake);
        memcpy(fake + 3, "MSDOS5.0", 8);
        fake[510] = 0x55; fake[511] = 0xAA;
        ck("a good volume mounts first",
           exfat_mount_dev(img_read, 0, 0) == 1, exfat_why());
        ck("  ... then a volume that is not exFAT is refused",
           exfat_mount_dev(fake_read, 0, 0) == 0, 0);
        ck("  ... by signature, not by geometry",
           exfat_why() == EXFAT_ENOTEXFAT, exfat_why());
        ck("  ... and the good one is no longer mounted either",
           exfat_mounted() == 0, exfat_mounted());

        /* exFAT, but TexFAT -- revision 2.x, a different filesystem
           wearing the same signature. Reading it as 1.x would be a
           plausible wrong answer rather than a refusal. */
        memset(fake, 0, sizeof fake);
        memcpy(fake + 3, "EXFAT   ", 8);
        fake[105] = 2;                      /* revision major */
        fake[108] = 9; fake[109] = 3;
        ck("TexFAT is refused", exfat_mount_dev(fake_read, 0, 0) == 0, 0);
        ck("  ... as a revision this reader does not know",
           exfat_why() == EXFAT_EREVISION, exfat_why());

        /* A 4K-sector volume. Real, and not what this reads. */
        memset(fake, 0, sizeof fake);
        memcpy(fake + 3, "EXFAT   ", 8);
        fake[105] = 1;
        fake[108] = 12;                     /* 4096-byte sectors */
        fake[109] = 1;
        ck("a 4K-sector volume is refused",
           exfat_mount_dev(fake_read, 0, 0) == 0, 0);
        ck("  ... as a sector size, NOT as 'not exFAT'",
           exfat_why() == EXFAT_ESECTORSIZE, exfat_why());

        /* ── ONE GUARD AT A TIME ──────────────────────────────────
           The first version of this built one broken boot sector and
           asserted EGEOMETRY, which several different guards all
           produce -- so deleting any ONE of them still refused and
           the injections into them were MISSED. Each case below is
           valid in every respect except the field it is testing. */
        {
            /* A base that mounts, so each case differs in one field.
               FAT at 128, 64 long; heap at 256; 64 clusters of 8
               sectors; root at cluster 2; volume long enough. */
            unsigned char base[512];
            memset(base, 0, sizeof base);
            memcpy(base + 3, "EXFAT   ", 8);
            base[105] = 1; base[108] = 9; base[109] = 3;
            base[72] = 0x00; base[73] = 0x10;      /* vol_length 4096 */
            base[80] = 128;                        /* fat_offset      */
            base[84] = 64;                         /* fat_length      */
            base[88] = 0x00; base[89] = 0x01;      /* heap at 256     */
            base[92] = 64;                         /* cluster_count   */
            base[96] = 2;                          /* root cluster    */

            memcpy(fake, base, 512);
            ck("the base boot sector is otherwise acceptable",
               exfat_mount_dev(fake_read, 0, 0) == 1, exfat_why());

            /* ONLY the heap offset is wrong: it starts inside the FAT. */
            memcpy(fake, base, 512);
            fake[88] = 100; fake[89] = 0;          /* heap at 100 < 192 */
            ck("a heap that starts inside the FAT is refused",
               exfat_mount_dev(fake_read, 0, 0) == 0, 0);
            ck("  ... as a geometry problem",
               exfat_why() == EXFAT_EGEOMETRY, exfat_why());

            /* ONLY the root cluster is wrong: past the cluster count.
               clust_lba() subtracts 2 and multiplies, so this is the
               difference between a refusal and a read at a wild
               offset on somebody's removable media. */
            memcpy(fake, base, 512);
            fake[96] = 200;                        /* > 64 clusters    */
            ck("a root cluster past the end of the heap is refused",
               exfat_mount_dev(fake_read, 0, 0) == 0, 0);
            ck("  ... as a geometry problem",
               exfat_why() == EXFAT_EGEOMETRY, exfat_why());
            memcpy(fake, base, 512);
            fake[96] = 1;                          /* below the first  */
            ck("a root cluster below the first data cluster is refused",
               exfat_mount_dev(fake_read, 0, 0) == 0, 0);

            /* ONLY the volume length is wrong: shorter than the heap
               it claims. */
            memcpy(fake, base, 512);
            fake[72] = 0x10; fake[73] = 0x00;      /* 16 sectors       */
            ck("a volume shorter than its own heap is refused",
               exfat_mount_dev(fake_read, 0, 0) == 0, 0);
            ck("  ... as a geometry problem",
               exfat_why() == EXFAT_EGEOMETRY, exfat_why());
        }

        /* A device that does not answer at all. */
        ck("a null read function is refused",
           exfat_mount_dev(0, 0, 0) == 0, 0);
        ck("  ... as I/O", exfat_why() == EXFAT_EIO, exfat_why());

        /* ... and every refusal has a distinct name. */
        ck("the refusal names are all different",
           strcmp(exfat_why_name(EXFAT_ENOTEXFAT),
                  exfat_why_name(EXFAT_EREVISION)) &&
           strcmp(exfat_why_name(EXFAT_EREVISION),
                  exfat_why_name(EXFAT_ESECTORSIZE)) &&
           strcmp(exfat_why_name(EXFAT_ESECTORSIZE),
                  exfat_why_name(EXFAT_EGEOMETRY)), 0);
        ck("  ... and none of them says 'unknown'",
           strcmp(exfat_why_name(EXFAT_ENOTEXFAT), "unknown") &&
           strcmp(exfat_why_name(EXFAT_EIO), "unknown"), 0);
        ck("  ... while a code out of range does",
           !strcmp(exfat_why_name(EXFAT_EREASONS), "unknown"), 0);
    }

    /* Back to the real volume for everything below. */
    ck("the real volume mounts again",
       exfat_mount_dev(img_read, 0, 0) == 1, exfat_why());

    /* ════════════════════════════════════════════════════════════════
       3. THE ROOT DIRECTORY
       ════════════════════════════════════════════════════════════════ */
    head("the root directory holds what was written to it");
    {
        exfat_dir_t d;
        exfat_dirent_t e;
        int n = 0, saw_readme = 0, saw_big = 0, saw_docs = 0,
            saw_sparse = 0, saw_frag = 0;
        ck("the root opens", exfat_opendir("/", &d) == EXFAT_OK, 0);
        while(exfat_readdir(&d, &e) == 1){
            n++;
            if(!strcmp(e.name, "README.TXT")){
                saw_readme = 1;
                ck("README.TXT has the size it was written with",
                   e.size == fixture("README.TXT")->size, (long)e.size);
                ck("  ... and is not a directory", e.is_dir == 0, e.is_dir);
            }
            if(!strcmp(e.name, "BIG.BIN")){
                saw_big = 1;
                ck("BIG.BIN has the size it was written with",
                   e.size == fixture("BIG.BIN")->size, (long)e.size);
            }
            if(!strcmp(e.name, "SPARSE.DAT")) saw_sparse = 1;
            if(!strcmp(e.name, "FRAG.BIN"))   saw_frag = 1;
            if(!strcmp(e.name, "DOCS")){
                saw_docs = 1;
                ck("DOCS is a directory", e.is_dir == 1, e.is_dir);
            }
        }
        ck("every root entry was listed", saw_readme && saw_big &&
           saw_docs && saw_sparse && saw_frag, n);
        /* The count is checked too: a reader that stops at the first
           deleted or unknown entry type lists a prefix and looks fine.
           And the fixture DELETES six filler files to make FRAG.BIN
           fragmented, so this also says the deleted sets were skipped
           rather than listed. */
        ck("  ... and there are exactly six of them", n == 6, n);
        ck("  ... with nothing read past the end of the image",
           g_refused == 0, g_refused);
    }

    /* ════════════════════════════════════════════════════════════════
       4. BOTH CLUSTER WALKS, AND THE FIXTURE REALLY HAS BOTH
       ════════════════════════════════════════════════════════════════ */
    head("the fixture contains a file of each allocation form");
    {
        exfat_dir_t d;
        exfat_dirent_t e;
        int chained = 0, contig = 0;
        exfat_opendir("/", &d);
        while(exfat_readdir(&d, &e) == 1){
            if(e.is_dir) continue;
            if(e.no_fat_chain) contig++; else chained++;
        }
        printf("    --   %d contiguous, %d FAT-chained\n", contig, chained);
        /* BOTH, and this is a hard requirement rather than a note.
           The first version of this fixture produced "3 contiguous, 0
           FAT-chained" and the check here said "at least one is
           contiguous" -- so the FAT chain walk, half the driver, was
           never executed and 83 checks passed anyway. The generator
           fragments a file on purpose now; if that ever stops working
           this says so instead of quietly testing one walk twice. */
        ck("the fixture has a CONTIGUOUS file", contig > 0, contig);
        ck("the fixture has a FAT-CHAINED file", chained > 0, chained);
    }

    head("the fragmented file reads back byte for byte");
    {
        /* The FAT chain walk, which is the half a fresh volume cannot
           exercise.

           WHICH file ends up fragmented is the allocator's choice, not
           ours: the generator punches holes and writes several files,
           and the FUSE driver decides what lands where. The first
           version of this block named FRAG.BIN and asserted it --
           and FRAG.BIN came out contiguous while SPARSE.DAT took the
           holes, so the check failed on a volume that was exactly
           right. Ask the volume which one it is. */
        exfat_dir_t d;
        exfat_dirent_t e, chained;
        int found = 0;
        memset(&chained, 0, sizeof chained);
        exfat_opendir("/", &d);
        while(exfat_readdir(&d, &e) == 1){
            if(!e.is_dir && !e.no_fat_chain){ chained = e; found = 1; break; }
        }
        ck("a FAT-chained file was found to test", found, found);
        if(found){
            const exfix_file_t *f = fixture(chained.name);
            printf("    --   the chained file is %s (%lu bytes)\n",
                   chained.name, (unsigned long)chained.size);
            ck("  it is one of the files the fixture wrote", f != 0, 0);
            if(f){
                unsigned char *buf = malloc((size_t)f->size);
                int r = exfat_read(chained.name, 0, buf, (uint32_t)f->size);
                ck("  it reads back whole", r == (int)f->size, r);
                ck("  and the crc agrees, so the chain was followed",
                   r > 0 &&
                   (unsigned long)crc32x(buf, (unsigned long)r) == f->crc,
                   r > 0 ? (long)crc32x(buf, (unsigned long)r) : -1);
                /* It really spans enough clusters for a chain to be a
                   chain rather than two links. */
                ck("  over many clusters",
                   f->size / exfat_cluster_bytes() > 50,
                   (long)(f->size / exfat_cluster_bytes()));
                /* And from the middle, which is where a chain walk
                   that counts clusters wrong goes wrong. */
                if(r == (int)f->size && f->size > 1003000){
                    unsigned char part[3000];
                    int n = exfat_read(chained.name, 1000003, part,
                                       sizeof part);
                    ck("  and from an awkward offset inside it",
                       n == (int)sizeof part, n);
                    ck("  with the same bytes",
                       n > 0 && memcmp(part, buf + 1000003,
                                       (size_t)n) == 0, 0);
                }
                free(buf);
            }
        }
    }

    /* ════════════════════════════════════════════════════════════════
       THREE THINGS A FRESHLY FORMATTED VOLUME CANNOT TEST
       All three were found by tools/check_exfat_suite.py: it deleted
       the code and 83 checks passed anyway, because the fixture had
       nothing that would notice.
       ════════════════════════════════════════════════════════════════ */
    head("a directory bigger than one cluster");
    {
        /* A cluster here is 4096 bytes = 128 entries, and a file with
           an ordinary name takes three. So MANY/ spans several
           clusters and listing it needs the directory's own FAT
           chain to be followed -- code that never ran while every
           directory in the fixture fit in one cluster. */
        exfat_dir_t d;
        exfat_dirent_t e;
        int n = 0;
        ck("MANY/ is a directory that needs more than one cluster",
           (uint64_t)EXFIX_MANY_N * 3u * 32u > exfat_cluster_bytes(),
           (long)EXFIX_MANY_N);
        ck("it opens", exfat_opendir(EXFIX_MANY_DIR, &d) == EXFAT_OK, 0);
        while(exfat_readdir(&d, &e) == 1) n++;
        ck("  ... and every one of its entries is listed",
           n == EXFIX_MANY_N, n);
        /* And a file at the far end of it is reachable, which the
           count alone does not prove -- a reader could list names
           from a chain it then cannot address. */
        {
            char path[128];
            unsigned char buf[32];
            int r;
            snprintf(path, sizeof path, "%s/entry-%03d.txt",
                     EXFIX_MANY_DIR, EXFIX_MANY_N - 1);
            r = exfat_read(path, 0, buf, sizeof buf);
            ck("  ... and the last one reads back", r > 0, r);
        }
    }

    head("an entry set that was deleted stays deleted");
    {
        /* The generator writes ZOMBIE.TMP last and removes it, so its
           entries are still in the root with the in-use bit cleared.
           A reader that ignores that bit lists a file whose clusters
           now belong to something else. */
        exfat_dir_t d;
        exfat_dirent_t e;
        int saw = 0;
        exfat_opendir("/", &d);
        while(exfat_readdir(&d, &e) == 1)
            if(!strcmp(e.name, EXFIX_ZOMBIE)) saw = 1;
        ck("the deleted file is not listed", saw == 0, saw);
        ck("  ... nor found by name",
           exfat_stat(EXFIX_ZOMBIE, &e) == -EXFAT_ENOTFOUND, 0);
    }

    head("listing stops at the end marker rather than scanning the cluster");
    {
        /* COUNT THE WIRE. A zero type byte means there are no more
           entries in this directory; treating it as "skip" produces
           exactly the same list and reads the whole cluster to do it.
           Nothing about the OUTCOME can tell the two apart, so the
           check is the number of sectors the device was asked for.

           DOCS holds three files, so its entries occupy well under
           one 512-byte sector; a reader that stops reads one sector
           of it, and one that scans reads all eight of the cluster. */
        exfat_dir_t d;
        exfat_dirent_t e;
        long before, used;
        /* The count must cover the LISTING and nothing else.
           exfat_opendir() walks the root to find DOCS, so counting
           from before it measured the path lookup as well and came
           to twenty -- more than a cluster, which made the check
           fail on a correct reader. */
        exfat_opendir("DOCS", &d);
        before = g_reads;
        while(exfat_readdir(&d, &e) == 1) { }
        used = g_reads - before;
        printf("    --   listing DOCS cost %ld sector reads\n", used);
        ck("it did not read the whole cluster to list three files",
           used < (long)(exfat_cluster_bytes() / 512), used);
    }

    head("a file reads back byte for byte");
    {
        unsigned char buf[4096];
        const exfix_file_t *f = fixture("README.TXT");
        int r = exfat_read("README.TXT", 0, buf, (uint32_t)sizeof buf);
        ck("the whole file came back", r == (int)f->size, r);
        ck("  ... and the bytes are the bytes",
           r > 0 && memcmp(buf, f->body, f->body_len) == 0, 0);
        ck("  ... and the crc agrees",
           r > 0 && (unsigned long)crc32x(buf, (unsigned long)r) == f->crc,
           r > 0 ? (long)crc32x(buf, (unsigned long)r) : -1);
    }

    head("a file that crosses clusters reads back byte for byte");
    {
        const exfix_file_t *f = fixture("BIG.BIN");
        unsigned char *buf = malloc((size_t)f->size);
        int r = exfat_read("BIG.BIN", 0, buf, (uint32_t)f->size);
        ck("100000 bytes came back", r == (int)f->size, r);
        ck("  ... and the crc agrees -- the cluster walk is right",
           r > 0 && (unsigned long)crc32x(buf, (unsigned long)r) == f->crc,
           r > 0 ? (long)crc32x(buf, (unsigned long)r) : -1);
        /* It really spans clusters, or this proves nothing. */
        ck("  ... and it really is more than one cluster",
           f->size > exfat_cluster_bytes(), (long)f->size);
        free(buf);
    }

    head("a long file reads back byte for byte");
    {
        const exfix_file_t *f = fixture("SPARSE.DAT");
        unsigned char *buf = malloc((size_t)f->size);
        int r = exfat_read("SPARSE.DAT", 0, buf, (uint32_t)f->size);
        ck("3 MB came back", r == (int)f->size, r);
        ck("  ... and the crc agrees",
           r > 0 && (unsigned long)crc32x(buf, (unsigned long)r) == f->crc,
           0);
        ck("  ... across many clusters",
           f->size / exfat_cluster_bytes() > 100,
           (long)(f->size / exfat_cluster_bytes()));
        free(buf);
    }

    /* ════════════════════════════════════════════════════════════════
       5. PARTIAL AND OFF-ALIGNMENT READS
       The arithmetic nobody gets right first time: an offset that is
       not a cluster boundary, not a sector boundary, and a length
       that runs off the end.
       ════════════════════════════════════════════════════════════════ */
    head("reading from the middle, off every alignment");
    {
        const exfix_file_t *f = fixture("BIG.BIN");
        unsigned char *whole = malloc((size_t)f->size);
        unsigned char part[5000];
        int r = exfat_read("BIG.BIN", 0, whole, (uint32_t)f->size);
        ck("the reference read worked", r == (int)f->size, r);

        struct { uint64_t off; uint32_t len; const char *what; } cases[] = {
            {    1,  100, "one byte in"                        },
            {  511,    2, "across a sector boundary"           },
            {  512,  100, "exactly on a sector boundary"       },
            { 4095,    2, "across a cluster boundary"          },
            { 4096,  100, "exactly on a cluster boundary"      },
            { 1234, 4321, "neither end on any boundary"        },
        };
        for(unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++){
            int n = exfat_read("BIG.BIN", cases[i].off, part,
                               cases[i].len);
            ck(cases[i].what, n == (int)cases[i].len, n);
            if(n == (int)cases[i].len)
                ck("  ... and the bytes match the whole-file read",
                   memcmp(part, whole + cases[i].off,
                          (size_t)cases[i].len) == 0, 0);
        }

        /* Past the end is 0 and is NOT a failure -- a caller reading
           to EOF in a loop must be able to tell "nothing left" from
           "something broke". */
        ck("a read starting past the end returns 0",
           exfat_read("BIG.BIN", f->size, part, 10) == 0, 0);
        ck("  ... and one that overruns is shortened, not refused",
           exfat_read("BIG.BIN", f->size - 10, part, 1000) == 10, 0);
        free(whole);
    }

    /* ════════════════════════════════════════════════════════════════
       6. SUBDIRECTORIES AND A NAME THAT NEEDS THREE ENTRIES
       ════════════════════════════════════════════════════════════════ */
    head("a subdirectory, and one two levels down");
    {
        exfat_dirent_t e;
        unsigned char buf[512];
        const exfix_file_t *f = fixture("DOCS/NOTES.TXT");
        ck("DOCS/NOTES.TXT is found",
           exfat_stat("DOCS/NOTES.TXT", &e) == EXFAT_OK, exfat_why());
        ck("  ... with the right size", e.size == f->size, (long)e.size);
        int r = exfat_read("DOCS/NOTES.TXT", 0, buf, sizeof buf);
        ck("  ... and reads back", r == (int)f->size, r);
        ck("  ... byte for byte",
           r > 0 && memcmp(buf, f->body, f->body_len) == 0, 0);

        f = fixture("DOCS/DEEP/NESTED.TXT");
        r = exfat_read("DOCS/DEEP/NESTED.TXT", 0, buf, sizeof buf);
        ck("two levels down reads back", r == (int)f->size, r);
        ck("  ... byte for byte",
           r > 0 && memcmp(buf, f->body, f->body_len) == 0, 0);
        /* A leading slash is the same path. */
        ck("a leading slash is the same path",
           exfat_stat("/DOCS/DEEP/NESTED.TXT", &e) == EXFAT_OK, 0);
    }

    head("a name longer than one 0xC1 entry");
    {
        /* 15 UTF-16 characters per name entry. A reader that takes the
           first one and stops gets the first fifteen characters of
           every long name, which matches nothing -- and reads as
           "file not found" rather than as a bug. */
        exfat_dirent_t e;
        char path[320];
        ck("the fixture's long name really needs more than one entry",
           strlen(EXFIX_LONGNAME) > 15, (long)strlen(EXFIX_LONGNAME));
        ck("  ... in fact more than two",
           strlen(EXFIX_LONGNAME) > 30, (long)strlen(EXFIX_LONGNAME));
        snprintf(path, sizeof path, "DOCS/%s", EXFIX_LONGNAME);
        ck("it is found by its whole name",
           exfat_stat(path, &e) == EXFAT_OK, exfat_why());
        ck("  ... and the name came back complete",
           !strcmp(e.name, EXFIX_LONGNAME), (long)strlen(e.name));
        /* And a prefix of it must NOT match, which is what a reader
           that stopped at fifteen characters would do. */
        {
            char cut[64];
            snprintf(cut, sizeof cut, "DOCS/%.15s", EXFIX_LONGNAME);
            ck("  ... while its first fifteen characters match nothing",
               exfat_stat(cut, &e) == -EXFAT_ENOTFOUND, 0);
        }
    }

    head("names are matched without regard to case");
    {
        exfat_dirent_t e;
        ck("readme.txt finds README.TXT",
           exfat_stat("readme.txt", &e) == EXFAT_OK, 0);
        ck("docs/notes.txt finds DOCS/NOTES.TXT",
           exfat_stat("docs/notes.txt", &e) == EXFAT_OK, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       7. WHAT IT REFUSES, AND THE REFUSALS ARE DISTINCT
       ════════════════════════════════════════════════════════════════ */
    head("the things it will not do, each named");
    {
        exfat_dirent_t e;
        unsigned char buf[64];
        exfat_dir_t d;
        ck("a file that is not there",
           exfat_stat("NOSUCH.TXT", &e) == -EXFAT_ENOTFOUND, 0);
        ck("  ... and in a subdirectory that is not there",
           exfat_stat("NOPE/NOSUCH.TXT", &e) == -EXFAT_ENOTFOUND, 0);
        ck("reading a directory is EISDIR, not ENOTFOUND",
           exfat_read("DOCS", 0, buf, sizeof buf) == -EXFAT_EISDIR, 0);
        ck("walking through a FILE as if it were a directory is ENOTDIR",
           exfat_stat("README.TXT/x", &e) == -EXFAT_ENOTDIR, 0);
        ck("opendir on a file is ENOTDIR",
           exfat_opendir("README.TXT", &d) == -EXFAT_ENOTDIR, 0);

        exfat_unmount();
        ck("unmounted, it says so", exfat_mounted() == 0, 0);
        ck("  ... and every call refuses with ENOTMOUNTED",
           exfat_stat("README.TXT", &e) == -EXFAT_ENOTMOUNTED &&
           exfat_read("README.TXT", 0, buf, 1) == -EXFAT_ENOTMOUNTED &&
           exfat_opendir("/", &d) == -EXFAT_ENOTMOUNTED, 0);
    }

    /* ════════════════════════════════════════════════════════════════
       8. A VOLUME AT AN OFFSET
       A real stick is a partition, not a whole disk. The driver takes
       a partition LBA and every address is relative to it; passing
       zero where it should be 2048 reads the MBR as a boot sector,
       which is a different failure from not supporting exFAT.
       ════════════════════════════════════════════════════════════════ */
    head("the volume is addressed from its partition start");
    {
        long before = g_refused;
        ck("mounting at the wrong offset refuses",
           exfat_mount_dev(img_read, 0, 2048) == 0, 0);
        ck("  ... as 'not an exFAT volume', since that is what it read",
           exfat_why() == EXFAT_ENOTEXFAT, exfat_why());
        ck("  ... without running off the end of the device",
           g_refused == before, g_refused - before);
        ck("and at the right one it mounts",
           exfat_mount_dev(img_read, 0, 0) == 1, exfat_why());
    }

    printf("\n%d/%d passed\n", total - fails, total);
    fclose(g_img);
    return fails ? 1 : 0;
}

/* The one-sector device the refusal tests use. At file scope because
   it has to outlive the block that sets it up. */
uint8_t *g_fake;
int fake_read(void *ctx, uint64_t lba, uint8_t *buf)
{
    (void)ctx;
    if(lba != 0 || !g_fake) return 0;
    memcpy(buf, g_fake, 512);
    return 1;
}
