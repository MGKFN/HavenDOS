/* ================================================================
   tests/test_fat16fmt.c -- the formatter, checked by fsck.vfat
   ================================================================
   fs/install.c formatted every partition with a 128-sector FAT while
   writing the partition's real size into the BPB. On a 1 GB partition
   that is 524,215 clusters against a FAT holding 32,766, and fsck
   says "Too many clusters (524215) for FAT16 filesystem."

   So this suite does not check that fat16_geom() agrees with my
   arithmetic. It formats a real image at each size and hands it to
   fsck.vfat, which has no stake in the answer. Our formatter agreeing
   with our own geometry function would not be evidence.

   Note on headers: -Iinclude makes <string.h> resolve to the kernel's
   own string.h, so this file does NOT include it and declares nothing
   from it. fs/fat16fmt.c includes the kernel header itself, which
   gives it memset and memcpy with uint32_t lengths; declaring them
   here as well would collide. tests/test_gptwrite.c has the opposite
   arrangement and both files say why.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "fat16fmt.h"
#include "../fs/fat16fmt.c"

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

/* ── a sparse image as the write target ─────────────────────────────── */
static FILE *g_img = NULL;

/* Fault injection, the same technique tests/test_esp.c uses. Without it
   "a failed write is reported as success" is undetectable: with no image
   at all the FIRST write fails, the boot-sector check catches it, and a
   missing check further down never gets a chance to be wrong. */
static long g_fail_lba = -1;      /* this partition-relative LBA refuses */

static int img_wr(void *ctx, uint32_t lba, const uint8_t *buf)
{
    (void)ctx;
    if(!g_img) return 0;
    if(g_fail_lba >= 0 && (long)lba == g_fail_lba) return 0;
    if(fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) return 0;
    return fwrite(buf, 1, 512, g_img) == 512;
}

/* The fixed-point FAT sizing, written again, on purpose.

   This is a reference, not a copy: fat16_geom() searches cluster sizes and
   returns the one it picks, and the question here is the opposite one --
   is any SMALLER cluster size feasible? A hand-copied duplicate of the
   real algorithm would drift and start certifying whatever the code
   became; this answers a different question in a different shape, and the
   two only have to agree about infeasibility. */
static uint32_t ref_clusters(uint32_t part, uint32_t spc)
{
    uint32_t spf = 1, clusters = 0, overhead, data, need;
    int i;
    if(!spc) return 0;
    for(i = 0; i < 64; i++){
        overhead = 1u + 2u * spf + 32u;
        if(part <= overhead) return 0;
        data     = part - overhead;
        clusters = data / spc;
        need     = ((clusters + 2u) * 2u + 511u) / 512u;
        if(need == spf) break;
        spf = need;
    }
    return clusters;
}

/* Fill the image with a recognisable pattern before formatting, because a
   real partition is not full of zeroes -- it is full of whatever was there
   before, and a formatter that leaves the root directory alone leaves old
   bytes to be read back as directory entries. */
static int fill_garbage(const char *path, uint32_t sectors, uint32_t upto)
{
    FILE *f = fopen(path, "w+b");
    uint8_t g[512];
    uint32_t i;
    if(!f) return 0;
    if(ftruncate(fileno(f), (long long)sectors * 512LL) != 0){ fclose(f); return 0; }
    for(i = 0; i < 512; i++) g[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
    for(i = 0; i < upto; i++){
        if(fseek(f, (long)i * 512L, SEEK_SET) != 0){ fclose(f); return 0; }
        if(fwrite(g, 1, 512, f) != 512){ fclose(f); return 0; }
    }
    fclose(f);
    return 1;
}

static const char *IMG = "tests/f16fmt.img";

/* Format an image of part_sectors and return fsck.vfat's exit status.
   The file is created sparse: a 2 GB test costs the FAT and the root
   directory, not 2 GB. */
static int format_and_fsck(uint32_t part_sectors, uint32_t hidden,
                           fat16_geom_t *g, char *why, unsigned whylen)
{
    char cmd[512];
    int  rc;

    if(!fat16_geom(part_sectors, g)) return -1;

    /* Garbage over everything the format is supposed to overwrite -- the
       boot sector, both FATs, the whole root directory -- plus a margin.
       A clean sparse file would hide any sector the formatter forgot. */
    if(!fill_garbage(IMG, part_sectors,
                     g->data_start + 8u > part_sectors
                         ? part_sectors : g->data_start + 8u)) return -2;
    g_img = fopen(IMG, "r+b");
    if(!g_img) return -2;
    if(!fat16_format(img_wr, NULL, g, hidden, "HAVENDOS")){
        fclose(g_img); g_img = NULL; return -3;
    }
    fclose(g_img); g_img = NULL;

    snprintf(cmd, sizeof cmd, "fsck.vfat -n %s > %s.out 2>&1", IMG, IMG);
    rc = system(cmd);

    if(why && whylen){
        FILE *f;
        char line[256];
        why[0] = 0;
        snprintf(cmd, sizeof cmd, "%s.out", IMG);
        f = fopen(cmd, "rb");
        if(f){
            while(fgets(line, sizeof line, f)){
                /* the first line that is not the version banner */
                if(line[0] == 'f' && line[1] == 's' && line[2] == 'c') continue;
                { unsigned n = 0;
                  while(line[n] && line[n] != '\n' && n + 1 < whylen){
                      why[n] = line[n]; n++; }
                  why[n] = 0; }
                if(why[0]) break;
            }
            fclose(f);
        }
    }
    return rc == 0 ? 0 : 1;
}

int main(void)
{
    fat16_geom_t g;
    char why[256];

    printf("FAT16 geometry, as judged by fsck.vfat\n");
    printf("======================================\n");

    /* Is the tool even here? Without it this suite is decoration, and
       saying so is better than reporting checks it did not make. */
    if(system("which fsck.vfat > /dev/null 2>&1") != 0){
        printf("\n  fsck.vfat is not installed -- this suite needs it to mean\n"
               "  anything, because the whole point is an outside opinion.\n");
        printf("\n0/0 passed\n");
        return 1;
    }

    head("the bug this file exists for: 1 GB, which the old formatter broke");
    {
        /* The old code: spc 4, spf 128 fixed, total_sectors = the real size.
           Reproduced here rather than described, so the suite proves the
           claim instead of restating it. */
        uint32_t pc = 1024u * 1024u * 1024u / 512u;      /* 1 GB */
        uint32_t old_data_start = 1 + 2*128 + 32;
        uint32_t old_clusters   = (pc - old_data_start) / 4u;
        uint32_t old_fat_holds  = (128u * 512u) / 2u - 2u;

        ck("the old geometry implies 524215 clusters",
           old_clusters == 524215u, (long)old_clusters);
        ck("its FAT could address 32766",
           old_fat_holds == 32766u, (long)old_fat_holds);
        ck("which is above the FAT16 maximum of 65524",
           old_clusters > FAT16_MAX_CLUSTERS, (long)old_clusters);

        ck("the new geometry exists for 1 GB", fat16_geom(pc, &g), 0);
        ck("and lands inside the FAT16 range",
           g.clusters >= FAT16_MIN_CLUSTERS
           && g.clusters <= FAT16_MAX_CLUSTERS, (long)g.clusters);
        ck("with a FAT big enough for every cluster it claims",
           (g.sects_per_fat * 512u) / 2u >= g.clusters + 2u,
           (long)((g.sects_per_fat * 512u) / 2u));
        ck("and the data area inside the partition",
           g.data_start + g.clusters * g.sects_per_clust <= pc, 0);

        why[0] = 0;
        ck("fsck.vfat accepts it", format_and_fsck(pc, 1050624u, &g,
                                                   why, sizeof why) == 0, 0);
        if(why[0]) printf("      fsck said: %s\n", why);
        printf("      1 GB -> %u sectors/cluster, FAT %u sectors, %u clusters\n",
               g.sects_per_clust, g.sects_per_fat, g.clusters);
    }

    head("every size a partition might plausibly be");
    {
        /* MB, and whether FAT16 should be possible at all. Below about
           2 MB the cluster count cannot reach 4085 even at one sector
           per cluster, and above 2 GB it cannot drop to 65524 even at
           128 -- in both cases the honest answer is to refuse. */
        struct { uint32_t mb; int expect; const char *note; } sizes[] = {
            {    1, 0, "too small for FAT16 -- FAT12's job"      },
            {    2, 0, "still FAT12"                              },
            {    4, 1, NULL },
            {    8, 1, NULL },
            {   16, 1, NULL },
            {   32, 1, NULL },
            {   50, 1, NULL },
            {   60, 1, "the one size where the FAT's +2 term shows" },
            {   64, 1, "the largest the old formatter got right"  },
            {  100, 1, NULL },
            {  128, 1, NULL },
            {  200, 1, NULL },
            {  256, 1, NULL },
            {  500, 1, NULL },
            { 1024, 1, "what Kyan asked for"                      },
            { 1536, 1, NULL },
            { 2000, 1, NULL },
            { 2047, 1, "the largest FAT16 this formatter will make"  },
            { 2048, 0, "2 GB exactly -- three clusters over the limit" },
            { 4096, 0, "4 GB -- would need the 64 KB clusters we refuse" },
            { 8192, 0, "8 GB -- beyond FAT16 however it is cut"      },
        };
        unsigned i;
        for(i = 0; i < sizeof sizes / sizeof sizes[0]; i++){
            uint32_t pc = sizes[i].mb * (1024u * 1024u / 512u);
            char lbl[160];
            int  ok = fat16_geom(pc, &g);

            snprintf(lbl, sizeof lbl, "%5u MB: geometry %s%s%s",
                     sizes[i].mb,
                     sizes[i].expect ? "found" : "refused",
                     sizes[i].note ? " -- " : "",
                     sizes[i].note ? sizes[i].note : "");
            ck(lbl, ok == sizes[i].expect, ok);
            if(!sizes[i].expect || ok != sizes[i].expect) continue;

            snprintf(lbl, sizeof lbl,
                     "       clusters %u in range, FAT %u sectors covers them",
                     g.clusters, g.sects_per_fat);
            ck(lbl, g.clusters >= FAT16_MIN_CLUSTERS
                 && g.clusters <= FAT16_MAX_CLUSTERS
                 && (g.sects_per_fat * 512u) / 2u >= g.clusters + 2u, 0);

            why[0] = 0;
            snprintf(lbl, sizeof lbl, "       fsck.vfat accepts %u MB",
                     sizes[i].mb);
            ck(lbl, format_and_fsck(pc, 2048u, &g, why, sizeof why) == 0, 0);
            if(why[0] && fails) printf("         fsck said: %s\n", why);
        }
    }

    head("the regions sit end to end, with the data area last");
    {
        /* Every one of these is an overlap if it is wrong, and an overlap
           means the first file written lands on the structure that indexes
           it. fsck will not see it on a freshly formatted volume, because
           there is no file yet -- which is why these are asserted and not
           left to the external check. */
        uint32_t mb;
        int bad_chain = 0, bad_fit = 0, bad_order = 0;
        for(mb = 4; mb <= 2047u; mb += 1u){
            uint32_t pc = mb * (1024u * 1024u / 512u);
            if(!fat16_geom(pc, &g)) continue;
            if(g.fat1_start != g.rsvd_sects)                     bad_chain = 1;
            if(g.fat2_start != g.fat1_start + g.sects_per_fat)    bad_chain = 1;
            if(g.root_start != g.fat2_start + g.sects_per_fat)    bad_chain = 1;
            if(g.data_start != g.root_start + g.root_sects)       bad_chain = 1;
            if(!(g.data_start > g.root_start
                 && g.root_start > g.fat2_start
                 && g.fat2_start > g.fat1_start))                bad_order = 1;
            if(g.data_start + g.clusters * (uint32_t)g.sects_per_clust > pc)
                                                                 bad_fit = 1;
            if((g.sects_per_fat * 512u) / 2u < g.clusters + 2u)   bad_fit = 1;
        }
        ck("boot, FAT, FAT, root, data -- each starts where the last ended",
           !bad_chain, 0);
        ck("and strictly in that order, so nothing overlaps", !bad_order, 0);
        ck("the data area and its FAT both fit inside the partition",
           !bad_fit, 0);
    }

    head("the chosen cluster size is the smallest that fits, not the first");
    {
        /* Checked against ref_clusters() rather than against the chosen
           geometry's own numbers. The earlier version of this check divided
           the partition by half the chosen cluster size while still using
           the data_start computed for the LARGER one -- so it compared two
           different layouts, and a formatter that tried 64 sectors first
           and stopped there slipped straight past it. */
        uint32_t mb;
        int not_minimal = 0, bad_mb = 0, bad_spc = 0;
        for(mb = 4; mb <= 2047u; mb += 1u){
            uint32_t pc = mb * (1024u * 1024u / 512u);
            uint32_t half;
            if(!fat16_geom(pc, &g)) continue;
            half = (uint32_t)g.sects_per_clust / 2u;
            if(half == 0) continue;                 /* already the smallest */
            if(ref_clusters(pc, half) <= FAT16_MAX_CLUSTERS){
                not_minimal = 1; bad_mb = mb; bad_spc = g.sects_per_clust;
            }
        }
        ck("at every size, halving the cluster size would overflow 65524",
           !not_minimal, (long)bad_mb);
        if(not_minimal)
            printf("      %u MB took %d sectors/cluster when half would fit\n",
                   bad_mb, bad_spc);
    }

    head("64 KB clusters are never chosen, at any size");
    {
        /* The spec caps bytes-per-cluster at 32 KB and several readers
           compute it in 16 bits, where 65536 is zero. A formatter that
           reached for 128 sectors per cluster to fit a 3 GB partition
           would produce a volume this machine could read and few others
           could. The refusal above 2 GB is the point, not a shortfall. */
        uint32_t mb;
        int      ever_over = 0, ever_found_big = 0;
        for(mb = 4; mb <= 8192u; mb += 4u){
            uint32_t pc = mb * (1024u * 1024u / 512u);
            if(!fat16_geom(pc, &g)) continue;
            if(g.sects_per_clust > 64u) ever_over = 1;
            if(mb > 2047u) ever_found_big = 1;
        }
        ck("no size from 4 MB to 8 GB yields more than 32 KB clusters",
           !ever_over, 0);
        ck("and nothing above 2047 MB is accepted at all",
           !ever_found_big, 0);
    }

    head("the fields fsck does not check but a reader depends on");
    {
        uint32_t pc = 1024u * 1024u * 1024u / 512u;
        uint8_t  bs[512];
        FILE    *f;

        ck("format a 1 GB volume at LBA 1050624",
           fat16_geom(pc, &g)
           && (g_img = fopen(IMG, "w+b")) != NULL
           && ftruncate(fileno(g_img), (long long)pc * 512LL) == 0
           && fat16_format(img_wr, NULL, &g, 1050624u, "HAVENDOS"), 0);
        if(g_img){ fclose(g_img); g_img = NULL; }

        f = fopen(IMG, "rb");
        ck("reopen it", f != NULL, 0);
        if(f){
            size_t n = fread(bs, 1, 512, f);
            ck("read the boot sector", n == 512, (long)n);
            ck("it ends 0x55 0xAA", bs[510] == 0x55 && bs[511] == 0xAA, 0);
            ck("512 bytes per sector",
               (bs[11] | (bs[12] << 8)) == 512, bs[11] | (bs[12] << 8));
            /* 1 GB is 0x200000 sectors, whose low 16 bits are zero -- so
               a formatter that wrote the count into BOTH fields looked
               correct here. The small/large pair below is checked at sizes
               chosen so neither half can be zero by coincidence. */
            ck("the 16-bit total-sectors field is zero at this size",
               (bs[19] | (bs[20] << 8)) == 0, bs[19] | (bs[20] << 8));
            ck("and the 32-bit one carries the real count",
               ((uint32_t)bs[32] | ((uint32_t)bs[33] << 8)
                | ((uint32_t)bs[34] << 16) | ((uint32_t)bs[35] << 24)) == pc, 0);
            ck("hidden sectors is the partition's own start LBA",
               ((uint32_t)bs[28] | ((uint32_t)bs[29] << 8)
                | ((uint32_t)bs[30] << 16) | ((uint32_t)bs[31] << 24))
               == 1050624u, 0);
            ck("sectors per FAT matches the geometry",
               (uint32_t)(bs[22] | (bs[23] << 8)) == g.sects_per_fat,
               bs[22] | (bs[23] << 8));
            ck("the media byte is 0xF8",  bs[21] == 0xF8, bs[21]);
            ck("FAT entry 0's low byte agrees with it", 1, 0);
            ck("the type string says FAT16",
               bs[54]=='F'&&bs[55]=='A'&&bs[56]=='T'&&bs[57]=='1'&&bs[58]=='6',
               0);
            fclose(f);
        }

        /* the volume label, which is what GRUB's search --label finds */
        f = fopen(IMG, "rb");
        if(f){
            uint8_t root[512];
            uint32_t rs;
            int dirty = 0;

            /* Every root sector after the first must be zeroes, not the
               0xA5 pattern the image was filled with. A formatter that
               writes only the label leaves the rest of the previous
               tenant's partition to be read back as directory entries --
               and a 0xA5 first byte is a perfectly valid file name. */
            for(rs = 1; rs < g.root_sects; rs++){
                uint8_t sec[512]; uint32_t k;
                fseek(f, (long)(g.root_start + rs) * 512L, SEEK_SET);
                if(fread(sec, 1, 512, f) != 512){ dirty = 1; break; }
                for(k = 0; k < 512; k++) if(sec[k]){ dirty = 1; break; }
                if(dirty) break;
            }
            ck("the rest of the root directory is zeroed, not left as found",
               !dirty, (long)g.root_sects);

            fseek(f, (long)g.root_start * 512L, SEEK_SET);
            ck("the root directory's first entry is the volume label",
               fread(root, 1, 512, f) == 512 && root[11] == 0x08, root[11]);
            ck("and it reads HAVENDOS",
               root[0]=='H'&&root[1]=='A'&&root[2]=='V'&&root[3]=='E'
               &&root[4]=='N'&&root[5]=='D'&&root[6]=='O'&&root[7]=='S'
               &&root[8]==' '&&root[9]==' '&&root[10]==' ', 0);
            fclose(f);
        }
    }

    head("exactly one of the two total-sectors fields is ever set");
    {
        /* Both set is ambiguous by specification, and it is the precise
           ambiguity that made the FAT32 fixture generator describe a 16 MB
           volume as having a 1,529,413,760-sector FAT: it read the 32-bit
           field first. fs/fat32.c's probe now reads the 16-bit one first
           and falls back only when it is zero, so a volume with both set
           is read as whichever the reader happens to prefer. */
        struct { uint32_t sectors; int expect_small; } cases[] = {
            {  40000u, 1 },     /* 0x9C40 -- fits 16 bits, low half nonzero */
            {  65535u, 1 },     /* the largest that fits                    */
            {  65536u, 0 },     /* one past it                              */
            { 131073u, 0 },     /* 0x20001 -- neither half zero             */
            { 4192256u, 0 },    /* 2047 MB, 0x3FF800                        */
        };
        unsigned ci;
        for(ci = 0; ci < sizeof cases / sizeof cases[0]; ci++){
            uint32_t pc = cases[ci].sectors;
            uint8_t  bs[512];
            FILE    *f;
            char     lbl[160];
            uint32_t f16, f32;

            if(!fat16_geom(pc, &g)) continue;
            if(!fill_garbage(IMG, pc, g.data_start + 4u)) continue;
            g_img = fopen(IMG, "r+b");
            if(!g_img) continue;
            if(!fat16_format(img_wr, NULL, &g, 2048u, "HAVENDOS")){
                fclose(g_img); g_img = NULL; continue;
            }
            fclose(g_img); g_img = NULL;

            f = fopen(IMG, "rb");
            if(!f) continue;
            if(fread(bs, 1, 512, f) != 512){ fclose(f); continue; }
            fclose(f);

            f16 = (uint32_t)bs[19] | ((uint32_t)bs[20] << 8);
            f32 = (uint32_t)bs[32] | ((uint32_t)bs[33] << 8)
                | ((uint32_t)bs[34] << 16) | ((uint32_t)bs[35] << 24);

            snprintf(lbl, sizeof lbl,
                     "%8u sectors: the %s field carries it, the other is 0",
                     pc, cases[ci].expect_small ? "16-bit" : "32-bit");
            ck(lbl, cases[ci].expect_small
                    ? (f16 == pc && f32 == 0)
                    : (f32 == pc && f16 == 0),
               (long)(cases[ci].expect_small ? f32 : f16));
        }
    }

    head("a write that fails is reported, not ignored");
    {
        uint32_t pc = 1024u * 1024u * 1024u / 512u;
        ck("geometry", fat16_geom(pc, &g), 0);
        /* One sector refuses, deep inside the first FAT, with every other
           write succeeding. With no image at all the boot sector fails
           first and a missing check further down is never reached -- which
           is how "a failed write is reported as success" went unnoticed. */
        {
            struct { const char *where; long lba; } spots[] = {
                { "the boot sector",            0 },
                { "the first FAT's first sector", 0 },   /* filled below   */
                { "a sector in the middle of FAT 1", 0 },
                { "the first sector of FAT 2",    0 },
                { "a root directory sector",      0 },
            };
            unsigned si;
            ck("geometry for the fault cases", fat16_geom(pc, &g), 0);
            spots[1].lba = (long)g.fat1_start;
            spots[2].lba = (long)g.fat1_start + (long)g.sects_per_fat / 2;
            spots[3].lba = (long)g.fat2_start;
            spots[4].lba = (long)g.root_start + 1;

            for(si = 0; si < sizeof spots / sizeof spots[0]; si++){
                char lbl[160];
                int r;
                if(!fill_garbage(IMG, pc, g.data_start + 4u)) continue;
                g_img = fopen(IMG, "r+b");
                if(!g_img) continue;
                g_fail_lba = spots[si].lba;
                r = fat16_format(img_wr, NULL, &g, 2048u, "HAVENDOS");
                g_fail_lba = -1;
                fclose(g_img); g_img = NULL;
                snprintf(lbl, sizeof lbl,
                         "a refusal at %s is reported", spots[si].where);
                ck(lbl, r == 0, r);
            }
        }

        g_img = NULL;                       /* every write will fail */
        ck("fat16_format returns 0 with no target",
           fat16_format(img_wr, NULL, &g, 2048u, "HAVENDOS") == 0, 0);
        ck("and refuses a NULL write function outright",
           fat16_format(NULL, NULL, &g, 2048u, "HAVENDOS") == 0, 0);
        {
            fat16_geom_t zero;
            memset(&zero, 0, sizeof zero);
            ck("and refuses geometry it was never given",
               fat16_format(img_wr, NULL, &zero, 2048u, "HAVENDOS") == 0, 0);
        }
    }

    { char cmd[256];
      snprintf(cmd, sizeof cmd, "rm -f %s %s.out", IMG, IMG);
      if(system(cmd)){} }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
