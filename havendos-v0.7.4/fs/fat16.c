/*
 * fat16.c — FAT16 filesystem for HavenDOS (v0.5.9.14)
 *
 * Backed by VirtIO block (drivers/virtio.c), operating on the
 * FAT16 partition that starts at sector FAT16_PART_LBA (2048,
 * matching the partition table written by the bootable-disk
 * build script — see build_disk.sh). This is the SAME partition
 * GRUB reads /boot/grub/grub.cfg and /boot/havendos.elf from,
 * so files HavenDOS writes here are what a freshly-booted disk
 * sees, and vice versa.
 *
 * Layout within the partition (standard FAT16, BPB-driven):
 *   Sector 0            : Boot sector (BPB) — read at mount,
 *                          NOT overwritten if already valid
 *                          (so GRUB's own boot code, if present,
 *                          survives a HavenDOS mount/format)
 *   Sectors 1..N         : FAT1
 *   Sectors N+1..2N      : FAT2 (mirror)
 *   Root directory        : fixed-size, after FAT2
 *   Data clusters          : after root directory
 *
 * All disk addresses below are PARTITION-RELATIVE; add
 * FAT16_PART_LBA before calling virtio_blk_read/write.
 *
 * ARM/TCG NOTE: All multi-byte reads and writes from/to disk
 * buffers (uint8_t arrays) use explicit byte-by-byte access.
 * Never cast a uint8_t* to uint16_t* or uint32_t* — on ARM
 * (UTM SE host, QEMU TCG) unaligned pointer reads/writes
 * produce silent garbage under TCG emulation.
 *
 * Subdirectories: minimal support — enough for /boot/grub to
 * exist as a real FAT16 subdirectory with its own cluster chain,
 * so externally-built disk images and HavenDOS-mounted disks
 * agree on layout. File API (read/write/list/delete) operates
 * on the root directory only, matching fat12.c's existing
 * contract used by vfs.c. Subdirectory creation is exposed via
 * fat16_mkdir() for the installer.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/virtio.h"

/* Partition start — must match build_disk.sh / sfdisk layout */
#define FAT16_PART_LBA   2048ULL

#define BYTES_PER_SECT   512

/* ---- FAT directory entry layout (32 bytes, standard FAT) ----
 * We do NOT cast disk buffers to this struct on ARM — unaligned
 * accesses on TCG are silent corruption. Instead we use the
 * byte-offset macros below for all multi-byte fields.
 * Single-byte fields (attr, name, ext) are safe to access
 * directly as uint8_t. We keep the typedef only for the
 * in-memory copy we assemble field-by-field before writing.
 */
typedef struct __attribute__((packed)) {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved[10];
    uint16_t time;
    uint16_t date;
    uint16_t start_cluster;
    uint32_t file_size;
} fat_dirent_t;

/* Byte offsets within a 32-byte directory entry */
#define DE_NAME          0
#define DE_EXT           8
#define DE_ATTR          11
#define DE_RESERVED      12   /* 10 bytes */
#define DE_TIME          22
#define DE_DATE          24
#define DE_START_CLUSTER 26   /* uint16_t */
#define DE_FILE_SIZE     28   /* uint32_t */

/* Safe byte-by-byte read/write helpers for dirent fields in a
 * uint8_t sector buffer.  'base' is the pointer to the first
 * byte of the 32-byte directory entry inside the buffer.       */
#define DE_GET_U16(base, off) \
    ((uint16_t)((base)[(off)]) | ((uint16_t)((base)[(off)+1]) << 8))
#define DE_GET_U32(base, off) \
    ((uint32_t)((base)[(off)])       | ((uint32_t)((base)[(off)+1]) <<  8) | \
     ((uint32_t)((base)[(off)+2]) << 16) | ((uint32_t)((base)[(off)+3]) << 24))
#define DE_SET_U16(base, off, val) do { \
    (base)[(off)]   = (uint8_t)((val) & 0xFF); \
    (base)[(off)+1] = (uint8_t)(((val) >> 8) & 0xFF); \
} while(0)
#define DE_SET_U32(base, off, val) do { \
    (base)[(off)]   = (uint8_t)((val) & 0xFF); \
    (base)[(off)+1] = (uint8_t)(((val) >>  8) & 0xFF); \
    (base)[(off)+2] = (uint8_t)(((val) >> 16) & 0xFF); \
    (base)[(off)+3] = (uint8_t)(((val) >> 24) & 0xFF); \
} while(0)

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20

/* ---- BPB fields we care about (read from sector 0) ---- */
static uint16_t bpb_bytes_per_sect;
static uint8_t  bpb_sects_per_clust;
static uint16_t bpb_rsvd_sects;
static uint8_t  bpb_num_fats;
static uint16_t bpb_root_ent_cnt;
static uint16_t bpb_sects_per_fat;
static uint32_t bpb_total_sects;

/* ---- Derived layout (partition-relative sector numbers) ---- */
static uint32_t fat1_start;
static uint32_t fat2_start;
static uint32_t root_start;
static uint32_t root_sects;
static uint32_t data_start;

/* FAT16 special values */
#define FAT_FREE   0x0000
#define FAT_EOF    0xFFF8
#define FAT_BAD    0xFFF7

/* ---- In-memory FAT cache ---- */
#define FAT_CACHE_MAX_SECTS 256   /* up to 128KB FAT — plenty for 64MB disk */
static uint8_t  fat_buf[FAT_CACHE_MAX_SECTS * BYTES_PER_SECT];
static int      fat_dirty  = 0;
static int      fat_loaded = 0;

static uint8_t  sector_buf[BYTES_PER_SECT];

static int      fat16_ready = 0;

/* ── Low-level partition-relative sector I/O ─────────────── */
/* ── Bus-agnostic I/O ─────────────────────────────────────────────
   fat16 can be backed by VirtIO (QEMU/UTM SE) or ATA/SATA (real PC).
   g_fat16_use_ata is set by fat16_init() after probing both buses.
   All internal code uses rd()/wr() — bus selection is invisible.    */
static int g_fat16_use_ata = 0;

extern int ata_read_sector(uint32_t lba, uint8_t *buf);
extern int ata_write_sector(uint32_t lba, const uint8_t *buf);
extern int ata_read_sector48(uint32_t lba, uint8_t *buf);
extern int ata_write_sector48(uint32_t lba, const uint8_t *buf);
extern int ata_lba48(void);

static int fat16_raw_read(uint64_t lba, uint8_t *buf){
    if(g_fat16_use_ata){
        if(ata_lba48()) return ata_read_sector48((uint32_t)lba, buf);
        return ata_read_sector((uint32_t)lba, buf);
    }
    return virtio_blk_read(lba, buf);
}
static int fat16_raw_write(uint64_t lba, const uint8_t *buf){
    if(g_fat16_use_ata){
        if(ata_lba48()) return ata_write_sector48((uint32_t)lba, buf);
        return ata_write_sector((uint32_t)lba, buf);
    }
    return virtio_blk_write(lba, buf);
}

static int rd(uint32_t part_lba, uint8_t *buf){
    return fat16_raw_read(FAT16_PART_LBA + part_lba, buf);
}
static int wr(uint32_t part_lba, const uint8_t *buf){
    return fat16_raw_write(FAT16_PART_LBA + part_lba, buf);
}

/* ── FAT16 entry get/set (2 bytes per entry, no nibble math) ── */
static uint16_t fat_get(uint16_t cluster){
    uint32_t off = (uint32_t)cluster * 2;
    return fat_buf[off] | ((uint16_t)fat_buf[off+1] << 8);
}
static void fat_set(uint16_t cluster, uint16_t val){
    uint32_t off = (uint32_t)cluster * 2;
    fat_buf[off]   = val & 0xFF;
    fat_buf[off+1] = (val >> 8) & 0xFF;
    fat_dirty = 1;
}

static int fat_load(void){
    if(fat_loaded) return 1;
    uint32_t sects = bpb_sects_per_fat;
    if(sects > FAT_CACHE_MAX_SECTS) sects = FAT_CACHE_MAX_SECTS;
    for(uint32_t i=0;i<sects;i++)
        if(!rd(fat1_start+i, fat_buf + i*BYTES_PER_SECT)) return 0;
    fat_loaded = 1; fat_dirty = 0;
    return 1;
}
static int fat_flush(void){
    if(!fat_dirty) return 1;
    uint32_t sects = bpb_sects_per_fat;
    if(sects > FAT_CACHE_MAX_SECTS) sects = FAT_CACHE_MAX_SECTS;
    for(uint32_t i=0;i<sects;i++)
        if(!wr(fat1_start+i, fat_buf + i*BYTES_PER_SECT)) return 0;
    if(bpb_num_fats>=2)
        for(uint32_t i=0;i<sects;i++)
            if(!wr(fat2_start+i, fat_buf + i*BYTES_PER_SECT)) return 0;
    fat_dirty = 0;
    return 1;
}

static uint16_t fat_alloc_cluster(void){
    /* BUG FIX: cap to loaded FAT size (FAT_CACHE_MAX_SECTS sectors).
       bpb_sects_per_fat may exceed our cache — reading beyond fat_buf
       would be an out-of-bounds access. */
    uint32_t fat_sects = bpb_sects_per_fat;
    if(fat_sects > FAT_CACHE_MAX_SECTS) fat_sects = FAT_CACHE_MAX_SECTS;
    uint32_t max = (fat_sects * BYTES_PER_SECT) / 2;
    if(max > 65535) max = 65535;
    for(uint32_t c=2;c<max;c++){
        if(fat_get((uint16_t)c)==FAT_FREE){
            fat_set((uint16_t)c, FAT_EOF);
            return (uint16_t)c;
        }
    }
    return 0;
}

static uint32_t cluster_to_lba(uint16_t cluster){
    /* BUG FIX: cluster values 0 and 1 are reserved; cluster-2 underflows
       to 0xFFFE/0xFFFF producing a massive LBA and corrupting wrong sectors.
       Callers already guard with c>=2, but belt-and-suspenders here. */
    if(cluster < 2) return 0xFFFFFFFFu;  /* sentinel — callers check rd()/wr() return */
    return data_start + ((uint32_t)(cluster-2) * bpb_sects_per_clust);
}

/* ── 8.3 filename helpers ─────────────────────────────────── */
static void name_to_83(const char *name, char out[8], char ext[3]){
    memset(out,' ',8); memset(ext,' ',3);
    int i=0;
    while(*name && *name!='.' && i<8){
        char c=*name++; if(c>='a'&&c<='z') c-=32;
        out[i++]=c;
    }
    if(*name=='.'){
        name++; i=0;
        while(*name && i<3){
            char c=*name++; if(c>='a'&&c<='z') c-=32;
            ext[i++]=c;
        }
    }
}
static void name_from_83(const char name[8], const char ext[3], char *out){
    int i=0,j=0;
    while(i<8 && name[i]!=' '){
        char c=name[i++]; if(c>='A'&&c<='Z') c+=32;
        out[j++]=c;
    }
    int has_ext=0;
    for(int k=0;k<3;k++) if(ext[k]!=' '){has_ext=1;break;}
    if(has_ext){
        out[j++]='.'; i=0;
        while(i<3 && ext[i]!=' '){
            char c=ext[i++]; if(c>='A'&&c<='Z') c+=32;
            out[j++]=c;
        }
    }
    out[j]=0;
}

/* ── Safe dirent helpers — operate on raw uint8_t sector buffer ──
 *
 * All functions below work on 'uint8_t *buf' (a 512-byte sector)
 * and address entries by byte offset (entry_index * 32).
 * Multi-byte fields are always read/written byte-by-byte via the
 * DE_GET_U16/DE_SET_U32 macros.  Never cast sector_buf to fat_dirent_t*.
 */

/* Return pointer to start of entry i within a 512-byte buf */
#define de_ptr(buf, i)  ((uint8_t *)(buf) + (i) * 32)

/* Write a complete directory entry into entry slot i in buf */
static void de_write(uint8_t *buf, int i,
                     const char n83[8], const char e83[3],
                     uint8_t attr,
                     uint16_t cluster, uint32_t size)
{
    uint8_t *d = de_ptr(buf, i);
    memset(d, 0, 32);
    memcpy(d + DE_NAME, n83, 8);
    memcpy(d + DE_EXT,  e83, 3);
    d[DE_ATTR] = attr;
    DE_SET_U16(d, DE_START_CLUSTER, cluster);
    DE_SET_U32(d, DE_FILE_SIZE,     size);
}

/* ── Root directory operations ───────────────────────────── */

/* Populate a fat_dirent_t from the raw bytes of entry i in buf */
static void de_to_struct(uint8_t *buf, int i, fat_dirent_t *out){
    uint8_t *d = de_ptr(buf, i);
    memcpy(out->name, d + DE_NAME, 8);
    memcpy(out->ext,  d + DE_EXT,  3);
    out->attr          = d[DE_ATTR];
    out->start_cluster = DE_GET_U16(d, DE_START_CLUSTER);
    out->file_size     = DE_GET_U32(d, DE_FILE_SIZE);
}

/* Write a fat_dirent_t back into entry i of buf */
static void de_from_struct(uint8_t *buf, int i, const fat_dirent_t *e){
    de_write(buf, i, e->name, e->ext, e->attr,
             e->start_cluster, e->file_size);
}

static int root_find(const char *name, fat_dirent_t *out, int *entry_idx){
    char n83[8],e83[3]; name_to_83(name,n83,e83);
    int per_sect = BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects;s++){
        if(!rd(root_start+s, sector_buf)) return -1;
        for(int i=0;i<per_sect;i++){
            uint8_t *d = de_ptr(sector_buf, i);
            uint8_t first = d[DE_NAME];
            if(first==0x00) return -1;
            if(first==0xE5) continue;
            if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
            if(memcmp(d+DE_NAME,n83,8)==0 && memcmp(d+DE_EXT,e83,3)==0){
                if(out) de_to_struct(sector_buf, i, out);
                if(entry_idx) *entry_idx=(int)(s*per_sect+i);
                return (int)(s*per_sect+i);
            }
        }
    }
    return -1;
}
static int root_alloc(fat_dirent_t *entry){
    int per_sect = BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects;s++){
        if(!rd(root_start+s, sector_buf)) return -1;
        for(int i=0;i<per_sect;i++){
            uint8_t *d = de_ptr(sector_buf, i);
            uint8_t first = d[DE_NAME];
            if(first==0x00||first==0xE5){
                de_from_struct(sector_buf, i, entry);
                if(!wr(root_start+s, sector_buf)) return -1;
                return (int)(s*per_sect+i);
            }
        }
    }
    return -1;
}
static int root_update(int entry_idx, fat_dirent_t *entry){
    int per_sect = BYTES_PER_SECT/32;
    uint32_t s=entry_idx/per_sect; int i=entry_idx%per_sect;
    if(!rd(root_start+s, sector_buf)) return 0;
    de_from_struct(sector_buf, i, entry);
    return wr(root_start+s, sector_buf);
}

static void fat_free_chain(uint16_t start){
    uint16_t c=start;
    /* BUG FIX: limit iterations to total possible clusters to break FAT loops.
       A corrupted FAT chain (e.g. cluster pointing back to itself) would
       otherwise spin forever, hanging the kernel. */
    uint32_t limit = (bpb_sects_per_fat * BYTES_PER_SECT) / 2;
    if(limit > 65535) limit = 65535;
    uint32_t steps = 0;
    while(c>=2 && c<FAT_BAD && steps < limit){
        steps++;
        uint16_t next=fat_get(c);
        fat_set(c,FAT_FREE);
        if(next>=FAT_EOF) break;
        c=next;
    }
}

/* ================================================================
   PUBLIC API — mirrors fat12.c's contract so vfs.c routes cleanly
   ================================================================ */

/*
 * fat16_init — mounts the FAT16 partition at FAT16_PART_LBA.
 * Reads the existing BPB (does NOT reformat) so a disk built by
 * build_disk.sh (mformat + GRUB) is recognised as-is.
 * Only auto-formats if MBR shows a valid FAT16 LBA partition entry.
 * A blank/unpartitioned disk returns 0 (not detected) so the ISO
 * installer can handle it rather than silently formatting it.
 * Returns 1 if ready, 0 on failure / no VirtIO disk / blank disk.
 */
int fat16_init(void){
    fat16_ready    = 0;
    g_fat16_use_ata = 0;

    /* Probe VirtIO first, then ATA/SATA.
       On QEMU/UTM SE virtio_blk_ready() is 1 and ATA is absent.
       On real PC hardware virtio_blk_ready() is 0 and ATA is present. */
    if(virtio_blk_ready()){
        g_fat16_use_ata = 0;
    } else {
        extern int ata_detected(void);
        if(!ata_detected()) return 0;   /* no disk at all */
        g_fat16_use_ata = 1;
    }

    /* ── Check MBR at LBA 0 for a valid FAT16 partition entry ──
       A blank disk has all-zero MBR — no 0x55 0xAA signature and no
       partition type byte.  If there is no valid partition table we
       return 0 so the ISO installer can handle the blank drive.
       We accept type 0x0E (FAT16 LBA) or 0x06 (FAT16).
    ─────────────────────────────────────────────────────────── */
    {
        uint8_t mbr[512];
        if(!fat16_raw_read(0, mbr)) return 0;
        int mbr_sig  = (mbr[510]==0x55 && mbr[511]==0xAA);
        uint8_t ptype = mbr[446+4];   /* first partition type byte */
        int has_fat16_part = mbr_sig && (ptype==0x0E || ptype==0x06 || ptype==0x04);
        /* Also accept a freshly installed disk that we formatted ourselves
           during a previous boot — those have our BPB sig at part LBA 0.
           Read the partition sector to check.                              */
        if(!has_fat16_part){
            /* Try reading the partition sector directly */
            uint8_t psec[512];
            if(!fat16_raw_read(FAT16_PART_LBA, psec)) return 0;
            int part_sig = (psec[510]==0x55 && psec[511]==0xAA);
            uint16_t bps2 = psec[11] | ((uint16_t)psec[12]<<8);
            /* Must have our volume label "HAVEN16" to trust it */
            int is_haven = (psec[3]=='H'&&psec[4]=='A'&&psec[5]=='V'&&
                            psec[6]=='E'&&psec[7]=='N');
            if(!(part_sig && bps2==512 && is_haven)) return 0;
            /* It's our own FAT16 — proceed to mount it below */
        }
    }

    if(!rd(0, sector_buf)) return 0;

    /* Valid FAT16 BPB always ends sector 0 with 0x55 0xAA */
    int has_sig = (sector_buf[510]==0x55 && sector_buf[511]==0xAA);
    /* Sanity: bytes-per-sector field must be 512 for us to trust it */
    uint16_t bps = sector_buf[11] | ((uint16_t)sector_buf[12]<<8);

    if(!(has_sig && bps==512)) return 0;   /* unrecognised BPB — not our disk */

    {   /* Mount existing FAT16 */
        bpb_bytes_per_sect  = bps;
        bpb_sects_per_clust = sector_buf[13];
        bpb_rsvd_sects      = sector_buf[14] | ((uint16_t)sector_buf[15]<<8);
        bpb_num_fats        = sector_buf[16];
        bpb_root_ent_cnt    = sector_buf[17] | ((uint16_t)sector_buf[18]<<8);
        bpb_sects_per_fat   = sector_buf[22] | ((uint16_t)sector_buf[23]<<8);
        uint32_t ts16 = sector_buf[19] | ((uint16_t)sector_buf[20]<<8);
        uint32_t ts32 = sector_buf[32] | ((uint32_t)sector_buf[33]<<8)
                       | ((uint32_t)sector_buf[34]<<16) | ((uint32_t)sector_buf[35]<<24);
        bpb_total_sects = ts16 ? ts16 : ts32;

        fat1_start = bpb_rsvd_sects;
        fat2_start = fat1_start + bpb_sects_per_fat;
        root_start = fat2_start + (uint32_t)(bpb_num_fats>=2 ? bpb_sects_per_fat : 0);
        root_sects = ((uint32_t)bpb_root_ent_cnt*32 + BYTES_PER_SECT-1)/BYTES_PER_SECT;
        data_start = root_start + root_sects;
    }

    if(bpb_sects_per_clust==0) bpb_sects_per_clust=1;

    fat_loaded=0;
    if(!fat_load()) return 0;

    fat16_ready = 1;
    return 1;
}

int fat16_detected(void){ return fat16_ready; }

int fat16_write(const char *name, const char *data, uint32_t size){
    if(!fat16_ready) return -1;

    fat_dirent_t entry;
    int idx = root_find(name,&entry,NULL);
    int is_new = (idx<0);

    if(!is_new){
        if(entry.start_cluster>=2) fat_free_chain(entry.start_cluster);
        entry.start_cluster=0; entry.file_size=0;
    } else {
        memset(&entry,0,sizeof(entry));
        name_to_83(name, entry.name, entry.ext);
        entry.attr = ATTR_ARCHIVE;
    }

    uint32_t remaining=size, offset=0;
    uint16_t first_cluster=0, prev_cluster=0;
    uint32_t bytes_per_clust = (uint32_t)bpb_sects_per_clust*BYTES_PER_SECT;

    while(remaining>0 || first_cluster==0){
        uint16_t c=fat_alloc_cluster();
        if(c==0) return -1;
        if(first_cluster==0) first_cluster=c;
        if(prev_cluster!=0) fat_set(prev_cluster,c);

        for(int sp=0; sp<bpb_sects_per_clust; sp++){
            uint8_t sect[BYTES_PER_SECT];
            uint32_t chunk = remaining > BYTES_PER_SECT ? BYTES_PER_SECT : remaining;
            memset(sect,0,BYTES_PER_SECT);
            if(chunk>0) memcpy(sect, data+offset, chunk);
            if(!wr(cluster_to_lba(c)+sp, sect)) return -1;
            offset += chunk;
            remaining = (remaining>chunk)?remaining-chunk:0;
            if(remaining==0 && chunk<BYTES_PER_SECT) break;
        }
        prev_cluster=c;
        if(remaining==0) break;
        (void)bytes_per_clust;
    }

    entry.start_cluster=first_cluster;
    entry.file_size=size;

    if(!fat_flush()) return -1;

    if(is_new){ if(root_alloc(&entry)<0) return -1; }
    else      { if(!root_update(idx,&entry)) return -1; }

    return (int)size;
}

int fat16_read(const char *name, char *buf, uint32_t bufsz){
    if(!fat16_ready) return -1;
    fat_dirent_t entry;
    if(root_find(name,&entry,NULL)<0) return -1;
    if(entry.start_cluster<2){ buf[0]=0; return 0; }

    uint32_t remaining = entry.file_size<bufsz ? entry.file_size : bufsz;
    uint32_t offset=0;
    uint16_t c=entry.start_cluster;

    while(c>=2 && c<FAT_EOF && remaining>0){
        for(int sp=0; sp<bpb_sects_per_clust && remaining>0; sp++){
            uint8_t sect[BYTES_PER_SECT];
            if(!rd(cluster_to_lba(c)+sp, sect)) return -1;
            uint32_t chunk = remaining>BYTES_PER_SECT?BYTES_PER_SECT:remaining;
            memcpy(buf+offset, sect, chunk);
            offset+=chunk; remaining-=chunk;
        }
        c=fat_get(c);
    }
    return (int)offset;
}

int fat16_delete(const char *name){
    if(!fat16_ready) return -1;
    fat_dirent_t entry;
    int idx=root_find(name,&entry,NULL);
    if(idx<0) return -1;
    if(entry.start_cluster>=2) fat_free_chain(entry.start_cluster);
    fat_flush();
    int per_sect=BYTES_PER_SECT/32;
    uint32_t s=idx/per_sect; int i=idx%per_sect;
    if(!rd(root_start+s, sector_buf)) return -1;
    /* Mark deleted — byte 0 of name = 0xE5 */
    de_ptr(sector_buf, i)[DE_NAME] = (uint8_t)0xE5;
    return wr(root_start+s, sector_buf) ? 0 : -1;
}

void fat16_list(void (*cb)(const char*,int,uint32_t)){
    if(!fat16_ready) return;
    char fname[16];
    int per_sect=BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects;s++){
        if(!rd(root_start+s, sector_buf)) return;
        for(int i=0;i<per_sect;i++){
            uint8_t *d = de_ptr(sector_buf, i);
            uint8_t first = d[DE_NAME];
            if(first==0x00) return;
            if(first==0xE5) continue;
            if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
            char n83[8], e83[3];
            memcpy(n83, d+DE_NAME, 8);
            memcpy(e83, d+DE_EXT,  3);
            name_from_83(n83, e83, fname);
            int is_dir = (d[DE_ATTR] & ATTR_DIRECTORY) ? 1 : 0;
            uint32_t fsize = DE_GET_U32(d, DE_FILE_SIZE);
            cb(fname, is_dir, fsize);
        }
    }
}

int fat16_exists(const char *name){
    return fat16_ready && root_find(name,NULL,NULL)>=0;
}

int fat16_rename(const char *old, const char *newname){
    if(!fat16_ready) return -1;
    fat_dirent_t entry;
    int idx=root_find(old,&entry,NULL);
    if(idx<0) return -1;
    name_to_83(newname, entry.name, entry.ext);
    return root_update(idx,&entry) ? 0 : -1;
}

/*
 * fat16_mkdir — create a top-level directory entry in root.
 * Minimal: allocates one cluster, writes "." and ".." entries.
 * Used by the installer to ensure /boot, /boot/grub exist when
 * HavenDOS itself is asked to lay out a fresh disk.
 */
int fat16_mkdir(const char *name){
    if(!fat16_ready) return -1;
    if(root_find(name,NULL,NULL)>=0) return 0; /* already exists */

    uint16_t c = fat_alloc_cluster();
    if(c==0) return -1;

    /* Write "." and ".." into the new directory's cluster.
     * Use de_write() — never cast the buffer to fat_dirent_t*
     * (unaligned struct writes are silent corruption on ARM/TCG). */
    uint8_t sect[BYTES_PER_SECT];
    memset(sect, 0, BYTES_PER_SECT);
    /* Entry 0: "."  — points to self */
    de_write(sect, 0,
             ".       ", "   ", ATTR_DIRECTORY, c, 0);
    /* Entry 1: ".." — points to root (cluster 0) */
    de_write(sect, 1,
             "..      ", "   ", ATTR_DIRECTORY, 0, 0);
    if(!wr(cluster_to_lba(c), sect)) return -1;

    fat_dirent_t entry;
    memset(&entry,0,sizeof(entry));
    name_to_83(name, entry.name, entry.ext);
    entry.attr = ATTR_DIRECTORY;
    entry.start_cluster = c;
    entry.file_size = 0;

    if(!fat_flush()) return -1;
    if(root_alloc(&entry)<0) return -1;
    return 0;
}

/* ── Subdirectory-aware read/write ──────────────────────────────
 *
 * fat16_write_path("/boot/havendos.elf", data, size)
 *   Finds the /boot directory cluster in root, then writes the
 *   file into that subdirectory. Only one level deep supported.
 *
 * fat16_read_path("/boot/havendos.elf", buf, bufsz)
 *   Same but reads.
 *
 * Falls back to flat root write if no slash found.
 * ─────────────────────────────────────────────────────────────── */

/* Find a named entry (dir or file) inside a directory cluster chain.
 * Returns start_cluster of found entry, 0 if not found.
 * Fills out_size if not NULL. */
static uint16_t subdir_find(uint16_t dir_clust,
                             const char *name83_8, const char *ext83_3,
                             uint32_t *out_size, uint16_t *out_prev_clust,
                             int *out_slot_sect, int *out_slot_i)
{
    uint16_t c = dir_clust;
    while(c >= 2 && c < FAT_EOF){
        uint32_t lba = cluster_to_lba(c);
        for(int sp = 0; sp < bpb_sects_per_clust; sp++){
            if(!rd(lba + sp, sector_buf)) return 0;
            int per = BYTES_PER_SECT / 32;
            for(int i = 0; i < per; i++){
                uint8_t *d = de_ptr(sector_buf, i);
                if(d[DE_NAME] == 0x00) goto done;
                if(d[DE_NAME] == 0xE5) continue;
                if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
                if(memcmp(d + DE_NAME, name83_8, 8) == 0 &&
                   memcmp(d + DE_EXT,  ext83_3,  3) == 0){
                    uint16_t sc = DE_GET_U16(d, DE_START_CLUSTER);
                    if(out_size)       *out_size       = DE_GET_U32(d, DE_FILE_SIZE);
                    if(out_prev_clust) *out_prev_clust = c;
                    if(out_slot_sect)  *out_slot_sect  = (int)(lba + sp);
                    if(out_slot_i)     *out_slot_i     = i;
                    return sc;
                }
            }
        }
        c = fat_get(c);
    }
done:
    return 0;
}

/* Allocate a new dirent slot inside a directory cluster.
 * Returns sector LBA of the slot, fills slot_i. */
static uint32_t subdir_alloc_slot(uint16_t dir_clust, int *slot_i){
    uint16_t c = dir_clust;
    while(c >= 2 && c < FAT_EOF){
        uint32_t lba = cluster_to_lba(c);
        for(int sp = 0; sp < bpb_sects_per_clust; sp++){
            if(!rd(lba + sp, sector_buf)) return 0;
            int per = BYTES_PER_SECT / 32;
            for(int i = 0; i < per; i++){
                uint8_t first = de_ptr(sector_buf, i)[DE_NAME];
                if(first == 0x00 || first == 0xE5){
                    *slot_i = i;
                    return lba + sp;
                }
            }
        }
        c = fat_get(c);
    }
    return 0;
}

int fat16_write_path(const char *path, const char *data, uint32_t size){
    if(!fat16_ready) return -1;

    /* ── Split path ── */
    const char *p = path; if(*p=='/') p++;
    const char *slash = p;
    while(*slash && *slash!='/') slash++;
    if(!*slash) return fat16_write(path, data, size);

    char dir_name[16]={0};
    int dlen=(int)(slash-p); if(dlen>15)dlen=15;
    for(int i=0;i<dlen;i++) dir_name[i]=p[i];
    const char *fname=slash+1;

    /* ── Find dir cluster — case-insensitive ── */
    char dn83[8],de83[3]; name_to_83(dir_name,dn83,de83);
    uint16_t dir_clust=0;
    {
        uint8_t rbuf[BYTES_PER_SECT];
        for(uint32_t s=0;s<root_sects&&!dir_clust;s++){
            if(!rd(root_start+s,rbuf)) return -1;
            for(int i=0;i<BYTES_PER_SECT/32;i++){
                uint8_t *d=rbuf+i*32;
                if(d[0]==0x00) goto rdone;
                if(d[0]==0xE5) continue;
                if(d[11]&ATTR_VOLUME_ID) continue;
                int m=1;
                for(int k=0;k<8;k++){
                    uint8_t a=d[k],b=(uint8_t)dn83[k];
                    if(a>='a'&&a<='z')a-=32;
                    if(b>='a'&&b<='z')b-=32;
                    if(a!=b){m=0;break;}
                }
                if(m){dir_clust=(uint16_t)(d[26]|((uint16_t)d[27]<<8));goto rdone;}
            }
        }
    }
rdone:
    if(!dir_clust) return fat16_write(fname,data,size);

    char fn83[8],fe83[3]; name_to_83(fname,fn83,fe83);

    /* ── Scan dir for existing entry — record its location ── */
    uint32_t ex_sect_lba=0; int ex_slot=-1; uint16_t ex_clust=0;
    {
        uint8_t dbuf[BYTES_PER_SECT];
        uint16_t c=dir_clust;
        while(c>=2&&c<FAT_EOF&&ex_slot<0){
            uint32_t lba=cluster_to_lba(c);
            for(int sp=0;sp<bpb_sects_per_clust&&ex_slot<0;sp++){
                if(!rd(lba+sp,dbuf)) goto scan_done;
                for(int i=0;i<BYTES_PER_SECT/32;i++){
                    uint8_t *d=dbuf+i*32;
                    if(d[0]==0x00) goto scan_done;
                    if(d[0]==0xE5) continue;
                    if(memcmp(d,fn83,8)==0&&memcmp(d+8,fe83,3)==0){
                        ex_clust=(uint16_t)(d[26]|((uint16_t)d[27]<<8));
                        ex_sect_lba=lba+sp;
                        ex_slot=i;
                        goto scan_done;
                    }
                }
            }
            c=fat_get(c);
        }
    }
scan_done:;

    /* ── Free old cluster chain ── */
    if(ex_clust>=2) fat_free_chain(ex_clust);

    /* ── Allocate new cluster chain ── */
    uint32_t remaining=size,offset=0;
    uint16_t first_c=0,prev_c=0;
    while(remaining>0||first_c==0){
        uint16_t c=fat_alloc_cluster(); if(!c) return -1;
        if(first_c==0) first_c=c;
        if(prev_c) fat_set(prev_c,c);
        prev_c=c;
        uint32_t lba=cluster_to_lba(c);
        for(int sp=0;sp<bpb_sects_per_clust;sp++){
            uint8_t sect[BYTES_PER_SECT];
            uint32_t chunk=(remaining>BYTES_PER_SECT)?BYTES_PER_SECT:remaining;
            memset(sect,0,BYTES_PER_SECT);
            if(chunk>0) memcpy(sect,data+offset,chunk);
            if(!wr(lba+sp,sect)) return -1;
            offset+=chunk; remaining-=chunk;
            if(remaining==0) break;
        }
        if(remaining==0) break;
    }
    if(!fat_flush()) return -1;

    /* ── Write dirent — update in place if exists, else find free slot ── */
    {
        uint8_t dbuf[BYTES_PER_SECT];
        uint32_t target_lba=0; int target_slot=-1;

        if(ex_slot>=0){
            /* Reuse existing slot — just update cluster+size */
            target_lba=ex_sect_lba; target_slot=ex_slot;
        } else {
            /* Find free slot in dir */
            uint16_t c=dir_clust;
            while(c>=2&&c<FAT_EOF){
                uint32_t lba=cluster_to_lba(c);
                for(int sp=0;sp<bpb_sects_per_clust;sp++){
                    if(!rd(lba+sp,dbuf)) return -1;
                    for(int i=0;i<BYTES_PER_SECT/32;i++){
                        uint8_t first=dbuf[i*32];
                        if(first==0x00||first==0xE5){
                            target_lba=lba+sp; target_slot=i; goto slot_found;
                        }
                    }
                }
                c=fat_get(c);
            }
        }
slot_found:
        if(target_slot<0) return -1;
        if(!rd(target_lba,dbuf)) return -1;
        uint8_t *d=dbuf+target_slot*32;
        memset(d,0,32);
        memcpy(d,    fn83,8);
        memcpy(d+8,  fe83,3);
        d[11]=ATTR_ARCHIVE;
        d[26]=(uint8_t)(first_c&0xFF);
        d[27]=(uint8_t)((first_c>>8)&0xFF);
        d[28]=(uint8_t)(size&0xFF);
        d[29]=(uint8_t)((size>>8)&0xFF);
        d[30]=(uint8_t)((size>>16)&0xFF);
        d[31]=(uint8_t)((size>>24)&0xFF);
        if(!wr(target_lba,dbuf)) return -1;
    }
    return (int)size;
}

int fat16_read_path(const char *path, char *buf, uint32_t bufsz){
    if(!fat16_ready) return -1;

    const char *p = path;
    if(*p == '/') p++;
    const char *slash = p;
    while(*slash && *slash != '/') slash++;

    if(!*slash) return fat16_read(path, buf, bufsz);

    char dir_name[16] = {0};
    int dlen = (int)(slash - p); if(dlen > 15) dlen = 15;
    for(int i = 0; i < dlen; i++) dir_name[i] = p[i];
    const char *fname = slash + 1;

    char dn83[8], de83_[3];
    name_to_83(dir_name, dn83, de83_);

    uint16_t dir_clust = 0;
    int per_sect = BYTES_PER_SECT / 32;
    for(uint32_t s = 0; s < root_sects && !dir_clust; s++){
        if(!rd(root_start + s, sector_buf)) return -1;
        for(int i = 0; i < per_sect; i++){
            uint8_t *d = de_ptr(sector_buf, i);
            if(d[DE_NAME] == 0x00) break;
            if(d[DE_NAME] == 0xE5) continue;
            if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
            { int _m=1; for(int _k=0;_k<8;_k++){ uint8_t _a=d[DE_NAME+_k],_b=(uint8_t)dn83[_k]; if(_a>='a'&&_a<='z')_a-=32; if(_b>='a'&&_b<='z')_b-=32; if(_a!=_b){_m=0;break;} } if(_m){ dir_clust=DE_GET_U16(d,DE_START_CLUSTER); break; } }
        }
    }

    if(!dir_clust) return -1;

    char fn83[8], fe83[3];
    name_to_83(fname, fn83, fe83);
    uint32_t fsize = 0;
    uint16_t fc = subdir_find(dir_clust, fn83, fe83, &fsize, NULL, NULL, NULL);
    if(!fc || fc < 2) return -1;

    uint32_t remaining = fsize < bufsz ? fsize : bufsz;
    uint32_t offset = 0;
    uint16_t c = fc;
    while(c >= 2 && c < FAT_EOF && remaining > 0){
        uint32_t lba = cluster_to_lba(c);
        for(int sp = 0; sp < bpb_sects_per_clust && remaining > 0; sp++){
            uint8_t sect[BYTES_PER_SECT];
            if(!rd(lba + sp, sect)) return -1;
            uint32_t chunk = remaining > BYTES_PER_SECT ? BYTES_PER_SECT : remaining;
            memcpy(buf + offset, sect, chunk);
            offset += chunk; remaining -= chunk;
        }
        c = fat_get(c);
    }
    return (int)offset;
}

/* Path-aware exists and delete — handles /dir/file one level deep */
int fat16_exists_path(const char *path){
    if(!fat16_ready) return 0;
    const char *p = path; if(*p=='/') p++;
    const char *slash = p;
    while(*slash && *slash!='/') slash++;
    if(!*slash) return fat16_exists(path);

    /* find dir cluster */
    char dn[16]={0}; int dlen=(int)(slash-p); if(dlen>15)dlen=15;
    for(int i=0;i<dlen;i++) dn[i]=p[i];
    char dn83[8],de83[3]; name_to_83(dn,dn83,de83);
    uint16_t dc=0;
    int per=BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects&&!dc;s++){
        if(!rd(root_start+s,sector_buf)) return 0;
        for(int i=0;i<per;i++){
            uint8_t *d=de_ptr(sector_buf,i);
            if(d[DE_NAME]==0x00) break;
            if(d[DE_NAME]==0xE5) continue;
            if(d[DE_ATTR]&ATTR_VOLUME_ID) continue;
            { int _m=1; for(int _k=0;_k<8;_k++){ uint8_t _a=d[DE_NAME+_k],_b=(uint8_t)dn83[_k]; if(_a>='a'&&_a<='z')_a-=32; if(_b>='a'&&_b<='z')_b-=32; if(_a!=_b){_m=0;break;} } if(_m){dc=DE_GET_U16(d,DE_START_CLUSTER);break;} }
        }
    }
    if(!dc) return 0;
    const char *fn=slash+1; char fn83[8],fe83[3]; name_to_83(fn,fn83,fe83);
    uint16_t fc=subdir_find(dc,fn83,fe83,NULL,NULL,NULL,NULL);
    return fc>=2?1:0;
}

int fat16_delete_path(const char *path){
    if(!fat16_ready) return -1;
    const char *p=path; if(*p=='/') p++;
    const char *slash=p;
    while(*slash&&*slash!='/') slash++;
    if(!*slash) return fat16_delete(path);

    char dn[16]={0}; int dlen=(int)(slash-p); if(dlen>15)dlen=15;
    for(int i=0;i<dlen;i++) dn[i]=p[i];
    char dn83[8],de83[3]; name_to_83(dn,dn83,de83);
    uint16_t dc=0;
    int per=BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects&&!dc;s++){
        if(!rd(root_start+s,sector_buf)) return -1;
        for(int i=0;i<per;i++){
            uint8_t *d=de_ptr(sector_buf,i);
            if(d[DE_NAME]==0x00) break;
            if(d[DE_NAME]==0xE5) continue;
            if(d[DE_ATTR]&ATTR_VOLUME_ID) continue;
            { int _m=1; for(int _k=0;_k<8;_k++){ uint8_t _a=d[DE_NAME+_k],_b=(uint8_t)dn83[_k]; if(_a>='a'&&_a<='z')_a-=32; if(_b>='a'&&_b<='z')_b-=32; if(_a!=_b){_m=0;break;} } if(_m){dc=DE_GET_U16(d,DE_START_CLUSTER);break;} }
        }
    }
    if(!dc) return -1;
    const char *fn=slash+1; char fn83[8],fe83[3]; name_to_83(fn,fn83,fe83);
    int slot_sect=0,slot_i=0;
    uint16_t fc=subdir_find(dc,fn83,fe83,NULL,NULL,&slot_sect,&slot_i);
    if(!fc||fc<2) return -1;
    fat_free_chain(fc);
    if(!rd((uint32_t)slot_sect,sector_buf)) return -1;
    de_ptr(sector_buf,slot_i)[DE_NAME]=0xE5; /* mark deleted */
    if(!wr((uint32_t)slot_sect,sector_buf)) return -1;
    return fat_flush()?0:-1;
}

/* Debug: dump raw root directory entries */
void fat16_debug_root(void){
    extern void vga_printf(const char *fmt, ...);
    if(!fat16_ready){ vga_printf("FAT16 not ready\n"); return; }
    vga_printf("spc=%u spf=%u root_start=%u root_sects=%u data_start=%u\n",
               (unsigned)bpb_sects_per_clust, (unsigned)bpb_sects_per_fat,
               (unsigned)root_start, (unsigned)root_sects, (unsigned)data_start);

    uint8_t rbuf[BYTES_PER_SECT];
    int per = BYTES_PER_SECT/32;
    int count = 0;
    uint16_t boot_clust = 0;

    vga_printf("Root dir:\n");
    for(uint32_t s=0; s<root_sects && s<4; s++){
        if(!rd(root_start+s, rbuf)){ vga_printf("  rd fail\n"); continue; }
        for(int i=0; i<per; i++){
            uint8_t *d = rbuf + i*32;
            if(d[0]==0x00){ vga_printf("  [%d] END\n", count); goto root_done; }
            if(d[0]==0xE5){ count++; continue; }
            uint16_t clust = (uint16_t)(d[26]|((uint16_t)d[27]<<8));
            uint32_t sz    = (uint32_t)d[28]|((uint32_t)d[29]<<8)|((uint32_t)d[30]<<16)|((uint32_t)d[31]<<24);
            char nm[9]={0}, ex[4]={0};
            for(int k=0;k<8;k++) nm[k]=(d[k]<32||d[k]>126)?'.':d[k];
            for(int k=0;k<3;k++) ex[k]=(d[11+k]<32||d[11+k]>126)?'.':d[11+k];
            vga_printf("  [%d] attr=%02X [%s.%s] clust=%u sz=%u\n",
                       count, d[11], nm, ex, (unsigned)clust, (unsigned)sz);
            /* Check for BOOT dir — case insensitive (install.c writes lowercase) */
            if((d[0]=='B'||d[0]=='b')&&(d[1]=='O'||d[1]=='o')&&
               (d[2]=='O'||d[2]=='o')&&(d[3]=='T'||d[3]=='t')&&d[4]==' ')
                boot_clust = clust;
            count++;
            if(count>16) goto root_done;
        }
    }
root_done:
    if(!boot_clust){ vga_printf("No /boot dir found.\n"); return; }

    /* Dump /boot cluster */
    uint32_t boot_lba = data_start + (uint32_t)(boot_clust-2)*bpb_sects_per_clust;
    vga_printf("/boot clust=%u lba=%u:\n", (unsigned)boot_clust, (unsigned)boot_lba);
    count=0;
    for(int sp=0; sp<bpb_sects_per_clust && sp<2; sp++){
        if(!rd(boot_lba+sp, rbuf)){ vga_printf("  rd fail sp=%d\n",sp); continue; }
        for(int i=0; i<per; i++){
            uint8_t *d = rbuf + i*32;
            if(d[0]==0x00){ vga_printf("  [%d] END\n",count); return; }
            if(d[0]==0xE5){ vga_printf("  [%d] DELETED\n",count); count++; continue; }
            uint16_t clust = (uint16_t)(d[26]|((uint16_t)d[27]<<8));
            uint32_t sz    = (uint32_t)d[28]|((uint32_t)d[29]<<8)|((uint32_t)d[30]<<16)|((uint32_t)d[31]<<24);
            char nm[9]={0}, ex[4]={0};
            for(int k=0;k<8;k++) nm[k]=(d[k]<32||d[k]>126)?'.':d[k];
            for(int k=0;k<3;k++) ex[k]=(d[11+k]<32||d[11+k]>126)?'.':d[11+k];
            vga_printf("  [%d] attr=%02X [%s.%s] clust=%u sz=%u\n",
                       count, d[11], nm, ex, (unsigned)clust, (unsigned)sz);
            count++;
            if(count>16) return;
        }
    }
}

