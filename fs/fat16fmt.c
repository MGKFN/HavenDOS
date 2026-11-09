/* ================================================================
   fs/fat16fmt.c -- see include/fat16fmt.h for why this exists
   ================================================================ */
#include "types.h"
#include "string.h"
#include "fat16fmt.h"

#define SECT 512u

int fat16_geom(uint32_t part_sectors, fat16_geom_t *g)
{
    /* Stops at 64 sectors -- 32 KB clusters -- deliberately. 128 would
       give 64 KB clusters, which Microsoft's own specification rules out
       (bytes per cluster must not exceed 32 KB) and which several
       implementations get wrong because 65536 does not fit the 16-bit
       value they compute it in. The cost of the cap is that FAT16 here
       tops out a little under 2 GB instead of a little under 4 GB, and
       the gain is that a volume we write is one everything can read.
       Above that, fat16_geom() refuses rather than producing a volume
       that works on this machine and nowhere else -- a partition over
       2 GB wants FAT32, and there is a FAT32 driver in this tree but not
       yet a FAT32 formatter. */
    static const uint8_t spc_table[] = { 1, 2, 4, 8, 16, 32, 64 };
    unsigned t;

    if(!g) return 0;
    memset(g, 0, sizeof *g);

    /* A partition has to hold a boot sector, two FATs and a root
       directory before it holds a single file. Anything this small is
       not a disagreement about cluster size. */
    if(part_sectors < 64u) return 0;

    for(t = 0; t < sizeof spc_table / sizeof spc_table[0]; t++){
        uint8_t  spc        = spc_table[t];
        uint16_t rsvd       = 1;
        uint8_t  nfats      = 2;
        uint16_t root_ents  = 512;
        uint32_t root_sects = ((uint32_t)root_ents * 32u + SECT - 1u) / SECT;
        uint32_t spf        = 1;
        uint32_t clusters   = 0;
        unsigned iter;

        /* Fixed point. Each round sizes the FAT for the cluster count the
           previous round's FAT left room for. It converges downward and
           fast -- a handful of rounds at most -- but the bound is here so
           that a pathological input cannot spin: a formatter that hangs
           looks exactly like a disk that has stopped responding, and the
           user is staring at a progress bar over their Windows install. */
        for(iter = 0; iter < 64u; iter++){
            uint32_t overhead, data, need;

            overhead = (uint32_t)rsvd + (uint32_t)nfats * spf + root_sects;
            if(part_sectors <= overhead){ clusters = 0; break; }
            data     = part_sectors - overhead;
            clusters = data / spc;

            /* +2: entries 0 and 1 are reserved and hold the media byte and
               the end-of-chain marker, not clusters. Forgetting them is an
               off-by-two that only bites on the very last cluster of a
               full volume. */
            need = ((clusters + 2u) * 2u + SECT - 1u) / SECT;
            if(need == spf) break;
            spf = need;
        }

        if(clusters < FAT16_MIN_CLUSTERS) continue;   /* FAT12 territory */
        if(clusters > FAT16_MAX_CLUSTERS) continue;   /* needs FAT32     */

        g->part_sectors    = part_sectors;
        g->sects_per_clust = spc;
        g->num_fats        = nfats;
        g->rsvd_sects      = rsvd;
        g->root_ents       = root_ents;
        g->sects_per_fat   = spf;
        g->fat1_start      = rsvd;
        g->fat2_start      = rsvd + spf;
        g->root_start      = rsvd + (uint32_t)nfats * spf;
        g->root_sects      = root_sects;
        g->data_start      = g->root_start + root_sects;
        g->clusters        = clusters;
        return 1;
    }

    /* Nothing fit. Smallest cluster size still leaves too few clusters
       (the partition is FAT12-sized), or the largest still leaves too
       many (it is FAT32-sized). Either way, not this filesystem. */
    return 0;
}

/* The label, as 11 bytes, space padded, upper case, no dot. */
static void label11(uint8_t *out, const char *label)
{
    int i = 0;
    for(i = 0; i < 11; i++) out[i] = ' ';
    for(i = 0; i < 11 && label && label[i]; i++){
        char c = label[i];
        if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = (uint8_t)c;
    }
}

int fat16_format(fat16_wr_fn wr, void *ctx, const fat16_geom_t *g,
                 uint32_t hidden_sects, const char *label)
{
    uint8_t  buf[512];
    uint32_t i;

    if(!wr || !g || !g->sects_per_clust || !g->sects_per_fat) return 0;

    /* ── the boot sector ─────────────────────────────────────────────
       Every multi-byte field is written a byte at a time, little-endian.
       The same rule as fs/fat32.c and fs/gpt_write.c: a struct cast onto
       a sector buffer faults on a strict-alignment host. */
    memset(buf, 0, sizeof buf);
    buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;   /* jmp short, nop     */
    memcpy(buf + 3, "HAVEN16 ", 8);                /* OEM name           */
    buf[11] = (uint8_t)(SECT & 0xFF);
    buf[12] = (uint8_t)(SECT >> 8);
    buf[13] = g->sects_per_clust;
    buf[14] = (uint8_t)(g->rsvd_sects & 0xFF);
    buf[15] = (uint8_t)(g->rsvd_sects >> 8);
    buf[16] = g->num_fats;
    buf[17] = (uint8_t)(g->root_ents & 0xFF);
    buf[18] = (uint8_t)(g->root_ents >> 8);

    /* total sectors: the 16-bit field when it fits, the 32-bit one
       otherwise, and never both. fs/fat32.c's own probe reads the 16-bit
       field FIRST and only falls back to the 32-bit one when it is zero,
       because a volume with both set is ambiguous -- and getting that
       order wrong is what made the FAT32 fixture generator report a
       16 MB volume as having a 1.5-billion-sector FAT. */
    if(g->part_sectors < 0x10000u){
        buf[19] = (uint8_t)(g->part_sectors & 0xFF);
        buf[20] = (uint8_t)(g->part_sectors >> 8);
    } else {
        buf[32] = (uint8_t)(g->part_sectors       & 0xFF);
        buf[33] = (uint8_t)((g->part_sectors >> 8) & 0xFF);
        buf[34] = (uint8_t)((g->part_sectors >> 16)& 0xFF);
        buf[35] = (uint8_t)((g->part_sectors >> 24)& 0xFF);
    }

    buf[21] = 0xF8;                                /* fixed disk         */
    buf[22] = (uint8_t)(g->sects_per_fat & 0xFF);
    buf[23] = (uint8_t)((g->sects_per_fat >> 8) & 0xFF);
    buf[24] = 63; buf[25] = 0;                     /* sectors per track  */
    buf[26] = 255; buf[27] = 0;                    /* heads              */
    buf[28] = (uint8_t)(hidden_sects        & 0xFF);
    buf[29] = (uint8_t)((hidden_sects >> 8) & 0xFF);
    buf[30] = (uint8_t)((hidden_sects >> 16)& 0xFF);
    buf[31] = (uint8_t)((hidden_sects >> 24)& 0xFF);
    buf[36] = 0x80;                                /* drive number       */
    buf[37] = 0x00;
    buf[38] = 0x29;                                /* extended BPB sig   */
    buf[39] = 'H'; buf[40] = 'D'; buf[41] = 'O'; buf[42] = 'S';
    label11(buf + 43, label);
    memcpy(buf + 54, "FAT16   ", 8);
    buf[510] = 0x55; buf[511] = 0xAA;
    if(!wr(ctx, 0, buf)) return 0;

    /* ── both FATs ───────────────────────────────────────────────────
       Entry 0 holds the media byte in its low 8 bits with the rest set;
       entry 1 is the end-of-chain marker. fsck compares the two FATs
       against each other, so they are written identically or not at all. */
    {
        uint8_t first[512];
        memset(first, 0, sizeof first);
        first[0] = 0xF8; first[1] = 0xFF;          /* entry 0: media     */
        first[2] = 0xFF; first[3] = 0xFF;          /* entry 1: EOC       */
        memset(buf, 0, sizeof buf);

        for(i = 0; i < g->sects_per_fat; i++){
            const uint8_t *src = (i == 0) ? first : buf;
            if(!wr(ctx, g->fat1_start + i, src)) return 0;
            if(g->num_fats >= 2 && !wr(ctx, g->fat2_start + i, src)) return 0;
        }
    }

    /* ── the root directory, and the volume label inside it ──────────
       The label in the BPB above is a convenience field that tools
       print. This entry is what actually names the volume, and it is
       what GRUB's `search --label` matches. Without it that search
       fails and the menu falls through to a hardcoded disk number. */
    memset(buf, 0, sizeof buf);
    for(i = 1; i < g->root_sects; i++)
        if(!wr(ctx, g->root_start + i, buf)) return 0;

    label11(buf, label);
    buf[11] = 0x08;                                /* ATTR_VOLUME_ID     */
    if(!wr(ctx, g->root_start, buf)) return 0;

    return 1;
}
