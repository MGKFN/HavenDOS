/* The real fs/fat16.c, compiled natively and pointed at a real disk image.
 *
 * "The updater never worked." Two independent faults were found by running it
 * in a VM; this suite is the third, and it is a filesystem one.
 *
 * Everything the updater touches lives in /boot, a subdirectory. The update
 * manager reported "No existing kernel found" for a kernel that was plainly
 * there, then "FAT16 write failed" — and yet a dump of the disk afterwards
 * showed a SECOND kernel had appeared beside the first:
 *
 *     /boot   havendos.elf   616100   <- the installed one, lower case
 *     /boot   HAVENDOS.ELF  1230708   <- written by the update, UPPER case
 *
 * So the write half-worked and produced a duplicate rather than replacing,
 * which is worse than failing: GRUB loads whichever it finds first, so the
 * machine would keep booting the old kernel while the disk filled up with
 * copies of the new one.
 *
 * Run against an image built by the installer:
 *     BOOTDISK=/path/to/disk.img ./test_fat16path
 * With no image it skips, so `make test` stays runnable anywhere.
 */

/* WARNING - this suite WRITES TO the image named by BOOTDISK, including into
   /boot.  Point it at a scratch copy, never at a disk you still want to boot:
   one run of it replaced a 631 KB /boot/havendos.elf with a 2 KB test file
   and the install stopped booting with "no multiboot header found". */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── the block layer fat16.c expects, backed by a file ── */
static FILE *g_img;
static int   g_ata;                     /* unused here; virtio path is taken */

int ata_lba48(void){ return 0; }
int ata_read_sector(uint32_t lba, uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_write_sector(uint32_t lba, const uint8_t *buf){ (void)lba;(void)buf; return 0; }
/* NVMe is a bus fs/fat16.c can read from since the installed-system
   boot failed on a real NVMe laptop. These suites drive the other buses,
   so the calls only have to link -- nvme_count() returning 0 keeps the
   detection order landing where each suite already expects. */
int nvme_count(void){ return 0; }
int nvme_read (int i, uint64_t l, uint32_t n, void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int nvme_write(int i, uint64_t l, uint32_t n, const void *b){
    (void)i;(void)l;(void)n;(void)b; return -1; }
int ata_read_sector48(uint64_t lba, uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_write_sector48(uint64_t lba, const uint8_t *buf){ (void)lba;(void)buf; return 0; }
int ata_identify(void){ return 0; }
int ata_present(void){ return g_ata; }

int virtio_blk_read(uint64_t lba, uint8_t *buf){
    if(!g_img) return 0;
    if(fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, g_img) == 512;
}
int virtio_blk_write(uint64_t lba, const uint8_t *buf){
    if(!g_img) return 0;
    if(fseeko(g_img, (off_t)lba * 512, SEEK_SET) != 0) return 0;
    if(fwrite(buf, 1, 512, g_img) != 512) return 0;
    fflush(g_img);
    return 1;
}
int virtio_blk_ready(void){ return g_img != NULL; }
int virtio_drive_count(void){ return g_img ? 1 : 0; }

void vga_puts(const char *s){ (void)s; }
void vga_putchar(char c){ (void)c; }
void vga_printf(const char *f, ...){ (void)f; }
int  ata_detected(void){ return 0; }

/* fs/fat16.c looks for its volume in a GPT when the MBR does not name
   one, which it has to: the additive installer puts the partition
   wherever the free space on a GPT disk is, and before this the installed
   system went looking at sector 2048 and found the installer instead.
   That brought fs/gpt.c in as a dependency of fs/fat16.c. */
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/gpt.c"

/* fs/fat16.c reaches AHCI now -- a machine whose SATA controller is in
   AHCI mode (every laptop) could otherwise be installed to and then not
   read itself back. These suites use neither bus; the stubs resolve the
   symbols and report no controller, so fat16 falls through to the one
   they do simulate. */
int ahci_count(void){ return 0; }
int ahci_is_atapi(int i){ (void)i; return 0; }
int ahci_read_sector (int i, uint64_t l, uint8_t *b){ (void)i;(void)l;(void)b; return 0; }
int ahci_write_sector(int i, uint64_t l, const uint8_t *b){ (void)i;(void)l;(void)b; return 0; }

/* v0.8.6: the bus selection fs/fat16.c and fs/fat32.c used to do
   themselves is fs/blkdev.c's now -- one block layer under what were
   three switches holding four, four and TWO buses. tests/blkbus.inc
   supplies the remaining back-end entry points blk_scan() calls, in
   terms of the stubs above, and pulls in the real fs/blkdev.c. */
#include "blkbus.inc"


/* v0.8.6.1: fs/fat16.c reports through klog() when the system's own
   volume does not come back after a borrow -- an unmounted filesystem
   with nothing on screen to explain it is worse than a noisy one.
   Swallowed here; the suite's own output is the readable thing. */
static void klog(int lvl, const char *fmt, ...){ (void)lvl; (void)fmt; }

#include "../fs/fat16.c"

static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what,
                           detail ? detail : ""); }
}
static void head(const char *t){ printf("\n%s\n", t); }

/* Count directory entries in /boot matching a name, case-insensitively, so a
   duplicate created with different case is visible. */
static int count_in_boot(const char *n83want, const char *e83want)
{
    uint16_t dir = 0;
    char n83[8], e83[3];
    name_to_83("boot", n83, e83);
    uint16_t bootc = 0; int isdir = 0;
    if(!dir_lookup(0, n83, e83, &bootc, &isdir) || !isdir) return -1;

    int n = 0;
    uint8_t buf[BYTES_PER_SECT];
    uint16_t c = bootc;
    while(c >= 2 && c < FAT_BAD){
        uint32_t lba = cluster_to_lba(c);
        for(int sp = 0; sp < bpb_sects_per_clust; sp++){
            if(!rd(lba + sp, buf)) return n;
            for(int i = 0; i < BYTES_PER_SECT/32; i++){
                uint8_t *d = buf + i*32;
                if(d[DE_NAME] == 0x00) return n;
                if(d[DE_NAME] == 0xE5) continue;
                if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
                if(n83_eq(d, n83want, e83want)) n++;
            }
        }
        c = fat_get(c);
    }
    return n;
}

int main(void)
{
    const char *img = getenv("BOOTDISK");
    printf("FAT16 subdirectory paths, against a real installed disk\n");
    printf("======================================================\n");
    if(!img){
        printf("\n  SKIP  set BOOTDISK=/path/to/disk.img to run this suite\n");
        printf("        (an image produced by the HavenDOS installer)\n\n");
        return 0;
    }
    g_img = fopen(img, "r+b");
    if(!g_img){ printf("  cannot open %s\n", img); return 1; }

    if(!fat16_init()){
        printf("  fat16_init() failed on %s\n", img);
        return 1;
    }
    printf("  mounted %s\n", img);

    char n83[8], e83[3];
    name_to_83("havendos.elf", n83, e83);

    head("the installer's own layout is findable");
    ck("/boot is a directory", ({
        char b8[8], b3[3]; name_to_83("boot", b8, b3);
        uint16_t c=0; int d=0;
        dir_lookup(0, b8, b3, &c, &d) && d && c >= 2;
    }), "dir_lookup could not find /boot in the root");

    ck("/boot/havendos.elf exists",
       fat16_exists_path("/boot/havendos.elf"),
       "fat16_exists_path said no - this is the "
       "\"No existing kernel found\" the update manager printed");

    ck("/boot/grub/grub.cfg exists too",
       fat16_exists_path("/boot/grub/grub.cfg"),
       "two levels deep failed");

    head("the name the installer wrote is the name we look up");
    /* The installer hardcodes make_dir(0,"boot    ") and
       write_file(boot,"havendos","elf") in LOWER case, while name_to_83()
       upper-cases. FAT16 short names are upper case by specification, so the
       on-disk entries are out of spec; the lookups survive only because
       n83_eq() folds case. root_find(), which uses memcmp, does not. */
    ck("the root-only and path APIs agree about a root file",
       fat16_exists_path("/havendos.txt") == fat16_exists("havendos.txt"),
       "root_find() and path_walk() disagree - one of them is still "
       "comparing 8.3 names case-sensitively");
    ck("and both can see it",
       fat16_exists_path("/havendos.txt") && fat16_exists("havendos.txt"),
       "the installer wrote it; neither API finds it");

    head("reading through a subdirectory");
    {
        static char buf[8];
        int n = fat16_read_path("/boot/havendos.elf", buf, sizeof(buf));
        ck("the kernel reads back", n >= 4, "fat16_read_path returned short");
        ck("and starts with the ELF magic",
           n >= 4 && (uint8_t)buf[0] == 0x7F && buf[1] == 'E'
                  && buf[2] == 'L' && buf[3] == 'F', "not an ELF");
    }

    head("writing must REPLACE, not duplicate");
    int before = count_in_boot(n83, e83);
    printf("        /boot holds %d entr%s named havendos.elf before the write\n",
           before, before == 1 ? "y" : "ies");
    ck("exactly one kernel entry to start with", before == 1, "");

    {
        /* A small payload: this is about directory bookkeeping, not size. */
        static char payload[2048];
        memset(payload, 0, sizeof(payload));
        payload[0] = 0x7F; payload[1] = 'E'; payload[2] = 'L'; payload[3] = 'F';
        for(int i = 4; i < (int)sizeof(payload); i++) payload[i] = (char)(i & 0xFF);

        /* The convention is BYTES WRITTEN on success, negative on failure --
           not 0 on success. update.c tested `!= 0` at all three of its write
           sites, so every successful write looked like a failure. */
        int w = fat16_write_path("/boot/havendos.elf", payload, sizeof(payload));
        ck("fat16_write_path returns the byte count, not 0",
           w == (int)sizeof(payload),
           "a caller testing `!= 0` for failure would misread this");
        ck("...and that is a success, not the failure update.c reported",
           w > 0, "negative return");

        int after = count_in_boot(n83, e83);
        printf("        /boot holds %d entr%s named havendos.elf after it\n",
               after, after == 1 ? "y" : "ies");
        ck("still exactly one entry afterwards", after == 1,
           "the write created a SECOND entry instead of replacing the first - "
           "GRUB would keep loading whichever it finds first");

        static char rb[8];
        int n = fat16_read_path("/boot/havendos.elf", rb, sizeof(rb));
        ck("reading it back gives what was just written",
           n >= 5 && (uint8_t)rb[0] == 0x7F && rb[4] == (char)4, "stale content");
    }

    fclose(g_img);
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
