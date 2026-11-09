/* ================================================================
   tests/test_espmake.c -- HavenDOS making its own ESP, on disks
                           laid out by a real partitioning tool
   ================================================================
   This is the most dangerous code in the tree. It edits a GUID
   Partition Table on a disk that, on the machine it was written for,
   has somebody's Windows install on it. A bug here does not show up
   as a wrong answer; it shows up as a laptop that will not start.

   So the disks are not hand-built fixtures and the verdicts are not
   ours:

     sgdisk   lays out every test disk, and reads the result back
     fsck.fat checks the filesystem we put in the partition we made

   tests/test_gptwrite.c already proves gpt_add_partition()'s blast
   radius against a hand-built sparse disk. This asks the question
   one layer up and on a real table: after espmk_create(), WHICH
   SECTORS OF THE WHOLE DISK CHANGED? The images here are small and
   dense so that every sector can be compared, and the answer has to
   be: the four GPT sectors, and the inside of the partition we just
   created. Nothing else, anywhere.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "types.h"
#include "gpt.h"
#include "gpt_write.h"
#include "fat32fmt.h"
#include "espmake.h"

#include "../fs/gpt.c"
#include "../fs/gpt_write.c"
#include "../fs/fat32fmt.c"
#include "../fs/espmake.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

/* ── the disk under test, as a plain file ───────────────────────── */
#define IMG       "tests/espmk.img"
#define DISK_MB   400u
#define DISK_SECT ((uint64_t)DISK_MB * 2048ull)

static FILE *g_disk;
static long  g_fail_write_lba = -1;

int inst_disk_read(int idx, uint64_t lba, uint8_t *buf)
{
    (void)idx;
    if(!g_disk || lba >= DISK_SECT) return 0;
    if(fseeko(g_disk, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    if(fread(buf, 1, 512, g_disk) != 512) return 0;
    return 1;
}
int inst_disk_write(int idx, uint64_t lba, const uint8_t *buf)
{
    (void)idx;
    if(!g_disk || lba >= DISK_SECT) return 0;
    if(g_fail_write_lba >= 0 && (long)lba == g_fail_write_lba) return 0;
    if(fseeko(g_disk, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    if(fwrite(buf, 1, 512, g_disk) != 512) return 0;
    fflush(g_disk);
    return 1;
}

static int run(const char *cmd)
{
    char b[1024];
    snprintf(b, sizeof b, "%s > /tmp/hd_espmk.out 2>&1", cmd);
    return system(b);
}

static void open_disk(void){ g_disk = fopen(IMG, "r+b"); }
static void close_disk(void){ if(g_disk){ fclose(g_disk); g_disk = NULL; } }

/* ── snapshots, so the blast radius is measured and not trusted ── */
static uint8_t *g_before, *g_after;

static int slurp(uint8_t *dst)
{
    FILE *f = fopen(IMG, "rb");
    size_t n;
    if(!f) return 0;
    n = fread(dst, 1, (size_t)DISK_SECT * 512, f);
    fclose(f);
    return n == (size_t)DISK_SECT * 512;
}

/* Returns the number of sectors that changed outside the allowed
   set, and reports the first few. */
static int changed_outside(uint64_t esp_first, uint64_t esp_last,
                           const uint64_t *gptsec, int ngpt)
{
    uint64_t i;
    int bad = 0, shown = 0, k;
    for(i = 0; i < DISK_SECT; i++){
        int allowed = 0;
        if(i >= esp_first && i <= esp_last) continue;
        for(k = 0; k < ngpt; k++) if(gptsec[k] == i){ allowed = 1; break; }
        if(allowed) continue;
        if(memcmp(g_before + i * 512, g_after + i * 512, 512) != 0){
            bad++;
            if(shown < 6){
                printf("      changed sector %llu (outside the partition and "
                       "the table)\n", (unsigned long long)i);
                shown++;
            }
        }
    }
    return bad;
}

/* ── disks, laid out by sgdisk ──────────────────────────────────── */
static int make_blank(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "rm -f %s && truncate -s %uM %s && sgdisk --zap-all %s",
             IMG, DISK_MB, IMG, IMG);
    return run(cmd) == 0;
}

/* A disk shaped like Windows': ESP, MSR, a data volume, free space. */
static int make_windows(void)
{
    char cmd[768];
    if(!make_blank()) return 0;
    snprintf(cmd, sizeof cmd,
             "sgdisk -n 1:2048:+100M -t 1:EF00 -c 1:'EFI System Partition' "
             "-n 2:0:+16M -t 2:0C01 -c 2:'Microsoft reserved' "
             "-n 3:0:+150M -t 3:0700 -c 3:'Basic data' %s", IMG);
    return run(cmd) == 0;
}

/* A disk with NO ESP: one Linux partition and a lot of free space.
   This is the case the whole file exists for. */
static int make_linux_no_esp(void)
{
    char cmd[768];
    if(!make_blank()) return 0;
    snprintf(cmd, sizeof cmd,
             "sgdisk -n 1:2048:+120M -t 1:8300 -c 1:'Linux filesystem' %s", IMG);
    return run(cmd) == 0;
}

/* An empty GPT: a brand new disk somebody just initialised. */
static int make_empty_gpt(void)
{
    char cmd[512];
    if(!make_blank()) return 0;
    snprintf(cmd, sizeof cmd, "sgdisk -o %s", IMG);
    return run(cmd) == 0;
}

/* Every sector allocated: nowhere for an ESP to go. */
static int make_full(void)
{
    char cmd[512];
    if(!make_blank()) return 0;
    snprintf(cmd, sizeof cmd,
             "sgdisk -n 1:2048:0 -t 1:8300 -c 1:'Linux filesystem' %s", IMG);
    return run(cmd) == 0;
}

/* An unrecognised partition type holding nothing at all. */
static int make_unknown_blank_part(void)
{
    char cmd[768];
    if(!make_blank()) return 0;
    snprintf(cmd, sizeof cmd,
             "sgdisk -n 1:2048:+60M -t 1:A504 -c 1:'mystery' "
             "-n 2:0:+60M -t 2:8300 -c 2:'Linux filesystem' %s", IMG);
    return run(cmd) == 0;
}

static int sgdisk_says_esp(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "sgdisk -p %s | grep -qi 'EF00'", IMG);
    return run(cmd) == 0;
}

/* Pull the new partition out and hand it to fsck.fat. */
static int fsck_partition(uint64_t first, uint64_t sectors, char *why, unsigned n)
{
    char cmd[768];
    int rc;
    snprintf(cmd, sizeof cmd,
             "dd if=%s of=/tmp/hd_espmk_part.img bs=512 skip=%llu count=%llu "
             "status=none", IMG,
             (unsigned long long)first, (unsigned long long)sectors);
    if(run(cmd) != 0) return 0;
    rc = run("fsck.fat -n /tmp/hd_espmk_part.img");
    if(why && n){
        FILE *f = fopen("/tmp/hd_espmk.out", "rb");
        char line[256];
        unsigned i = 0;
        why[0] = 0;
        if(f){
            while(fgets(line, sizeof line, f)){
                if(line[0] == 'f' && line[1] == 's' && line[2] == 'c') continue;
                if(line[0] == '\n') continue;
                while(line[i] && line[i] != '\n' && i + 1 < n){ why[i] = line[i]; i++; }
                why[i] = 0;
                if(why[0]) break;
            }
            fclose(f);
        }
    }
    return rc == 0;
}

int main(void)
{
    espmk_survey_t sv;
    uint64_t lba = 0, sect = 0;
    int idx = -1, rc;
    char why[256];

    printf("HavenDOS creating its own EFI System Partition\n");
    printf("==============================================\n");

    if(system("which sgdisk > /dev/null 2>&1") != 0
       || system("which fsck.fat > /dev/null 2>&1") != 0){
        printf("\n  sgdisk and fsck.fat are both needed for this suite to\n"
               "  mean anything, so this is a SKIP and not a pass.\n");
        printf("\nSKIP\n");
        return 77;
    }

    g_before = malloc((size_t)DISK_SECT * 512);
    g_after  = malloc((size_t)DISK_SECT * 512);
    if(!g_before || !g_after){ printf("out of memory\n"); return 1; }

    /* ── 1: the survey, on a disk that already has Windows ───────── */
    head("a Windows-shaped disk is recognised and LEFT ALONE");
    ck("sgdisk built it", make_windows(), 0);
    open_disk();
    ck("survey succeeds", espmk_survey(0, &sv) == ESPMK_OK, 0);
    ck("it is a GPT disk", sv.has_gpt, 0);
    ck("three partitions are seen", sv.n_parts == 3, sv.n_parts);
    ck("the ESP is found", sv.esp_index == 0, sv.esp_index);
    ck("it is the one sgdisk made, at LBA 2048", sv.esp_lba == 2048,
       (long)sv.esp_lba);
    ck("Windows is detected", sv.has_windows, 0);
    ck("Linux is not claimed", !sv.has_linux, 0);
    ck("free space is found after the partitions", sv.n_gaps >= 1, sv.n_gaps);
    ck("creating a second ESP is REFUSED",
       espmk_create(0, 100, 1, &lba, &sect, &idx) == ESPMK_E_HAVEESP, 0);
    close_disk();

    /* ── 2: the case this file exists for ────────────────────────── */
    head("a disk with Linux and NO ESP: one is created");
    ck("sgdisk built it", make_linux_no_esp(), 0);
    open_disk();
    ck("survey succeeds", espmk_survey(0, &sv) == ESPMK_OK, 0);
    ck("no ESP is found", sv.esp_index == -1, sv.esp_index);
    ck("Linux is detected", sv.has_linux, 0);
    ck("Windows is not claimed", !sv.has_windows, 0);

    ck("a DRY RUN reports it would work",
       espmk_create(0, 100, 1, &lba, &sect, &idx) == ESPMK_OK, 0);
    close_disk();
    ck("...and the dry run wrote NOTHING: sgdisk still sees no ESP",
       !sgdisk_says_esp(), 0);

    ck("the disk snapshots before", slurp(g_before), 0);
    open_disk();
    rc = espmk_create(0, 100, 0, &lba, &sect, &idx);
    close_disk();
    ck("the real create succeeds", rc == ESPMK_OK, rc);
    if(rc != ESPMK_OK) printf("      %s\n", espmk_strerror(rc));
    ck("it is 1 MB aligned", (lba % 2048ull) == 0, (long)lba);
    ck("it is the size asked for", sect == 100ull * 2048ull, (long)sect);

    head("sgdisk and fsck.fat agree with us about what we made");
    ck("sgdisk now reports an EF00 partition", sgdisk_says_esp(), 0);
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "sgdisk -v %s | grep -qi 'No problems found'", IMG);
        ck("sgdisk -v finds no problems with the table", run(cmd) == 0, 0);
    }
    ck("fsck.fat accepts the filesystem we put in it",
       fsck_partition(lba, sect, why, sizeof why), 0);
    if(why[0]) printf("      fsck said: %s\n", why);

    head("THE BLAST RADIUS: which sectors of the whole disk changed");
    ck("the disk snapshots after", slurp(g_after), 0);
    {
        /* The four sectors gpt_write.c is allowed to touch, worked
           out from the table rather than assumed: LBA 1 and the
           entry sector holding our index, and the same two at the
           backup copy near the end of the disk. */
        uint64_t allowed[8];
        int n = 0;
        uint64_t last = DISK_SECT - 1;
        uint64_t ent_sec = (uint64_t)(idx / 4);     /* 128-byte entries */
        allowed[n++] = 1;                            /* primary header  */
        allowed[n++] = 2 + ent_sec;                  /* primary entries */
        allowed[n++] = last;                         /* backup header   */
        allowed[n++] = last - 32 + ent_sec;          /* backup entries  */
        ck("nothing outside the new partition and the table changed",
           changed_outside(lba, lba + sect - 1, allowed, n) == 0,
           changed_outside(lba, lba + sect - 1, allowed, n));
        ck("the Linux partition's first sector is untouched",
           memcmp(g_before + 2048 * 512, g_after + 2048 * 512, 512) == 0, 0);
        ck("the protective MBR is untouched",
           memcmp(g_before, g_after, 512) == 0, 0);
    }

    /* ── 3: a brand new disk with nothing on it ──────────────────── */
    head("a freshly initialised GPT with nothing on it");
    ck("sgdisk built it", make_empty_gpt(), 0);
    open_disk();
    ck("survey succeeds", espmk_survey(0, &sv) == ESPMK_OK, 0);
    ck("no partitions", sv.n_parts == 0, sv.n_parts);
    ck("no ESP", sv.esp_index == -1, sv.esp_index);
    rc = espmk_create(0, 100, 0, &lba, &sect, &idx);
    close_disk();
    ck("an ESP is created anyway", rc == ESPMK_OK, rc);
    ck("sgdisk sees it", sgdisk_says_esp(), 0);
    ck("fsck.fat accepts it", fsck_partition(lba, sect, why, sizeof why), 0);

    /* ── 4: refusals ─────────────────────────────────────────────── */
    head("disks it must refuse, each for its own reason");
    ck("sgdisk built a full disk", make_full(), 0);
    open_disk();
    rc = espmk_create(0, 100, 1, &lba, &sect, &idx);
    close_disk();
    ck("a disk with no free space is refused as NOSPACE",
       rc == ESPMK_E_NOSPACE, rc);

    ck("a blank un-partitioned disk is made", make_blank(), 0);
    open_disk();
    rc = espmk_survey(0, &sv);
    close_disk();
    ck("a disk with no GPT is refused as NOTGPT", rc == ESPMK_E_NOTGPT, rc);

    /* ── 5: an unrecognised partition is OFFERED, never taken ────── */
    head("an unknown, empty partition is reported but not consumed");
    ck("sgdisk built it", make_unknown_blank_part(), 0);
    open_disk();
    ck("survey succeeds", espmk_survey(0, &sv) == ESPMK_OK, 0);
    ck("the unknown partition is listed as a candidate",
       sv.n_unused == 1, sv.n_unused);
    ck("...and it is the one at LBA 2048",
       sv.n_unused == 1 && sv.unused_lba[0] == 2048,
       sv.n_unused ? (long)sv.unused_lba[0] : -1);
    ck("the disk snapshots before", slurp(g_before) && 1, 0);
    rc = espmk_create(0, 100, 0, &lba, &sect, &idx);
    close_disk();
    ck("create succeeds", rc == ESPMK_OK, rc);
    ck("the ESP went in FREE SPACE, not in the unknown partition",
       lba > 2048ull + 60ull * 2048ull, (long)lba);
    ck("the disk snapshots after", slurp(g_after), 0);
    ck("the unknown partition's sectors are untouched",
       memcmp(g_before + 2048ull * 512, g_after + 2048ull * 512,
              60u * 1024u * 1024u) == 0, 0);

    /* ── 6: a failed write is reported, not swallowed ────────────── */
    head("a refused write during the format is reported");
    ck("sgdisk built a clean disk", make_linux_no_esp(), 0);
    open_disk();
    {
        gpt_gap_t gap;
        s_espmk_disk = 0;
        /* Where the ESP will land, so the injection can target the
           boot sector it is about to write. */
        if(gpt_find_free(rd_cb, 0, 100ull * 2048ull, &gap) == GPTW_OK){
            uint64_t first = (gap.first_lba + 2047ull) & ~2047ull;
            g_fail_write_lba = (long)first;
            rc = espmk_create(0, 100, 0, &lba, &sect, &idx);
            g_fail_write_lba = -1;
            ck("a refused boot-sector write fails the create",
               rc == ESPMK_E_FORMAT, rc);
        } else {
            ck("the injection target was found", 0, 0);
        }
    }
    close_disk();

    free(g_before); free(g_after);
    unlink(IMG);
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
