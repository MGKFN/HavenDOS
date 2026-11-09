/* ================================================================
   tests/test_fat32fmt.c -- the FAT32 formatter, judged from outside
   ================================================================
   v0.8.8.8. This volume is where the machine's boot loader lives. If
   the geometry is wrong the firmware does not report a bad FAT, it
   reports no operating system, and the only way back is another
   machine and a USB stick.

   So nothing here asks whether fat32_geom() agrees with arithmetic in
   this file. It formats a real image at each size and hands it to
   two programs with no stake in the answer:

     fsck.fat  -- does the volume check out
     mtools    -- can an independent FAT32 implementation MOUNT it,
                  write a file into it, and read the file back

   The second matters more than the first. fsck is a static check;
   mcopy has to allocate a cluster, follow the FAT, extend the root
   directory and write a directory entry, which is the whole of what
   fs/esp.c will do to this volume when it installs the loader.

   And because a judge that has never convicted anybody is not a
   judge, the suite corrupts a formatted image on purpose and
   requires fsck to catch it.

   Note on headers: -Iinclude makes <string.h> resolve to the kernel's
   own string.h, so this file does not include it. fs/fat32fmt.c
   includes the kernel header itself and gets memset/memcpy with
   uint32_t lengths; declaring them here as well would collide. Same
   arrangement as tests/test_fat16fmt.c, which says so too.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "fat32fmt.h"
#include "../fs/fat32fmt.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

static const char *IMG = "tests/f32fmt.img";
static FILE *g_img = NULL;
static long  g_fail_lba = -1;

static int img_wr(void *ctx, uint32_t lba, const uint8_t *buf)
{
    (void)ctx;
    if(!g_img) return 0;
    if(g_fail_lba >= 0 && (long)lba == g_fail_lba) return 0;
    if(fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) return 0;
    return fwrite(buf, 1, 512, g_img) == 512;
}

/* A real partition is full of whatever was there before, not zeroes.
   A formatter that forgets a sector leaves old bytes to be read back
   as a directory entry or a FAT chain, and a clean sparse file would
   hide exactly that. */
static int fill_garbage(const char *path, uint64_t sectors, uint64_t upto)
{
    FILE *f = fopen(path, "w+b");
    uint8_t g[512];
    uint64_t i;
    unsigned k;
    if(!f) return 0;
    if(ftruncate(fileno(f), (long long)sectors * 512LL) != 0){ fclose(f); return 0; }
    for(k = 0; k < 512; k++) g[k] = (uint8_t)(0x5A ^ (k & 0xFF));
    for(i = 0; i < upto; i++){
        if(fseeko(f, (off_t)i * 512, SEEK_SET) != 0){ fclose(f); return 0; }
        if(fwrite(g, 1, 512, f) != 512){ fclose(f); return 0; }
    }
    fclose(f);
    return 1;
}

static int run(const char *cmd)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s > /tmp/hd_f32.out 2>&1", cmd);
    return system(buf);
}

static void first_line(char *out, unsigned n)
{
    FILE *f = fopen("/tmp/hd_f32.out", "rb");
    unsigned i = 0;
    out[0] = 0;
    if(!f) return;
    { char line[256];
      while(fgets(line, sizeof line, f)){
          if(line[0] == 'f' && line[1] == 's' && line[2] == 'c') continue;
          if(line[0] == '\n') continue;
          while(line[i] && line[i] != '\n' && i + 1 < n){ out[i] = line[i]; i++; }
          out[i] = 0;
          if(out[0]) break;
      }
    }
    fclose(f);
}

/* Format an image of part_sectors. Returns 0 if the format itself
   failed, 1 if it wrote. */
static int format_img(uint64_t part_sectors, uint32_t hidden,
                      fat32_geom_t *g)
{
    if(!fat32_geom((uint32_t)part_sectors, g)) return 0;
    if(!fill_garbage(IMG, part_sectors,
                     g->data_start + 64u > part_sectors
                         ? part_sectors : g->data_start + 64u)) return 0;
    g_img = fopen(IMG, "r+b");
    if(!g_img) return 0;
    if(!fat32_format(img_wr, NULL, g, hidden, 0x1234ABCDu, "HAVENESP")){
        fclose(g_img); g_img = NULL; return 0;
    }
    fclose(g_img); g_img = NULL;
    return 1;
}

static int fsck_ok(char *why, unsigned n)
{
    char cmd[512];
    int rc;
    snprintf(cmd, sizeof cmd, "fsck.fat -n %s", IMG);
    rc = run(cmd);
    if(why) first_line(why, n);
    return rc == 0;
}

int main(void)
{
    fat32_geom_t g;
    char why[256];

    printf("FAT32 geometry and format, as judged by fsck.fat and mtools\n");

    if(system("which fsck.fat > /dev/null 2>&1") != 0){
        printf("\n  fsck.fat is not installed -- this suite needs it to mean\n"
               "  anything, so it is a SKIP and not a pass.\n");
        printf("\nSKIP\n");
        return 77;
    }

    /* ── 1: sizes that must work ─────────────────────────────────── */
    head("every size a real ESP is given, formatted and checked");
    {
        static const struct { const char *name; uint64_t mb; } sizes[] = {
            {"34 MB  (just above the FAT32 floor)",     34},
            {"64 MB",                                   64},
            {"100 MB (the usual Windows ESP)",         100},
            {"260 MB (the cluster-table boundary)",    260},
            {"261 MB (the other side of it)",          261},
            {"512 MB",                                 512},
            {"1 GB",                                  1024},
            {"2 GB",                                  2048},
            {"8 GB",                                  8192},
            {"16 GB",                                16384},
            {"32 GB",                                32768},
            {"64 GB",                                65536},
            {0, 0}
        };
        int i;
        for(i = 0; sizes[i].name; i++){
            uint64_t sect = sizes[i].mb * 2048ull;
            char what[160];
            if(sect > 0xFFFFFFFFull) continue;
            printf("    %s\n", sizes[i].name);
            snprintf(what, sizeof what, "%s: geometry exists", sizes[i].name);
            ck(what, format_img(sect, 2048u, &g), (long)sect);
            snprintf(what, sizeof what, "%s: fsck.fat accepts it", sizes[i].name);
            ck(what, fsck_ok(why, sizeof why), 0);
            if(why[0] && !fsck_ok(NULL, 0)) printf("      fsck said: %s\n", why);

            snprintf(what, sizeof what, "%s: cluster count is in FAT32 range",
                     sizes[i].name);
            ck(what, g.clusters >= FAT32_MIN_CLUSTERS
                  && g.clusters <= FAT32_MAX_CLUSTERS, (long)g.clusters);

            /* The FAT has to be able to address every cluster it
               claims. This is the exact relationship fat16fmt.h was
               written about, asked again on the other filesystem. */
            snprintf(what, sizeof what, "%s: the FAT can address every cluster",
                     sizes[i].name);
            ck(what, (uint64_t)g.sects_per_fat * 512ull / 4ull
                     >= (uint64_t)g.clusters + 2ull, (long)g.sects_per_fat);

            snprintf(what, sizeof what, "%s: the volume fits the partition",
                     sizes[i].name);
            ck(what, (uint64_t)g.data_start
                     + (uint64_t)g.clusters * g.sects_per_clust <= sect,
               (long)g.data_start);
        }
    }

    /* ── 2: sizes that must be refused ───────────────────────────── */
    head("sizes that are NOT FAT32, refused rather than mislabelled");
    {
        static const struct { const char *name; uint64_t sect; } bad[] = {
            {"0 sectors",                                    0},
            {"8 sectors",                                    8},
            {"1023 sectors (under the floor)",            1023},
            {"16 MB",                        16ull * 2048ull},
            {"30 MB",                        30ull * 2048ull},
            {"32 MB (just under the cluster floor)",
                                             32ull * 2048ull},
            {0, 0}
        };
        int i;
        for(i = 0; bad[i].name; i++){
            char what[160];
            snprintf(what, sizeof what, "%s is refused", bad[i].name);
            ck(what, fat32_geom((uint32_t)bad[i].sect, &g) == 0,
               (long)bad[i].sect);
        }
    }

    /* ── 3: an independent FAT32 implementation uses the volume ──── */
    head("mtools mounts it, writes a file, and reads it back");
    if(system("which mcopy > /dev/null 2>&1") != 0){
        printf("    mtools is not installed -- these checks are SKIPPED,\n"
               "    which means the strongest judge in this suite did not sit.\n");
    } else {
        char cmd[768];
        ck("a 100 MB volume formats", format_img(100ull * 2048ull, 2048u, &g), 0);

        snprintf(cmd, sizeof cmd, "minfo -i %s ::", IMG);
        ck("minfo reads the BPB", run(cmd) == 0, 0);

        snprintf(cmd, sizeof cmd,
                 "minfo -i %s :: | grep -q 'sector size: 512 bytes'", IMG);
        ck("minfo agrees the sector size is 512", run(cmd) == 0, 0);

        /* The directory operations fs/esp.c will perform: make the
           EFI/BOOT path and put a loader-sized file in it. */
        snprintf(cmd, sizeof cmd, "mmd -i %s ::/EFI ::/EFI/BOOT", IMG);
        ck("mmd creates /EFI/BOOT", run(cmd) == 0, 0);

        ck("a payload file exists to copy",
           system("head -c 900000 /dev/urandom > /tmp/hd_f32_payload.bin") == 0, 0);

        snprintf(cmd, sizeof cmd,
                 "mcopy -i %s /tmp/hd_f32_payload.bin ::/EFI/BOOT/BOOTX64.EFI", IMG);
        ck("mcopy writes an 879 KB loader into it", run(cmd) == 0, 0);

        snprintf(cmd, sizeof cmd,
                 "mdir -i %s ::/EFI/BOOT | grep -q BOOTX64", IMG);
        ck("mdir lists it afterwards", run(cmd) == 0, 0);

        snprintf(cmd, sizeof cmd,
                 "mcopy -n -i %s ::/EFI/BOOT/BOOTX64.EFI /tmp/hd_f32_back.bin", IMG);
        ck("mcopy reads it back out", run(cmd) == 0, 0);
        ck("the bytes that came back are the bytes that went in",
           system("cmp -s /tmp/hd_f32_payload.bin /tmp/hd_f32_back.bin") == 0, 0);

        ck("fsck.fat still accepts it with a file in it",
           fsck_ok(why, sizeof why), 0);
    }

    /* ── 4: does the judge have teeth ────────────────────────────── */
    head("corrupting a good volume on purpose, so fsck proves it is judging");
    {
        FILE *f;
        uint8_t s[512];


        /* Baseline: a volume fsck accepts. */
        ck("the baseline volume is clean",
           format_img(100ull * 2048ull, 2048u, &g) && fsck_ok(NULL, 0), 0);

        /* Now break the FAT size the BPB declares. This is the exact
           defect fat16fmt.h documents -- a FAT too small for the
           cluster count the BPB implies -- committed deliberately. */
        f = fopen(IMG, "r+b");
        ck("the image reopens", f != NULL, 0);
        if(f){
            if(fseek(f, 0, SEEK_SET) == 0 && fread(s, 1, 512, f) == 512){
                s[0x24] = 2; s[0x25] = 0; s[0x26] = 0; s[0x27] = 0;
                fseek(f, 0, SEEK_SET);
                fwrite(s, 1, 512, f);
            }
            fclose(f);
        }
        ck("fsck REJECTS a FAT too small for the cluster count",
           !fsck_ok(why, sizeof why), 0);
        if(why[0]) printf("      fsck said: %s\n", why);

        /* And the signature. BOTH copies of it.

           Zeroing only the one at sector 0 does NOT make fsck fail:
           it finds the backup at sector 6, says "There are
           differences between boot sector and its backup", repairs
           from it and exits 0. That is fsck working correctly and the
           injection being wrong -- and it is also a real property of
           the volume worth knowing, because it means a machine whose
           boot sector gets damaged still has a readable ESP. The
           injection is recorded rather than deleted. */
        ck("the baseline is clean again",
           format_img(100ull * 2048ull, 2048u, &g) && fsck_ok(NULL, 0), 0);
        f = fopen(IMG, "r+b");
        if(f){
            uint8_t z[2] = { 0, 0 };
            if(fseek(f, 510, SEEK_SET) == 0) fwrite(z, 1, 2, f);
            fclose(f);
        }
        ck("one damaged signature is REPAIRED from the backup, not fatal",
           fsck_ok(why, sizeof why), 0);

        /* Zeroing BOTH signatures does not make fsck fail either:
           it reported "1 files, 1/201616 clusters" and exited 0.
           fsck.fat does not police the 0xAA55 trailer at all.

           So this is OUR check and it is labelled as ours. It is
           worth having anyway -- firmware very much does care, and a
           formatter that stopped writing the trailer would otherwise
           pass every external judge in this file -- but it is not
           independent evidence and it is not presented as any.

           Two injections wrong in a row here, both recorded: the
           first assumed fsck would reject a damaged boot sector when
           it quietly repairs from the backup, and the second assumed
           it checked the trailer when it does not. */
        ck("the baseline is clean a third time",
           format_img(100ull * 2048ull, 2048u, &g) && fsck_ok(NULL, 0), 0);
        {
            uint8_t b0[512], b6[512];
            FILE *r = fopen(IMG, "rb");
            int got0 = 0, got6 = 0;
            if(r){
                if(fread(b0, 1, 512, r) == 512) got0 = 1;
                if(fseek(r, (long)FAT32_BACKUP_BOOT * 512L, SEEK_SET) == 0
                   && fread(b6, 1, 512, r) == 512) got6 = 1;
                fclose(r);
            }
            ck("(ours, not fsck's) the boot sector carries 0x55AA",
               got0 && b0[510] == 0x55 && b0[511] == 0xAA, 0);
            ck("(ours, not fsck's) so does the backup at sector 6",
               got6 && b6[510] == 0x55 && b6[511] == 0xAA, 0);
            ck("(ours) the backup is a byte-for-byte copy of sector 0",
               got0 && got6 && memcmp(b0, b6, 512) == 0, 0);
            ck("(ours) the BPB says FAT32",
               got0 && b0[0x52] == 'F' && b0[0x53] == 'A'
                    && b0[0x54] == 'T' && b0[0x55] == '3'
                    && b0[0x56] == '2', 0);
            ck("(ours) sectors_per_fat_16 is zero, as FAT32 requires",
               got0 && b0[0x16] == 0 && b0[0x17] == 0, 0);
        }
    }

    /* ── 5: a failed write is reported, not swallowed ────────────── */
    head("every critical sector, refused one at a time");
    {
        static const struct { const char *name; int which; } crit[] = {
            {"the boot sector",            0},
            {"the FSInfo sector",          1},
            {"the backup boot sector",     2},
            {"the first FAT's first sector", 3},
            {"the second FAT's first sector", 4},
            {"the root cluster",           5},
            {0, 0}
        };
        int i;
        ck("geometry for the injection target", fat32_geom(100u * 2048u, &g), 0);
        for(i = 0; crit[i].name; i++){
            char what[160];
            long lba = 0;
            switch(crit[i].which){
            case 0: lba = 0; break;
            case 1: lba = FAT32_FSINFO_SECT; break;
            case 2: lba = FAT32_BACKUP_BOOT; break;
            case 3: lba = g.fat1_start; break;
            case 4: lba = g.fat2_start; break;
            case 5: lba = g.data_start; break;
            }
            fill_garbage(IMG, 100ull * 2048ull, g.data_start + 64u);
            g_img = fopen(IMG, "r+b");
            g_fail_lba = lba;
            snprintf(what, sizeof what,
                     "a refused write to %s is reported", crit[i].name);
            ck(what, fat32_format(img_wr, NULL, &g, 2048u, 1u, "X") == 0, lba);
            g_fail_lba = -1;
            if(g_img){ fclose(g_img); g_img = NULL; }
        }
    }

    unlink(IMG);
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
