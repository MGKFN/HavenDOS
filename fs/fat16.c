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

#include "blkdev.h"
/* Its own header, so the compiler compares every prototype in it with
   the function here. It did not until v0.8.6.1, and fat16_list() had
   been declared with three arguments and defined with one for as long
   as both have existed -- invisible because the only caller wrote its
   own local extern rather than using the header. */
#include "fat16.h"
#include "../include/klog.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../include/virtio.h"
#include "../include/gpt.h"

/* Where the volume starts on the disk.
   
   This was a constant: 2048, the only place the whole-disk installer ever
   puts a partition. The additive installer puts one wherever the free
   space on a GPT disk happens to be -- on an 8 GB test disk that was
   11053056 -- and a constant cannot find it.

   What that cost, before it was a variable: the install completed, the
   firmware ran the loader, GRUB ran, the kernel started, fat16_init()
   looked at sector 2048, found Windows' data rather than a FAT16 BPB, and
   kernel/main.c read "no FAT16 partition" as "this machine needs
   installing" and launched the installer. Every suite was green. The
   machine booted. It booted the wrong thing.

   Default 2048, so a disk installed by the whole-disk installer mounts by
   exactly the path it always did. */
#define FAT16_DEFAULT_PART_LBA   2048ULL
static uint64_t g_part_lba = FAT16_DEFAULT_PART_LBA;

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
   This used to be a four-way switch on a local FAT16_BUS_* constant, and
   it was one of three such switches in the tree -- the installer's and
   fs/fat32.c's being the others. They held four, four and TWO buses, so
   fat32_init() could not see an NVMe or an AHCI disk at all, two releases
   after "the installer had one block layer and the filesystem had
   another" was declared fixed.

   Worse, this file's PROBE ORDER was VirtIO, ATA, AHCI, NVMe while the
   installer's registration order was VirtIO, NVMe, AHCI, ATA -- so on a
   machine where both the NVMe and the legacy ATA ports answered, the
   installer called the NVMe disk 0 and this layer bound itself to the
   ATA one, and nothing on any screen could tell you.

   Both are now fs/blkdev.c's: blk_boot_dev() answers "which disk is this
   machine's own", carrying this file's preference order and its argument,
   and blk_read/blk_write do the dispatch. What is left here is a device
   index. The comment in the version this replaces -- "The AHCI case
   exists for exactly this bug on exactly this screen. The comment was
   written and the lesson was not finished" -- is why the switch is gone
   rather than extended a fifth time.                                   */
static int g_fat16_dev = -1;

static int fat16_raw_read(uint64_t lba, uint8_t *buf){
    return blk_read(g_fat16_dev, lba, buf);
}
static int g_borrowed;      /* see fat16_borrow(), below */
static int g_borrow_rw;     /* see fat16_borrow_rw(), below */

static int fat16_raw_write(uint64_t lba, const uint8_t *buf){
    /* A borrowed volume is somebody else's. Refused here rather than in
       every caller, for the same reason fs/fat32.c takes a NULL write
       function: a refusal that depends on discipline is not one.

       v0.8.6.3: unless it was borrowed THROUGH THE OTHER DOOR.
       fat16_borrow_rw() is a separate entry point that sets g_borrow_rw,
       and it is the only thing in this file that sets it. The read-only
       borrow was not given a flag that could be turned on; `cp` had to
       ask for a different function by name. Two doors, because the
       difference between them is whether this kernel may write to a
       partition it does not own. */
    if(g_borrowed && !g_borrow_rw) return 0;
    return blk_write(g_fat16_dev, lba, buf);
}

/* v0.7.19 — declared early because root_find() and subdir_find() sit above
   its definition and both now use it.  See the note on each. */
static int n83_eq(const uint8_t *d, const char *n83, const char *e83);

static int rd(uint32_t part_lba, uint8_t *buf){
    return fat16_raw_read(g_part_lba + part_lba, buf);
}
static int wr(uint32_t part_lba, const uint8_t *buf){
    return fat16_raw_write(g_part_lba + part_lba, buf);
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

    /* v0.7.12 FIX — the cap above bounds the read against fat_buf[] but says
       nothing about the DISK.  A FAT is normally rounded up to a whole number
       of sectors, so it always describes more cluster slots than the data area
       actually contains; mformat can leave hundreds spare.  Those trailing
       entries read as 0x0000 (free), so a volume that is genuinely full hands
       back a cluster number past the last real one and cluster_to_lba() then
       points at sectors BEYOND THE END OF THE PARTITION — i.e. into whatever
       the next partition or the unallocated tail happens to be.  Bound the
       search by the real cluster count as well. */
    if(bpb_total_sects > data_start && bpb_sects_per_clust){
        uint32_t data_clusters =
            (bpb_total_sects - data_start) / bpb_sects_per_clust;
        uint32_t last = data_clusters + 2;      /* clusters are 2-based */
        if(last < max) max = last;
    }

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
/* v0.7.12 FIX — the basename loop stopped at i==8 but left `name` pointing at
   the 9th character, which is not '.', so the `if(*name=='.')` below never
   fired and the EXTENSION WAS SILENTLY DISCARDED for any name longer than
   eight characters before the dot.  "verylongname.txt" became "VERYLONG" with
   a blank extension, and so did "verylongname.bin" — two different files
   collapsing onto one directory entry, the second overwriting the first.
   Skip forward to the real dot instead of assuming we stopped on it. */
/* ── DOTFILES (v0.9.3) ────────────────────────────────────────────────
   ".shell_history" used to become a BLANK name with the extension "SHE":
   the loop below stopped at the leading dot before copying anything. Every
   dotfile the system keeps -- .accounts, .theme, .desktop, the shell
   history -- was an entry fsck.fat calls a "bad short file name" and
   offers to rename to FSCK0000.000, which is what Windows' chkdsk would do
   too, taking the accounts file with it. harness/esp_update.py found it by
   running fsck.fat on an installed FAT16 volume after a shell had been used.
   (tools/check_fat32_suite.py had named it "fs/fat16.c's dotfile bug" when
   it made sure FAT32 could not have it; FAT16 kept it.)

   A leading dot is now an underscore: ".shell_history" is _SHELL_H,
   ".accounts" is _ACCOUNT -- legal 8.3, and distinct where the old form
   was not (".bpkg_db" and ".bpkg" were both blank-dot-BPK). root_find()
   still finds a file written the old way and renames its entry in place
   the first time it is asked, so nothing a system saved before 0.9.3 is
   lost. */
static int is_dotfile(const char *n){
    return n && n[0]=='.' && n[1] && !(n[1]=='.' && !n[2]);
}
static void name_to_83(const char *name, char out[8], char ext[3]){
    memset(out,' ',8); memset(ext,' ',3);
    int i=0;
    if(is_dotfile(name)){ out[i++]='_'; name++; }
    while(*name && *name!='.' && i<8){
        char c=*name++; if(c>='a'&&c<='z') c-=32;
        out[i++]=c;
    }
    while(*name && *name!='.') name++;      /* truncated basename: find the dot */
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

/* What a dotfile's entry looked like before 0.9.3: a blank name, and the
   three characters after the dot as the extension. */
static void legacy_dot_83(const char *name, char out[8], char ext[3]){
    memset(out,' ',8); memset(ext,' ',3);
    name++;
    for(int i=0;i<3 && name[i] && name[i]!='.';i++){
        char c=name[i]; if(c>='a'&&c<='z') c-=32;
        ext[i]=c;
    }
}
static int root_find_83(const char n83[8], const char e83[3],
                        fat_dirent_t *out, int *entry_idx, uint32_t *sect_out);
static int root_find(const char *name, fat_dirent_t *out, int *entry_idx){
    char n83[8],e83[3]; name_to_83(name,n83,e83);
    uint32_t sect=0;
    int r=root_find_83(n83,e83,out,entry_idx,&sect);
    if(r>=0 || !is_dotfile(name)) return r;
    /* Not under its own name: perhaps under the pre-0.9.3 one. Found
       there, the entry is renamed in place -- once -- and the caller sees
       the file it asked for. */
    {
        char l83[8],le3[3]; legacy_dot_83(name,l83,le3);
        r=root_find_83(l83,le3,out,entry_idx,&sect);
        if(r<0) return r;
        /* sector_buf still holds the sector the entry is in. A refused
           write (a read-only borrow) leaves the old name, still found. */
        {
            uint8_t *d = de_ptr(sector_buf, r % (BYTES_PER_SECT/32));
            memcpy(d + DE_NAME, n83, 8);
            memcpy(d + DE_EXT,  e83, 3);
            if(wr(root_start+sect, sector_buf) && out){
                memcpy(out->name, n83, 8); memcpy(out->ext, e83, 3);
            }
        }
        return r;
    }
}
static int root_find_83(const char n83[8], const char e83[3],
                        fat_dirent_t *out, int *entry_idx, uint32_t *sect_out){
    int per_sect = BYTES_PER_SECT/32;
    for(uint32_t s=0;s<root_sects;s++){
        if(!rd(root_start+s, sector_buf)) return -1;
        for(int i=0;i<per_sect;i++){
            uint8_t *d = de_ptr(sector_buf, i);
            uint8_t first = d[DE_NAME];
            if(first==0x00) return -1;
            if(first==0xE5) continue;
            if(d[DE_ATTR] & ATTR_VOLUME_ID) continue;
            /* v0.7.19: was memcmp, i.e. case-sensitive.  The installer
               writes its entries in lower case ("havendos.txt") while
               name_to_83() upper-cases, so nothing the installer created
               could be found through this path. */
            if(n83_eq(d,n83,e83)){
                if(out) de_to_struct(sector_buf, i, out);
                if(entry_idx) *entry_idx=(int)(s*per_sect+i);
                if(sect_out) *sect_out=s;
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

/* ── finding the volume on a GPT disk ───────────────────────────────
   Only ever mounts a partition that is ours, and "ours" is checked twice:
   the GPT type GUID the additive installer writes, and the OEM name in
   the BPB that fs/fat16fmt.c writes. Mounting any FAT16 that happened to
   be on the disk would mean mounting somebody's data partition and
   writing a /havendos.txt into it.

   The ESP is skipped explicitly even though it is FAT32 and would fail
   the checks anyway -- it is the one partition on the disk where a
   mistake is unrecoverable, so it is excluded by name rather than by
   luck. */
static int gpt_rd_cb(void *ctx, uint64_t lba, void *buf)
{
    (void)ctx;
    /* fs/gpt.c wants 0 for success; fat16_raw_read returns 1. */
    return fat16_raw_read(lba, (uint8_t *)buf) ? 0 : -1;
}

static int find_gpt_volume(void)
{
    static const uint8_t OURS[16] = {
        'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'
    };
    gpt_part_t parts[GPT_MAX_PARTS];
    uint8_t    sec[512];
    int n, i, k;

    if(!gpt_present(gpt_rd_cb, 0)) return 0;
    n = gpt_scan(gpt_rd_cb, 0, parts, GPT_MAX_PARTS);
    if(n <= 0) return 0;

    for(i = 0; i < n; i++){
        int ours = 1;
        if(parts[i].is_esp) continue;
        for(k = 0; k < 16; k++)
            if(parts[i].type_guid[k] != OURS[k]){ ours = 0; break; }
        if(!ours) continue;
        if(parts[i].first_lba == 0) continue;

        if(!fat16_raw_read(parts[i].first_lba, sec)) continue;
        if(!(sec[510]==0x55 && sec[511]==0xAA)) continue;
        if((sec[11] | ((uint16_t)sec[12]<<8)) != 512) continue;
        /* the OEM name fs/fat16fmt.c writes */
        if(!(sec[3]=='H'&&sec[4]=='A'&&sec[5]=='V'&&sec[6]=='E'&&sec[7]=='N'))
            continue;

        g_part_lba = parts[i].first_lba;
        return 1;
    }
    return 0;
}

static int mount_at_part_lba(void);

/*
 * fat16_init — mounts the FAT16 partition at g_part_lba.
 * Reads the existing BPB (does NOT reformat) so a disk built by
 * build_disk.sh (mformat + GRUB) is recognised as-is.
 * Only auto-formats if MBR shows a valid FAT16 LBA partition entry.
 * A blank/unpartitioned disk returns 0 (not detected) so the ISO
 * installer can handle it rather than silently formatting it.
 * Returns 1 if ready, 0 on failure / no VirtIO disk / blank disk.
 */
int fat16_init(void){
    fat16_ready   = 0;

    /* Which disk is this machine's own. One question, one answer, in
       fs/blkdev.c -- where the preference order is written down with the
       argument for it, and where the ATAPI skip lives so this layer
       cannot bind itself to a DVD-ROM again (it did, on q35, and then
       reported the machine unformatted and ran the installer on a disk
       that had been installed to minutes earlier).

       blk_scan() is called here rather than assumed: fat16_init() runs
       before the installer on a cold boot, so on that path nothing else
       has built the table yet. It is idempotent. */
    if(blk_count() == 0) blk_scan();
    g_fat16_dev = blk_boot_dev();
    if(g_fat16_dev < 0) return 0;              /* no disk at all */

    /* Every call starts from the default, so a failed GPT search cannot
       leave a previous attempt's LBA behind for the MBR path to use. */
    g_part_lba = FAT16_DEFAULT_PART_LBA;

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
            if(!fat16_raw_read(g_part_lba, psec)) return 0;
            int part_sig = (psec[510]==0x55 && psec[511]==0xAA);
            uint16_t bps2 = psec[11] | ((uint16_t)psec[12]<<8);
            /* Must have our volume label "HAVEN16" to trust it */
            int is_haven = (psec[3]=='H'&&psec[4]=='A'&&psec[5]=='V'&&
                            psec[6]=='E'&&psec[7]=='N');
            if(!(part_sig && bps2==512 && is_haven)){
                /* Not at 2048. On a GPT disk it never is: the protective
                   MBR has one 0xEE entry and the real table is at LBA 1,
                   so everything above has been asking the wrong sector.
                   Look there before giving up. */
                if(!find_gpt_volume()) return 0;
            }
            /* It's our own FAT16 — proceed to mount it below */
        }
    }

    return mount_at_part_lba();
}

/* ── mounting whatever is at g_part_lba on g_fat16_dev ───────────────
   Split out of fat16_init() in v0.8.6.1 so `fat16` can point the driver
   at a volume on another disk. Everything above this in fat16_init() is
   FINDING the volume -- the MBR, the GPT, this machine's own disk --
   and this is reading it. Two callers, one body. */
static int mount_at_part_lba(void)
{
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
/* Which bus the mounted volume is actually on.  `disk` used to assume VirtIO,
   which was true when FAT16 only ever lived there; since the installer writes
   to IDE as well, saying so is the difference between a status line and a
   guess. */
int fat16_on_ata (void){
    return fat16_ready && blk_kind(g_fat16_dev) != BLK_BUS_VIRTIO;
}
/* The mounted volume's disk, as an index into fs/blkdev.c's table -- so
   `disk` and the boot log can ask blk_bus_name() for the word instead of
   being handed a number and a legend to decode. fat16_bus() returned a
   local 0/1/2/3 that only this file's constants explained, and the legend
   in the comment above it was already a bus out of date. */
int fat16_dev    (void){ return fat16_ready ? g_fat16_dev : -1; }

/* ════════════════════════════════════════════════════════════════════
   Borrowing the driver for a volume on another disk -- v0.8.6.1
   ════════════════════════════════════════════════════════════════════

   `fat16` exists because somebody typed it on the HP and got
   "command not found". The disk was registered, `disk` listed it, and
   there was no way to look at a FAT16 volume on anything but the boot
   disk.

   This driver holds ONE volume: a device index, a partition LBA, a BPB
   and a cached FAT. So looking at somebody's stick means displacing the
   system's own volume and putting it back afterwards -- which is
   exactly what fs/fat32.c's fat32_vol_mount_ro() / fat32_vol_release()
   pair does, and the same hazard: the volume most likely to turn up is
   not ours.

   ── WHY THE FAT CACHE IS NOT SAVED ──────────────────────────────────
   fat_buf is 128 KB. Copying it twice per borrow is a second 128 KB
   somewhere, and a kernel with a 255 KB heap has nowhere to put it. So
   the return path RE-MOUNTS from the saved device and LBA instead,
   which reloads the FAT from the disk it came from. Slower, and it
   cannot be subtly wrong in the way a half-restored cache can.

   ── READ ONLY AT THE DRIVER, NOT BY DISCIPLINE ──────────────────────
   g_borrowed makes fat16_raw_write() refuse before it reaches a sector,
   for the same reason fs/fat32.c is handed a NULL write function: the
   partition on the other end belongs to somebody else, and a refusal
   that depends on every caller remembering is not a refusal. */
void fat16_return(void);

static int      g_saved_dev;
static uint64_t g_saved_lba;
static int      g_saved_ready;

/* ── THE WRITABLE BORROW, AND WHAT IT REFUSES ────────────────────────
   v0.8.6.3. `cp` copies a file from the system's own volume onto a
   volume on a USB stick, and that cannot be done through a read-only
   borrow. So there is a second entry point, and everything that makes
   it safe is a REFUSAL rather than a convention:

     - the disk must be REMOVABLE. This is the one that matters. On the
       laptop this project is aimed at, disk 0 holds Windows; a writable
       borrow of a fixed disk is a write to somebody's operating system
       through a door marked "copy a file to my stick". blk_removable()
       is asked, and a fixed disk is refused with its name on screen.
     - the disk must not be read-only, and neither must the volume.
     - it must not be the boot device, even if that device somehow
       reports removable: the system's own volume is reached through the
       VFS, which knows about open files, and this does not.
     - the FAT must fit the cache -- same refusal the read-only borrow
       makes, and for a writable volume it is worse: a cluster lookup
       past the cached FAT would allocate a cluster that is already in
       use on the real volume.
     - one at a time, as before.

   Callers get a plain 0 and the klog line says which refusal fired. */
static int borrow_core(int dev, uint64_t part_lba, int writable);

int fat16_borrow(int dev, uint64_t part_lba)
{
    return borrow_core(dev, part_lba, 0);
}

int fat16_borrow_rw(int dev, uint64_t part_lba)
{
    if(g_borrowed) return 0;            /* one at a time */
    if(dev < 0) return 0;

    if(!blk_removable(dev)){
        klog(KLOG_WARN, "FAT16: refusing to write to %s disk %d -- a "
                        "writable borrow is for removable media only",
             blk_bus_name(dev), dev);
        return 0;
    }
    if(blk_readonly(dev)){
        klog(KLOG_WARN, "FAT16: %s disk %d is read-only",
             blk_bus_name(dev), dev);
        return 0;
    }
    if(dev == blk_boot_dev()){
        klog(KLOG_WARN, "FAT16: disk %d is this system's own boot disk "
                        "-- not borrowed for writing", dev);
        return 0;
    }
    return borrow_core(dev, part_lba, 1);
}

static int borrow_core(int dev, uint64_t part_lba, int writable)
{
    if(g_borrowed) return 0;            /* one at a time */
    if(dev < 0) return 0;

    g_saved_dev   = g_fat16_dev;
    g_saved_lba   = g_part_lba;
    g_saved_ready = fat16_ready;

    g_fat16_dev = dev;
    g_part_lba  = part_lba;
    fat16_ready = 0;
    fat_loaded  = 0;
    fat_dirty   = 0;
    g_borrowed  = 1;                    /* set BEFORE the mount: it
                                           reads, and a read is all it
                                           may do from here */
    g_borrow_rw = writable;             /* and the mount itself reads
                                           only, either way */

    if(!mount_at_part_lba()){
        fat16_return();
        return 0;
    }

    /* ── A FAT THAT DOES NOT FIT IS REFUSED, NOT TRUNCATED ───────────
       fat_load() clamps to FAT_CACHE_MAX_SECTS, which is safe -- it
       cannot overrun the buffer -- but it is SILENT: a volume whose FAT
       is larger loads the first 256 sectors and every cluster lookup
       past them reads whatever is left in the cache. That is a wrong
       answer presented as a right one, which is the worst shape a
       result can have.

       256 sectors is 128 KB, which is exactly a maximal FAT16 (65524
       clusters at two bytes each), so this only fires on a volume whose
       formatter rounded BPB_FATSz16 up past what the cluster count
       needs. It fires rather than guessing.

       Only on the BORROW path. fat16_init()'s clamp is long-standing
       behaviour on this machine's own volume and changing it could stop
       an installed system mounting itself -- which is not a thing to
       risk in a patch release for a case that cannot occur on a volume
       this installer created. */
    if(bpb_sects_per_fat > FAT_CACHE_MAX_SECTS){
        klog(KLOG_WARN, "FAT16: that volume's FAT is %u sectors, more "
                        "than the %u this kernel caches -- refused "
                        "rather than read wrong",
             (uint32_t)bpb_sects_per_fat, (uint32_t)FAT_CACHE_MAX_SECTS);
        fat16_return();
        return 0;
    }
    return 1;
}

void fat16_return(void)
{
    if(!g_borrowed) return;

    /* ── THE FLUSH HAPPENS HERE, OR IT MUST NOT HAPPEN AT ALL ────────
       The order of the next dozen lines is the most dangerous thing in
       v0.8.6.3, so it is written out rather than left to be read off
       the sequence.

       While a borrow is outstanding, g_fat16_dev points at the STICK.
       After the switch below it points at the machine's own disk. A FAT
       cache flushed on the wrong side of that switch writes the stick's
       allocation table over the system volume's -- which does not fail,
       does not warn, and destroys an installed HavenDOS.

       So: a writable borrow flushes FIRST, while the device still names
       the stick. fat16_write() already flushes as it goes, so this is
       the partial-failure case rather than the normal one.

       And then, whatever happened, fat_dirty is cleared BEFORE the
       device switches. A failed flush leaves the stick's table in the
       cache; carrying that past the switch is the accident this comment
       exists to prevent. The data is already lost at that point -- the
       only remaining question is whose disk it lands on, and the answer
       has to be nobody's.

       A read-only borrow has nothing of the stick's to write and takes
       the same path with nothing to do, which is what it did before
       this was written.  tests/test_fat16borrow.c asserts the order
       from the other side: it counts writes per device. */
    if(g_borrow_rw && fat_dirty){
        if(!fat_flush())
            klog(KLOG_WARN, "FAT16: the borrowed volume's FAT would not "
                            "write back -- files copied to it may be "
                            "incomplete");
    }
    fat_dirty   = 0;

    g_borrowed  = 0;
    g_borrow_rw = 0;
    g_fat16_dev = g_saved_dev;
    g_part_lba  = g_saved_lba;
    fat16_ready = 0;
    fat_loaded  = 0;
    if(g_saved_ready && g_saved_dev >= 0){
        if(!mount_at_part_lba()){
            /* The system's own volume did not come back. Said out loud
               rather than left as a silently unmounted filesystem --
               every vfs_read() after this would fail for a reason
               nothing on screen explains. */
            klog(KLOG_WARN, "FAT16: the system volume did not remount "
                            "after borrowing a disk");
        }
    }
}

int fat16_borrowed(void){ return g_borrowed; }
int fat16_borrowed_rw(void){ return g_borrowed && g_borrow_rw; }

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

    /* v0.7.12: FAT_BAD (0xFFF7) is a reserved marker, not a cluster.  The old
       bound `c<FAT_EOF` let it through, and cluster_to_lba(0xFFF7) resolves to
       a sector far outside the volume. */
    while(c>=2 && c<FAT_BAD && remaining>0){
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
                /* v0.7.19 — THE reason 'update' could not replace the kernel.
                   dir_lookup(), written for the v0.7.13 path work, compares
                   with n83_eq() and folds case; this function kept the older
                   memcmp.  So /boot was found (lower case on disk, matched
                   case-insensitively) and then havendos.elf inside it was NOT
                   (compared byte-for-byte against the upper-cased needle).

                   fat16_exists_path() therefore answered "no" for a kernel
                   that was plainly there, and fat16_write_path() went on to
                   allocate a FRESH slot -- leaving two entries in /boot,
                   "havendos.elf" and "HAVENDOS.ELF", with GRUB loading
                   whichever it reached first.  Worse than a clean failure. */
                if(n83_eq(d, name83_8, ext83_3)){
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

/* ══ v0.7.13: arbitrary-depth path resolution ════════════════════════════
 *
 * Every one of the four *_path() entry points used to do the same thing:
 * split at the FIRST slash, look the left half up in the root directory, and
 * treat everything after it as a filename.  So "HOME/kyan/notes.txt" resolved
 * to directory "HOME" and a file literally named "kyan/notes.txt", which
 * name_to_83() rendered as the 8.3 entry "KYAN/NOT.TXT" — a directory entry
 * with a SLASH in its name, sitting in the wrong directory.
 *
 * The shell builds two-level paths for every user file (HOME/<user>/...), so
 * in practice every file a user created landed somewhere it did not belong
 * under a corrupted name.  It round-tripped — write and read mangled
 * identically — which is why nothing looked broken day to day, but two files
 * whose first eight characters collide would silently overwrite each other.
 *
 * These helpers walk every component properly, at any depth, and can create
 * missing intermediate directories.  They also replace four copy-pasted
 * root-scan loops with one implementation.
 * ══════════════════════════════════════════════════════════════════════ */

/* Compare two 8.3 name fields case-insensitively. */
static int n83_eq(const uint8_t *d, const char *n83, const char *e83){
    for(int k=0;k<8;k++){
        uint8_t a=d[DE_NAME+k], b=(uint8_t)n83[k];
        if(a>='a'&&a<='z')a-=32;
        if(b>='a'&&b<='z')b-=32;
        if(a!=b) return 0;
    }
    for(int k=0;k<3;k++){
        uint8_t a=d[DE_EXT+k], b=(uint8_t)e83[k];
        if(a>='a'&&a<='z')a-=32;
        if(b>='a'&&b<='z')b-=32;
        if(a!=b) return 0;
    }
    return 1;
}

/* Look a child up inside `parent` (0 = the FAT16 root directory).
   Returns 1 when found, filling *out_clust and *out_is_dir. */
static int dir_lookup(uint16_t parent, const char *n83, const char *e83,
                      uint16_t *out_clust, int *out_is_dir)
{
    uint8_t buf[BYTES_PER_SECT];
    const int per = BYTES_PER_SECT/32;

    if(parent == 0){
        for(uint32_t s=0; s<root_sects; s++){
            if(!rd(root_start+s, buf)) return 0;
            for(int i=0;i<per;i++){
                uint8_t *d = buf + i*32;
                if(d[DE_NAME]==0x00) return 0;
                if(d[DE_NAME]==0xE5) continue;
                if(d[DE_ATTR]&ATTR_VOLUME_ID) continue;
                if(n83_eq(d,n83,e83)){
                    if(out_clust)  *out_clust = DE_GET_U16(d, DE_START_CLUSTER);
                    if(out_is_dir) *out_is_dir = (d[DE_ATTR]&ATTR_DIRECTORY)?1:0;
                    return 1;
                }
            }
        }
        return 0;
    }

    uint16_t c = parent;
    while(c>=2 && c<FAT_BAD){
        uint32_t lba = cluster_to_lba(c);
        for(int sp=0; sp<bpb_sects_per_clust; sp++){
            if(!rd(lba+sp, buf)) return 0;
            for(int i=0;i<per;i++){
                uint8_t *d = buf + i*32;
                if(d[DE_NAME]==0x00) return 0;
                if(d[DE_NAME]==0xE5) continue;
                if(d[DE_ATTR]&ATTR_VOLUME_ID) continue;
                if(n83_eq(d,n83,e83)){
                    if(out_clust)  *out_clust = DE_GET_U16(d, DE_START_CLUSTER);
                    if(out_is_dir) *out_is_dir = (d[DE_ATTR]&ATTR_DIRECTORY)?1:0;
                    return 1;
                }
            }
        }
        c = fat_get(c);
    }
    return 0;
}

/* Create directory `name` inside `parent` (0 = root).  Returns its first
   cluster, or 0 on failure.  This is what subdir_alloc_slot() was written for
   and never wired up to. */
static uint16_t dir_mkdir_at(uint16_t parent, const char *name){
    char n83[8], e83[3];
    name_to_83(name, n83, e83);

    uint16_t existing=0; int is_dir=0;
    if(dir_lookup(parent, n83, e83, &existing, &is_dir))
        return is_dir ? existing : 0;      /* a FILE is in the way */

    uint16_t c = fat_alloc_cluster();
    if(c == 0) return 0;

    /* "." and ".." — ".." of a root child is 0 by spec, not the root LBA. */
    uint8_t sect[BYTES_PER_SECT];
    memset(sect, 0, BYTES_PER_SECT);
    de_write(sect, 0, ".       ", "   ", ATTR_DIRECTORY, c,      0);
    de_write(sect, 1, "..      ", "   ", ATTR_DIRECTORY, parent, 0);
    if(!wr(cluster_to_lba(c), sect)) return 0;
    /* Zero the remaining sectors of the cluster so stale bytes are not read
       as directory entries. */
    memset(sect, 0, BYTES_PER_SECT);
    for(int sp=1; sp<bpb_sects_per_clust; sp++)
        if(!wr(cluster_to_lba(c)+sp, sect)) return 0;

    if(!fat_flush()) return 0;

    if(parent == 0){
        fat_dirent_t e;
        memset(&e, 0, sizeof(e));
        name_to_83(name, e.name, e.ext);
        e.attr = ATTR_DIRECTORY;
        e.start_cluster = c;
        e.file_size = 0;
        if(root_alloc(&e) < 0) return 0;
    } else {
        int slot = -1;
        uint32_t slba = subdir_alloc_slot(parent, &slot);
        if(!slba || slot < 0) return 0;    /* directory full — see note below */
        uint8_t dbuf[BYTES_PER_SECT];
        if(!rd(slba, dbuf)) return 0;
        de_write(dbuf, slot, n83, e83, ATTR_DIRECTORY, c, 0);
        if(!wr(slba, dbuf)) return 0;
    }
    return c;
}

/* Resolve every component of `path` except the last.
 * *out_dir receives the containing directory's cluster (0 = root) and
 * *out_leaf points at the final component within `path`.
 * create != 0 makes missing intermediate directories.
 * Returns 1 on success, 0 if a component is missing or is not a directory. */
static int path_walk(const char *path, int create,
                     uint16_t *out_dir, const char **out_leaf)
{
    const char *p = path;
    while(*p == '/') p++;

    uint16_t dir = 0;
    for(;;){
        const char *slash = p;
        while(*slash && *slash != '/') slash++;
        if(!*slash) break;                 /* p is the final component */

        int len = (int)(slash - p);
        if(len > 0){                       /* skip empty "//" components */
            char comp[16];
            int n = len > 15 ? 15 : len;
            for(int i=0;i<n;i++) comp[i] = p[i];
            comp[n] = 0;

            char n83[8], e83[3];
            name_to_83(comp, n83, e83);
            uint16_t child=0; int is_dir=0;
            if(dir_lookup(dir, n83, e83, &child, &is_dir)){
                if(!is_dir) return 0;      /* a file where a directory was asked for */
                dir = child;
            } else if(create){
                child = dir_mkdir_at(dir, comp);
                if(!child) return 0;
                dir = child;
            } else return 0;
        }
        p = slash + 1;
        while(*p == '/') p++;
    }

    if(!*p) return 0;                      /* trailing slash: no filename */
    *out_dir  = dir;
    *out_leaf = p;
    return 1;
}

int fat16_write_path(const char *path, const char *data, uint32_t size){
    if(!fat16_ready) return -1;

    /* ── Resolve the containing directory at any depth, creating missing
       intermediate directories.  Writing is the one operation where
       auto-creating the parents is right: the shell hands us
       HOME/<user>/<file> for a home directory that may not exist yet. ── */
    uint16_t dir_clust = 0;
    const char *fname  = 0;
    if(!path_walk(path, 1, &dir_clust, &fname)) return -1;
    if(dir_clust == 0) return fat16_write(fname, data, size);

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
                    /* v0.7.19: a fourth copy of the 8.3 comparison lived
                       here, also case-sensitive.  Fixing subdir_find() alone
                       was not enough: this scan is what decides whether the
                       write REPLACES an entry or allocates a new slot, so a
                       case mismatch here is exactly what produced the second
                       "HAVENDOS.ELF" beside the installer's "havendos.elf". */
                    if(n83_eq(d,fn83,fe83)){
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

    uint16_t dir_clust = 0;
    const char *fname  = 0;
    if(!path_walk(path, 0, &dir_clust, &fname)) return -1;
    if(dir_clust == 0) return fat16_read(fname, buf, bufsz);

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
    uint16_t dc=0; const char *fn=0;
    if(!path_walk(path, 0, &dc, &fn)) return 0;
    if(dc == 0) return fat16_exists(fn);
    char fn83[8],fe83[3]; name_to_83(fn,fn83,fe83);
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

