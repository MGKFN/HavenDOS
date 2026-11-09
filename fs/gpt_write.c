/* ================================================================
   fs/gpt_write.c -- adding a partition to a GPT that already has one
   ================================================================
   See include/gpt_write.h for the contract. The short version, because
   it is the whole design:

     Adding a partition writes FOUR SECTORS. The entry array is 32
     sectors per copy and 31 of them are never written, so "every other
     partition is left alone" is not a promise this code keeps, it is a
     thing this code cannot do otherwise.

   There is no function here that writes a whole table, and there should
   never be one. fs/install.c's step 1 zeroes LBA 0..63 and the last 33
   sectors, which on a GPT disk is both headers and the entry array; that
   operation is why the installer has refused GPT disks outright since
   0.8.1. This file is what replaces it, and it replaces it by being
   incapable of the same mistake.
   ================================================================ */

#include "types.h"
#include "gpt.h"
#include "gpt_write.h"

/* include/string.h, not three hand-written prototypes. They used to be
   declared here with uint32_t lengths, which matches size_t in the 32-bit
   kernel and does NOT match it in a native test -- so a suite that
   included this file alongside any other that includes string.h would not
   compile, which is to say: alongside fs/fat32.c, which is exactly what
   tests/test_installadd.c has to do to test the install end to end. */
#include "string.h"

#define SECT      512u
#define HDR_SIG   "EFI PART"

/* Bounds we will work inside. A table outside them is one we do not
   understand well enough to modify, and GPTW_E_UNSUPP says so rather
   than guessing. 128 entries of 128 bytes is what every real disk has. */
#define MAX_ENTRIES   256u
#define MAX_ENT_SECS  64u            /* 256 * 128 / 512 */

/* HAVENDOS-PART-01 in ASCII. See the header for why it is not Microsoft
   basic data: a type Windows does not recognise is a partition Windows
   leaves entirely alone, which is the point. */
const uint8_t GPT_TYPE_HAVENDOS[16] = {
    'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1'
};

/* ── little-endian field access, byte at a time ─────────────────────
   Same rule as the filesystems: never cast a sector buffer to a struct.
   Under UTM SE's interpreter an unaligned access through a cast is
   silent garbage, and this is the one file where silent garbage costs
   somebody their partition table. */
static uint32_t gw_ld32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8)
         | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t gw_ld64(const uint8_t *p){
    uint64_t v = 0;
    for(int i=0;i<8;i++) v |= (uint64_t)p[i] << (8*i);
    return v;
}
static void gw_st32(uint8_t *p, uint32_t v){
    for(int i=0;i<4;i++) p[i] = (uint8_t)(v >> (8*i));
}
static void gw_st64(uint8_t *p, uint64_t v){
    for(int i=0;i<8;i++) p[i] = (uint8_t)(v >> (8*i));
}

/* ── the table, as read ─────────────────────────────────────────── */
typedef struct {
    uint8_t  hdr[SECT];          /* the primary header, verbatim        */
    uint8_t  bhdr[SECT];         /* the backup header, verbatim         */
    uint64_t my_lba, alt_lba;    /* 1 and the last LBA, normally        */
    uint64_t first_usable, last_usable;
    uint64_t ent_lba, bent_lba;
    uint32_t n_ent, ent_size, hdr_size;
    uint32_t ent_sectors;
    uint8_t  ent[MAX_ENT_SECS * SECT];   /* the whole array, for the CRC */
} gpt_t;

static int hdr_ok(const uint8_t *h)
{
    return memcmp(h, HDR_SIG, 8) == 0;
}

/* Verify a header's own CRC, which is computed over HeaderSize bytes with
   the CRC field itself zeroed. */
static int hdr_crc_ok(const uint8_t *h)
{
    uint8_t t[SECT];
    uint32_t sz = gw_ld32(h + 12), want = gw_ld32(h + 16);
    if(sz < 92 || sz > SECT) return 0;
    memcpy(t, h, SECT);
    gw_st32(t + 16, 0);
    return gpt_crc32(t, sz) == want;
}

/* Read and validate everything, before any decision is taken about it. */
static int load(gpt_rd_fn rd, void *ctx, gpt_t *g)
{
    if(!rd || !g) return GPTW_E_PARAM;
    memset(g, 0, sizeof *g);

    if(rd(ctx, 1, g->hdr) != 0)     return GPTW_E_READ;
    if(!hdr_ok(g->hdr))             return GPTW_E_NOTGPT;
    if(!hdr_crc_ok(g->hdr))         return GPTW_E_HDRCRC;

    g->hdr_size     = gw_ld32(g->hdr + 12);
    g->my_lba       = gw_ld64(g->hdr + 24);
    g->alt_lba      = gw_ld64(g->hdr + 32);
    g->first_usable = gw_ld64(g->hdr + 40);
    g->last_usable  = gw_ld64(g->hdr + 48);
    g->ent_lba      = gw_ld64(g->hdr + 72);
    g->n_ent        = gw_ld32(g->hdr + 80);
    g->ent_size     = gw_ld32(g->hdr + 84);

    if(g->n_ent == 0 || g->n_ent > MAX_ENTRIES)       return GPTW_E_UNSUPP;
    if(g->ent_size < 128 || g->ent_size > SECT)       return GPTW_E_UNSUPP;
    if(g->ent_size & (g->ent_size - 1))               return GPTW_E_UNSUPP;
    if(g->last_usable < g->first_usable)              return GPTW_E_UNSUPP;
    if(g->ent_lba < 2)                                return GPTW_E_UNSUPP;

    g->ent_sectors = (g->n_ent * g->ent_size + SECT - 1) / SECT;
    if(g->ent_sectors > MAX_ENT_SECS)                 return GPTW_E_UNSUPP;

    for(uint32_t s = 0; s < g->ent_sectors; s++)
        if(rd(ctx, g->ent_lba + s, g->ent + s * SECT) != 0) return GPTW_E_READ;

    if(gpt_crc32(g->ent, g->n_ent * g->ent_size) != gw_ld32(g->hdr + 88))
        return GPTW_E_ENTCRC;

    /* The backup is not optional. A disk whose backup header is gone is a
       disk something has already damaged, and adding to it is how a
       half-broken table becomes a wholly broken one. Refuse, and let the
       person repair it with a tool that is meant for that. */
    if(g->alt_lba < 2)              return GPTW_E_BACKUP;
    if(rd(ctx, g->alt_lba, g->bhdr) != 0) return GPTW_E_BACKUP;
    if(!hdr_ok(g->bhdr))            return GPTW_E_BACKUP;
    if(!hdr_crc_ok(g->bhdr))        return GPTW_E_BACKUP;

    g->bent_lba = gw_ld64(g->bhdr + 72);
    if(g->bent_lba < 2)             return GPTW_E_BACKUP;
    /* The two copies must already agree about the entries. If they do not,
       something else has written here and we do not know which copy is
       current. */
    if(gw_ld32(g->bhdr + 88) != gw_ld32(g->hdr + 88)) return GPTW_E_BACKUP;
    if(gw_ld32(g->bhdr + 80) != g->n_ent)          return GPTW_E_BACKUP;
    if(gw_ld32(g->bhdr + 84) != g->ent_size)       return GPTW_E_BACKUP;

    return GPTW_OK;
}

static const uint8_t *entry(const gpt_t *g, uint32_t i){
    return g->ent + (uint64_t)i * g->ent_size;
}
static int entry_used(const gpt_t *g, uint32_t i){
    const uint8_t *e = entry(g, i);
    for(int k = 0; k < 16; k++) if(e[k]) return 1;
    return 0;
}

/* ── gaps ───────────────────────────────────────────────────────── */
/* Unallocated runs inside FirstUsable..LastUsable, found by sweeping the
   used ranges rather than by sorting them: the entry array is at most 128
   entries, so an O(n^2) sweep is a few thousand comparisons and needs no
   scratch buffer and no sort to get wrong. */
int gpt_find_gaps(gpt_rd_fn rd, void *ctx, gpt_gap_t *out, int max)
{
    gpt_t g;
    int r = load(rd, ctx, &g);
    if(r != GPTW_OK) return r;
    if(!out || max <= 0) return GPTW_E_PARAM;

    int n = 0;
    uint64_t cur = g.first_usable;

    while(cur <= g.last_usable && n < max){
        /* the used range that starts soonest at or after cur */
        uint64_t best_a = 0, best_b = 0;
        int found = 0;
        for(uint32_t i = 0; i < g.n_ent; i++){
            if(!entry_used(&g, i)) continue;
            const uint8_t *e = entry(&g, i);
            uint64_t a = gw_ld64(e + 32), b = gw_ld64(e + 40);
            if(b < a) continue;
            if(b < cur) continue;                 /* entirely behind us */
            if(!found || a < best_a){ best_a = a; best_b = b; found = 1; }
        }
        if(!found){
            out[n].first_lba = cur;
            out[n].last_lba  = g.last_usable;
            out[n].sectors   = g.last_usable - cur + 1;
            n++;
            break;
        }
        if(best_a > cur){
            out[n].first_lba = cur;
            out[n].last_lba  = best_a - 1;
            out[n].sectors   = best_a - cur;
            n++;
        }
        if(best_b + 1 <= cur) break;              /* no forward progress */
        cur = best_b + 1;
    }
    return n;
}

int gpt_find_free(gpt_rd_fn rd, void *ctx, uint64_t need, gpt_gap_t *out)
{
    gpt_gap_t gaps[16];
    if(!out || need == 0) return GPTW_E_PARAM;
    int n = gpt_find_gaps(rd, ctx, gaps, 16);
    if(n < 0) return n;
    for(int i = 0; i < n; i++){
        if(gaps[i].sectors >= need){ *out = gaps[i]; return GPTW_OK; }
    }
    return GPTW_E_NOSPACE;
}

/* ── adding one ─────────────────────────────────────────────────── */
/* See include/gpt_write.h. The structure deliberately mirrors
   gpt_add_partition() below it line for line: same load, same CRC
   recomputation over the whole array, same four-sector write set in
   the same order, backup first and primary last. The only difference
   is that the entry is zeroed instead of filled in. Two functions
   that do the same dangerous thing should not look different. */
int gpt_remove_partition(gpt_rd_fn rd, gpt_wr_fn wr, void *ctx,
                         int entry_index,
                         const uint8_t expect_type[16],
                         uint64_t expect_first_lba,
                         uint64_t *written, int *n_written)
{
    gpt_t g;
    int r;
    uint64_t wlist[4];
    int nw = 0;
    uint32_t slot;

    if(n_written) *n_written = 0;
    if(!expect_type) return GPTW_E_PARAM;
    if(entry_index < 0) return GPTW_E_PARAM;

    r = load(rd, ctx, &g);
    if(r != GPTW_OK) return r;

    slot = (uint32_t)entry_index;
    if(slot >= g.n_ent) return GPTW_E_PARAM;
    if(!entry_used(&g, slot)) return GPTW_E_EMPTY;

    /* ── all three must agree ──
       Index alone is not enough. A caller that has just rescanned a
       table it changed a moment ago can hold a stale index, and the
       cost of acting on one here is somebody else's partition. */
    {
        const uint8_t *e = entry(&g, slot);
        int k;
        for(k = 0; k < 16; k++)
            if(e[k] != expect_type[k]) return GPTW_E_MISMATCH;
        if(gw_ld64(e + 32) != expect_first_lba) return GPTW_E_MISMATCH;
    }

    /* ── clear it, in a copy of the array ──
       The array in memory is modified so the CRC can be computed over
       all of it. Only the single SECTOR holding this entry is written. */
    memset(g.ent + (uint64_t)slot * g.ent_size, 0, g.ent_size);

    {
        uint32_t new_ent_crc = gpt_crc32(g.ent, g.n_ent * g.ent_size);
        uint32_t per_sec  = SECT / g.ent_size;
        uint32_t ent_sec  = slot / per_sec;
        uint8_t ph[SECT], bh[SECT];

        memcpy(ph, g.hdr,  SECT);
        memcpy(bh, g.bhdr, SECT);
        gw_st32(ph + 88, new_ent_crc);
        gw_st32(bh + 88, new_ent_crc);
        gw_st32(ph + 16, 0); gw_st32(ph + 16, gpt_crc32(ph, gw_ld32(ph + 12)));
        gw_st32(bh + 16, 0); gw_st32(bh + 16, gpt_crc32(bh, gw_ld32(bh + 12)));

        wlist[nw++] = g.bent_lba + ent_sec;
        wlist[nw++] = g.alt_lba;
        wlist[nw++] = g.ent_lba  + ent_sec;
        wlist[nw++] = g.my_lba;

        if(written && n_written){
            for(int i2 = 0; i2 < nw; i2++) written[i2] = wlist[i2];
            *n_written = nw;
        }

        if(!wr) return GPTW_OK;

        {
            const uint8_t *esec = g.ent + (uint64_t)ent_sec * SECT;
            if(wr(ctx, wlist[0], esec) != 0) return GPTW_E_WRITE;
            if(wr(ctx, wlist[1], bh)   != 0) return GPTW_E_WRITE;
            if(wr(ctx, wlist[2], esec) != 0) return GPTW_E_WRITE;
            if(wr(ctx, wlist[3], ph)   != 0) return GPTW_E_WRITE;
        }
    }
    return GPTW_OK;
}

int gpt_add_partition(gpt_rd_fn rd, gpt_wr_fn wr, void *ctx,
                      uint64_t first_lba, uint64_t last_lba,
                      const uint8_t type_guid[16], const char *name,
                      int *entry_index_out,
                      uint64_t *written, int *n_written)
{
    gpt_t g;
    int r;
    uint64_t wlist[4];
    int nw = 0;

    if(n_written) *n_written = 0;
    if(!type_guid) return GPTW_E_PARAM;
    if(last_lba < first_lba) return GPTW_E_PARAM;

    r = load(rd, ctx, &g);
    if(r != GPTW_OK) return r;

    /* ── geometry ── */
    if(first_lba < g.first_usable || last_lba > g.last_usable)
        return GPTW_E_GEOM;

    /* ── overlap: against every used entry, both ends ── */
    for(uint32_t i = 0; i < g.n_ent; i++){
        if(!entry_used(&g, i)) continue;
        const uint8_t *e = entry(&g, i);
        uint64_t a = gw_ld64(e + 32), b = gw_ld64(e + 40);
        if(b < a) continue;
        if(first_lba <= b && a <= last_lba) return GPTW_E_OVERLAP;
    }

    /* ── a free slot ── */
    uint32_t slot = 0;
    int have_slot = 0;
    for(uint32_t i = 0; i < g.n_ent; i++){
        if(!entry_used(&g, i)){ slot = i; have_slot = 1; break; }
    }
    if(!have_slot) return GPTW_E_NOSLOT;

    /* ── build the one entry, in a copy of the array ──
       The array in memory is modified so the CRC can be computed over all
       of it. Only the single SECTOR holding this entry is ever written. */
    {
        uint8_t *e = g.ent + (uint64_t)slot * g.ent_size;
        memset(e, 0, g.ent_size);
        memcpy(e, type_guid, 16);
        /* A unique partition GUID. No entropy source is guaranteed this
           early, so it is derived from values that are already unique on
           this disk: the disk GUID, the slot, and the start LBA. It only
           has to not collide, and it cannot. */
        {
            uint8_t *u = e + 16;
            memcpy(u, g.hdr + 56, 16);           /* the disk's own GUID */
            u[0] ^= (uint8_t)(slot + 1);
            u[1] ^= (uint8_t)(first_lba);
            u[2] ^= (uint8_t)(first_lba >> 8);
            u[3] ^= (uint8_t)(first_lba >> 16);
            u[4] ^= (uint8_t)(first_lba >> 24);
            u[5] ^= (uint8_t)(first_lba >> 32);
            u[6] ^= 0x5A;
            u[7] ^= 0xA5;
        }
        gw_st64(e + 32, first_lba);
        gw_st64(e + 40, last_lba);
        gw_st64(e + 48, 0);                          /* no attribute flags */
        if(name){
            for(int k = 0; k < 35 && name[k]; k++)
                e[56 + k*2] = (uint8_t)name[k];   /* UTF-16LE, ASCII subset */
        }
    }

    uint32_t new_ent_crc = gpt_crc32(g.ent, g.n_ent * g.ent_size);

    /* which sector of the array holds it */
    uint32_t per_sec  = SECT / g.ent_size;
    uint32_t ent_sec  = slot / per_sec;

    /* ── the two headers, as they will be ── */
    uint8_t ph[SECT], bh[SECT];
    memcpy(ph, g.hdr,  SECT);
    memcpy(bh, g.bhdr, SECT);
    gw_st32(ph + 88, new_ent_crc);
    gw_st32(bh + 88, new_ent_crc);
    gw_st32(ph + 16, 0); gw_st32(ph + 16, gpt_crc32(ph, gw_ld32(ph + 12)));
    gw_st32(bh + 16, 0); gw_st32(bh + 16, gpt_crc32(bh, gw_ld32(bh + 12)));

    /* ── the write set, named before anything happens ──
       Backup first, primary last. The primary is the copy firmware reads,
       so if power is lost at any point the copy that matters is either
       wholly the old table or wholly the new one. Neither header is ever
       zeroed, not even for an instant. */
    wlist[nw++] = g.bent_lba + ent_sec;
    wlist[nw++] = g.alt_lba;
    wlist[nw++] = g.ent_lba  + ent_sec;
    wlist[nw++] = g.my_lba;

    if(written && n_written){
        for(int i = 0; i < nw; i++) written[i] = wlist[i];
        *n_written = nw;
    }
    if(entry_index_out) *entry_index_out = (int)slot;

    /* A NULL write function is a dry run: everything above has been
       checked, so returning OK here is a real statement that the write
       would succeed, not a guess. */
    if(!wr) return GPTW_OK;

    const uint8_t *esec = g.ent + (uint64_t)ent_sec * SECT;
    if(wr(ctx, wlist[0], esec) != 0) return GPTW_E_WRITE;
    if(wr(ctx, wlist[1], bh)   != 0) return GPTW_E_WRITE;
    if(wr(ctx, wlist[2], esec) != 0) return GPTW_E_WRITE;
    if(wr(ctx, wlist[3], ph)   != 0) return GPTW_E_WRITE;

    return GPTW_OK;
}

/* ── backup ─────────────────────────────────────────────────────── */
int gpt_backup_table(gpt_rd_fn rd, void *ctx, uint8_t *buf, uint32_t buflen)
{
    gpt_t g;
    int r = load(rd, ctx, &g);
    if(r != GPTW_OK) return r;
    if(!buf) return GPTW_E_PARAM;

    uint32_t need = (1 + g.ent_sectors) * 2 * SECT;
    if(buflen < need) return GPTW_E_PARAM;

    uint32_t off = 0;
    memcpy(buf + off, g.hdr, SECT); off += SECT;
    for(uint32_t s = 0; s < g.ent_sectors; s++){
        if(rd(ctx, g.ent_lba + s, buf + off) != 0) return GPTW_E_READ;
        off += SECT;
    }
    memcpy(buf + off, g.bhdr, SECT); off += SECT;
    for(uint32_t s = 0; s < g.ent_sectors; s++){
        if(rd(ctx, g.bent_lba + s, buf + off) != 0) return GPTW_E_READ;
        off += SECT;
    }
    return (int)off;
}

const char *gpt_write_strerror(int code)
{
    switch(code){
    case GPTW_OK:          return "ok";
    case GPTW_E_READ:      return "a sector would not read";
    case GPTW_E_WRITE:     return "a sector would not write";
    case GPTW_E_NOTGPT:    return "this disk has no GPT";
    case GPTW_E_HDRCRC:    return "the partition table fails its own checksum";
    case GPTW_E_ENTCRC:    return "the partition entries fail their checksum";
    case GPTW_E_BACKUP:    return "the backup partition table is missing or disagrees";
    case GPTW_E_GEOM:      return "that range is outside the usable area";
    case GPTW_E_OVERLAP:   return "that range overlaps an existing partition";
    case GPTW_E_NOSLOT:    return "the partition table is full";
    case GPTW_E_NOSPACE:   return "no free space big enough";
    case GPTW_E_PARAM:     return "bad parameters";
    case GPTW_E_UNSUPP:    return "a partition table shape this will not modify";
    default:               return "unknown error";
    }
}
