/* ================================================================
   fs/fat32fmt.c -- see include/fat32fmt.h for why this exists.
   ================================================================ */
#include "fat32fmt.h"
#include "string.h"

#define SECT 512u

static void w16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Microsoft's own cluster-size table for FAT32, which is what every
   other formatter uses and therefore what every tool that inspects
   the result expects to see. Picking our own would not be wrong, but
   it would make every comparison against mkfs.fat a difference to
   explain rather than a match. */
static uint8_t spc_for(uint32_t sectors)
{
    uint64_t mb = (uint64_t)sectors / 2048u;      /* 512-byte sectors  */
    if(mb <=   260u) return 1;
    if(mb <=  8192u) return 8;
    if(mb <= 16384u) return 16;
    if(mb <= 32768u) return 32;
    return 64;
}

int fat32_geom(uint32_t part_sectors, fat32_geom_t *g)
{
    uint32_t spc, fat_sects, prev, clusters, data, i;

    if(!g || part_sectors < 1024u) return 0;
    memset(g, 0, sizeof *g);

    spc = spc_for(part_sectors);

    for(;;){
        if(part_sectors <= FAT32_RSVD_SECTS + 8u) return 0;

        /* Iterate to a fixed point: the FAT sizes the data area and
           the data area sizes the FAT. Eight rounds is far more than
           it ever takes; the bound is here so a pathological input
           cannot spin instead of failing. */
        fat_sects = 1;
        for(i = 0; i < 8; i++){
            prev = fat_sects;
            if(part_sectors <= FAT32_RSVD_SECTS + 2u * fat_sects) return 0;
            data      = part_sectors - FAT32_RSVD_SECTS - 2u * fat_sects;
            clusters  = data / spc;
            /* +2 for the two reserved FAT entries, 4 bytes each. */
            fat_sects = ((clusters + 2u) * 4u + SECT - 1u) / SECT;
            if(fat_sects == prev) break;
        }

        /* ── AND THEN CHECK IT, BECAUSE CONVERGING IS NOT ENOUGH ───
           The loop above stops when fat_sects stops changing. That
           is not the same as the FAT being big enough: the value it
           settles on was computed from the PREVIOUS round's cluster
           count, and at some sizes the final count is a hair larger.

           At 64 MB this produced a FAT of 1008 sectors -- 129,024
           entries -- for 129,024 clusters, which need 129,026. Two
           entries short, and fsck.fat said so in exactly those
           words:

             Filesystem has 129024 clusters but only space for
             129022 FAT entries.

           This is the same defect include/fat16fmt.h was written
           about -- a FAT that cannot address the clusters the BPB
           implies -- arrived at by a different route, and it is only
           here as a comment rather than in a shipped volume because
           the suite handed the image to fsck instead of asking the
           geometry whether it agreed with itself.

           Growing the FAT shrinks the data area, which lowers the
           cluster count, which lowers what the FAT needs -- so the
           two converge from opposite directions and this terminates.
           The bound is belt and braces. */
        for(i = 0; i < 16; i++){
            uint32_t need;
            if(part_sectors <= FAT32_RSVD_SECTS + 2u * fat_sects) return 0;
            data     = part_sectors - FAT32_RSVD_SECTS - 2u * fat_sects;
            clusters = data / spc;
            need     = ((clusters + 2u) * 4u + SECT - 1u) / SECT;
            if(need <= fat_sects) break;
            fat_sects = need;
        }

        if(part_sectors <= FAT32_RSVD_SECTS + 2u * fat_sects) return 0;
        data     = part_sectors - FAT32_RSVD_SECTS - 2u * fat_sects;
        clusters = data / spc;

        /* The invariant the whole file exists for, asserted rather
           than assumed: every cluster the BPB implies must have a
           FAT entry to live in. */
        if((uint64_t)fat_sects * (SECT / 4u) < (uint64_t)clusters + 2ull)
            return 0;

        if(clusters > FAT32_MAX_CLUSTERS){
            if(spc >= 128u) return 0;
            spc *= 2u;
            continue;                 /* too many: bigger clusters     */
        }
        if(clusters < FAT32_MIN_CLUSTERS){
            /* Fewer clusters than FAT32 is allowed to have. A smaller
               cluster size is the only thing that raises the count,
               and at one sector per cluster there is nothing left to
               try -- the partition is simply too small to be FAT32. */
            if(spc <= 1u) return 0;
            spc /= 2u;
            continue;
        }
        break;
    }

    g->part_sectors    = part_sectors;
    g->sects_per_clust = (uint8_t)spc;
    g->num_fats        = 2;
    g->rsvd_sects      = (uint16_t)FAT32_RSVD_SECTS;
    g->sects_per_fat   = fat_sects;
    g->fat1_start      = FAT32_RSVD_SECTS;
    g->fat2_start      = FAT32_RSVD_SECTS + fat_sects;
    g->data_start      = FAT32_RSVD_SECTS + 2u * fat_sects;
    g->root_clust      = 2;
    g->clusters        = clusters;
    return 1;
}

/* An 11-byte, space-padded label field, never reading past the string. */
static void label_11(uint8_t *d, const char *label)
{
    int i, end = 0;
    for(i = 0; i < 11; i++){
        if(!end && (!label || !label[i])) end = 1;
        d[i] = end ? (uint8_t)' ' : (uint8_t)label[i];
    }
}

static void boot_sector(uint8_t *s, const fat32_geom_t *g,
                        uint32_t hidden, uint32_t volid, const char *label)
{
    memset(s, 0, SECT);

    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;         /* jmp short + nop  */
    /* "MSWIN4.1" rather than anything of ours. Some firmware and some
       older tools special-case the OEM string, and none of them have
       ever been tested against "HAVENDOS". There is nothing to gain
       by being the first. */
    memcpy(s + 3, "MSWIN4.1", 8);

    w16(s + 0x0B, (uint16_t)SECT);
    s[0x0D] = g->sects_per_clust;
    w16(s + 0x0E, g->rsvd_sects);
    s[0x10] = g->num_fats;
    w16(s + 0x11, 0);                /* no fixed root dir on FAT32     */
    w16(s + 0x13, 0);                /* total_16 unused                */
    s[0x15] = 0xF8;                  /* fixed disk                     */
    w16(s + 0x16, 0);                /* sects_per_fat_16 MUST be zero  */
    w16(s + 0x18, 63);
    w16(s + 0x1A, 255);
    w32(s + 0x1C, hidden);
    w32(s + 0x20, g->part_sectors);

    w32(s + 0x24, g->sects_per_fat);
    w16(s + 0x28, 0);                /* both FATs live                 */
    w16(s + 0x2A, 0);                /* version 0.0                    */
    w32(s + 0x2C, g->root_clust);
    w16(s + 0x30, (uint16_t)FAT32_FSINFO_SECT);
    w16(s + 0x32, (uint16_t)FAT32_BACKUP_BOOT);

    s[0x40] = 0x80;
    s[0x42] = 0x29;                  /* extended boot signature        */
    w32(s + 0x43, volid);
    /* v0.9.3: stops AT the terminator. This read label[i] for every i
       below 11 whatever came first, so "HAVENBOOT" -- nine characters --
       had its tenth and eleventh bytes taken from past the end of the
       string. They happened to be zero in every build so far, which is
       luck and not a property: the next literal in .rodata decides. */
    label_11(s + 0x47, label);
    memcpy(s + 0x52, "FAT32   ", 8);

    s[510] = 0x55; s[511] = 0xAA;
}

static void fsinfo_sector(uint8_t *s, const fat32_geom_t *g)
{
    memset(s, 0, SECT);
    w32(s + 0x000, 0x41615252u);     /* "RRaA"                          */
    w32(s + 0x1E4, 0x61417272u);     /* "rrAa"                          */
    /* One cluster is in use: the root directory. */
    w32(s + 0x1E8, g->clusters - 1u);
    w32(s + 0x1EC, g->root_clust + 1u);
    s[510] = 0x55; s[511] = 0xAA;
}

int fat32_format(fat32_wr_fn wr, void *ctx, const fat32_geom_t *g,
                 uint32_t hidden_sects, uint32_t volume_id,
                 const char *label)
{
    uint8_t s[SECT];
    uint32_t i, f, base;

    if(!wr || !g || !g->sects_per_fat) return 0;

    /* ── the boot sector, and its backup ─────────────────────────── */
    boot_sector(s, g, hidden_sects, volume_id, label);
    if(!wr(ctx, 0, s)) return 0;
    if(!wr(ctx, FAT32_BACKUP_BOOT, s)) return 0;

    /* ── FSInfo, and its backup ──────────────────────────────────── */
    fsinfo_sector(s, g);
    if(!wr(ctx, FAT32_FSINFO_SECT, s)) return 0;
    if(!wr(ctx, FAT32_BACKUP_BOOT + FAT32_FSINFO_SECT, s)) return 0;

    /* ── the rest of the reserved area ───────────────────────────── */
    memset(s, 0, SECT);
    /* Sector 2 and the backup's sector 2 carry the 0xAA55 trailer in
       every volume mkfs.fat produces; fsck.fat warns without it. */
    s[510] = 0x55; s[511] = 0xAA;
    if(!wr(ctx, 2, s)) return 0;
    if(!wr(ctx, FAT32_BACKUP_BOOT + 2u, s)) return 0;
    memset(s, 0, SECT);
    for(i = 3; i < g->rsvd_sects; i++){
        if(i == FAT32_BACKUP_BOOT || i == FAT32_BACKUP_BOOT + 1u
           || i == FAT32_BACKUP_BOOT + 2u) continue;
        if(!wr(ctx, i, s)) return 0;
    }

    /* ── both FATs ───────────────────────────────────────────────── */
    for(f = 0; f < g->num_fats; f++){
        base = (f == 0) ? g->fat1_start : g->fat2_start;

        memset(s, 0, SECT);
        /* Entry 0 carries the media byte in its low 8 bits; entry 1
           is the clean-shutdown and hard-error flags, both set. Entry
           2 is the root directory, a single cluster, end of chain. */
        w32(s + 0, 0x0FFFFFF8u);
        w32(s + 4, 0x0FFFFFFFu);
        w32(s + 8, 0x0FFFFFFFu);
        if(!wr(ctx, base, s)) return 0;

        memset(s, 0, SECT);
        for(i = 1; i < g->sects_per_fat; i++)
            if(!wr(ctx, base + i, s)) return 0;
    }

    /* ── the root cluster: the label, then zeroes ────────────────── */
    memset(s, 0, SECT);
    if(label && label[0]){
        label_11(s, label);
        s[11] = 0x08;                /* ATTR_VOLUME_ID                 */
    }
    if(!wr(ctx, g->data_start, s)) return 0;

    memset(s, 0, SECT);
    for(i = 1; i < g->sects_per_clust; i++)
        if(!wr(ctx, g->data_start + i, s)) return 0;

    return 1;
}
