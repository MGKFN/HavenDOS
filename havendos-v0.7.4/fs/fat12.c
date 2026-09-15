/*
 * fat12.c — FAT12 filesystem for BOOT OS
 *
 * Layout (1.44MB floppy-style, but on ATA disk):
 *   Sector 0:     Boot sector (BPB)
 *   Sectors 1-9:  FAT1 (9 sectors)
 *   Sectors 10-18:FAT2 (mirror)
 *   Sectors 19-32:Root directory (14 sectors = 224 entries)
 *   Sectors 33+:  Data clusters
 *
 * We use a 16MB virtual disk (32768 sectors of 512 bytes)
 * but keep FAT12 layout for simplicity — max ~4085 clusters
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/io.h"

/* Forward declarations for ATA */
extern int      ata_detected(void);
extern int      ata_read_sector(uint32_t lba, uint8_t *buf);
extern int      ata_write_sector(uint32_t lba, const uint8_t *buf);

/* ---- BPB (BIOS Parameter Block) constants ---- */
#define BYTES_PER_SECT  512
#define SECTS_PER_CLUST 1
#define RSVD_SECTS      1       /* boot sector */
#define NUM_FATS        2
#define ROOT_ENT_CNT    224
#define TOTAL_SECTS     65536   /* 32MB disk */
#define MEDIA_DESC      0xF8    /* fixed disk */
#define SECTS_PER_FAT   12
#define SECTS_PER_TRACK 63
#define NUM_HEADS       255

/* Derived */
#define FAT1_START      1
#define FAT2_START      (FAT1_START + SECTS_PER_FAT)
#define ROOT_START      (FAT2_START + SECTS_PER_FAT)
#define ROOT_SECTS      ((ROOT_ENT_CNT * 32 + BYTES_PER_SECT - 1) / BYTES_PER_SECT)
#define DATA_START      (ROOT_START + ROOT_SECTS)

/* FAT12 special values */
#define FAT_FREE        0x000
#define FAT_EOF         0xFF8
#define FAT_BAD         0xFF7

/* Directory entry (32 bytes) */
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

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20

/* ---- In-memory FAT table ---- */
static uint8_t fat_buf[SECTS_PER_FAT * BYTES_PER_SECT];
static int     fat_dirty = 0;
static int     fat_loaded = 0;

/* Sector cache — single sector to avoid huge stack allocations */
static uint8_t sector_buf[BYTES_PER_SECT];

/* ---- FAT12 3-nibble entry read/write ---- */
static uint16_t fat_get(uint16_t cluster) {
    uint32_t offset = cluster + (cluster / 2); /* offset = cluster * 1.5 */
    uint16_t val = fat_buf[offset] | ((uint16_t)fat_buf[offset+1] << 8);
    if (cluster & 1) val >>= 4;
    else             val &= 0x0FFF;
    return val;
}

static void fat_set(uint16_t cluster, uint16_t val) {
    uint32_t offset = cluster + (cluster / 2);
    if (cluster & 1) {
        fat_buf[offset]   = (fat_buf[offset]   & 0x0F) | ((val & 0x0F) << 4);
        fat_buf[offset+1] = (val >> 4) & 0xFF;
    } else {
        fat_buf[offset]   = val & 0xFF;
        fat_buf[offset+1] = (fat_buf[offset+1] & 0xF0) | ((val >> 8) & 0x0F);
    }
    fat_dirty = 1;
}

/* ---- FAT load / flush ---- */
static int fat_load(void) {
    if (fat_loaded) return 1;
    for (int i = 0; i < SECTS_PER_FAT; i++) {
        if (!ata_read_sector(FAT1_START + i, fat_buf + i * BYTES_PER_SECT)) return 0;
    }
    fat_loaded = 1;
    fat_dirty  = 0;
    return 1;
}

static int fat_flush(void) {
    if (!fat_dirty) return 1;
    /* Write FAT1 */
    for (int i = 0; i < SECTS_PER_FAT; i++) {
        if (!ata_write_sector(FAT1_START + i, fat_buf + i * BYTES_PER_SECT)) return 0;
    }
    /* Write FAT2 (mirror) */
    for (int i = 0; i < SECTS_PER_FAT; i++) {
        if (!ata_write_sector(FAT2_START + i, fat_buf + i * BYTES_PER_SECT)) return 0;
    }
    fat_dirty = 0;
    return 1;
}

/* ---- Cluster allocation ---- */
static uint16_t fat_alloc_cluster(void) {
    /* Clusters start at 2 in FAT */
    uint16_t max = (SECTS_PER_FAT * BYTES_PER_SECT * 2) / 3;
    if (max > 4085) max = 4085;
    for (uint16_t c = 2; c < max; c++) {
        if (fat_get(c) == FAT_FREE) {
            fat_set(c, FAT_EOF);
            return c;
        }
    }
    return 0; /* disk full */
}

/* ---- Cluster → LBA ---- */
static uint32_t cluster_to_lba(uint16_t cluster) {
    return DATA_START + (cluster - 2) * SECTS_PER_CLUST;
}

/* ---- 8.3 filename helpers ---- */
static void name_to_83(const char *name, char out[8], char ext[3]) {
    memset(out, ' ', 8);
    memset(ext, ' ', 3);
    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
        out[i++] = c;
    }
    if (*name == '.') {
        name++;
        i = 0;
        while (*name && i < 3) {
            char c = *name++;
            if (c >= 'a' && c <= 'z') c -= 32;
            ext[i++] = c;
        }
    }
}

static void name_from_83(const char name[8], const char ext[3], char *out) {
    int i = 0, j = 0;
    while (i < 8 && name[i] != ' ') {
        char c = name[i++];
        if (c >= 'A' && c <= 'Z') c += 32; /* lowercase output */
        out[j++] = c;
    }
    /* Check if extension exists */
    int has_ext = 0;
    for (int k = 0; k < 3; k++) if (ext[k] != ' ') { has_ext = 1; break; }
    if (has_ext) {
        out[j++] = '.';
        i = 0;
        while (i < 3 && ext[i] != ' ') {
            char c = ext[i++];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[j++] = c;
        }
    }
    out[j] = 0;
}

/* ---- Root directory helpers ---- */
static int root_find(const char *name, fat_dirent_t *out, int *entry_idx) {
    char n83[8], e83[3];
    name_to_83(name, n83, e83);
    for (int s = 0; s < ROOT_SECTS; s++) {
        if (!ata_read_sector(ROOT_START + s, sector_buf)) return -1;
        fat_dirent_t *entries = (fat_dirent_t*)sector_buf;
        int per_sect = BYTES_PER_SECT / 32;
        for (int i = 0; i < per_sect; i++) {
            uint8_t first = (uint8_t)entries[i].name[0];
            if (first == 0x00) return -1; /* end of directory */
            if (first == 0xE5) continue;  /* deleted */
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            if (memcmp(entries[i].name, n83, 8) == 0 &&
                memcmp(entries[i].ext,  e83, 3) == 0) {
                if (out)       *out = entries[i];
                if (entry_idx) *entry_idx = s * per_sect + i;
                return s * per_sect + i;
            }
        }
    }
    return -1;
}

static int root_alloc(fat_dirent_t *entry) {
    for (int s = 0; s < ROOT_SECTS; s++) {
        if (!ata_read_sector(ROOT_START + s, sector_buf)) return -1;
        fat_dirent_t *entries = (fat_dirent_t*)sector_buf;
        int per_sect = BYTES_PER_SECT / 32;
        for (int i = 0; i < per_sect; i++) {
            uint8_t first = (uint8_t)entries[i].name[0];
            if (first == 0x00 || first == 0xE5) {
                /* Write entry here */
                entries[i] = *entry;
                if (!ata_write_sector(ROOT_START + s, sector_buf)) return -1;
                return s * per_sect + i;
            }
        }
    }
    return -1; /* root full */
}

static int root_update(int entry_idx, fat_dirent_t *entry) {
    int per_sect = BYTES_PER_SECT / 32;
    int s = entry_idx / per_sect;
    int i = entry_idx % per_sect;
    if (!ata_read_sector(ROOT_START + s, sector_buf)) return 0;
    fat_dirent_t *entries = (fat_dirent_t*)sector_buf;
    entries[i] = *entry;
    return ata_write_sector(ROOT_START + s, sector_buf);
}

/* ---- Free cluster chain ---- */
static void fat_free_chain(uint16_t start) {
    uint16_t c = start;
    while (c >= 2 && c < FAT_BAD) {
        uint16_t next = fat_get(c);
        fat_set(c, FAT_FREE);
        if (next >= FAT_EOF) break;
        c = next;
    }
}

/* ================================================================
   PUBLIC API
   ================================================================ */

static int fat12_ready = 0;

/*
 * fat12_init — called at boot if ATA disk detected
 * Writes boot sector + blank FAT + blank root dir if disk is fresh
 * Returns 1 if ready, 0 on failure
 */
int fat12_init(void) {
    fat12_ready = 0;
    if (!ata_detected()) return 0;

    /* Read sector 0 — check for existing FAT12 signature */
    if (!ata_read_sector(0, sector_buf)) return 0;

    /* Check OEM signature we wrote */
    int fresh = (memcmp(sector_buf + 3, "HAVENDOS", 8) != 0);

    if (fresh) {
        /* Write boot sector with BPB */
        memset(sector_buf, 0, BYTES_PER_SECT);
        /* Jump + NOP */
        sector_buf[0] = 0xEB; sector_buf[1] = 0x3C; sector_buf[2] = 0x90;
        /* OEM name */
        memcpy(sector_buf + 3, "HAVENDOS", 8);
        /* BPB */
        *(uint16_t*)(sector_buf+11) = BYTES_PER_SECT;
        sector_buf[13] = SECTS_PER_CLUST;
        *(uint16_t*)(sector_buf+14) = RSVD_SECTS;
        sector_buf[16] = NUM_FATS;
        *(uint16_t*)(sector_buf+17) = ROOT_ENT_CNT;
        *(uint16_t*)(sector_buf+19) = 0; /* >65535 sectors → use 32-bit field */
        sector_buf[21] = MEDIA_DESC;
        *(uint16_t*)(sector_buf+22) = SECTS_PER_FAT;
        *(uint16_t*)(sector_buf+24) = SECTS_PER_TRACK;
        *(uint16_t*)(sector_buf+26) = NUM_HEADS;
        *(uint32_t*)(sector_buf+28) = 0; /* hidden sectors */
        *(uint32_t*)(sector_buf+32) = TOTAL_SECTS;
        /* Boot signature */
        sector_buf[510] = 0x55; sector_buf[511] = 0xAA;

        if (!ata_write_sector(0, sector_buf)) return 0;

        /* Blank FAT tables */
        memset(fat_buf, 0, sizeof(fat_buf));
        /* First two FAT entries are reserved */
        fat_buf[0] = MEDIA_DESC; fat_buf[1] = 0xFF; fat_buf[2] = 0xFF;
        fat_dirty = 1; fat_loaded = 1;
        if (!fat_flush()) return 0;

        /* Blank root directory */
        memset(sector_buf, 0, BYTES_PER_SECT);
        for (int s = 0; s < ROOT_SECTS; s++) {
            if (!ata_write_sector(ROOT_START + s, sector_buf)) return 0;
        }
    }

    /* Load FAT into memory */
    fat_loaded = 0;
    if (!fat_load()) return 0;

    fat12_ready = 1;
    return 1;
}

int fat12_detected(void) { return fat12_ready; }

/*
 * fat12_write — create or overwrite a file
 * Returns bytes written, -1 on error
 */
int fat12_write(const char *name, const char *data, uint32_t size) {
    if (!fat12_ready) return -1;

    fat_dirent_t entry;
    int idx = root_find(name, &entry, NULL);
    int is_new = (idx < 0);

    if (!is_new) {
        /* Free existing cluster chain */
        if (entry.start_cluster >= 2) fat_free_chain(entry.start_cluster);
        entry.start_cluster = 0;
        entry.file_size     = 0;
    } else {
        memset(&entry, 0, sizeof(entry));
        name_to_83(name, entry.name, entry.ext);
        entry.attr = ATTR_ARCHIVE;
        entry.date = 0x5365; /* arbitrary fixed date */
        entry.time = 0x0000;
    }

    /* Write data in cluster-sized chunks */
    uint32_t remaining = size;
    uint32_t offset    = 0;
    uint16_t first_cluster = 0;
    uint16_t prev_cluster  = 0;

    while (remaining > 0 || first_cluster == 0) {
        uint16_t c = fat_alloc_cluster();
        if (c == 0) return -1; /* disk full */

        if (first_cluster == 0) first_cluster = c;
        if (prev_cluster != 0) fat_set(prev_cluster, c);

        uint8_t sect[BYTES_PER_SECT];
        uint32_t chunk = remaining > BYTES_PER_SECT ? BYTES_PER_SECT : remaining;
        memset(sect, 0, BYTES_PER_SECT);
        if (chunk > 0) memcpy(sect, data + offset, chunk);

        if (!ata_write_sector(cluster_to_lba(c), sect)) return -1;

        offset    += chunk;
        remaining  = (remaining > chunk) ? remaining - chunk : 0;
        prev_cluster = c;

        if (remaining == 0) break;
    }

    entry.start_cluster = first_cluster;
    entry.file_size     = size;

    if (!fat_flush()) return -1;

    if (is_new) {
        if (root_alloc(&entry) < 0) return -1;
    } else {
        if (!root_update(idx, &entry)) return -1;
    }

    return (int)size;
}

/*
 * fat12_read — read a file into buf
 * Returns bytes read, -1 if not found
 */
int fat12_read(const char *name, char *buf, uint32_t bufsz) {
    if (!fat12_ready) return -1;

    fat_dirent_t entry;
    if (root_find(name, &entry, NULL) < 0) return -1;
    if (entry.start_cluster < 2) { buf[0]=0; return 0; }

    uint32_t remaining = entry.file_size < bufsz ? entry.file_size : bufsz;
    uint32_t offset    = 0;
    uint16_t c = entry.start_cluster;

    while (c >= 2 && c < FAT_EOF && remaining > 0) {
        uint8_t sect[BYTES_PER_SECT];
        if (!ata_read_sector(cluster_to_lba(c), sect)) return -1;
        uint32_t chunk = remaining > BYTES_PER_SECT ? BYTES_PER_SECT : remaining;
        memcpy(buf + offset, sect, chunk);
        offset    += chunk;
        remaining -= chunk;
        c = fat_get(c);
    }

    return (int)offset;
}

/*
 * fat12_delete — mark directory entry as deleted, free clusters
 */
int fat12_delete(const char *name) {
    if (!fat12_ready) return -1;

    fat_dirent_t entry;
    int idx = root_find(name, &entry, NULL);
    if (idx < 0) return -1;

    if (entry.start_cluster >= 2) fat_free_chain(entry.start_cluster);
    fat_flush();

    /* Mark entry deleted */
    int per_sect = BYTES_PER_SECT / 32;
    int s = idx / per_sect;
    int i = idx % per_sect;
    if (!ata_read_sector(ROOT_START + s, sector_buf)) return -1;
    fat_dirent_t *entries = (fat_dirent_t*)sector_buf;
    entries[i].name[0] = (char)0xE5; /* deleted marker */
    return ata_write_sector(ROOT_START + s, sector_buf) ? 0 : -1;
}

/*
 * fat12_list — iterate all files in root directory
 */
void fat12_list(void (*cb)(const char*, int, uint32_t)) {
    if (!fat12_ready) return;
    char fname[16];
    for (int s = 0; s < ROOT_SECTS; s++) {
        if (!ata_read_sector(ROOT_START + s, sector_buf)) return;
        fat_dirent_t *entries = (fat_dirent_t*)sector_buf;
        int per_sect = BYTES_PER_SECT / 32;
        for (int i = 0; i < per_sect; i++) {
            uint8_t first = (uint8_t)entries[i].name[0];
            if (first == 0x00) return; /* end */
            if (first == 0xE5) continue; /* deleted */
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            name_from_83(entries[i].name, entries[i].ext, fname);
            int is_dir = (entries[i].attr & ATTR_DIRECTORY) ? 1 : 0;
            cb(fname, is_dir, entries[i].file_size);
        }
    }
}

int fat12_exists(const char *name) {
    return fat12_ready && root_find(name, NULL, NULL) >= 0;
}

int fat12_rename(const char *old, const char *newname) {
    if (!fat12_ready) return -1;
    fat_dirent_t entry;
    int idx = root_find(old, &entry, NULL);
    if (idx < 0) return -1;
    name_to_83(newname, entry.name, entry.ext);
    return root_update(idx, &entry) ? 0 : -1;
}
