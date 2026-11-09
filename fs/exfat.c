/* ════════════════════════════════════════════════════════════════════
   fs/exfat.c -- exFAT, read-only.  v0.8.8.1 "Helios".

   include/exfat.h says why this exists and what it deliberately does
   not do. What is here is the four structures and the two cluster
   walks.

   ── THE THING THAT CATCHES A READER OUT ──────────────────────────────

   A file's clusters are NOT always a FAT chain. The stream extension
   entry carries GeneralSecondaryFlags, and bit 1 -- NoFatChain -- says
   the allocation is one contiguous run and the FAT holds nothing for
   it. mkfs and most writers set it for any file laid down in one go,
   which is most files on most sticks.

   So a reader that follows the FAT works on a fragmented volume and
   walks garbage on a fresh one, and a reader that assumes contiguity
   does the opposite. Both are wrong half the time, in a way that
   looks like a corrupt disk rather than a missing feature. Both forms
   are implemented here and tests/test_exfat.c has a file of each.

   ── AND A NAME IS A SET, NOT A FIELD ─────────────────────────────────

   One file is three or more 32-byte entries that must be consecutive:

       0x85  File          SecondaryCount, SetChecksum, attributes
       0xC0  Stream        NameLength, FirstCluster, DataLength, flags
       0xC1  Name          15 UTF-16 characters, repeated as needed

   SecondaryCount counts the entries after the first. A reader that
   takes the first 0xC1 and stops gets the first fifteen characters of
   every long name, which matches nothing and reads as "file not
   found" rather than as a bug.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/exfat.h"
#include "../include/string.h"
#include "../include/types.h"

#define SECT_SZ 512

/* ── entry types ─────────────────────────────────────────────────── */
#define E_INUSE        0x80u
#define E_FILE         0x85u
#define E_STREAM       0xC0u
#define E_NAME         0xC1u
#define E_BITMAP       0x81u
#define E_UPCASE       0x82u
#define E_LABEL        0x83u

#define ATTR_DIRECTORY 0x0010u

#define SECFLAG_NOFATCHAIN 0x02u

/* exFAT's FAT marks the end of a chain with 0xFFFFFFFF, and 0xFFFFFFF7
   is a bad cluster. Anything below 2 is not a data cluster. */
#define CL_FIRST       2u
#define CL_BAD         0xFFFFFFF7u
#define CL_EOF         0xFFFFFFFFu

static exfat_read_fn g_rd;
static void         *g_ctx;
static uint64_t      g_part;        /* partition start, in sectors    */
static int           g_mounted;
static int           g_why = EXFAT_OK;

static uint32_t g_fat_off, g_fat_len, g_heap_off, g_nclust, g_root;
static uint32_t g_spc;              /* sectors per cluster            */
/* log2 of the cluster size in bytes. A SHIFT, not a divisor, and the
   reason is that this is a freestanding 32-bit kernel: dividing a
   uint64_t by a uint32_t emits a call to libgcc's __udivmoddi4, which
   is not linked, and the whole kernel then fails at the LINK step.
   A native suite never sees it -- 64-bit division is free there -- so
   `make` is the only thing that catches it, and it did.
   Both values are powers of two by construction (the boot sector
   stores them as shifts), so there is nothing lost. */
static uint32_t g_clust_shift;
static uint64_t g_vol_sect;

/* ONE sector buffer, and it is this file's. fs/ shares buffers all
   over and include/kpoll.h lists this file among the reasons
   kernel_poll() must not reenter a subsystem -- fs/blkdev.c's
   blk_read() is bracketed for it. */
static uint8_t  g_sec[SECT_SZ];
/* ── and which sector is in it ───────────────────────────────────────
 * dir_next_raw() hands back one 32-byte entry at a time and read the
 * sector afresh for each -- sixteen reads of the same sector per
 * sector of directory. On a USB stick that is sixteen bulk transfers
 * where one would do, and the suite noticed it as twenty sector reads
 * to list a directory with three files in it.
 *
 * One sector, keyed by its address. Nothing in this file writes to
 * g_sec other than through rd_sect(), so there is no way for the copy
 * to go stale while mounted -- and it is dropped on mount and unmount
 * rather than trusted across a change of volume, which is the one way
 * a cache like this becomes a wrong answer instead of a slow one. */
static uint64_t g_sec_lba;
static int      g_sec_valid;

static uint16_t ld16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ld32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t ld64(const uint8_t *p){
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

int exfat_mounted(void){ return g_mounted; }
int exfat_why(void){ return g_why; }

const char *exfat_why_name(int why)
{
    switch(why){
        case EXFAT_OK:          return "ok";
        case EXFAT_ENOTEXFAT:   return "not an exFAT volume";
        case EXFAT_EREVISION:   return "an exFAT revision this reader does not know";
        case EXFAT_ESECTORSIZE: return "sectors are not 512 bytes";
        case EXFAT_EGEOMETRY:   return "the boot sector contradicts itself";
        case EXFAT_EIO:         return "the device did not answer";
        case EXFAT_ENOTMOUNTED: return "no exFAT volume is mounted";
        case EXFAT_ENOTFOUND:   return "no such file or directory";
        case EXFAT_EISDIR:      return "that is a directory";
        case EXFAT_ENOTDIR:     return "that is not a directory";
    }
    return "unknown";
}

uint32_t exfat_cluster_bytes(void){ return g_spc * SECT_SZ; }
uint32_t exfat_cluster_count(void){ return g_nclust; }
uint32_t exfat_root_cluster(void){ return g_root; }
uint64_t exfat_volume_bytes(void){ return g_vol_sect * (uint64_t)SECT_SZ; }

void exfat_unmount(void)
{
    g_mounted = 0; g_rd = 0; g_ctx = 0; g_part = 0;
    g_sec_valid = 0; g_sec_lba = 0;
    g_fat_off = g_fat_len = g_heap_off = g_nclust = g_root = 0;
    g_spc = 0; g_clust_shift = 0; g_vol_sect = 0;
}

static int rd_sect(uint64_t lba, uint8_t *buf)
{
    if(!g_rd) return 0;
    if(buf == g_sec && g_sec_valid && g_sec_lba == lba) return 1;
    if(!g_rd(g_ctx, g_part + lba, buf)) {
        if(buf == g_sec) g_sec_valid = 0;
        return 0;
    }
    if(buf == g_sec){ g_sec_lba = lba; g_sec_valid = 1; }
    return 1;
}

/* ── mount ───────────────────────────────────────────────────────────
 *
 * Every field is checked against the others rather than taken on
 * trust, and the reason is the one v0.8.6 wrote down about a disk
 * picker: a structure read from somebody's removable media is INPUT,
 * and arithmetic on an unchecked length is how a reader walks off the
 * end of a buffer on a malformed volume rather than refusing it.
 */
int exfat_mount_dev(exfat_read_fn rd, void *ctx, uint64_t part_lba)
{
    uint8_t bs[SECT_SZ];
    uint32_t bps_shift, spc_shift;
    static const char sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
    int i;

    exfat_unmount();
    g_why = EXFAT_OK;
    if(!rd){ g_why = EXFAT_EIO; return 0; }

    g_rd = rd; g_ctx = ctx; g_part = part_lba;
    if(!rd_sect(0, bs)){ g_why = EXFAT_EIO; exfat_unmount(); return 0; }

    for(i = 0; i < 8; i++)
        if((char)bs[3 + i] != sig[i]){
            g_why = EXFAT_ENOTEXFAT; exfat_unmount(); return 0;
        }

    /* FileSystemRevision: major in the high byte. 1.x is exFAT as
       everything writes it; 2.x is TexFAT, which has a second FAT and
       different semantics, and reading it as 1.x would be a plausible
       wrong answer rather than a refusal. */
    if(bs[105] != 1){ g_why = EXFAT_EREVISION; exfat_unmount(); return 0; }

    bps_shift = bs[108];
    spc_shift = bs[109];
    if(bps_shift != 9){ g_why = EXFAT_ESECTORSIZE; exfat_unmount(); return 0; }
    /* A cluster is at most 32 MB by the specification, so the two
       shifts together cannot exceed 25. Checked because g_spc is used
       as a multiplier on every cluster address below. */
    if(spc_shift > 16){ g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0; }

    g_vol_sect = ld64(bs + 72);
    g_fat_off  = ld32(bs + 80);
    g_fat_len  = ld32(bs + 84);
    g_heap_off = ld32(bs + 88);
    g_nclust   = ld32(bs + 92);
    g_root     = ld32(bs + 96);
    g_spc      = 1u << spc_shift;
    g_clust_shift = spc_shift + 9;   /* 9 = log2(512), checked above */

    if(g_fat_off == 0 || g_fat_len == 0 || g_heap_off == 0 ||
       g_nclust == 0 || g_spc == 0){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }
    /* The heap starts after the FAT, the root is a real cluster, and
       the volume is at least as long as the heap it claims. Each of
       these being false is a volume that would otherwise be read with
       arithmetic that silently wraps. */
    if(g_heap_off < g_fat_off + g_fat_len){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }
    if(g_root < CL_FIRST || g_root >= CL_FIRST + g_nclust){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }
    if(g_vol_sect < (uint64_t)g_heap_off +
                    (uint64_t)g_nclust * (uint64_t)g_spc){
        g_why = EXFAT_EGEOMETRY; exfat_unmount(); return 0;
    }

    g_mounted = 1;
    return 1;
}

/* The first sector of a cluster. Cluster 2 is the first in the heap. */
static uint64_t clust_lba(uint32_t c)
{
    return (uint64_t)g_heap_off + (uint64_t)(c - CL_FIRST) * g_spc;
}

static int clust_valid(uint32_t c)
{
    return c >= CL_FIRST && c < CL_FIRST + g_nclust;
}

/* The FAT entry for a cluster: 4 bytes, 128 per 512-byte sector. */
static int fat_next(uint32_t c, uint32_t *out)
{
    uint64_t lba = (uint64_t)g_fat_off + (c / (SECT_SZ / 4));
    if(!clust_valid(c)) return 0;
    if(!rd_sect(lba, g_sec)) return 0;
    *out = ld32(g_sec + (c % (SECT_SZ / 4)) * 4);
    return 1;
}

/* ── the two cluster walks, behind one function ──────────────────────
 *
 * `no_chain` picks which. A caller never chooses: it passes the flag
 * the stream extension entry gave it, and this decides. That is the
 * whole lesson of fs/blkdev.c and net/netdev.c one layer along -- the
 * thing that CHOOSES has to be one place a grep can see, or a second
 * walk appears somewhere else and only one of the two gets fixed.
 */
static int next_cluster(uint32_t cur, int no_chain, uint32_t *out)
{
    if(no_chain){
        uint32_t n = cur + 1;
        if(!clust_valid(n)) return 0;
        *out = n;
        return 1;
    }
    {
        uint32_t n;
        if(!fat_next(cur, &n)) return 0;
        if(n == CL_EOF || n == CL_BAD || !clust_valid(n)) return 0;
        *out = n;
        return 1;
    }
}

/* ── names ───────────────────────────────────────────────────────────
 * The 0xC1 entries hold UTF-16. This narrows to 8-bit: a character
 * above 0xFF becomes '?', which is a character the volume cannot
 * contain, so a name that was narrowed can never collide with a real
 * one. Silently dropping it could. */
static void utf16_append(char *dst, int cap, int *len, const uint8_t *p,
                         int chars)
{
    int i;
    for(i = 0; i < chars; i++){
        uint16_t w = ld16(p + i * 2);
        if(w == 0) return;
        if(*len >= cap - 1) return;
        dst[(*len)++] = (w < 0x100) ? (char)w : '?';
        dst[*len] = 0;
    }
}

static char fold(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* ASCII case-folded, because the up-case table on the volume is not
   read -- stated in include/exfat.h rather than left to be discovered
   by somebody whose filenames are not English. */
static int name_eq(const char *a, const char *b)
{
    while(*a && *b){
        if(fold(*a) != fold(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ── directory iteration ─────────────────────────────────────────── */

static int dir_start(uint32_t cluster, exfat_dir_t *d)
{
    d->cluster = cluster;
    d->sector_in = 0;
    d->off = 0;
    d->started = 1;
    d->done = 0;
    return 1;
}

/* One 32-byte entry at the cursor, advancing it. 0 at the end. */
static int dir_next_raw(exfat_dir_t *d, uint8_t *out)
{
    if(d->done || !clust_valid(d->cluster)) return 0;
    if(d->sector_in >= g_spc){
        /* A DIRECTORY is a cluster chain too, and always a FAT one:
           the NoFatChain flag lives on a file's stream entry, and the
           root has no entry at all. */
        uint32_t n;
        if(!fat_next(d->cluster, &n) || n == CL_EOF || n == CL_BAD ||
           !clust_valid(n)){ d->done = 1; return 0; }
        d->cluster = n;
        d->sector_in = 0;
        d->off = 0;
    }
    if(!rd_sect(clust_lba(d->cluster) + d->sector_in, g_sec)){
        d->done = 1; return 0;
    }
    memcpy(out, g_sec + d->off, 32);
    d->off += 32;
    if(d->off >= SECT_SZ){ d->off = 0; d->sector_in++; }
    return 1;
}

int exfat_opendir(const char *path, exfat_dir_t *d)
{
    exfat_dirent_t e;
    int r;
    if(!g_mounted) return -EXFAT_ENOTMOUNTED;
    if(!d) return -EXFAT_ENOTFOUND;

    if(!path || !path[0] || (path[0] == '/' && !path[1])){
        dir_start(g_root, d);
        return EXFAT_OK;
    }
    r = exfat_stat(path, &e);
    if(r != EXFAT_OK) return r;
    if(!e.is_dir) return -EXFAT_ENOTDIR;
    dir_start(e.first_cluster, d);
    return EXFAT_OK;
}

int exfat_readdir(exfat_dir_t *d, exfat_dirent_t *out)
{
    uint8_t ent[32];
    if(!g_mounted) return -EXFAT_ENOTMOUNTED;
    if(!d || !d->started || !out) return 0;

    while(dir_next_raw(d, ent)){
        uint8_t type = ent[0];
        int secondary, got_stream = 0, namelen = 0, nlen = 0;
        int i;

        if(type == 0x00){ d->done = 1; return 0; }   /* end of directory */
        if(!(type & E_INUSE)) continue;              /* a deleted set    */
        if(type != E_FILE) continue;                 /* bitmap, upcase,
                                                        label, unknown   */

        /* SecondaryCount is how many entries follow in THIS set. A
           reader that scans for the next 0xC0 instead would walk into
           the following file's set on a volume with a deleted entry
           in between. */
        secondary = ent[1];
        memset(out, 0, sizeof *out);
        out->is_dir = (ld16(ent + 4) & ATTR_DIRECTORY) ? 1 : 0;

        for(i = 0; i < secondary; i++){
            uint8_t sub[32];
            if(!dir_next_raw(d, sub)) return 0;
            if(sub[0] == E_STREAM){
                got_stream        = 1;
                out->no_fat_chain = (sub[1] & SECFLAG_NOFATCHAIN) ? 1 : 0;
                namelen           = sub[3];
                out->first_cluster= ld32(sub + 20);
                out->size         = ld64(sub + 24);
            } else if(sub[0] == E_NAME && got_stream){
                /* 15 UTF-16 characters per entry, and as many entries
                   as the name needs. Bounded by NameLength so a
                   malformed set cannot run the buffer over. */
                int want = namelen - nlen;
                if(want > 15) want = 15;
                if(want > 0){
                    utf16_append(out->name, (int)sizeof out->name,
                                 &nlen, sub + 2, want);
                }
            }
        }
        if(!got_stream || out->name[0] == 0) continue;
        return 1;
    }
    d->done = 1;
    return 0;
}

/* ── path lookup ─────────────────────────────────────────────────── */

static const char *path_next(const char *p, char *comp, int cap)
{
    int n = 0;
    while(*p == '/') p++;
    while(*p && *p != '/'){
        if(n < cap - 1) comp[n++] = *p;
        p++;
    }
    comp[n] = 0;
    return p;
}

int exfat_stat(const char *path, exfat_dirent_t *out)
{
    exfat_dir_t d;
    exfat_dirent_t e;
    char comp[256];
    uint32_t cur = 0;
    int have = 0;

    if(!g_mounted) return -EXFAT_ENOTMOUNTED;
    if(!path || !out) return -EXFAT_ENOTFOUND;

    cur = g_root;
    while(*path){
        path = path_next(path, comp, (int)sizeof comp);
        if(!comp[0]) break;
        dir_start(cur, &d);
        have = 0;
        while(exfat_readdir(&d, &e) == 1){
            if(name_eq(e.name, comp)){ have = 1; break; }
        }
        if(!have) return -EXFAT_ENOTFOUND;
        if(*path){
            /* more to walk, so this one has to be a directory */
            if(!e.is_dir) return -EXFAT_ENOTDIR;
            cur = e.first_cluster;
        }
    }
    if(!have) return -EXFAT_ENOTFOUND;
    *out = e;
    return EXFAT_OK;
}

/* ── reading a file ──────────────────────────────────────────────── */

int exfat_read(const char *path, uint64_t off, void *buf, uint32_t len)
{
    exfat_dirent_t e;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t cbytes, done = 0;
    uint32_t cl;
    uint64_t skip;
    int r;

    if(!g_mounted) return -EXFAT_ENOTMOUNTED;
    if(!buf || len == 0) return 0;
    r = exfat_stat(path, &e);
    if(r != EXFAT_OK) return r;
    if(e.is_dir) return -EXFAT_EISDIR;

    if(off >= e.size) return 0;                 /* past the end is not
                                                   a failure */
    if((uint64_t)len > e.size - off) len = (uint32_t)(e.size - off);

    cbytes = g_spc * SECT_SZ;
    cl = e.first_cluster;
    if(!clust_valid(cl)) return -EXFAT_EGEOMETRY;

    /* Walk to the cluster the offset lands in, by the same two-form
       walk the rest of this file uses. Shifts rather than division --
       see g_clust_shift. */
    for(skip = off >> g_clust_shift; skip > 0; skip--){
        uint32_t n;
        if(!next_cluster(cl, e.no_fat_chain, &n)) return -EXFAT_EGEOMETRY;
        cl = n;
    }

    {
        uint32_t in_cl = (uint32_t)(off & (uint64_t)(cbytes - 1));
        while(done < len){
            uint32_t sec = in_cl / SECT_SZ;
            uint32_t in_s = in_cl % SECT_SZ;
            uint32_t take = SECT_SZ - in_s;
            if(take > len - done) take = len - done;

            if(!rd_sect(clust_lba(cl) + sec, g_sec)) return -EXFAT_EIO;
            memcpy(dst + done, g_sec + in_s, take);
            done += take;
            in_cl += take;

            if(in_cl >= cbytes && done < len){
                uint32_t n;
                if(!next_cluster(cl, e.no_fat_chain, &n))
                    return -EXFAT_EGEOMETRY;
                cl = n;
                in_cl = 0;
            }
        }
    }
    return (int)done;
}
