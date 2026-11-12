/*
 * fat32.c -- FAT32 for HavenDOS.  Phase A of the 1.0 build plan.
 *
 * See include/fat32.h for why this is a separate driver from fs/fat16.c and
 * for the list of differences that actually bite.  What follows are the
 * rules this file obeys, each of which has cost somebody time before.
 *
 * ARM/TCG: every multi-byte field is read and written BYTE BY BYTE.  Never
 * cast a sector buffer to a struct pointer.  Under UTM SE's interpreter an
 * unaligned 16- or 32-bit access through a cast uint8_t* is silent garbage,
 * not a fault, which is the worst of the two outcomes.
 *
 * No 64-bit division anywhere.  A freestanding kernel does not link libgcc,
 * so __udivmoddi4 is a link error rather than a slow path.  LBAs are 64-bit
 * and are only ever ADDED to; every divide and modulo below is on a 32-bit
 * sector or cluster number.
 *
 * Every chain walk is bounded by the volume's cluster count.  A FAT that
 * loops back on itself is a corrupt volume, not a reason to hang the kernel
 * in a loop with no exit -- fs/fat16.c learned this one the hard way.
 *
 * Pure polling, no IRQ dependency: inherited from the block layer, which is
 * polled for the same reason everything else here is.
 */

#include "blkdev.h"
#include "../include/types.h"
#include "../include/string.h"
#include "../include/fat32.h"
#include "../include/gpt.h"

#define SECT_SZ        512u
#define DIRENT_SZ      32u
#define ENT_PER_SECT   (SECT_SZ / DIRENT_SZ)      /* 16 */
#define FAT_PER_SECT   (SECT_SZ / 4u)             /* 128 */

/* ---- directory entry byte offsets (never a struct over a buffer) ---- */
#define DE_NAME        0    /* 11 bytes: 8 base + 3 extension, space padded */
#define DE_ATTR        11
#define DE_NTRES       12   /* bit 3: base is lower case, bit 4: ext is    */
#define DE_CRT_TENTH   13
#define DE_CRT_TIME    14
#define DE_CRT_DATE    16
#define DE_ACC_DATE    18
#define DE_CLUS_HI     20   /* uint16 -- the half that does not exist on FAT16 */
#define DE_WRT_TIME    22
#define DE_WRT_DATE    24
#define DE_CLUS_LO     26   /* uint16 */
#define DE_SIZE        28   /* uint32 */

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F     /* RO|HID|SYS|VOL -- never a real file */

#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10

#define DE_FREE        0xE5
#define DE_END         0x00

#define CLUS_FREE      0x00000000u
#define CLUS_BAD       0x0FFFFFF7u
#define CLUS_EOC       0x0FFFFFF8u   /* >= this is end of chain */
#define CLUS_MASK      0x0FFFFFFFu   /* the low 28 bits ARE the cluster     */
#define CLUS_RESERVED  0xF0000000u   /* the top 4 are not ours to touch     */

#define F32_NAME_MAX   256           /* an LFN is at most 255 characters    */

/* ── little-endian field access, byte at a time ─────────────────────── */
static uint16_t ld16(const uint8_t *p){
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t ld32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st16(uint8_t *p, uint16_t v){
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void st32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)( v        & 0xFF); p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ── mounted volume state ───────────────────────────────────────────── */
static fat32_read_fn  g_rd;
static fat32_write_fn g_wr;
static void          *g_ctx;
static uint64_t       g_part;        /* LBA of the volume's boot sector    */
static int            g_mounted;

static uint32_t g_sects_per_clust;
static uint32_t g_rsvd_sects;
static uint32_t g_num_fats;
static uint32_t g_fat_sz;            /* sectors in ONE FAT                 */
static uint32_t g_total_sects;
static uint32_t g_root_clus;
static uint32_t g_fat_start;         /* volume-relative sector of FAT 1    */
static uint32_t g_data_start;        /* volume-relative sector of cluster 2*/
static uint32_t g_clusters;          /* count of DATA clusters             */
static uint32_t g_fsinfo_sect;       /* 0 when the volume has none         */

/* FSInfo's two numbers.  g_free_known says whether we still believe the
   count; the moment we cannot account for a change it goes to zero and the
   field is written back as "unknown" rather than as a number we guessed. */
static uint32_t g_free_count;
static uint32_t g_next_free;
static int      g_free_known;
static int      g_fsinfo_dirty;

/* ── the FAT, one sector at a time ──────────────────────────────────── */
/* fs/fat16.c caches the entire FAT in a 128 KB array.  That is viable for a
   volume with at most 65524 clusters and it is not viable here: the FAT on
   the 64 MB test fixture is 993 sectors and a 512 MB ESP's is larger.  One
   windowed sector with write-back costs a read per 128 cluster lookups and
   bounds the memory at 512 bytes. */
static uint8_t  g_fatw[SECT_SZ];
static uint32_t g_fatw_sect;         /* FAT-relative sector in the window  */
static int      g_fatw_valid;
static int      g_fatw_dirty;

static uint8_t  g_sec[SECT_SZ];      /* scratch for directory read/modify  */

/* ── block access, volume-relative ──────────────────────────────────── */
static int f32_rd(uint32_t sect, uint8_t *buf){
    if(!g_rd) return 0;
    return g_rd(g_ctx, g_part + (uint64_t)sect, buf);
}
static int f32_wr(uint32_t sect, const uint8_t *buf){
    if(!g_wr) return 0;                     /* read-only mount             */
    return g_wr(g_ctx, g_part + (uint64_t)sect, buf);
}

/* ── FAT window ─────────────────────────────────────────────────────── */
static int fatw_flush(void){
    if(!g_fatw_valid || !g_fatw_dirty) return 1;
    if(!g_wr) return 0;
    /* Every FAT gets the update, not just the first.  A volume whose two
       FATs disagree is one fsck will "repair" by picking one of them, and
       which one it picks is not something to leave to chance. */
    for(uint32_t i = 0; i < g_num_fats; i++)
        if(!f32_wr(g_fat_start + i * g_fat_sz + g_fatw_sect, g_fatw)) return 0;
    g_fatw_dirty = 0;
    return 1;
}
static int fatw_load(uint32_t fat_sect){
    if(g_fatw_valid && g_fatw_sect == fat_sect) return 1;
    if(!fatw_flush()) return 0;
    g_fatw_valid = 0;
    if(fat_sect >= g_fat_sz) return 0;
    if(!f32_rd(g_fat_start + fat_sect, g_fatw)) return 0;
    g_fatw_sect  = fat_sect;
    g_fatw_valid = 1;
    g_fatw_dirty = 0;
    return 1;
}

static int clus_valid(uint32_t c){
    return c >= 2 && c <= g_clusters + 1;
}

/* Read the FAT entry for cluster c.  Returns 1 and stores the masked value;
   returns 0 on a read error or an out-of-range cluster. */
static int fat_get(uint32_t c, uint32_t *out){
    if(c > g_clusters + 1) return 0;
    uint32_t s = c / FAT_PER_SECT;
    uint32_t o = (c % FAT_PER_SECT) * 4u;
    if(!fatw_load(s)) return 0;
    *out = ld32(g_fatw + o) & CLUS_MASK;
    return 1;
}
/* Write the FAT entry for cluster c, PRESERVING the reserved top four bits
   of whatever is already there.  They are not ours; a driver that zeroes
   them is a driver whose volumes other implementations quietly distrust. */
static int fat_set(uint32_t c, uint32_t val){
    if(c > g_clusters + 1) return 0;
    uint32_t s = c / FAT_PER_SECT;
    uint32_t o = (c % FAT_PER_SECT) * 4u;
    if(!fatw_load(s)) return 0;
    uint32_t old = ld32(g_fatw + o);
    st32(g_fatw + o, (old & CLUS_RESERVED) | (val & CLUS_MASK));
    g_fatw_dirty = 1;
    return 1;
}

static uint32_t clus_to_sect(uint32_t c){
    return g_data_start + (c - 2) * g_sects_per_clust;
}

/* ── FSInfo ─────────────────────────────────────────────────────────── */
#define FSI_LEAD_SIG   0x41615252u
#define FSI_STRUCT_SIG 0x61417272u
#define FSI_TRAIL_SIG  0xAA550000u
#define FSI_FREE_OFF   488
#define FSI_NEXT_OFF   492
#define FSI_UNKNOWN    0xFFFFFFFFu

static void fsinfo_load(void){
    g_free_count = FSI_UNKNOWN;
    g_next_free  = 2;
    g_free_known = 0;
    g_fsinfo_dirty = 0;
    if(!g_fsinfo_sect || g_fsinfo_sect >= g_rsvd_sects) return;
    if(!f32_rd(g_fsinfo_sect, g_sec)) return;
    if(ld32(g_sec + 0)   != FSI_LEAD_SIG)   return;
    if(ld32(g_sec + 484) != FSI_STRUCT_SIG) return;
    if(ld32(g_sec + 508) != FSI_TRAIL_SIG)  return;
    uint32_t fc = ld32(g_sec + FSI_FREE_OFF);
    uint32_t nf = ld32(g_sec + FSI_NEXT_OFF);
    /* Believe them only as far as they can possibly be true.  A free count
       larger than the volume has clusters is a stale structure, and acting
       on it is worse than ignoring it. */
    if(fc != FSI_UNKNOWN && fc <= g_clusters){ g_free_count = fc; g_free_known = 1; }
    if(nf != FSI_UNKNOWN && clus_valid(nf))     g_next_free  = nf;
}

static int fsinfo_store(void){
    if(!g_fsinfo_sect || !g_wr || !g_fsinfo_dirty) return 1;
    if(g_fsinfo_sect >= g_rsvd_sects) return 1;
    if(!f32_rd(g_fsinfo_sect, g_sec)) return 0;
    if(ld32(g_sec + 0)   != FSI_LEAD_SIG)   return 1;   /* no structure to keep */
    if(ld32(g_sec + 484) != FSI_STRUCT_SIG) return 1;
    /* Writing 0xFFFFFFFF is the spec's "I do not know", it is legal, and
       every implementation recomputes from the FAT when it sees one.  That
       is the correct thing to leave behind when the count was lost; what is
       NOT correct is leaving the old number there beside a FAT that no
       longer matches it. */
    st32(g_sec + FSI_FREE_OFF, g_free_known ? g_free_count : FSI_UNKNOWN);
    st32(g_sec + FSI_NEXT_OFF, clus_valid(g_next_free) ? g_next_free : FSI_UNKNOWN);
    if(!f32_wr(g_fsinfo_sect, g_sec)) return 0;
    g_fsinfo_dirty = 0;
    return 1;
}

static void free_took(uint32_t n){          /* n clusters just allocated */
    if(g_free_known){
        if(g_free_count >= n) g_free_count -= n;
        else                  g_free_known = 0;
    }
    g_fsinfo_dirty = 1;
}
static void free_gave(uint32_t n){          /* n clusters just released  */
    if(g_free_known){
        if(g_free_count + n <= g_clusters) g_free_count += n;
        else                               g_free_known = 0;
    }
    g_fsinfo_dirty = 1;
}

int fat32_flush(void){
    int ok = fatw_flush();
    if(!fsinfo_store()) ok = 0;
    return ok;
}

/* ── cluster allocation ─────────────────────────────────────────────── */
/* Allocate one free cluster, mark it end-of-chain, return it in *out.
   Starts from the FSInfo hint and wraps once; returns 0 when the volume is
   full, which callers must treat as a failure and not as cluster zero. */
static int clus_alloc(uint32_t *out){
    if(!g_wr) return 0;
    uint32_t start = clus_valid(g_next_free) ? g_next_free : 2;
    uint32_t c = start;
    uint32_t tried = 0;
    while(tried <= g_clusters){
        uint32_t v;
        if(!fat_get(c, &v)) return 0;
        if(v == CLUS_FREE){
            if(!fat_set(c, CLUS_EOC)) return 0;
            g_next_free = (c + 1 <= g_clusters + 1) ? c + 1 : 2;
            free_took(1);
            *out = c;
            return 1;
        }
        tried++;
        c++;
        if(c > g_clusters + 1) c = 2;
    }
    return 0;                               /* full */
}

static int clus_zero(uint32_t c){
    uint8_t z[SECT_SZ];
    memset(z, 0, SECT_SZ);
    uint32_t base = clus_to_sect(c);
    for(uint32_t i = 0; i < g_sects_per_clust; i++)
        if(!f32_wr(base + i, z)) return 0;
    return 1;
}

/* Release a whole chain.  Bounded: a FAT that points back into itself must
   end this loop, not the kernel. */
static int chain_free(uint32_t c){
    uint32_t steps = 0, freed = 0;
    while(clus_valid(c) && steps <= g_clusters){
        uint32_t next;
        if(!fat_get(c, &next)) return 0;
        if(!fat_set(c, CLUS_FREE)) return 0;
        freed++;
        if(c < g_next_free) g_next_free = c;
        steps++;
        if(next < 2 || next >= CLUS_BAD) break;
        c = next;
    }
    if(freed) free_gave(freed);
    return 1;
}

/* Walk to the last cluster of a chain. */
static int chain_last(uint32_t start, uint32_t *out){
    uint32_t c = start, steps = 0;
    if(!clus_valid(c)) return 0;
    for(;;){
        uint32_t next;
        if(!fat_get(c, &next)) return 0;
        if(next < 2 || next >= CLUS_BAD) break;
        if(!clus_valid(next)) break;
        c = next;
        if(++steps > g_clusters) return 0;
    }
    *out = c;
    return 1;
}

/* Append one freshly allocated cluster to a chain (or start one when
   start==0).  *added is the new cluster. */
static int chain_extend(uint32_t start, uint32_t *added){
    uint32_t nc;
    if(!clus_alloc(&nc)) return 0;
    if(clus_valid(start)){
        uint32_t last;
        if(!chain_last(start, &last)) return 0;
        if(!fat_set(last, nc)) return 0;
    }
    *added = nc;
    return 1;
}

/* ── names ──────────────────────────────────────────────────────────── */
static char up(char c){ return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char lo(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ieq(const char *a, const char *b){
    while(*a && *b){ if(up(*a) != up(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* The checksum that ties a run of LFN slots to the 8.3 entry after them.
   Defined over the raw 11-byte name field, and it is the ONLY thing that
   says a long name belongs to the short entry it precedes -- a run whose
   checksum does not match is an orphan left by some other implementation
   and must be ignored rather than used. */
static uint8_t lfn_sum(const uint8_t *n11){
    uint8_t s = 0;
    for(int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + n11[i]);
    return s;
}

/* Character slots inside a 32-byte LFN entry: 5 + 6 + 2 UCS-2 units. */
static const uint8_t LFN_OFF[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };

/* Render the 11-byte 8.3 field as a dotted name, honouring the two case
   flags Windows writes in NTRes.  "readme.md" is stored by mkfs/mcopy as
   READMEMD with both flags set and NO long name at all, so a driver that
   ignores NTRes reports it as README.MD and fails to match what the user
   typed. */
static void sname_render(const uint8_t *d, char *out){
    int j = 0;
    int lb = (d[DE_NTRES] & NTRES_LOWER_BASE) != 0;
    int le = (d[DE_NTRES] & NTRES_LOWER_EXT)  != 0;
    for(int i = 0; i < 8; i++){
        uint8_t c = d[DE_NAME + i];
        if(c == ' ') break;
        if(i == 0 && c == 0x05) c = 0xE5;   /* escaped KANJI lead byte */
        out[j++] = lb ? lo((char)c) : (char)c;
    }
    if(d[DE_NAME + 8] != ' '){
        out[j++] = '.';
        for(int i = 8; i < 11; i++){
            uint8_t c = d[DE_NAME + i];
            if(c == ' ') break;
            out[j++] = le ? lo((char)c) : (char)c;
        }
    }
    out[j] = 0;
}

static int is_83_char(char c){
    if(c >= 'A' && c <= 'Z') return 1;
    if(c >= '0' && c <= '9') return 1;
    if(c >= 'a' && c <= 'z') return 1;
    return c=='$'||c=='%'||c=='\''||c=='-'||c=='_'||c=='@'||c=='~'||c=='`'
        || c=='!'||c=='('||c==')'||c=='{'||c=='}'||c=='^'||c=='#'||c=='&';
}

/* Can this name be stored as a plain 8.3 entry with no long name beside it?
   A leading dot cannot: ".config" has an EMPTY basename, and an empty
   basename is what fs/fat16.c's name_to_83() actually produces today --
   every dotfile HavenDOS has ever written to a FAT16 volume is a directory
   entry with a blank name field and a three-letter extension.  No two of
   them collide yet, which is the only reason it has been survivable.  This
   driver does not inherit it: a name that is not 8.3-legal gets a generated
   short name and a long name beside it, which is what the name was. */
static int fits_83(const char *n, uint8_t *out11, uint8_t *ntres){
    int i = 0, base = 0, ext = 0, dots = 0;
    const char *dot = 0;
    int lower_base = 0, upper_base = 0, lower_ext = 0, upper_ext = 0;

    if(!n[0]) return 0;
    for(const char *p = n; *p; p++){
        if(*p == '.'){ dots++; dot = p; continue; }
        if(!is_83_char(*p)) return 0;
    }
    if(dots > 1) return 0;
    if(dot && dot[1] == 0) return 0;            /* trailing dot           */

    for(const char *p = n; *p && *p != '.'; p++){
        if(base >= 8) return 0;
        if(*p >= 'a' && *p <= 'z') lower_base = 1;
        if(*p >= 'A' && *p <= 'Z') upper_base = 1;
        base++;
    }
    /* THE guard, and the only one: a short name must have a non-empty
       basename.  It is what stops ".cfg" from becoming the eleven bytes
       "        CFG" -- a directory entry whose name field is blank, which
       is what fs/fat16.c's name_to_83() produces for every dotfile the
       system writes today.  Within one implementation that round-trips and
       so looks harmless; across implementations it does not, and any two
       dotfiles sharing a three-character tail become the same file.
       An earlier draft also tested n[0]=='.' up front.  That was redundant
       -- a leading dot always lands here with base==0 -- and the redundancy
       hid this line from the bug-injection harness, which reported the
       guard as untested because removing either one alone changed nothing. */
    if(base == 0) return 0;
    if(dot) for(const char *p = dot + 1; *p; p++){
        if(ext >= 3) return 0;
        if(*p >= 'a' && *p <= 'z') lower_ext = 1;
        if(*p >= 'A' && *p <= 'Z') upper_ext = 1;
        ext++;
    }
    /* Mixed case in one component cannot be expressed by the case flags. */
    if(lower_base && upper_base) return 0;
    if(lower_ext  && upper_ext)  return 0;

    memset(out11, ' ', 11);
    i = 0;
    for(const char *p = n; *p && *p != '.'; p++) out11[i++] = (uint8_t)up(*p);
    if(dot){ i = 0; for(const char *p = dot + 1; *p; p++) out11[8 + i++] = (uint8_t)up(*p); }
    *ntres = (uint8_t)((lower_base ? NTRES_LOWER_BASE : 0)
                     | (lower_ext  ? NTRES_LOWER_EXT  : 0));
    return 1;
}

/* The basis short name for a long one: strip leading dots and spaces, drop
   embedded spaces and illegal characters, upper-case, take up to 8 and the
   last extension's first 3.  The ~N tail is added by the caller once it
   knows what else is in the directory. */
static void basis_83(const char *n, uint8_t *out11){
    const char *p = n, *dot = 0;
    memset(out11, ' ', 11);
    while(*p == '.' || *p == ' ') p++;          /* ".config" -> "config"  */
    for(const char *q = p; *q; q++) if(*q == '.') dot = q;
    int i = 0;
    for(const char *q = p; *q && (!dot || q < dot); q++){
        if(*q == ' ') continue;
        if(i >= 8) break;
        out11[i++] = (uint8_t)(is_83_char(*q) ? up(*q) : '_');
    }
    if(i == 0) out11[i++] = '_';                /* never a blank basename */
    if(dot){
        int e = 0;
        for(const char *q = dot + 1; *q && e < 3; q++){
            if(*q == ' ') continue;
            out11[8 + e++] = (uint8_t)(is_83_char(*q) ? up(*q) : '_');
        }
    }
}

/* Put "~n" at the end of the basename, shortening it to fit. */
static void tilde_83(uint8_t *n11, uint32_t n){
    char dig[8];
    int nd = 0;
    if(n == 0) n = 1;
    while(n && nd < 7){ dig[nd++] = (char)('0' + (n % 10)); n /= 10; }
    int len = 8 - 1 - nd;                       /* room for '~' and digits */
    if(len < 1) len = 1;
    /* Trim the basename to len, then append ~digits. */
    int b = 0;
    while(b < 8 && n11[b] != ' ') b++;
    if(b > len) b = len;
    n11[b++] = '~';
    for(int i = nd - 1; i >= 0 && b < 8; i--) n11[b++] = (uint8_t)dig[i];
    while(b < 8) n11[b++] = ' ';
}

/* ── directory cursor and iteration ─────────────────────────────────── */
/* A position in a directory, as (cluster, sector within it, entry within
   the sector).  Directories are cluster chains on FAT32 including the root,
   so one cursor type covers both and there is no special root path for a
   bug to hide in. */
typedef struct {
    uint32_t clus;
    uint32_t sec;        /* 0 .. sects_per_clust-1 */
    int      idx;        /* 0 .. ENT_PER_SECT-1    */
} f32_cur_t;

static void cur_start(f32_cur_t *c, uint32_t dir_clus){
    c->clus = dir_clus; c->sec = 0; c->idx = 0;
}
/* Advance one entry.  Returns 1 if the cursor now points at a real slot,
   0 at the end of the chain, -1 on a read error. */
static int cur_step(f32_cur_t *c){
    c->idx++;
    if((uint32_t)c->idx < ENT_PER_SECT) return 1;
    c->idx = 0;
    c->sec++;
    if(c->sec < g_sects_per_clust) return 1;
    c->sec = 0;
    uint32_t next;
    if(!fat_get(c->clus, &next)) return -1;
    if(next < 2 || next >= CLUS_BAD || !clus_valid(next)){ c->clus = 0; return 0; }
    c->clus = next;
    return 1;
}
static int cur_read(const f32_cur_t *c, uint8_t *e32){
    if(!clus_valid(c->clus)) return 0;
    if(!f32_rd(clus_to_sect(c->clus) + c->sec, g_sec)) return 0;
    memcpy(e32, g_sec + (uint32_t)c->idx * DIRENT_SZ, DIRENT_SZ);
    return 1;
}
static int cur_write(const f32_cur_t *c, const uint8_t *e32){
    if(!clus_valid(c->clus)) return 0;
    uint32_t s = clus_to_sect(c->clus) + c->sec;
    if(!f32_rd(s, g_sec)) return 0;                 /* read-modify-write   */
    memcpy(g_sec + (uint32_t)c->idx * DIRENT_SZ, e32, DIRENT_SZ);
    return f32_wr(s, g_sec);
}
/* Patch one byte of the entry under the cursor -- used to stamp 0xE5. */
static int cur_mark_free(const f32_cur_t *c){
    if(!clus_valid(c->clus)) return 0;
    uint32_t s = clus_to_sect(c->clus) + c->sec;
    if(!f32_rd(s, g_sec)) return 0;
    g_sec[(uint32_t)c->idx * DIRENT_SZ] = DE_FREE;
    return f32_wr(s, g_sec);
}

typedef struct {
    char      name[F32_NAME_MAX];   /* long name when there is one         */
    char      sname[13];            /* the 8.3 form, always                */
    uint8_t   attr;
    uint32_t  clus;
    uint32_t  size;
    f32_cur_t at;                   /* the short entry                     */
    f32_cur_t first;                /* its first LFN slot, or == at        */
    int       nslots;               /* LFN slots + 1                       */
} f32_ent_t;

typedef struct {
    f32_cur_t cur;
    int       started;
    uint32_t  steps;
    /* LFN run being accumulated */
    char      lname[F32_NAME_MAX];
    int       lvalid;
    int       lnext;                /* the sequence number expected next   */
    uint8_t   lsum;
    int       lslots;
    f32_cur_t lfirst;
} f32_iter_t;

static void iter_begin(f32_iter_t *it, uint32_t dir_clus){
    cur_start(&it->cur, dir_clus);
    it->started = 0;
    it->steps   = 0;
    it->lvalid  = 0;
    it->lslots  = 0;
    it->lnext   = 0;
}

/* Next real entry.  1 = got one, 0 = end of directory, -1 = error. */
static int iter_next(f32_iter_t *it, f32_ent_t *out){
    uint8_t e[DIRENT_SZ];
    uint32_t limit = (g_clusters + 2) * g_sects_per_clust * ENT_PER_SECT;

    for(;;){
        if(it->started){
            int r = cur_step(&it->cur);
            if(r <= 0) return r;
        } else {
            it->started = 1;
            if(!clus_valid(it->cur.clus)) return 0;
        }
        if(++it->steps > limit) return -1;      /* a FAT loop ends here    */
        if(!cur_read(&it->cur, e)) return -1;

        uint8_t first = e[0];
        if(first == DE_END) return 0;           /* nothing beyond this     */
        if(first == DE_FREE){ it->lvalid = 0; it->lslots = 0; continue; }

        if((e[DE_ATTR] & ATTR_LFN) == ATTR_LFN){
            uint8_t ord = first;
            int n = ord & 0x3F;
            if(n < 1 || n > 20){ it->lvalid = 0; continue; }
            if(ord & 0x40){                     /* last slot = first seen  */
                it->lvalid = 1;
                it->lsum   = e[DE_CRT_TENTH];
                it->lnext  = n;
                it->lslots = 0;
                it->lfirst = it->cur;
                memset(it->lname, 0, sizeof it->lname);
                if(n * 13 < F32_NAME_MAX) it->lname[n * 13] = 0;
            }
            if(!it->lvalid) continue;
            if(n != it->lnext || e[DE_CRT_TENTH] != it->lsum){
                it->lvalid = 0;                 /* out of order or orphaned */
                continue;
            }
            int base = (n - 1) * 13;
            for(int k = 0; k < 13; k++){
                uint16_t u = ld16(e + LFN_OFF[k]);
                int pos = base + k;
                if(pos >= F32_NAME_MAX - 1) break;
                if(u == 0x0000 || u == 0xFFFF) continue;
                it->lname[pos] = (u < 0x80) ? (char)u : '_';
            }
            it->lnext--;
            it->lslots++;
            continue;
        }

        if(e[DE_ATTR] & ATTR_VOLUME_ID){        /* the label, not a file   */
            it->lvalid = 0; it->lslots = 0; continue;
        }

        /* A real 8.3 entry: it ends any LFN run that precedes it. */
        memset(out, 0, sizeof *out);
        sname_render(e, out->sname);
        if(it->lvalid && it->lnext == 0 && lfn_sum(e + DE_NAME) == it->lsum){
            int j = 0;
            while(j < F32_NAME_MAX - 1 && it->lname[j]) { out->name[j] = it->lname[j]; j++; }
            out->name[j] = 0;
            out->first   = it->lfirst;
            out->nslots  = it->lslots + 1;
        } else {
            for(int j = 0; j < 13; j++) out->name[j] = out->sname[j];
            out->first  = it->cur;
            out->nslots = 1;
        }
        out->attr = e[DE_ATTR];
        out->clus = ((uint32_t)ld16(e + DE_CLUS_HI) << 16) | ld16(e + DE_CLUS_LO);
        out->size = ld32(e + DE_SIZE);
        out->at   = it->cur;
        it->lvalid = 0; it->lslots = 0;
        return 1;
    }
}

/* Two iterators, never used at the same time: one for lookups and walks,
   one for the short-name uniquifier, which has to scan a directory while a
   write is already in progress.  Statics rather than stack because a 512
   byte buffer per nesting level is not something a kernel stack should be
   asked for. */
static f32_iter_t g_it_a, g_it_b;

static int dir_find(uint32_t dir_clus, const char *name, f32_ent_t *out){
    f32_ent_t e;
    iter_begin(&g_it_a, dir_clus);
    for(;;){
        int r = iter_next(&g_it_a, &e);
        if(r < 0) return -1;
        if(r == 0) return 0;
        if(ieq(e.name, name) || ieq(e.sname, name)){
            if(out) *out = e;
            return 1;
        }
    }
}

/* Is this 11-byte short name already in the directory? */
static int sname_taken(uint32_t dir_clus, const uint8_t *n11){
    f32_ent_t e;
    uint8_t want[12];
    char rendered[13], cand[13];
    memcpy(want, n11, 11);
    want[11] = 0;
    {   /* render the candidate the same way entries are rendered, so the
           comparison is between two strings rather than between a string and
           a padded field */
        uint8_t fake[DIRENT_SZ];
        memset(fake, 0, DIRENT_SZ);
        memcpy(fake + DE_NAME, n11, 11);
        sname_render(fake, cand);
    }
    iter_begin(&g_it_b, dir_clus);
    for(;;){
        int r = iter_next(&g_it_b, &e);
        if(r < 0) return -1;
        if(r == 0) return 0;
        {
            int k = 0;
            while(k < 12 && e.sname[k]){ rendered[k] = e.sname[k]; k++; }
            rendered[k] = 0;
        }
        if(ieq(rendered, cand)) return 1;
    }
}

/* ── path walking ───────────────────────────────────────────────────── */
/* Copy the next '/'-separated component of *p into comp, advancing *p.
   Returns 0 when there is nothing left. */
static int next_comp(const char **p, char *comp, int max){
    const char *s = *p;
    while(*s == '/') s++;
    if(!*s){ *p = s; return 0; }
    int i = 0;
    while(*s && *s != '/'){
        if(i < max - 1) comp[i++] = *s;
        s++;
    }
    comp[i] = 0;
    *p = s;
    return 1;
}

/* Resolve a path.
 *   returns  1  found; *out is the entry
 *            0  the parent directory exists but the final component does
 *               not; *parent is its cluster and leaf[] its name
 *           -1  a component along the way is missing, is not a directory,
 *               or the volume could not be read
 */
static int path_walk(const char *path, f32_ent_t *out,
                     uint32_t *parent, char *leaf, int leafmax){
    if(!g_mounted) return -1;
    const char *p = path ? path : "";
    char comp[F32_NAME_MAX];
    uint32_t dir = g_root_clus;
    f32_ent_t e;

    if(!next_comp(&p, comp, sizeof comp)){      /* "" or "/" is the root   */
        if(parent) *parent = g_root_clus;
        if(leaf && leafmax) leaf[0] = 0;
        if(out){
            memset(out, 0, sizeof *out);
            out->attr = ATTR_DIRECTORY;
            out->clus = g_root_clus;
        }
        return 1;
    }
    for(;;){
        char nextc[F32_NAME_MAX];
        const char *save = p;
        int more = next_comp(&p, nextc, sizeof nextc);
        if(!more){                              /* comp is the last one    */
            if(parent) *parent = dir;
            if(leaf){
                int i = 0;
                while(comp[i] && i < leafmax - 1){ leaf[i] = comp[i]; i++; }
                leaf[i] = 0;
            }
            int r = dir_find(dir, comp, &e);
            if(r < 0) return -1;
            if(r == 0) return 0;
            if(out) *out = e;
            return 1;
        }
        (void)save;
        int r = dir_find(dir, comp, &e);
        if(r <= 0) return -1;                   /* intermediate missing    */
        if(!(e.attr & ATTR_DIRECTORY)) return -1;
        dir = e.clus ? e.clus : g_root_clus;    /* ".."ptr to root is 0    */
        {   int i = 0; while(nextc[i] && i < (int)sizeof comp - 1){ comp[i] = nextc[i]; i++; } comp[i] = 0; }
    }
}

/* ── mount ──────────────────────────────────────────────────────────── */
int fat_probe_type(const uint8_t *bs){
    if(!bs) return FATTYPE_NONE;
    if(!(bs[510] == 0x55 && bs[511] == 0xAA)) return FATTYPE_NONE;
    uint32_t bps  = ld16(bs + 11);
    uint32_t spc  = bs[13];
    uint32_t rsvd = ld16(bs + 14);
    uint32_t nfat = bs[16];
    uint32_t rec  = ld16(bs + 17);
    /* The 16-bit field FIRST.  Offset 36 is BPB_FATSz32 only on FAT32; on
       FAT12/16 it is the extended boot record, so reading it first reports
       a 16 MB FAT16 volume as having a 1529413760-sector FAT.  Found by the
       fixture generator making exactly that mistake. */
    uint32_t fsz  = ld16(bs + 22);
    if(fsz == 0) fsz = ld32(bs + 36);
    uint32_t tot  = ld16(bs + 19);
    if(tot == 0) tot = ld32(bs + 32);

    if(bps != SECT_SZ) return FATTYPE_NONE;     /* 512 only, everywhere    */
    if(spc == 0 || (spc & (spc - 1))) return FATTYPE_NONE;   /* power of 2 */
    if(nfat == 0 || nfat > 4) return FATTYPE_NONE;
    if(rsvd == 0 || fsz == 0 || tot == 0) return FATTYPE_NONE;

    uint32_t rootsects = ((rec * 32u) + (bps - 1)) / bps;
    uint32_t datastart = rsvd + nfat * fsz + rootsects;
    if(datastart >= tot) return FATTYPE_NONE;
    uint32_t clusters  = (tot - datastart) / spc;

    /* The count of data clusters is the definition and the only thing that
       is.  Not the partition type byte, not the "FAT32   " string at offset
       82, both of which are routinely wrong on real media -- a volume
       labelled FAT16 with 200000 clusters is a FAT32 volume with a lie in
       it, and reading its FAT 16 bits at a time produces garbage chains. */
    if(clusters < 4085)  return FATTYPE_12;
    if(clusters < 65525) return FATTYPE_16;
    return FATTYPE_32;
}

/* ── WHAT IS THIS, IF IT IS NOT FAT32? ────────────────────────────────
 *
 * v0.8.6.1. fat_probe_type() answers "not a FAT volume" for an exFAT
 * partition, an NTFS one and a blank one alike, and `fat32` turned all
 * three into the same word: "none found".
 *
 * On the HP that was actively misleading. A 64 GB USB stick ships
 * formatted exFAT, so the scan read its boot sector, correctly decided
 * it was not FAT32, and printed nothing at all -- leaving "the stick is
 * not readable" indistinguishable from "the driver cannot see the
 * disk", which is the question the person is actually asking.
 *
 * This names it instead. The two signatures are at offset 3, which is
 * OEMName on a FAT volume and a filesystem name on these: an exFAT boot
 * sector carries "EXFAT   " and an NTFS one "NTFS    ", and both zero
 * or omit the BPB fields fat_probe_type() needs -- which is exactly why
 * they come back as NONE rather than as a wrong answer.
 *
 * Returns a word for a person, never 0. It does NOT mean the volume can
 * be read; that is what fat_probe_type() is for. */
const char *fat_fs_name(const uint8_t *bs)
{
    static const char ex[8] = { 'E','X','F','A','T',' ',' ',' ' };
    static const char nt[8] = { 'N','T','F','S',' ',' ',' ',' ' };
    int i, blank = 1;

    if(!bs) return "unreadable";

    for(i = 0; i < 8; i++) if((char)bs[3 + i] != ex[i]) break;
    if(i == 8) return "exFAT";
    for(i = 0; i < 8; i++) if((char)bs[3 + i] != nt[i]) break;
    if(i == 8) return "NTFS";

    switch(fat_probe_type(bs)){
    case FATTYPE_32: return "FAT32";
    case FATTYPE_16: return "FAT16";
    case FATTYPE_12: return "FAT12";
    default: break;
    }

    if(!(bs[510] == 0x55 && bs[511] == 0xAA)) {
        /* No signature at all. An entirely blank sector is a partition
           that was allocated and never formatted, which is a different
           thing from one holding a filesystem nobody here knows. */
        for(i = 0; i < 512; i++) if(bs[i]) { blank = 0; break; }
        return blank ? "not formatted" : "no boot signature";
    }
    return "unknown";
}

/* ── THE SYSTEM VOLUME IS PARKED, NOT DROPPED (v0.9.3) ──────────────
   This driver holds one mounted volume, and until 0.9.3 that only mattered
   to an installed system on FAT32 in theory: the installer always made
   FAT16, so the one slot was the ESP's or a stick's to borrow and nobody's
   to lose. 0.9.3 installs FAT32 above 2047 MB, so the slot is now the
   SYSTEM's -- and esp.c, update_src.c, uninstall.c and the `fat32`
   command all borrow it. Each of them unmounted when done and asked the
   VFS to remount, and in between every vfs_read() of a settings file
   reached the stick.

   So the volume fat32_init() mounted is kept aside, whole, while another
   one is borrowed:

     fat32_mount_dev()  with the system mounted   -> park it, mount the other
     fat32_unmount()    with a borrowed one       -> drop it, the system returns
     fat32_unmount()    with the system mounted   -> nothing. Only
                        fat32_init() replaces the system volume, so no
                        borrower's tidy-up can take the machine's own
                        filesystem away -- including the one that unmounts
                        twice after a failed mount, of which there are two.

   And fat32_sys_begin()/fat32_sys_end() put the system volume back for the
   length of ONE operation while a borrow is still open. That is what lets
   fs/update.c back up and write a kernel on a FAT32 system while the
   update source is still mounted from the ESP: the two volumes take turns
   in the slot, and each one's FAT window and FSInfo are flushed before it
   is put aside, so neither turn can leave the other's sectors half-written.

   The state below is EVERY global the mount owns. A field added to the
   mount and not to this struct is the bug this design invites, so
   tests/test_fat32sys.c parks one real volume, mounts another, writes to
   both, and fsck.fat's both images afterwards. */
typedef struct {
    fat32_read_fn  rd;  fat32_write_fn wr;  void *ctx;  uint64_t part;
    int      mounted;
    uint32_t spc, rsvd, nfats, fatsz, total, root, fat_start, data_start;
    uint32_t clusters, fsinfo_sect;
    uint32_t free_count, next_free;  int free_known, fsinfo_dirty;
} f32_state_t;

static f32_state_t g_sys;         /* the system volume, while parked       */
static int g_sys_parked;          /* g_sys holds it                        */
static int g_cur_is_sys;          /* what is mounted IS the system volume  */
static f32_state_t g_borrow;      /* the borrowed one, during a sys turn   */
static int g_sys_turn;            /* fat32_sys_begin() swapped them        */

static void state_save(f32_state_t *s){
    s->rd = g_rd; s->wr = g_wr; s->ctx = g_ctx; s->part = g_part;
    s->mounted = g_mounted;
    s->spc = g_sects_per_clust; s->rsvd = g_rsvd_sects; s->nfats = g_num_fats;
    s->fatsz = g_fat_sz; s->total = g_total_sects; s->root = g_root_clus;
    s->fat_start = g_fat_start; s->data_start = g_data_start;
    s->clusters = g_clusters; s->fsinfo_sect = g_fsinfo_sect;
    s->free_count = g_free_count; s->next_free = g_next_free;
    s->free_known = g_free_known; s->fsinfo_dirty = g_fsinfo_dirty;
}
static void state_load(const f32_state_t *s){
    g_rd = s->rd; g_wr = s->wr; g_ctx = s->ctx; g_part = s->part;
    g_mounted = s->mounted;
    g_sects_per_clust = s->spc; g_rsvd_sects = s->rsvd; g_num_fats = s->nfats;
    g_fat_sz = s->fatsz; g_total_sects = s->total; g_root_clus = s->root;
    g_fat_start = s->fat_start; g_data_start = s->data_start;
    g_clusters = s->clusters; g_fsinfo_sect = s->fsinfo_sect;
    g_free_count = s->free_count; g_next_free = s->next_free;
    g_free_known = s->free_known; g_fsinfo_dirty = s->fsinfo_dirty;
    /* The FAT window belonged to whichever volume was in the slot before;
       it was flushed when that one was put aside, and it is not this one's. */
    g_fatw_valid = 0; g_fatw_dirty = 0;
}

/* Flush and forget whatever is in the slot. Touches nothing parked. */
static void slot_drop(void){
    if(g_mounted) fat32_flush();
    g_mounted = 0; g_rd = 0; g_wr = 0; g_ctx = 0; g_part = 0;
    g_fatw_valid = 0; g_fatw_dirty = 0;
    g_clusters = 0; g_free_known = 0; g_fsinfo_dirty = 0;
    g_cur_is_sys = 0;
}

/* Bring the parked system volume back into the slot, if there is one. */
static void sys_return(void){
    if(!g_sys_parked) return;
    state_load(&g_sys);
    g_sys_parked = 0;
    g_cur_is_sys = 1;
}

void fat32_unmount(void){
    if(g_sys_turn) fat32_sys_end();      /* never strand a turn half-done  */
    if(g_cur_is_sys && g_mounted) return;   /* see the block above        */
    slot_drop();
    sys_return();
}

/* Drop everything, system volume included. fat32_init() only. */
static void unmount_all(void){
    g_sys_turn = 0;
    slot_drop();
    g_sys_parked = 0;
}

int fat32_sys_begin(void){
    if(g_sys_turn) return 1;                       /* already its turn     */
    if(g_cur_is_sys && g_mounted) return 1;        /* nothing is borrowed  */
    if(!g_sys_parked) return 0;                    /* there is no system   */
    if(g_mounted) fat32_flush();
    state_save(&g_borrow);
    state_load(&g_sys);
    g_sys_parked = 0; g_cur_is_sys = 1; g_sys_turn = 1;
    return 1;
}

void fat32_sys_end(void){
    if(!g_sys_turn) return;
    if(g_mounted) fat32_flush();
    state_save(&g_sys);
    g_sys_parked = 1;
    state_load(&g_borrow);
    g_cur_is_sys = 0; g_sys_turn = 0;
}

int fat32_system_mounted(void){
    return (g_cur_is_sys && g_mounted) || g_sys_parked;
}
int fat32_borrowed(void){ return g_mounted && !g_cur_is_sys; }

int fat32_mount_dev(fat32_read_fn rd, fat32_write_fn wr, void *ctx,
                    uint64_t part_lba)
{
    uint8_t bs[SECT_SZ];
    /* Park the system volume rather than lose it; drop anything else. A
       borrow opened during a system turn would orphan the turn, so the
       turn is closed first -- the borrow then parks the system as usual. */
    if(g_sys_turn) fat32_sys_end();
    if(g_cur_is_sys && g_mounted){
        fat32_flush();
        state_save(&g_sys);
        g_sys_parked = 1;
        g_cur_is_sys = 0;
        g_mounted = 0;
    }
    slot_drop();
    if(!rd || !rd(ctx, part_lba, bs) || fat_probe_type(bs) != FATTYPE_32){
        sys_return();
        return 0;
    }

    g_rd = rd; g_wr = wr; g_ctx = ctx; g_part = part_lba;

    g_sects_per_clust = bs[13];
    g_rsvd_sects      = ld16(bs + 14);
    g_num_fats        = bs[16];
    g_fat_sz          = ld16(bs + 22);
    if(g_fat_sz == 0) g_fat_sz = ld32(bs + 36);
    g_total_sects     = ld16(bs + 19);
    if(g_total_sects == 0) g_total_sects = ld32(bs + 32);
    g_root_clus       = ld32(bs + 44);
    g_fsinfo_sect     = ld16(bs + 48);

    g_fat_start  = g_rsvd_sects;
    g_data_start = g_rsvd_sects + g_num_fats * g_fat_sz;   /* RootEntCnt==0 */
    g_clusters   = (g_total_sects - g_data_start) / g_sects_per_clust;

    if(!clus_valid(g_root_clus)){ slot_drop(); sys_return(); return 0; }

    g_fatw_valid = 0; g_fatw_dirty = 0;
    g_mounted = 1;
    fsinfo_load();
    return 1;
}

/* Mount a volume AS THE SYSTEM VOLUME: the one a borrow parks and an
   unmount never drops. fat32_init() is the kernel's only caller; it is
   public so tests/test_fat32sys.c can stand a real image in for the boot
   disk without fs/blkdev.c. Replaces whatever was mounted, parked or not. */
int fat32_mount_system(fat32_read_fn rd, fat32_write_fn wr, void *ctx,
                       uint64_t part_lba)
{
    unmount_all();
    if(!fat32_mount_dev(rd, wr, ctx, part_lba)) return 0;
    g_cur_is_sys = 1;
    return 1;
}

int fat32_detected(void){ return g_mounted; }
uint32_t fat32_total_clusters(void){ return g_mounted ? g_clusters : 0; }
uint32_t fat32_root_cluster(void){ return g_mounted ? g_root_clus : 0; }
uint32_t fat32_cluster_bytes(void){
    return g_mounted ? g_sects_per_clust * SECT_SZ : 0;
}
/* The free count as FSInfo claims it, or 0xFFFFFFFF when we do not know --
   never a guess.  Counting it properly means walking the whole FAT, which
   is a deliberate choice this does not make on behalf of a caller. */
uint32_t fat32_free_clusters(void){
    if(!g_mounted || !g_free_known) return FSI_UNKNOWN;
    return g_free_count;
}

/* ── reading ────────────────────────────────────────────────────────── */
/* The one read primitive: walk the chain from byte `off`, copying `len`.
   fat32_read() and fat32_read_at() are both this, and the streaming install
   in fs/install.c needs exactly this shape -- the kernel is 631 KB and the
   heap is 255 KB, so it cannot be buffered. */
static int read_chain(uint32_t start, uint32_t fsize,
                      uint32_t off, char *buf, uint32_t len)
{
    if(off >= fsize) return 0;
    if(len > fsize - off) len = fsize - off;
    if(len == 0) return 0;
    if(!clus_valid(start)) return 0;

    uint32_t cbytes = g_sects_per_clust * SECT_SZ;
    uint32_t skip   = off / cbytes;
    uint32_t inclus = off % cbytes;
    uint32_t c = start, steps = 0;

    while(skip--){
        uint32_t next;
        if(!fat_get(c, &next)) return -1;
        if(next < 2 || next >= CLUS_BAD || !clus_valid(next)) return -1;
        c = next;
        if(++steps > g_clusters) return -1;
    }

    uint32_t done = 0;
    steps = 0;
    while(done < len && clus_valid(c)){
        uint32_t base = clus_to_sect(c);
        uint32_t s    = inclus / SECT_SZ;
        uint32_t o    = inclus % SECT_SZ;
        for(; s < g_sects_per_clust && done < len; s++, o = 0){
            if(!f32_rd(base + s, g_sec)) return -1;
            uint32_t n = SECT_SZ - o;
            if(n > len - done) n = len - done;
            memcpy(buf + done, g_sec + o, n);
            done += n;
        }
        inclus = 0;
        if(done >= len) break;
        uint32_t next;
        if(!fat_get(c, &next)) return -1;
        if(next < 2 || next >= CLUS_BAD || !clus_valid(next)) break;
        c = next;
        if(++steps > g_clusters) return -1;
    }
    return (int)done;
}

int fat32_read_at(const char *path, uint32_t off, char *buf, uint32_t len){
    f32_ent_t e;
    if(!g_mounted || !buf) return -1;
    if(path_walk(path, &e, 0, 0, 0) != 1) return -1;
    if(e.attr & ATTR_DIRECTORY) return -1;
    return read_chain(e.clus, e.size, off, buf, len);
}

int fat32_read(const char *path, char *buf, uint32_t maxlen){
    return fat32_read_at(path, 0, buf, maxlen);
}

uint32_t fat32_file_size(const char *path){
    f32_ent_t e;
    if(!g_mounted) return 0;
    if(path_walk(path, &e, 0, 0, 0) != 1) return 0;
    return (e.attr & ATTR_DIRECTORY) ? 0 : e.size;
}

int fat32_exists(const char *path){
    f32_ent_t e;
    if(!g_mounted) return 0;
    return path_walk(path, &e, 0, 0, 0) == 1;
}

int fat32_is_dir(const char *path){
    f32_ent_t e;
    if(!g_mounted) return 0;
    if(path_walk(path, &e, 0, 0, 0) != 1) return 0;
    return (e.attr & ATTR_DIRECTORY) != 0;
}

static void list_cluster(uint32_t dir_clus,
                         void (*cb)(const char *, int, uint32_t))
{
    f32_ent_t e;
    iter_begin(&g_it_a, dir_clus);
    for(;;){
        int r = iter_next(&g_it_a, &e);
        if(r <= 0) return;
        if(e.name[0] == '.' && (e.name[1] == 0 ||
           (e.name[1] == '.' && e.name[2] == 0))) continue;   /* . and ..  */
        cb(e.name, (e.attr & ATTR_DIRECTORY) != 0, e.size);
    }
}

void fat32_list(void (*cb)(const char *, int, uint32_t)){
    if(!g_mounted || !cb) return;
    list_cluster(g_root_clus, cb);
}

void fat32_list_dir(const char *dir, void (*cb)(const char *, int, uint32_t)){
    f32_ent_t e;
    if(!g_mounted || !cb) return;
    if(path_walk(dir, &e, 0, 0, 0) != 1) return;
    if(!(e.attr & ATTR_DIRECTORY)) return;
    list_cluster(e.clus ? e.clus : g_root_clus, cb);
}

/* ── writing ────────────────────────────────────────────────────────── */
/* Find `need` consecutive free slots in a directory, extending its chain
   when there is no room.  Leaves *out at the first of them. */
static int dir_alloc_slots(uint32_t dir_clus, int need, f32_cur_t *out){
    f32_cur_t c, run_start;
    uint8_t e[DIRENT_SZ];
    int run = 0;
    uint32_t steps = 0;
    uint32_t limit = (g_clusters + 2) * g_sects_per_clust * ENT_PER_SECT;

    cur_start(&c, dir_clus);
    run_start = c;
    for(;;){
        if(++steps > limit) return 0;
        if(!cur_read(&c, e)) return 0;
        if(e[0] == DE_END || e[0] == DE_FREE){
            if(run == 0) run_start = c;
            if(++run >= need){ *out = run_start; return 1; }
            /* An end-of-directory marker means every slot after it in this
               cluster is free too, but the chain may still end before we
               have enough of them -- so keep walking rather than assuming. */
        } else {
            run = 0;
        }
        int r = cur_step(&c);
        if(r < 0) return 0;
        if(r == 0){
            /* Out of chain.  Grow it; a FAT32 root directory can grow, which
               is the whole point of it being a chain. */
            uint32_t nc;
            if(!chain_extend(dir_clus, &nc)) return 0;
            if(!clus_zero(nc)) return 0;
            if(!fat_set(nc, CLUS_EOC)) return 0;
            c.clus = nc; c.sec = 0; c.idx = 0;
            if(run == 0) run_start = c;
            if(++run >= need){ *out = run_start; return 1; }
            continue;
        }
    }
}

/* Write the LFN run and the 8.3 entry, starting at `at`. */
static int write_entry(f32_cur_t at, const char *name,
                       const uint8_t *n11, uint8_t ntres,
                       int nlfn, uint8_t attr, uint32_t clus, uint32_t size)
{
    uint8_t e[DIRENT_SZ];
    uint8_t sum = lfn_sum(n11);
    int len = 0; while(name[len]) len++;

    /* LFN slots go on disk in REVERSE order: the slot holding the LAST
       thirteen characters is written first and carries the 0x40 flag. */
    for(int s = nlfn; s >= 1; s--){
        memset(e, 0, DIRENT_SZ);
        e[0] = (uint8_t)(s | (s == nlfn ? 0x40 : 0));
        e[DE_ATTR]      = ATTR_LFN;
        e[DE_NTRES]     = 0;
        e[DE_CRT_TENTH] = sum;
        st16(e + DE_CLUS_LO, 0);            /* must be zero in an LFN slot */
        int base = (s - 1) * 13;
        for(int k = 0; k < 13; k++){
            int pos = base + k;
            uint16_t u;
            if(pos < len)       u = (uint8_t)name[pos];
            else if(pos == len) u = 0x0000;         /* the terminator      */
            else                u = 0xFFFF;         /* padding             */
            st16(e + LFN_OFF[k], u);
        }
        if(!cur_write(&at, e)) return 0;
        if(cur_step(&at) <= 0) return 0;
    }

    memset(e, 0, DIRENT_SZ);
    memcpy(e + DE_NAME, n11, 11);
    e[DE_ATTR]  = attr;
    e[DE_NTRES] = ntres;
    st16(e + DE_CLUS_HI, (uint16_t)(clus >> 16));
    st16(e + DE_CLUS_LO, (uint16_t)(clus & 0xFFFF));
    st32(e + DE_SIZE, size);
    /* Dates are left at zero rather than invented.  There is an RTC driver
       in the tree and wiring it in here is a separate, honest change; a
       timestamp made up at write time is the same class of thing as a boot
       log that describes machine state in a string literal. */
    return cur_write(&at, e);
}

/* Settle on the short name for `name` in `dir`, appending ~1, ~2, ... until
   it is unique.  Returns the number of LFN slots needed (0 when the name is
   8.3-legal on its own). */
static int make_sname(uint32_t dir, const char *name,
                      uint8_t *n11, uint8_t *ntres, int *nlfn)
{
    if(fits_83(name, n11, ntres)){ *nlfn = 0; return 1; }
    *ntres = 0;
    int len = 0; while(name[len]) len++;
    int slots = (len + 12) / 13;
    if(slots < 1) slots = 1;
    if(slots > 20) slots = 20;
    *nlfn = slots;

    uint8_t basis[11];
    basis_83(name, basis);
    for(uint32_t n = 1; n <= 999999u; n++){
        memcpy(n11, basis, 11);
        tilde_83(n11, n);
        int t = sname_taken(dir, n11);
        if(t < 0) return 0;
        if(!t) return 1;
    }
    return 0;
}

/* Free every slot of an entry: its LFN run and then the 8.3 entry. */
static int entry_erase(const f32_ent_t *e){
    f32_cur_t c = e->first;
    for(int i = 0; i < e->nslots; i++){
        if(!cur_mark_free(&c)) return 0;
        if(i + 1 < e->nslots && cur_step(&c) <= 0) return 0;
    }
    return 1;
}

/* Rewrite the cluster and size of an existing 8.3 entry in place. */
static int entry_set_data(const f32_ent_t *e, uint32_t clus, uint32_t size){
    uint8_t d[DIRENT_SZ];
    if(!cur_read(&e->at, d)) return 0;
    st16(d + DE_CLUS_HI, (uint16_t)(clus >> 16));
    st16(d + DE_CLUS_LO, (uint16_t)(clus & 0xFFFF));
    st32(d + DE_SIZE, size);
    return cur_write(&e->at, d);
}

/* Lay `len` bytes down a fresh chain.  *start is 0 for an empty file --
   which is correct on FAT32 and is not the same as cluster 0. */
static int write_chain(const char *buf, uint32_t len, uint32_t *start){
    uint32_t first = 0, cur = 0, done = 0;

    if(len == 0){ *start = 0; return 1; }

    while(done < len){
        uint32_t nc;
        if(!clus_alloc(&nc)){
            if(first) chain_free(first);
            return 0;
        }
        if(first == 0) first = nc;
        else if(!fat_set(cur, nc)){ chain_free(first); return 0; }
        cur = nc;

        uint32_t base = clus_to_sect(nc);
        for(uint32_t s = 0; s < g_sects_per_clust && done < len; s++){
            uint32_t n = len - done;
            if(n > SECT_SZ) n = SECT_SZ;
            memset(g_sec, 0, SECT_SZ);
            memcpy(g_sec, buf + done, n);
            if(!f32_wr(base + s, g_sec)){ chain_free(first); return 0; }
            done += n;
        }
    }
    *start = first;
    return 1;
}

int fat32_write(const char *path, const char *buf, uint32_t len){
    f32_ent_t e;
    uint32_t dir = 0;
    char leaf[F32_NAME_MAX];
    if(!g_mounted || !g_wr) return 0;
    if(len && !buf) return 0;

    int found = path_walk(path, &e, &dir, leaf, sizeof leaf);
    if(found < 0) return 0;
    if(!leaf[0]) return 0;                      /* cannot write the root   */

    if(found == 1){
        if(e.attr & ATTR_DIRECTORY) return 0;
        /* Replace in place.  The entry and its long name are already right;
           only the chain and the size change.  Releasing the old chain
           before laying the new one down keeps the volume from needing room
           for two copies of the file at once, which on a nearly full stick
           is the difference between a write that works and one that does
           not. */
        uint32_t old = e.clus;
        if(old && !chain_free(old)) return 0;
        uint32_t start = 0;
        if(!write_chain(buf, len, &start)){
            /* The old chain is already gone, so the entry must stop pointing
               at it before we return.  A zero-length file is a visible,
               honest failure; a directory entry whose first cluster is in
               the free list is the kind that fsck calls a cross-link and
               the next allocation quietly overwrites. */
            entry_set_data(&e, 0, 0);
            fat32_flush();
            return 0;
        }
        if(!entry_set_data(&e, start, len)){
            chain_free(start);
            entry_set_data(&e, 0, 0);
            fat32_flush();
            return 0;
        }
        return fat32_flush() ? 1 : 0;
    }

    uint8_t n11[11], ntres;
    int nlfn = 0;
    if(!make_sname(dir, leaf, n11, &ntres, &nlfn)) return 0;

    f32_cur_t at;
    if(!dir_alloc_slots(dir, nlfn + 1, &at)) return 0;

    uint32_t start = 0;
    if(!write_chain(buf, len, &start)) return 0;
    if(!write_entry(at, leaf, n11, ntres, nlfn, ATTR_ARCHIVE, start, len)){
        chain_free(start);
        return 0;
    }
    return fat32_flush() ? 1 : 0;
}

int fat32_delete(const char *path){
    f32_ent_t e;
    if(!g_mounted || !g_wr) return 0;
    if(path_walk(path, &e, 0, 0, 0) != 1) return 0;
    if(e.attr & ATTR_DIRECTORY) return 0;       /* rmdir is its own thing  */
    if(e.clus && !chain_free(e.clus)) return 0;
    if(!entry_erase(&e)) return 0;
    return fat32_flush() ? 1 : 0;
}

/* ── fat32_rename (v0.9.3) ───────────────────────────────────────────
   vfs_rename() returned 0 for FAT32 -- "no rename of its own yet" -- which
   was invisible while FAT32 was only ever somebody else's volume and is a
   lost settings file the day it is the system's own: notepad's save-as and
   the file manager both rename.

   It moves the DIRECTORY ENTRY, never the data: a new entry naming the
   same first cluster, size and attributes is written, and only then is the
   old one erased. A failure between the two leaves both names pointing at
   one chain, which fsck reports and repairs, rather than neither name,
   which loses the file. The new name must not exist.

   A directory may be renamed in place and is refused across directories:
   its ".." would still name the old parent, and rewriting that is a
   second write that can fail on its own. */
int fat32_rename(const char *from, const char *to){
    f32_ent_t e, e2;
    uint32_t  src_dir = 0, dst_dir = 0;
    char      leaf[F32_NAME_MAX];
    uint8_t   n11[11], ntres;
    int       nlfn = 0;
    f32_cur_t at;

    if(!g_mounted || !g_wr) return 0;
    if(path_walk(from, &e, &src_dir, 0, 0) != 1) return 0;
    if(!e.nslots) return 0;                       /* the root has no entry  */
    if(e.sname[0] == '.') return 0;               /* "." and ".."           */
    if(path_walk(to, &e2, &dst_dir, leaf, sizeof leaf) != 0) return 0;
    if(!leaf[0]) return 0;
    if((e.attr & ATTR_DIRECTORY) && dst_dir != src_dir) return 0;

    if(!make_sname(dst_dir, leaf, n11, &ntres, &nlfn)) return 0;
    if(!dir_alloc_slots(dst_dir, nlfn + 1, &at)) return 0;
    if(!write_entry(at, leaf, n11, ntres, nlfn, e.attr, e.clus, e.size))
        return 0;
    if(!entry_erase(&e)) return 0;
    return fat32_flush() ? 1 : 0;
}

/* ── fat32_rmdir ─────────────────────────────────────────────────────
   v0.8.8.8. fat32_delete() refuses a directory -- "rmdir is its own
   thing" -- and until now nothing in the tree needed one, because
   HavenDOS only ever ADDED to an EFI System Partition. The uninstall
   path has to take \EFI\HAVENDOS away again, and leaving an empty
   directory behind on somebody's boot partition is not "fully
   removed".

   IT REFUSES A DIRECTORY THAT IS NOT EMPTY, and the check is the
   careful part. "Empty" means: no live 8.3 entry other than "." and
   "..". A deleted slot (0xE5) does not count, the end-of-directory
   marker (0x00) stops the scan, and an orphaned long-name slot DOES
   count as occupied -- an LFN run with no 8.3 entry after it is a
   damaged directory, and freeing its clusters would turn a damaged
   directory into lost data.

   The order matters too: the entry is erased only after the chain is
   freed, so a failure half way through leaves a directory that still
   points at its own clusters rather than clusters nothing owns. */
int fat32_rmdir(const char *path){
    f32_ent_t e;
    f32_cur_t c;
    uint8_t   d[DIRENT_SZ];
    int       st;

    if(!g_mounted || !g_wr) return 0;
    if(path_walk(path, &e, 0, 0, 0) != 1) return 0;
    if(!(e.attr & ATTR_DIRECTORY)) return 0;    /* use fat32_delete        */
    if(!e.clus) return 0;                       /* the root has no entry   */

    cur_start(&c, e.clus);
    for(;;){
        if(!cur_read(&c, d)) return 0;
        if(d[0] == 0x00) break;                 /* end of directory        */
        if(d[0] != 0xE5){
            int dot = (d[0] == '.' &&
                       (d[1] == ' ' || (d[1] == '.' && d[2] == ' ')));
            if(!dot) return 0;                  /* something lives here    */
        }
        st = cur_step(&c);
        if(st < 0) return 0;
        if(st == 0) break;
    }

    if(!chain_free(e.clus)) return 0;
    if(!entry_erase(&e)) return 0;
    return fat32_flush() ? 1 : 0;
}

int fat32_mkdir(const char *path){
    f32_ent_t e;
    uint32_t dir = 0;
    char leaf[F32_NAME_MAX];
    if(!g_mounted || !g_wr) return 0;

    int found = path_walk(path, &e, &dir, leaf, sizeof leaf);
    if(found < 0) return 0;
    if(found == 1) return (e.attr & ATTR_DIRECTORY) ? 1 : 0;
    if(!leaf[0]) return 0;

    uint32_t nc;
    if(!clus_alloc(&nc)) return 0;
    if(!clus_zero(nc)) return 0;

    /* "." and ".." come first, and ".."'s cluster is ZERO when the parent is
       the root -- the root has no entry of its own to point at, and writing
       its real cluster number there is a difference fsck notices. */
    {
        uint8_t d[DIRENT_SZ];
        f32_cur_t c;
        cur_start(&c, nc);
        memset(d, 0, DIRENT_SZ);
        memset(d + DE_NAME, ' ', 11);
        d[DE_NAME] = '.';
        d[DE_ATTR] = ATTR_DIRECTORY;
        st16(d + DE_CLUS_HI, (uint16_t)(nc >> 16));
        st16(d + DE_CLUS_LO, (uint16_t)(nc & 0xFFFF));
        if(!cur_write(&c, d)){ chain_free(nc); return 0; }
        if(cur_step(&c) <= 0){ chain_free(nc); return 0; }
        uint32_t pc = (dir == g_root_clus) ? 0 : dir;
        memset(d, 0, DIRENT_SZ);
        memset(d + DE_NAME, ' ', 11);
        d[DE_NAME] = '.'; d[DE_NAME + 1] = '.';
        d[DE_ATTR] = ATTR_DIRECTORY;
        st16(d + DE_CLUS_HI, (uint16_t)(pc >> 16));
        st16(d + DE_CLUS_LO, (uint16_t)(pc & 0xFFFF));
        if(!cur_write(&c, d)){ chain_free(nc); return 0; }
    }

    uint8_t n11[11], ntres;
    int nlfn = 0;
    if(!make_sname(dir, leaf, n11, &ntres, &nlfn)){ chain_free(nc); return 0; }
    f32_cur_t at;
    if(!dir_alloc_slots(dir, nlfn + 1, &at)){ chain_free(nc); return 0; }
    if(!write_entry(at, leaf, n11, ntres, nlfn, ATTR_DIRECTORY, nc, 0)){
        chain_free(nc);
        return 0;
    }
    return fat32_flush() ? 1 : 0;
}

/* ── probing the local disks ────────────────────────────────────────── */
/* The same two buses fs/fat16.c probes, in the same order, through the same
   functions.  An ESP on an NVMe or a USB stick comes in through
   fat32_mount_dev() instead -- this entry point is only for a FAT32
   partition on the disk the machine already knows how to read. */
/* ── The disk this machine already knows how to read ──────────────
   This was a two-way switch -- VirtIO or legacy ATA -- and it was the
   worst of the three in the tree, because the other two had been taught
   about AHCI and NVMe and this one never was. fat32_init() on a modern
   laptop found no disk it could read and returned 0, two releases after
   "the installer had one block layer and the filesystem had another, and
   only one of them got taught about NVMe" was written down as the lesson
   of v0.8.3.1. The lesson was recorded in a comment and the third copy
   was never looked at.

   It is fs/blkdev.c's question now, answered by the same blk_boot_dev()
   fs/fat16.c asks, so the two cannot disagree about which disk is the
   machine's own -- which they did: fat16's probe order was VirtIO, ATA,
   AHCI, NVMe and the installer's was VirtIO, NVMe, AHCI, ATA.

   fat32_mount_dev() already took a read/write pair and a context, which
   is why this is five lines instead of fifty: the abstraction was here
   all along and this entry point was the one caller that did not use it. */
static int g_f32_dev = -1;

static int local_read(void *ctx, uint64_t lba, uint8_t *buf){
    (void)ctx; return blk_read(g_f32_dev, lba, buf);
}
static int local_write(void *ctx, uint64_t lba, const uint8_t *buf){
    (void)ctx; return blk_write(g_f32_dev, lba, buf);
}

/* The volume label a HavenDOS install volume carries, in the FAT32 BPB at
   offset 71 -- fs/fat32fmt.c writes it there and as the root's label entry,
   and the GRUB menu finds the partition by it (search --label HAVENDOS). */
static int bpb_label_is_ours(const uint8_t *bs){
    static const char L[11] = { 'H','A','V','E','N','D','O','S',' ',' ',' ' };
    for(int i = 0; i < 11; i++) if(bs[71 + i] != (uint8_t)L[i]) return 0;
    return 1;
}

static int gpt_cb(void *ctx, uint64_t lba, void *buf){
    return local_read(ctx, lba, (uint8_t *)buf) ? 0 : -1;   /* gpt.c: 0 = ok */
}

/* ── v0.9.3: THE INSTALL VOLUME ON A GPT DISK ────────────────────────
   Until this release fat32_init() read the MBR and nothing else. On a GPT
   disk the MBR is the protective one -- a single 0xEE entry -- so a FAT32
   volume the additive installer had made could never have been found, and
   it never needed to be, because the installer only made FAT16.

   It finds one now by the same two facts fs/fat16.c's find_gpt_volume()
   uses: the type GUID only the HavenDOS installer writes, and then the
   volume itself. The ESP is skipped BY NAME first, although it would fail
   the GUID test anyway -- it is FAT32, it is first on the disk, and it is
   the one partition where mounting the wrong thing as the system volume
   would end in somebody's Windows not booting. */
static int find_gpt_fat32(void){
    static const uint8_t OURS[16] = {
        'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'
    };
    gpt_part_t parts[GPT_MAX_PARTS];
    int n, i, k;

    if(!gpt_present(gpt_cb, 0)) return 0;
    n = gpt_scan(gpt_cb, 0, parts, GPT_MAX_PARTS);
    for(i = 0; i < n; i++){
        int ours = 1;
        if(parts[i].is_esp) continue;
        for(k = 0; k < 16; k++)
            if(parts[i].type_guid[k] != OURS[k]){ ours = 0; break; }
        if(!ours || parts[i].first_lba == 0) continue;
        if(fat32_mount_system(local_read, local_write, 0, parts[i].first_lba))
            return 1;
    }
    return 0;
}

int fat32_init(void){
    uint8_t mbr[SECT_SZ], bs[SECT_SZ];

    unmount_all();
    if(blk_count() == 0) blk_scan();
    g_f32_dev = blk_boot_dev();
    if(g_f32_dev < 0) return 0;              /* no disk at all */

    if(find_gpt_fat32()) return 1;

    if(!local_read(0, 0, mbr)) return 0;
    if(!(mbr[510] == 0x55 && mbr[511] == 0xAA)) return 0;

    /* Two passes over the MBR. The first takes only a volume labelled
       HAVENDOS -- what the whole-disk installer writes from 0.9.3 on -- so
       a disk that also carries somebody's FAT32 data partition ahead of
       ours mounts ours. The second is the behaviour before 0.9.3, kept so
       that no disk that mounted yesterday stops mounting today. */
    for(int pass = 0; pass < 2; pass++){
        for(int i = 0; i < 4; i++){
            const uint8_t *p = mbr + 446 + i * 16;
            uint8_t  type = p[4];
            uint32_t lba  = ld32(p + 8);
            uint32_t cnt  = ld32(p + 12);
            if(!lba || !cnt) continue;
            /* 0x0B is FAT32 with CHS addressing, 0x0C the same with LBA.
               The byte is a hint only: fat32_mount_dev() re-derives the
               type from the cluster count and will refuse a partition that
               is labelled FAT32 and is not one. */
            if(type != 0x0B && type != 0x0C) continue;
            if(pass == 0){
                if(!local_read(0, lba, bs) || !bpb_label_is_ours(bs)) continue;
            }
            if(fat32_mount_system(local_read, local_write, 0, lba)) return 1;
        }
    }
    return 0;
}
