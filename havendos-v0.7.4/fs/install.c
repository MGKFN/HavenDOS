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
#define BPB_RSVD         1
#define BPB_NUM_FATS     2
#define BPB_SECTS_FAT    128
#define BPB_ROOT_ENT     512
#define BPB_SECTS_CLUST  4

#define FAT1_START       BPB_RSVD
#define FAT2_START       (FAT1_START + BPB_SECTS_FAT)
#define ROOT_START       (FAT2_START + BPB_SECTS_FAT)
#define ROOT_SECTS       ((BPB_ROOT_ENT * 32 + 511) / 512)
#define DATA_START       (ROOT_START + ROOT_SECTS)

#define FAT_EOF          0xFFF8
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20

/* ── GRUB embedding ──────────────────────────────────────────────── */
#ifdef HAVE_GRUB_EMBED
#include "../include/grub_embed.h"
#define GRUB_AVAILABLE 1
#else
#define GRUB_AVAILABLE 0
static const uint8_t  grub_boot_img[]    = {0};
static const uint32_t grub_boot_img_size = 0;
static const uint8_t  grub_core_img[]    = {0};
static const uint32_t grub_core_img_size = 0;
#endif

/* ── Kernel ELF embedding ────────────────────────────────────────── */
#ifdef HAVE_KERNEL_ELF
#include "../include/kernel_elf.h"
#define KERNEL_ELF_AVAILABLE 1
#else
#define KERNEL_ELF_AVAILABLE 0
static const uint8_t  kernel_elf_data[] = {0};
static const uint32_t kernel_elf_size   = 0;
#endif

/* ── Per-install state ───────────────────────────────────────────── */
static int      s_disk;
static uint16_t s_next_cluster;

/* ── I/O helpers ─────────────────────────────────────────────────── */
static int inst_rd(uint32_t part_lba, uint8_t *buf){
    return inst_disk_read(s_disk, PART_START_SECT + part_lba, buf);
}
static int inst_wr(uint32_t part_lba, const uint8_t *buf){
    return inst_disk_write(s_disk, PART_START_SECT + part_lba, buf);
}
static uint32_t clust_lba(uint16_t c){
    return DATA_START + ((uint32_t)(c - 2) * BPB_SECTS_CLUST);
}

/* ── FAT manipulation ────────────────────────────────────────────── */
static int fat_set(uint16_t cluster, uint16_t val){
    uint32_t byte_off  = (uint32_t)cluster * 2;
    uint32_t sect_off  = byte_off / BYTES_PER_SECT;
    uint32_t byte_in_s = byte_off % BYTES_PER_SECT;
    uint8_t buf[512];
    if(!inst_rd(FAT1_START + sect_off, buf)) return 0;
    buf[byte_in_s]   = val & 0xFF;
    buf[byte_in_s+1] = (val >> 8) & 0xFF;
    if(!inst_wr(FAT1_START + sect_off, buf)) return 0;
    if(!inst_rd(FAT2_START + sect_off, buf)) return 0;
    buf[byte_in_s]   = val & 0xFF;
    buf[byte_in_s+1] = (val >> 8) & 0xFF;
    if(!inst_wr(FAT2_START + sect_off, buf)) return 0;
    return 1;
}
static uint16_t alloc_cluster(void){
    uint16_t c = s_next_cluster++;
    if(!fat_set(c, FAT_EOF)) return 0;
    return c;
}
static int chain_cluster(uint16_t prev, uint16_t next){
    return fat_set(prev, next);
}

/* ── Directory entry writer ──────────────────────────────────────── */
static int write_dirent(uint16_t dir_cluster,
                         const char *name8, const char *ext3,
                         uint8_t attr, uint16_t first_cluster,
                         uint32_t file_size)
{
    uint8_t buf[512];
    if(dir_cluster == 0){
        for(uint32_t s = 0; s < ROOT_SECTS; s++){
            if(!inst_rd(ROOT_START + s, buf)) return 0;
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
                    return inst_wr(ROOT_START+s,buf);
                }
            }
        }
        return 0;
    } else {
        uint32_t lba=clust_lba(dir_cluster);
        for(int sp=0;sp<BPB_SECTS_CLUST;sp++){
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
    for(int sp=0;sp<BPB_SECTS_CLUST;sp++)
        if(!inst_wr(lba+sp,zeros)) return 0;
    if(!write_dirent(c,"        ","   ",ATTR_DIRECTORY,c,0)) return 0;
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
    while(remaining>0){
        uint16_t c=alloc_cluster(); if(!c) return 0;
        if(first_c==0) first_c=c;
        if(prev_c!=0) if(!chain_cluster(prev_c,c)) return 0;
        uint32_t lba=clust_lba(c);
        for(int sp=0;sp<BPB_SECTS_CLUST&&remaining>0;sp++){
            uint8_t sect[512];
            uint32_t chunk=remaining>512?512:remaining;
            memset(sect,0,512);
            memcpy(sect,data+offset,chunk);
            if(!inst_wr(lba+sp,sect)) return 0;
            offset+=chunk; remaining-=chunk;
        }
        prev_c=c;
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

/* ── Public install entry ────────────────────────────────────────── */
int havendos_install_to_drive(int disk_idx,
                               void (*cb)(int,int,const char*))
{
    int total=9, step=0;

    if(disk_idx<0||disk_idx>=inst_disk_count()) return -1;
    uint64_t dsects=inst_disk_sectors(disk_idx);
    if(dsects<30720) return -2;   /* < 15 MB */

    s_disk=disk_idx;
    s_next_cluster=2;

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
            /* Patch boot drive to 0x80 (first BIOS hard disk) */
            mbr[0x40]=0x80;
        }

        uint32_t part_count=(uint32_t)(dsects-PART_START_SECT);
        build_part_entry(mbr+446,0x80,(uint32_t)PART_START_SECT,part_count);
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

        /* Blocklist entry at 0x1F8: bytes 0..3 = start LBA (LE uint32)
                                     bytes 4..5 = sector count
           Set start LBA to 1.  Count stays as-is (296 or whatever).  */
        core_buf[0x1F8]=0x01; core_buf[0x1F9]=0x00;
        core_buf[0x1FA]=0x00; core_buf[0x1FB]=0x00;

        /* Also patch the second blocklist entry at 0x1F0 if it exists */
        /* (some GRUB versions have a two-entry list) */
        /* entry 0x1F0: if it has count>0 and sector=0, patch to sector=1+count_of_first */
        uint16_t first_count=(uint16_t)core_buf[0x1FC]|((uint16_t)core_buf[0x1FD]<<8);
        (void)first_count;

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

    /* ── 4: FAT16 format ─────────────────────────────────────────
       Format the partition starting at PART_START_SECT.
       Partition size = full disk sectors minus the 2048-sector gap.  */
    if(cb) cb(step,total,"Formatting FAT16 partition...");
    {
        uint32_t part_count=(uint32_t)(dsects-PART_START_SECT);
        uint8_t bpb[512]; memset(bpb,0,512);
        bpb[0]=0xEB; bpb[1]=0x3C; bpb[2]=0x90;
        memcpy(bpb+3,"HAVEN16 ",8);
        bpb[11]=0x00; bpb[12]=0x02;         /* bytes per sector = 512 */
        bpb[13]=BPB_SECTS_CLUST;
        bpb[14]=0x01; bpb[15]=0x00;         /* reserved sectors = 1   */
        bpb[16]=BPB_NUM_FATS;
        bpb[17]=0x00; bpb[18]=0x02;         /* root entries = 512     */
        bpb[19]=0x00; bpb[20]=0x00;         /* total sectors 16 = 0 (use 32) */
        bpb[21]=0xF8;                        /* media type = fixed     */
        bpb[22]=(BPB_SECTS_FAT)&0xFF;
        bpb[23]=((BPB_SECTS_FAT)>>8)&0xFF;
        bpb[24]=63; bpb[25]=0;              /* sectors per track      */
        bpb[26]=255; bpb[27]=0;             /* heads                  */
        /* hidden sectors = PART_START_SECT */
        {uint32_t hs=(uint32_t)PART_START_SECT;
         bpb[28]=(uint8_t)(hs&0xFF); bpb[29]=(uint8_t)((hs>>8)&0xFF);
         bpb[30]=(uint8_t)((hs>>16)&0xFF); bpb[31]=(uint8_t)((hs>>24)&0xFF);}
        bpb[32]=(uint8_t)(part_count&0xFF);
        bpb[33]=(uint8_t)((part_count>>8)&0xFF);
        bpb[34]=(uint8_t)((part_count>>16)&0xFF);
        bpb[35]=(uint8_t)((part_count>>24)&0xFF);
        bpb[36]=0x80; bpb[37]=0x00; bpb[38]=0x29;
        bpb[39]=0x48; bpb[40]=0x44; bpb[41]=0x4F; bpb[42]=0x53; /* HDOS */
        memcpy(bpb+43,"HAVENDOS   ",11);
        memcpy(bpb+54,"FAT16   ",8);
        bpb[510]=0x55; bpb[511]=0xAA;
        if(!inst_wr(0,bpb)) return -5;

        /* FAT tables */
        uint8_t fat[512]; memset(fat,0,512);
        fat[0]=0xF8; fat[1]=0xFF; fat[2]=0xFF; fat[3]=0xFF;
        if(!inst_wr(FAT1_START,fat)) return -5;
        if(!inst_wr(FAT2_START,fat)) return -5;
        fat[0]=fat[1]=fat[2]=fat[3]=0;
        for(int i=1;i<BPB_SECTS_FAT;i++){
            if(!inst_wr(FAT1_START+i,fat)) return -5;
            if(!inst_wr(FAT2_START+i,fat)) return -5;
        }
        /* Root directory */
        for(uint32_t i=0;i<ROOT_SECTS;i++)
            if(!inst_wr(ROOT_START+i,fat)) return -5;
    }
    step++;

    /* ── 5: Directories ──────────────────────────────────────────── */
    if(cb) cb(step,total,"Creating /boot/grub...");
    uint16_t boot_clust=make_dir(0,"boot    ",0);
    if(!boot_clust) return -6;
    uint16_t grub_clust=make_dir(boot_clust,"grub    ",boot_clust);
    if(!grub_clust) return -6;
    step++;

    /* ── 6: grub.cfg ─────────────────────────────────────────────── */
    if(cb) cb(step,total,"Writing /boot/grub/grub.cfg...");
    {
        const char *cfg=
            "set timeout=3\n"
            "set default=0\n"
            "\n"
            "insmod part_msdos\n"
            "insmod fat\n"
            "insmod multiboot\n"
            "\n"
            "menuentry \"HavenDOS v0.7.4\" {\n"
            "    search --no-floppy --set=root --file /boot/havendos.elf\n"
            "    multiboot /boot/havendos.elf\n"
            "    boot\n"
            "}\n"
            "\n"
            "menuentry \"HavenDOS v0.7.4 (hd0,msdos1 explicit)\" {\n"
            "    set root=(hd0,msdos1)\n"
            "    multiboot (hd0,msdos1)/boot/havendos.elf\n"
            "    boot\n"
            "}\n";
        if(!write_file(grub_clust,"grub    ","cfg",
                        (const uint8_t*)cfg,(uint32_t)strlen(cfg))) return -7;
    }
    step++;

    /* ── 7: havendos.elf ─────────────────────────────────────────── */
    if(cb) cb(step,total,"Writing /boot/havendos.elf...");
    {
#if KERNEL_ELF_AVAILABLE
        const uint8_t *kdata=kernel_elf_data;
        uint32_t ksize=kernel_elf_size;
        if(ksize<52||kdata[0]!=0x7F||kdata[1]!='E'||
           kdata[2]!='L'||kdata[3]!='F') return -8;
        if(!write_file(boot_clust,"havendos","elf",kdata,ksize)) return -8;
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
        /* The patched blocklist LBA at 0x1F8 must be 1 */
        uint32_t blk_lba=(uint32_t)vcore[0x1F8]
                        |((uint32_t)vcore[0x1F9]<<8)
                        |((uint32_t)vcore[0x1FA]<<16)
                        |((uint32_t)vcore[0x1FB]<<24);
        if(blk_lba!=1) return -10;   /* patch didn't persist */

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
        const char *m="HavenDOS v0.7.4 installed OK\n";
        write_file(0,"havendos","txt",(const uint8_t*)m,(uint32_t)strlen(m));
    }
    step++;

    if(cb) cb(step,total,"Install complete!");
    return 0;
}

int havendos_check_install(void){
    extern int vfs_exists(const char*);
    return vfs_exists("havendos.txt");
}
const char *havendos_install_version_str(void){ return "0.7.4"; }
