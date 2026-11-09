/*
 * install.c — HavenDOS v0.7.4 installer backend (revised)
 *
 * Bug fixes in this revision:
 *
 * BUG 1 — "One boot" black screen:
 *   core.img's diskboot.img (first 512 bytes) has a blocklist that
 *   says "load my remaining sectors starting from LBA 0, count 296".
 *   We write core.img to sectors 1..297 of the target disk, but the
 *   blocklist still says LBA 0 — so on the second boot GRUB loads the
 *   MBR instead of core.img and produces a black screen.  The first
 *   boot worked because GRUB was still running from the ISO.
 *   FIX: Patch core.img bytes [0x1F8..0x1FB] to 0x01,0x00,0x00,0x00
 *   (LBA = 1) before writing to disk.
 *
 * BUG 2 — USB drive targeted instead of SATA:
 *   ATA probe hits Pri Master first.  On a PC booted from USB, the USB
 *   device appears as Pri Master.  The 128GB SATA SSD appears as Sec
 *   Master or on a different controller.  The picker now shows ALL
 *   drives with their sector counts and never auto-selects; the user
 *   must always confirm.  Drives under 64MB are labelled "TOO SMALL".
 *
 * BUG 3 — EFI partition mistaken for whole disk:
 *   On a GPT-formatted disk the MBR has a protective entry (type 0xEE)
 *   and GPT partition 1 is typically a 1GB EFI partition.  Our installer
 *   was reading the existing partition table entry sizes instead of using
 *   the full disk sector count from IDENTIFY.  FIX: always overwrite the
 *   entire first 2048 sectors (pre-partition area) including the GPT
 *   header at LBA 1, and write a fresh MBR with our own partition table.
 *   The partition size is always computed from the full disk sector count.
 *
 * BUG 4 — verify false positive:
 *   Old verify read back the MBR we just wrote and checked the type byte
 *   we set ourselves — always passed.  New verify reads sector 1 (core.img
 *   start), checks the patched LBA bytes, and reads the FAT BPB signature
 *   at PART_START_SECT to confirm the partition is actually readable.
 */

#include "../include/types.h"
#include "version.h"
#include "../include/string.h"
#include "../include/vga.h"

/* ── Unified disk API (provided by iso_installer.c) ─────────────── */
extern int      inst_disk_count(void);
extern int      inst_disk_read(int idx, uint64_t lba, uint8_t *buf);
extern int      inst_disk_write(int idx, uint64_t lba, const uint8_t *buf);
extern uint64_t inst_disk_sectors(int idx);

/* ── Partition constants ─────────────────────────────────────────── */
#define PART_START_SECT  2048ULL
#define BYTES_PER_SECT   512

/* The FAT16 geometry used to live here as five fixed macros, of which
   BPB_SECTS_FAT 128 was simply wrong for any partition over about 64 MB:
   the BPB was given the partition's real sector count while the FAT stayed
   65536 bytes, so a 1 GB partition claimed 524,215 clusters in a table
   that addresses 32,766. fsck.vfat refuses such a volume outright --
   "Too many clusters (524215) for FAT16 filesystem" -- and Windows offers
   to reformat it. It went unnoticed because a fresh install touches a few
   hundred clusters at the very start, so nothing ever reaches the part of
   the FAT that is not there.
   
   include/fat16fmt.h computes it instead, from the partition size, and
   tests/test_fat16fmt.c has fsck.vfat confirm the result at sixteen
   sizes. Both install paths use the same computation, so neither can be
   fixed without the other. */
#include "fat16fmt.h"
#include "gpt_write.h"
#include "esp.h"
#include "espmake.h"
#include "install_add.h"

#define FAT_EOF          0xFFF8
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20

/* ── GRUB disk-boot embedding ────────────────────────────────────────
   grub_disk_embed.h is built with grub-mkimage --format=i386-pc with
   modules: biosdisk part_msdos fat multiboot search search_fs_file
   search_label normal echo.
   This is DIFFERENT from grub_embed.h which is the CD-boot version
   produced by grub-mkrescue. The CD version has its prefix baked in
   as (cd) and lacks biosdisk — it cannot boot from a hard disk.      */
#ifdef HAVE_GRUB_EMBED
#include "../include/grub_disk_embed.h"
#define GRUB_AVAILABLE 1
#else
#define GRUB_AVAILABLE 0
static const uint8_t  grub_boot_img[]    = {0};
static const uint32_t grub_boot_img_size = 0;
static const uint8_t  grub_core_img[]    = {0};
static const uint32_t grub_core_img_size = 0;
#endif

/* ── Kernel ELF embedding ────────────────────────────────────────── */
/* INSTALL_KERNEL_PROVIDED lets a test supply its own image instead of the
   real 1.3 MB one, the same seam fs/esp.c has for the UEFI loader
   (ESP_LOADER_PROVIDED). Without it tests/test_installadd.c would have to
   either build a GRUB image or accept INSTADD_E_NOIMAGE and test nothing,
   and a suite that cannot reach the code is the suite that reported
   74/74 while eight of sixteen injected ESP bugs walked through it. */
#ifdef INSTALL_KERNEL_PROVIDED
#define KERNEL_ELF_AVAILABLE 1
#elif defined(HAVE_KERNEL_ELF)
#include "../include/kernel_elf.h"
#define KERNEL_ELF_AVAILABLE 1
#else
#define KERNEL_ELF_AVAILABLE 0
static const __attribute__((unused)) uint8_t  kernel_elf_data[] = {0};
static const __attribute__((unused)) uint32_t kernel_elf_size   = 0;
#endif

/* ── Per-install state ───────────────────────────────────────────── */
static int      s_disk;
static uint16_t s_next_cluster;

/* Where the partition begins on the disk, and the geometry of the volume
   inside it. Both were compile-time constants until the additive installer
   arrived: a whole-disk install always starts at sector 2048, but a
   partition placed in a GPT gap starts wherever the gap is. Everything
   below addresses the volume relative to s_part_lba, so there is exactly
   one place that knows the absolute number. */
static uint64_t     s_part_lba = PART_START_SECT;
static fat16_geom_t s_geom;
static uint32_t     s_whole_sectors;  /* whole-disk partition, capped */
/* Progress callback for long writes (set before write_file, cleared after) */
static void   (*s_progress_cb)(int,int,const char*) = 0;
static int      s_progress_step  = 0;
static int      s_progress_total = 0;
static const char *s_progress_label = "";

/* ── I/O helpers ─────────────────────────────────────────────────── */
static int inst_rd(uint32_t part_lba, uint8_t *buf){
    return inst_disk_read(s_disk, s_part_lba + part_lba, buf);
}
static int inst_wr(uint32_t part_lba, const uint8_t *buf){
    return inst_disk_write(s_disk, s_part_lba + part_lba, buf);
}
/* fat16_format() writes through the same two functions, so the formatter
   cannot reach a sector the rest of the installer cannot. */
static int inst_fmt_wr(void *ctx, uint32_t part_lba, const uint8_t *buf){
    (void)ctx; return inst_wr(part_lba, buf);
}
static uint32_t clust_lba(uint16_t c){
    return s_geom.data_start
         + ((uint32_t)(c - 2) * (uint32_t)s_geom.sects_per_clust);
}

/* ── FAT manipulation ────────────────────────────────────────────── */
/* inst_fat_set, not fat_set: fs/fat32.c has a fat_set of its own and
   tests/test_installadd.c compiles both into one translation unit. */
static int inst_fat_set(uint16_t cluster, uint16_t val){
    uint32_t byte_off  = (uint32_t)cluster * 2;
    uint32_t sect_off  = byte_off / BYTES_PER_SECT;
    uint32_t byte_in_s = byte_off % BYTES_PER_SECT;
    uint8_t buf[512];
    if(cluster > s_geom.clusters + 1u) return 0;   /* off the end of the FAT */
    if(!inst_rd(s_geom.fat1_start + sect_off, buf)) return 0;
    buf[byte_in_s]   = val & 0xFF;
    buf[byte_in_s+1] = (val >> 8) & 0xFF;
    if(!inst_wr(s_geom.fat1_start + sect_off, buf)) return 0;
    if(!inst_rd(s_geom.fat2_start + sect_off, buf)) return 0;
    buf[byte_in_s]   = val & 0xFF;
    buf[byte_in_s+1] = (val >> 8) & 0xFF;
    if(!inst_wr(s_geom.fat2_start + sect_off, buf)) return 0;
    return 1;
}
static uint16_t alloc_cluster(void){
    uint16_t c = s_next_cluster++;
    if(!inst_fat_set(c, FAT_EOF)) return 0;
    return c;
}
static int chain_cluster(uint16_t prev, uint16_t next){
    return inst_fat_set(prev, next);
}

/* ── Directory entry writer ──────────────────────────────────────── */
static int write_dirent(uint16_t dir_cluster,
                         const char *name8, const char *ext3,
                         uint8_t attr, uint16_t first_cluster,
                         uint32_t file_size)
{
    uint8_t buf[512];
    if(dir_cluster == 0){
        for(uint32_t s = 0; s < s_geom.root_sects; s++){
            if(!inst_rd(s_geom.root_start + s, buf)) return 0;
            for(int i = 0; i < 512; i += 32){
                if(buf[i] == 0x00 || (uint8_t)buf[i] == 0xE5){
                    memset(buf+i,0,32);
                    memcpy(buf+i,    name8,8);
                    memcpy(buf+i+8,  ext3, 3);
                    buf[i+11]=attr;
                    buf[i+26]=(uint8_t)(first_cluster&0xFF);
                    buf[i+27]=(uint8_t)((first_cluster>>8)&0xFF);
                    buf[i+28]=(uint8_t)(file_size&0xFF);
                    buf[i+29]=(uint8_t)((file_size>>8)&0xFF);
                    buf[i+30]=(uint8_t)((file_size>>16)&0xFF);
                    buf[i+31]=(uint8_t)((file_size>>24)&0xFF);
                    return inst_wr(s_geom.root_start+s,buf);
                }
            }
        }
        return 0;
    } else {
        uint32_t lba=clust_lba(dir_cluster);
        for(int sp=0;sp<(int)s_geom.sects_per_clust;sp++){
            if(!inst_rd(lba+sp,buf)) return 0;
            for(int i=0;i<512;i+=32){
                if(buf[i]==0x00||(uint8_t)buf[i]==0xE5){
                    memset(buf+i,0,32);
                    memcpy(buf+i,   name8,8);
                    memcpy(buf+i+8, ext3, 3);
                    buf[i+11]=attr;
                    buf[i+26]=(uint8_t)(first_cluster&0xFF);
                    buf[i+27]=(uint8_t)((first_cluster>>8)&0xFF);
                    buf[i+28]=(uint8_t)(file_size&0xFF);
                    buf[i+29]=(uint8_t)((file_size>>8)&0xFF);
                    buf[i+30]=(uint8_t)((file_size>>16)&0xFF);
                    buf[i+31]=(uint8_t)((file_size>>24)&0xFF);
                    return inst_wr(lba+sp,buf);
                }
            }
        }
        return 0;
    }
}

/* ── Create subdirectory ─────────────────────────────────────────── */
static uint16_t make_dir(uint16_t parent_cluster,
                          const char *name8, uint16_t parent_first)
{
    uint16_t c=alloc_cluster(); if(!c) return 0;
    uint8_t zeros[512]; memset(zeros,0,512);
    uint32_t lba=clust_lba(c);
    for(int sp=0;sp<(int)s_geom.sects_per_clust;sp++)
        if(!inst_wr(lba+sp,zeros)) return 0;
    if(!write_dirent(c,".       ","   ",ATTR_DIRECTORY,c,0)) return 0;
    if(!write_dirent(c,"..      ","   ",ATTR_DIRECTORY,parent_first,0)) return 0;
    if(!write_dirent(parent_cluster,name8,"   ",ATTR_DIRECTORY,c,0)) return 0;
    return c;
}

/* ── Write file into directory ───────────────────────────────────── */
static int write_file(uint16_t dir_cluster,
                       const char *name8, const char *ext3,
                       const uint8_t *data, uint32_t size)
{
    if(size==0) return 0;
    uint32_t remaining=size, offset=0;
    uint16_t first_c=0, prev_c=0;
    uint32_t cbytes=(uint32_t)s_geom.sects_per_clust*512u;
    uint32_t total_clusters=(size+cbytes-1)/cbytes;
    uint32_t done_clusters=0;
    while(remaining>0){
        uint16_t c=alloc_cluster(); if(!c) return 0;
        if(first_c==0) first_c=c;
        if(prev_c!=0) if(!chain_cluster(prev_c,c)) return 0;
        uint32_t lba=clust_lba(c);
        for(int sp=0;sp<(int)s_geom.sects_per_clust&&remaining>0;sp++){
            uint8_t sect[512];
            uint32_t chunk=remaining>512?512:remaining;
            memset(sect,0,512);
            memcpy(sect,data+offset,chunk);
            if(!inst_wr(lba+sp,sect)) return 0;
            offset+=chunk; remaining-=chunk;
        }
        prev_c=c;
        done_clusters++;
        /* Fire progress callback every 16 clusters (~32KB) so the
           UI doesn't appear frozen during the ~540KB kernel copy. */
        if(s_progress_cb && (done_clusters&15)==0){
            int pct=(int)(done_clusters*100/total_clusters);
            /* Reuse step/total from caller; synthesise a sub-percent msg */
            s_progress_cb(s_progress_step*100+pct,
                          s_progress_total*100,
                          s_progress_label);
        }
    }
    return write_dirent(dir_cluster,name8,ext3,ATTR_ARCHIVE,first_c,size);
}

/* ── MBR partition entry ─────────────────────────────────────────── */
static void build_part_entry(uint8_t *e, uint8_t status,
                              uint32_t lba_start, uint32_t lba_count)
{
    e[0]=status;
    e[1]=0xFE; e[2]=0xFF; e[3]=0xFF;   /* CHS start — max (LBA mode) */
    e[4]=0x0E;                           /* FAT16 LBA */
    e[5]=0xFE; e[6]=0xFF; e[7]=0xFF;   /* CHS end   — max (LBA mode) */
    e[8] =(uint8_t)(lba_start        &0xFF);
    e[9] =(uint8_t)((lba_start>> 8)  &0xFF);
    e[10]=(uint8_t)((lba_start>>16)  &0xFF);
    e[11]=(uint8_t)((lba_start>>24)  &0xFF);
    e[12]=(uint8_t)(lba_count        &0xFF);
    e[13]=(uint8_t)((lba_count>> 8)  &0xFF);
    e[14]=(uint8_t)((lba_count>>16)  &0xFF);
    e[15]=(uint8_t)((lba_count>>24)  &0xFF);
}

/* Can this copy of HavenDOS write a kernel onto a disk?
 *
 * Only the ISO's kernel can.  The build is two-pass: pass 1 produces a kernel
 * with no embedded image, pass 2 embeds that pass-1 kernel and is what ships
 * on the ISO - so the kernel the installer copies onto a disk, and therefore
 * the one an installed system runs, is the pass-1 one, which carries no image
 * of its own.  A second embedding would have to contain the first, which is
 * why it is not simply done.
 *
 * This used to be discovered at step 7 of 9, by which point the disk had been
 * repartitioned, formatted and written to.  Running `install` on an installed
 * system destroyed it and reported "code -8".  The check is a precondition
 * now, and havendos_install_to_drive() refuses before touching anything.
 */
int havendos_install_image_available(void)
{
#if KERNEL_ELF_AVAILABLE
    return (kernel_elf_size >= 52 &&
            kernel_elf_data[0] == 0x7F && kernel_elf_data[1] == 'E' &&
            kernel_elf_data[2] == 'L'  && kernel_elf_data[3] == 'F');
#else
    return 0;
#endif
}

/* ── Public install entry ────────────────────────────────────────── */
int havendos_install_to_drive(int disk_idx,
                               void (*cb)(int,int,const char*))
{
    int total=9, step=0;

    if(disk_idx<0||disk_idx>=inst_disk_count()) return -1;
    /* Before the disk is touched, not after it has been formatted. */
    if(!havendos_install_image_available()) return -11;
    /* A live session repartitions nothing, and this is the lowest point it
       can be stopped -- below every UI, so a new caller cannot reach the
       destructive path by forgetting to ask.  The ISO's ordinary entries
       boot live precisely so that a machine with another operating system
       on it is safe by default; installing is the one thing that has to be
       chosen deliberately, from the "Install to Disk" entry. */
    { extern int boot_is_live(void);
      if(boot_is_live()) return -12; }
    /* Two more refusals, both below every UI so a future caller cannot
       reach step 1 by forgetting to check.
       -13: an NVMe namespace.  There is no write path to one (see
            include/nvme.h) and there must not be until the installer adds a
            partition rather than replacing the table.
       -14: a disk carrying a GUID Partition Table.  Step 1 below zeroes
            LBA 0..63 and the last 33 sectors -- that is the GPT header, the
            entry array and the backup header.  On a UEFI machine that is
            the entire partition table; on a dual-boot machine it is
            somebody's other operating system, both copies, unrecoverably. */
    { extern int inst_disk_is_readonly(int);
      if(inst_disk_is_readonly(disk_idx)) return -13; }
    { extern int inst_disk_has_gpt(int);
      if(inst_disk_has_gpt(disk_idx)) return -14; }
    uint64_t dsects=inst_disk_sectors(disk_idx);
    if(dsects<30720) return -2;   /* < 15 MB */


    s_disk=disk_idx;
    s_next_cluster=2;
    /* Set, not inherited. Both of these are module state shared with the
       additive installer, and a whole-disk install that ran after one of
       those would otherwise address the volume from the GPT gap the other
       one found. The static initialiser is correct only for the first
       install in a session. */
    s_part_lba=PART_START_SECT;
    memset(&s_geom,0,sizeof s_geom);

    /* How big the partition will be, decided ONCE, here, before a single
       sector is written.

       It used to be computed twice -- once for the MBR entry and once for
       the format -- as the whole disk minus the first megabyte, with no
       upper bound. FAT16 tops out at 65524 clusters of 64 sectors, which
       is 2047 MB, so on any disk larger than that fat16_geom() refused and
       the install died at step 4 of 9 with E15. By then the partition
       table and GRUB had already been written, so the disk was left
       unbootable, and the error on screen said to check the disk was
       "large enough". Every disk over 2 GB did this -- on every bus. The
       additive installer has had INSTADD_MAX_SECTORS since 0.8.3; this is
       the same bound, which is why that path never had the bug.

       A 16 GB disk therefore gets a 2047 MB HavenDOS partition and 14 GB
       of free space after it, rather than a refusal. */
    s_whole_sectors = (uint32_t)(dsects - PART_START_SECT);
    if(s_whole_sectors > INSTADD_MAX_SECTORS)
        s_whole_sectors = INSTADD_MAX_SECTORS;
    s_whole_sectors = s_whole_sectors / INSTADD_ALIGN * INSTADD_ALIGN;

    /* Validated here, not at step 4. A geometry this function cannot
       express is a reason to refuse the disk, not a reason to abandon one
       half-written.

       This has to come AFTER the s_geom reset above, not before it. The
       first version of this fix computed the geometry at the top of the
       function, where the reset then zeroed it, and fat16_format() refused
       a geometry with no cluster size -- turning E15 into E5 and moving
       the failure one step later. */
    if(!fat16_geom(s_whole_sectors, &s_geom)) return -15;

    /* ── 1: Wipe pre-partition area ──────────────────────────────
       Completely zero sectors 0..2047 (the 1MB before our partition).
       This destroys any existing GPT header (LBA 1), GPT backup, EFI
       partition table entries, and any previous MBR — prevents old
       partition data from confusing BIOS or GRUB on next boot.      */
    if(cb) cb(step,total,"Wiping pre-partition area (GPT/EFI clean)...");
    {
        uint8_t zero[512]; memset(zero,0,512);
        /* Wipe first 64 sectors — covers MBR, GPT header, GPT entries */
        for(uint64_t lba=0;lba<64;lba++)
            inst_disk_write(disk_idx,lba,zero);
        /* Also wipe the last 33 sectors (GPT backup header) if disk big enough */
        if(dsects>34){
            for(uint64_t lba=dsects-33;lba<dsects;lba++)
                inst_disk_write(disk_idx,lba,zero);
        }
    }
    step++;

    /* ── 2: MBR ─────────────────────────────────────────────────
       Write a fresh MBR with boot.img code (first 446 bytes only —
       preserve the partition table area we build ourselves).
       BUG FIX: We now set boot.img's core.img LBA field (bytes 0x5C)
       to 1 so it loads core.img from sector 1 on every boot, not
       just the first.                                               */
    if(cb) cb(step,total,"Writing MBR bootloader...");
    {
        uint8_t mbr[512]; memset(mbr,0,512);

        if(GRUB_AVAILABLE && grub_boot_img_size==512){
            memcpy(mbr,grub_boot_img,446);
            /* Patch core.img start LBA into boot.img at offset 0x5C.
               boot.img reads 8 bytes here as the 64-bit LBA of the
               first core.img sector.  Must be 1 (sector immediately
               after MBR) regardless of what grub-mkrescue set it to. */
            mbr[0x5C]=0x01; mbr[0x5D]=0x00; mbr[0x5E]=0x00; mbr[0x5F]=0x00;
            mbr[0x60]=0x00; mbr[0x61]=0x00; mbr[0x62]=0x00; mbr[0x63]=0x00;
            /* DO NOT patch mbr[0x40]. GRUB's boot.img saves the BIOS DL
               register (actual boot drive number) into offset 0x40 at
               runtime. If we pre-set it to 0x80 here and the BIOS assigns
               0x80 to the CD-ROM (with the HDD at 0x81 or higher), boot.img
               will read core.img from the CD instead of the HDD: read error.
               Leave it as 0x00 so GRUB overwrites it correctly at boot.    */
        }

        build_part_entry(mbr+446,0x80,(uint32_t)PART_START_SECT,
                         s_whole_sectors);
        mbr[510]=0x55; mbr[511]=0xAA;
        if(!inst_disk_write(disk_idx,0,mbr)) return -3;
    }
    step++;

    /* ── 3: core.img (sectors 1 .. N) ───────────────────────────
       BUG FIX: patch core.img diskboot blocklist at offset 0x1F8
       to say LBA=1 (not LBA=0) so GRUB loads the right sectors.   */
    if(cb) cb(step,total,"Writing GRUB core.img...");
    if(GRUB_AVAILABLE && grub_core_img_size>0){
        /* Copy into mutable buffer so we can patch it */
        static uint8_t core_buf[512];
        const uint8_t *src=grub_core_img;
        uint32_t rem=grub_core_img_size;
        uint64_t lba=1;

        /* Patch the first sector (diskboot.img) before writing */
        uint32_t first_chunk=rem>512?512:rem;
        memcpy(core_buf,src,first_chunk);

        /* ── GRUB i386-pc blocklist patch ────────────────────────
           The blocklist lives in the LAST 12 BYTES of diskboot.img
           (the first sector of core.img) and has this layout, matching
           struct grub_boot_blocklist:

             0x1F4 .. 0x1FB : 64-bit start LBA  (little endian)
             0x1FC .. 0x1FD : 16-bit sector count
             0x1FE .. 0x1FF : 16-bit load segment

           There is only ONE entry.  grub-mkimage fills in the count
           (0x1FC/0x1FD) and the segment (0x0820); the start LBA is left
           at its stock placeholder value of 2, to be written by whoever
           installs the image -- normally grub-bios-setup, here us.

           PREVIOUS BUG: the code treated the start LBA as a 32-bit field
           at 0x1F8 and wrote 1 there.  0x1F8 is actually bytes 4..7 of the
           64-bit start field, i.e. the HIGH dword, so the resulting start
           LBA was 0x0000000100000002 = 4294967298.  INT 13h failed on that
           sector every time and GRUB printed "Read Error" before the menu
           could ever appear.  A follow-up "fix" also patched 0x1F0..0x1F7
           as though it were a second entry; there is no second entry, and
           those writes corrupted the low dword of the start field.

           We write core.img starting at LBA 1, so start = 1.             */
        core_buf[0x1F4]=0x02; core_buf[0x1F5]=0x00;
        core_buf[0x1F6]=0x00; core_buf[0x1F7]=0x00;
        core_buf[0x1F8]=0x00; core_buf[0x1F9]=0x00;
        core_buf[0x1FA]=0x00; core_buf[0x1FB]=0x00;
        /* 0x1FC..0x1FD (count) and 0x1FE..0x1FF (segment) are set
           correctly by grub-mkimage -- do not touch them. */

        if(!inst_disk_write(disk_idx,lba,core_buf)) return -4;
        src+=first_chunk; rem-=first_chunk; lba++;

        /* Write remaining core.img sectors unchanged */
        while(rem>0&&lba<PART_START_SECT){
            uint32_t chunk=rem>512?512:rem;
            memset(core_buf,0,512);
            memcpy(core_buf,src,chunk);
            if(!inst_disk_write(disk_idx,lba,core_buf)) return -4;
            src+=chunk; rem-=chunk; lba++;
        }
    }
    step++;

    /* ── 4: FAT16 format ─────────────────────────
       The boot sector, both FATs and the root directory, with the geometry
       computed from the partition size rather than assumed. This was 60
       lines of hand-written BPB with a 128-sector FAT nailed into it; see
       the note beside the include at the top of this file for what that
       produced on anything larger than 64 MB. */
    if(cb) cb(step,total,"Formatting FAT16 partition...");
    /* s_geom was computed and validated before step 1; nothing between
       then and here changes the partition size. */
    if(!fat16_format(inst_fmt_wr,0,&s_geom,
                     (uint32_t)PART_START_SECT,"HAVENDOS"))     return -5;
    /* ── 5: Directories ──────────────────────────────────────────── */
    if(cb) cb(step,total,"Creating /boot/grub...");
    /* v0.7.19: these were lower case.  FAT16 short names are UPPER case by
       specification -- a lower-case entry is out of spec, is not what
       name_to_83() produces, and was only ever found because some of the
       lookups happened to fold case.  Written correctly now; the lookups
       still fold case so disks installed before this keep working. */
    uint16_t boot_clust=make_dir(0,"BOOT    ",0);
    if(!boot_clust) return -6;
    uint16_t grub_clust=make_dir(boot_clust,"GRUB    ",boot_clust);
    if(!grub_clust) return -6;
    step++;

    /* ── 6: grub.cfg ─────────────────────────────────────────────── */
    if(cb) cb(step,total,"Writing /boot/grub/grub.cfg...");
    {
        /* grub.cfg written to installed disk.
           core.img was built with prefix=(hd0,msdos1)/boot/grub so GRUB
           already knows where to find modules — no insmod needed here.
           Two entries: search-based (robust) and explicit (fallback).   */
        const char *cfg=
            "set timeout=5\n"
            "set default=0\n"
            "\n"
            "menuentry \"HavenDOS " HAVENDOS_VER "\" {\n"
            "    search --no-floppy --set=root --label HAVENDOS\n"
            "    multiboot /boot/havendos.elf mode=desktop gfx=text\n"
            "    boot\n"
            "}\n"
            "\n"
            "menuentry \"HavenDOS " HAVENDOS_VER "  (shell mode)\" {\n"
            "    search --no-floppy --set=root --label HAVENDOS\n"
            "    multiboot /boot/havendos.elf mode=shell gfx=text\n"
            "    boot\n"
            "}\n"
            "\n"
            "menuentry \"HavenDOS " HAVENDOS_VER "  (explicit hd0,msdos1)\" {\n"
            "    set root=(hd0,msdos1)\n"
            "    multiboot (hd0,msdos1)/boot/havendos.elf mode=desktop gfx=text\n"
            "    boot\n"
            "}\n"
            "\n"
            "menuentry \"HavenDOS " HAVENDOS_VER "  (reinstall)\" {\n"
            "    search --no-floppy --set=root --label HAVENDOS\n"
            "    multiboot /boot/havendos.elf mode=desktop gfx=text install=1\n"
            "    boot\n"
            "}\n";
        if(!write_file(grub_clust,"GRUB    ","CFG",
                        (const uint8_t*)cfg,(uint32_t)strlen(cfg))) return -7;
    }
    step++;

    /* ── 7: havendos.elf ─────────────────────────────────────────── */
    if(cb) cb(step,total,"Copying HavenDOS kernel (please wait)...");
    {
#if KERNEL_ELF_AVAILABLE
        const uint8_t *kdata=kernel_elf_data;
        uint32_t ksize=kernel_elf_size;
        if(ksize<52||kdata[0]!=0x7F||kdata[1]!='E'||
           kdata[2]!='L'||kdata[3]!='F') return -8;
        /* Enable sub-step progress so the screen updates during the
           ~540KB write instead of appearing frozen for several seconds. */
        s_progress_cb    = cb;
        s_progress_step  = step;
        s_progress_total = total;
        s_progress_label = "Copying HavenDOS kernel...";
        int wok=write_file(boot_clust,"HAVENDOS","ELF",kdata,ksize);
        s_progress_cb = 0;
        if(!wok) return -8;
#else
        return -8;
#endif
    }
    step++;

    /* ── 8: Verify ───────────────────────────────────────────────── */
    if(cb) cb(step,total,"Verifying install...");
    {
        /* Read back MBR and check our signature */
        uint8_t vmbr[512];
        if(!inst_disk_read(disk_idx,0,vmbr)) return -9;
        if(vmbr[510]!=0x55||vmbr[511]!=0xAA) return -9;
        if(vmbr[446+4]!=0x0E) return -9;  /* our partition type */

        /* Read back core.img sector 1 and verify LBA patch took */
        uint8_t vcore[512];
        if(!inst_disk_read(disk_idx,1,vcore)) return -9;
        /* The blocklist start LBA is the 64-bit field at 0x1F4..0x1FB.
           Low dword must be 1, high dword must be 0.                   */
        uint32_t blk_lba_lo=(uint32_t)vcore[0x1F4]
                           |((uint32_t)vcore[0x1F5]<<8)
                           |((uint32_t)vcore[0x1F6]<<16)
                           |((uint32_t)vcore[0x1F7]<<24);
        uint32_t blk_lba_hi=(uint32_t)vcore[0x1F8]
                           |((uint32_t)vcore[0x1F9]<<8)
                           |((uint32_t)vcore[0x1FA]<<16)
                           |((uint32_t)vcore[0x1FB]<<24);
        if(blk_lba_lo!=2||blk_lba_hi!=0) return -10; /* patch didn't persist */

        /* Read back FAT BPB and verify our volume label */
        uint8_t vbpb[512];
        if(!inst_rd(0,vbpb)) return -9;
        if(vbpb[510]!=0x55||vbpb[511]!=0xAA) return -9;
        if(vbpb[54]!='F'||vbpb[55]!='A'||vbpb[56]!='T') return -9;
    }
    step++;

    /* ── 9: Marker ───────────────────────────────────────────────── */
    if(cb) cb(step,total,"Finalising...");
    {
        const char *m="HavenDOS " HAVENDOS_VER " installed OK\n";
        write_file(0,"HAVENDOS","TXT",(const uint8_t*)m,(uint32_t)strlen(m));
    }
    step++;

    if(cb) cb(step,total,"Install complete!");
    return 0;
}

int havendos_check_install(void){
    extern int vfs_exists(const char*);
    return vfs_exists("havendos.txt");
}
const char *havendos_install_version_str(void){ return HAVENDOS_VER_NUM; }

/* ================================================================
   The additive installer -- see include/install_add.h
   ================================================================ */

/* The one gate. Every write the additive path makes goes through
   inst_wr() or gpt_add_partition(); this flag turns the first into a
   counter and the second is given a NULL write function, which
   fs/gpt_write.c already treats as a dry run.

   A separate "pretend" code path that reported what it thought the real
   one would do would be worth nothing: the preview would be a second
   implementation, and the second implementation is the one that is
   wrong. */
static int      s_add_dry   = 0;
static uint32_t s_add_wrote = 0;

static int add_wr(void *ctx, uint32_t part_lba, const uint8_t *buf)
{
    (void)ctx;
    s_add_wrote++;
    if(s_add_dry) return 1;
    return inst_wr(part_lba, buf);
}

/* Disk-absolute, for fs/gpt_write.c.

   THE RETURN CONVENTIONS ARE OPPOSITE and that is the whole reason these
   two functions exist. gpt_rd_fn and gpt_wr_fn return 0 for SUCCESS
   (include/gpt.h:27, and fs/gpt.c tests `!= 0` for failure);
   inst_disk_read and inst_disk_write return 1 for success. Returning one
   straight through the other makes every read look like a failure, which
   is how the first version of this was written and what
   tests/test_installadd.c caught on its first run: gpt_present() said the
   512 GB Windows disk had no GPT. shell/iso_installer.c's gpt_rd() has
   converted correctly since 0.8, with the same `? 0 : -1`. */
static int add_disk_rd(void *ctx, uint64_t lba, void *buf)
{
    return inst_disk_read((int)(long)ctx, lba, (uint8_t *)buf) ? 0 : -1;
}
static int add_disk_wr(void *ctx, uint64_t lba, const void *buf)
{
    return inst_disk_write((int)(long)ctx, lba, (const uint8_t *)buf) ? 0 : -1;
}

/* ASCII "HAVENDOS-PART-01", deliberately not Microsoft Basic Data:
   Windows enumerates basic-data partitions and would offer to format
   this one. An unrecognised type GUID is left alone. */
static const uint8_t ADD_TYPE_GUID[16] = {
    'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'
};

const char *havendos_install_add_strerror(int code)
{
    switch(code){
    case INSTADD_OK:         return "ok";
    case INSTADD_E_DISK:     return "no such disk";
    case INSTADD_E_NOGPT:    return "this disk has no GPT partition table";
    case INSTADD_E_NOSPACE:  return "no unallocated space on this disk -- "
                                    "shrink a partition in Windows first";
    case INSTADD_E_TOOSMALL: return "the free space is smaller than 16 MB";
    case INSTADD_E_GEOM:     return "no FAT16 layout fits that size";
    case INSTADD_E_ENTRY:    return "every GPT entry slot is in use";
    case INSTADD_E_GPTWRITE: return "the partition table write failed";
    case INSTADD_E_FORMAT:   return "formatting the new partition failed";
    case INSTADD_E_FILES:    return "writing into the new partition failed";
    case INSTADD_E_ESP:      return "the EFI System Partition step failed";
    case INSTADD_E_NOIMAGE:  return "this kernel carries no installable image";
    case INSTADD_E_READONLY: return "there is no write path to this disk yet";
    case INSTADD_E_LIVE:     return "a live session can preview but not write "
                                    "-- boot the \"Install to Disk\" entry";
    case INSTADD_E_VERIFY:   return "written, read back, and did not match";
    case INSTADD_E_VOLSPACE: return "the new partition cannot hold the kernel";
    case INSTADD_E_ALREADY:  return "HavenDOS is already installed on this "
                                    "disk -- boot it instead, or use "
                                    "`update` to upgrade it";
    default:                 return "unknown error";
    }
}

/* At most INSTADD_SHORT_MAX characters each -- the Notes column of the
   drive picker starts at column 42 and the screen is 80 wide. */
const char *havendos_install_add_shortstr(int code)
{
    switch(code){
    case INSTADD_OK:         return "ready";
    case INSTADD_E_DISK:     return "no such disk";
    case INSTADD_E_NOGPT:    return "no partition table";
    case INSTADD_E_NOSPACE:  return "no free space - shrink first";
    case INSTADD_E_TOOSMALL: return "free space under 16MB";
    case INSTADD_E_GEOM:     return "no layout fits that size";
    case INSTADD_E_ENTRY:    return "no free table entry";
    case INSTADD_E_GPTWRITE: return "table write failed";
    case INSTADD_E_FORMAT:   return "format failed";
    case INSTADD_E_FILES:    return "file write failed";
    case INSTADD_E_ESP:      return "EFI partition unusable";
    case INSTADD_E_NOIMAGE:  return "no installable image";
    case INSTADD_E_READONLY: return "no write path yet";
    case INSTADD_E_LIVE:     return "live session - preview only";
    case INSTADD_E_VERIFY:   return "read back wrong";
    case INSTADD_E_VOLSPACE: return "too small for the kernel";
    case INSTADD_E_ALREADY:  return "HavenDOS already installed";
    default:                 return "unknown error";
    }
}

int havendos_find_existing(int disk)
{
    gpt_part_t p[GPT_MAX_PARTS];
    int n, i, k;
    extern int inst_disk_gpt_parts(int, gpt_part_t *, int);
    extern int inst_disk_has_gpt(int);

    if(disk < 0 || disk >= inst_disk_count()) return -1;
    if(!inst_disk_has_gpt(disk)) return -1;
    n = inst_disk_gpt_parts(disk, p, GPT_MAX_PARTS);
    for(i = 0; i < (n < 0 ? 0 : n); i++){
        int same = 1;
        for(k = 0; k < 16; k++)
            if(p[i].type_guid[k] != ADD_TYPE_GUID[k]){ same = 0; break; }
        if(same) return i;
    }
    return -1;
}

int havendos_install_additive(int disk, int dry_run, inst_add_t *rep,
                              void (*cb)(int,int,const char*))
{
    int      total = 7, step = 0;
    gpt_gap_t gap;
    uint64_t  esp_first = 0, esp_sects = 0, esp_reserve = 0;
    int       need_esp = 0;
    uint64_t first, last, want;
    int      r, entry = -1, nw = 0;
    uint64_t written[8];
    uint32_t need_bytes;

    if(!rep) return INSTADD_E_DISK;
    memset(rep, 0, sizeof *rep);
    rep->disk = disk;

    if(disk < 0 || disk >= inst_disk_count())  return INSTADD_E_DISK;
    if(!havendos_install_image_available())    return INSTADD_E_NOIMAGE;
    rep->kernel_bytes = kernel_elf_size;

    /* A live session may look. It may not write -- same rule as the
       whole-disk path, and placed the same way: below every UI, so a new
       caller cannot reach the writes by forgetting to ask. The preview is
       allowed precisely because looking first is the safe thing to do and
       should not require rebooting into a different menu entry. */
    if(!dry_run){
        extern int boot_is_live(void);
        if(boot_is_live())                      return INSTADD_E_LIVE;
        extern int inst_disk_is_readonly(int);
        if(inst_disk_is_readonly(disk))         return INSTADD_E_READONLY;
    }

    /* ── 1: the table, and a gap in it ──────────────────────────── */
    if(cb) cb(step, total, "Reading the partition table...");
    {
        extern int inst_disk_has_gpt(int);
        if(!inst_disk_has_gpt(disk))            return INSTADD_E_NOGPT;
    }
    /* Before looking for a gap, because after a successful install there is
       no gap -- and "no free space" is then a true answer to the wrong
       question. The disk is not full and it is not broken; it already has
       what the user is trying to put on it. */
    if(havendos_find_existing(disk) >= 0)       return INSTADD_E_ALREADY;

    /* ── v0.8.8.8: IS THERE A BOOT PARTITION AT ALL? ─────────────
       Asked BEFORE the gap is chosen, because the answer changes how
       much of it HavenDOS may have. Until this release the install
       required an EFI System Partition that already existed, which
       on a real machine meant it required Windows. */
    {
        espmk_survey_t sv;
        int sr = espmk_survey(disk, &sv);
        if(sr == ESPMK_OK){
            rep->disk_has_windows = sv.has_windows;
            rep->disk_has_linux   = sv.has_linux;
            rep->disk_n_unused    = sv.n_unused;
            need_esp              = (sv.esp_index < 0);
        } else {
            /* A survey that will not run is not a licence to guess.
               esp_find() further down is the authority either way;
               this only decides whether to RESERVE space. */
            rep->espmk_rc = sr;
            need_esp = 0;
        }
    }
    esp_reserve = need_esp ? (uint64_t)ESPMK_WANT_MB * 2048ull : 0ull;

    r = gpt_find_free(add_disk_rd, (void *)(long)disk,
                      INSTADD_MIN_SECTORS + esp_reserve, &gap);
    if(r == GPTW_E_NOSPACE && esp_reserve){
        /* Not enough for the ESP we would like. Try the floor before
           refusing the machine outright -- a 48 MB boot partition is
           a working one, and the alternative is no install at all. */
        esp_reserve = (uint64_t)ESPMK_MIN_MB * 2048ull;
        r = gpt_find_free(add_disk_rd, (void *)(long)disk,
                          INSTADD_MIN_SECTORS + esp_reserve, &gap);
    }
    if(r == GPTW_E_NOSPACE)                     return INSTADD_E_NOSPACE;
    if(r != GPTW_OK)                            return INSTADD_E_NOGPT;

    rep->gap_first   = gap.first_lba;
    rep->gap_last    = gap.last_lba;
    rep->gap_sectors = gap.sectors;
    step++;

    /* ── 2: where exactly the partition goes ────────────────────── */
    /* Aligned up to 1 MB. The gap's own start is wherever the previous
       partition happened to end, and an unaligned partition is slower on
       every SSD and looks wrong in Windows' disk manager, which on a
       machine you have just installed a second operating system onto is
       its own kind of alarming. */
    first = (gap.first_lba + (INSTADD_ALIGN - 1)) / INSTADD_ALIGN
            * INSTADD_ALIGN;
    if(first > gap.last_lba)                    return INSTADD_E_TOOSMALL;

    /* The ESP goes at the FRONT of the gap and HavenDOS after it.
       Front rather than back because the back of a gap is where the
       next partition begins, and a boot partition that has to move
       if the gap is ever extended is a boot partition that will one
       day be moved by somebody who does not know what it is. */
    if(need_esp){
        esp_first = first;
        esp_sects = esp_reserve;
        if(esp_first + esp_sects > gap.last_lba) return INSTADD_E_TOOSMALL;
        first = (esp_first + esp_sects + (INSTADD_ALIGN - 1))
                / INSTADD_ALIGN * INSTADD_ALIGN;
        if(first > gap.last_lba)                 return INSTADD_E_TOOSMALL;
        rep->esp_new_first   = esp_first;
        rep->esp_new_sectors = esp_sects;
    }

    want = gap.last_lba - first + 1;
    if(want > INSTADD_MAX_SECTORS) want = INSTADD_MAX_SECTORS;
    /* A whole number of megabytes, so the last partial megabyte of a gap
       is left free rather than half-claimed. */
    want = want / INSTADD_ALIGN * INSTADD_ALIGN;
    if(want < INSTADD_MIN_SECTORS)              return INSTADD_E_TOOSMALL;
    last = first + want - 1;

    rep->part_first   = first;
    rep->part_last    = last;
    rep->part_sectors = want;

    if(!fat16_geom((uint32_t)want, &rep->geom)) return INSTADD_E_GEOM;
    rep->vol_free_bytes = rep->geom.clusters
                        * (uint32_t)rep->geom.sects_per_clust * 512u;

    /* What goes in it: the kernel, the menu, two directories, and the
       marker file -- each rounded up to a whole cluster, because that is
       how a filesystem spends space. */
    {
        uint32_t cb_bytes = (uint32_t)rep->geom.sects_per_clust * 512u;
        uint32_t rnd;
        need_bytes = 0;
        rnd = (rep->kernel_bytes + cb_bytes - 1) / cb_bytes * cb_bytes;
        need_bytes += rnd;
        need_bytes += cb_bytes * 4u;    /* grub.cfg, /boot, /boot/grub, marker */
        rep->vol_need_bytes = need_bytes;
        if(need_bytes > rep->vol_free_bytes)    return INSTADD_E_VOLSPACE;
    }
    step++;

    /* ── 3: the four sectors outside it ─────────────────────────── */
    /* Asked, not told: gpt_add_partition() with a NULL write function
       does every check and reports exactly which sectors it WOULD write,
       without writing them. The report carries those LBAs whether this is
       a dry run or not, so the numbers shown in the preview are the
       numbers the write then uses. */
    if(cb) cb(step, total, "Planning the partition table change...");
    r = gpt_add_partition(add_disk_rd, 0, (void *)(long)disk,
                          first, last, ADD_TYPE_GUID, "HavenDOS",
                          &entry, written, &nw);
    if(r == GPTW_E_NOSLOT)                      return INSTADD_E_ENTRY;
    if(r != GPTW_OK)                            return INSTADD_E_GPTWRITE;
    rep->gpt_entry  = entry;
    rep->n_outside  = nw > 4 ? 4 : nw;
    for(r = 0; r < rep->n_outside; r++) rep->outside[r] = written[r];
    step++;

    /* ── 4: the EFI System Partition, surveyed ──────────────────── */
    if(cb) cb(step, total, "Checking the EFI System Partition...");
    /* esp_find() mounts it read-only and surveys; then the same install
       that will run for real is run as a dry run, so the preview reports
       the refusal the write would hit rather than merely that an ESP
       exists. */
    if(need_esp){
        /* Dry run, so a preview says whether the ESP could be made
           rather than discovering it at write time -- by which point
           HavenDOS's own partition is already in the table. */
        rep->espmk_rc = espmk_create_at(disk, esp_first, esp_sects, 1,
                                        &rep->esp_new_entry);
        if(rep->espmk_rc != ESPMK_OK)           return INSTADD_E_ESPMAKE;
        /* esp_find() cannot survey a partition that does not exist
           yet, so the ESP checks below are skipped in the plan and
           done for real after it is created. */
        rep->esp_rc = ESP_OK;
    } else {
        rep->esp_rc = esp_find(disk, &rep->esp);
        if(rep->esp_rc != ESP_OK)               return INSTADD_E_ESP;
        rep->esp_rc = esp_install(disk, &rep->esp, entry, 1 /* dry run */);
        if(rep->esp_rc != ESP_OK)               return INSTADD_E_ESP;
    }
    step++;

    /* Everything above is a question. Nothing has been written.
       A dry run stops here with a complete answer. */
    rep->plan_ok = 1;
    if(dry_run){
        if(cb) cb(total, total, "Preview complete -- nothing was written.");
        return INSTADD_OK;
    }

    /* ── 5: the boot partition first, if we are making one ──────── */
    /* Before HavenDOS's own partition, so that if the ESP cannot be
       made the table still has nothing of ours in it. The reverse
       order would leave a HavenDOS partition on somebody's disk that
       nothing can boot. */
    if(need_esp){
        if(cb) cb(step, total, "Creating an EFI System Partition...");
        rep->espmk_rc = espmk_create_at(disk, esp_first, esp_sects, 0,
                                        &rep->esp_new_entry);
        if(rep->espmk_rc != ESPMK_OK)           return INSTADD_E_ESPMAKE;
        rep->esp_created = 1;
    }

    /* ── 5b: add the partition ──────────────────────────────────── */
    if(cb) cb(step, total, "Adding the partition to the table...");
    nw = 0;
    r = gpt_add_partition(add_disk_rd, add_disk_wr, (void *)(long)disk,
                          first, last, ADD_TYPE_GUID, "HavenDOS",
                          &entry, written, &nw);
    if(r != GPTW_OK)                            return INSTADD_E_GPTWRITE;
    step++;

    /* From here the volume is addressed relative to its own start. */
    s_disk         = disk;
    s_part_lba     = first;
    s_next_cluster = 2;
    s_geom         = rep->geom;
    s_add_dry      = 0;
    s_add_wrote    = 0;

    /* ── 6: format and fill ─────────────────────────────────────── */
    if(cb) cb(step, total, "Formatting the new partition...");
    if(!fat16_format(add_wr, 0, &s_geom, (uint32_t)first, "HAVENDOS"))
        return INSTADD_E_FORMAT;

    {
        uint16_t boot_clust, grub_clust;
        boot_clust = make_dir(0, "BOOT    ", 0);
        if(!boot_clust)                         return INSTADD_E_FILES;
        grub_clust = make_dir(boot_clust, "GRUB    ", boot_clust);
        if(!grub_clust)                         return INSTADD_E_FILES;

        /* The menu on the HavenDOS partition. The UEFI loader on the ESP
           has its own, which is the one the firmware reads; this one is
           here so a BIOS chainload or a manual `configfile` finds a menu
           where it expects one, and so the partition is self-describing
           if someone goes looking with another tool. */
        {
            const char *cfg =
                "set timeout=5\n"
                "set default=0\n"
                "\n"
                "menuentry \"HavenDOS " HAVENDOS_VER "\" {\n"
                "    search --no-floppy --set=root --label HAVENDOS\n"
                "    multiboot /boot/havendos.elf mode=desktop gfx=text\n"
                "    boot\n"
                "}\n";
            if(!write_file(grub_clust, "GRUB    ", "CFG",
                           (const uint8_t *)cfg, (uint32_t)strlen(cfg)))
                return INSTADD_E_FILES;
        }

        if(cb) cb(step, total, "Copying HavenDOS kernel (please wait)...");
        s_progress_cb    = cb;
        s_progress_step  = step;
        s_progress_total = total;
        s_progress_label = "Copying HavenDOS kernel...";
        r = write_file(boot_clust, "HAVENDOS", "ELF",
                       kernel_elf_data, kernel_elf_size);
        s_progress_cb = 0;
        if(!r)                                  return INSTADD_E_FILES;

        {
            const char *m = "HavenDOS " HAVENDOS_VER " installed OK\n";
            if(!write_file(0, "HAVENDOS", "TXT",
                           (const uint8_t *)m, (uint32_t)strlen(m)))
                return INSTADD_E_FILES;
        }
    }
    step++;

    /* ── 7: the boot files on the ESP ───────────────────────────── */
    /* Last, deliberately. Until this runs the machine boots exactly as it
       did before -- there is a new partition with an operating system in
       it that nothing points at yet. If any step above failed, nothing
       has been added to the boot path, and the disk still boots Windows
       the way it always did. */
    if(cb) cb(step, total, "Installing boot files on the EFI partition...");
    rep->esp_rc = esp_install(disk, &rep->esp, entry, 0);
    if(rep->esp_rc != ESP_OK)                   return INSTADD_E_ESP;

    /* ── read it back ───────────────────────────────────────────── */
    {
        uint8_t v[512];
        if(!inst_rd(0, v))                      return INSTADD_E_VERIFY;
        if(v[510] != 0x55 || v[511] != 0xAA)    return INSTADD_E_VERIFY;
        if(v[54] != 'F' || v[55] != 'A' || v[56] != 'T')
                                                return INSTADD_E_VERIFY;
        /* and that the table really says what we asked it to */
        if(!inst_disk_read(disk, 1, v))         return INSTADD_E_VERIFY;
        if(v[0] != 'E' || v[1] != 'F' || v[2] != 'I' || v[3] != ' ')
                                                return INSTADD_E_VERIFY;
    }

    if(cb) cb(total, total, "Install complete.");
    return INSTADD_OK;
}

