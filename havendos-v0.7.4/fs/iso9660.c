/*
 * iso9660.c — Minimal ISO 9660 CD-ROM filesystem reader for HavenDOS
 *
 * Supports:
 *   - Primary Volume Descriptor detection
 *   - Root directory traversal
 *   - File lookup by absolute path (e.g. "/boot/havendos.elf")
 *   - File read into a caller-supplied buffer
 *
 * Sector size: 2048 bytes (standard ISO 9660).
 * Uses atapi_read_sector() for all I/O — pure polling, no IRQs.
 *
 * Limitations (intentional — we only need update):
 *   - No Rock Ridge / Joliet extensions parsed
 *   - Filenames uppercased on ISO (havendos.elf → HAVENDOS.ELF;1)
 *   - Max path depth: 8 components
 *   - Max filename length: 64 chars
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/atapi.h"

#define ISO_SECTOR_SIZE  2048
#define ISO_PVD_SECTOR   16   /* Primary Volume Descriptor always at sector 16 */

/* ── ISO 9660 on-disk structures (packed, byte-order safe) ─── */

/* Directory record — variable length, fields at fixed byte offsets */
#define DR_LEN          0   /* uint8:  length of this record (0 = padding) */
#define DR_EAR          1   /* uint8:  extended attribute record length */
#define DR_EXTENT_LO    2   /* uint32 LE: LBA of file data */
#define DR_SIZE_LO      10  /* uint32 LE: size of file data in bytes */
#define DR_FLAGS        25  /* uint8:  file flags (bit 1 = directory) */
#define DR_NAMELEN      32  /* uint8:  length of file identifier */
#define DR_NAME         33  /* char[]: file identifier */

#define FLAG_DIRECTORY  0x02

/* ── Sector cache: one sector at a time ─────────────────────── */
static uint8_t  g_sec_buf[ISO_SECTOR_SIZE];
static uint32_t g_cached_lba = 0xFFFFFFFF;

static int iso_read_sector(uint32_t lba) {
    if (lba == g_cached_lba) return 1;
    if (!atapi_read_sector(lba, g_sec_buf)) return 0;
    g_cached_lba = lba;
    return 1;
}

/* ── LE32 read from byte array ──────────────────────────────── */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── Root directory state (filled by iso9660_init) ─────────── */
static uint32_t g_root_lba  = 0;
static uint32_t g_root_size = 0;
static int      g_ready     = 0;

/* ── String helpers ─────────────────────────────────────────── */

/* Uppercase ASCII in-place */
static void iso_upper(char *s) {
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s -= 32;
}

/* Compare ISO name (possibly with ";1" version suffix) to target */
static int iso_name_match(const char *iso_name, int iso_len, const char *target) {
    /* strip ";1" version suffix if present */
    int cmp_len = iso_len;
    if (cmp_len >= 2 && iso_name[cmp_len-2] == ';') cmp_len -= 2;
    int tlen = strlen(target);
    if (cmp_len != tlen) return 0;
    for (int i = 0; i < cmp_len; i++) {
        char a = iso_name[i]; if (a >= 'a' && a <= 'z') a -= 32;
        char b = target[i];   if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* ── Public API ─────────────────────────────────────────────── */

/*
 * iso9660_init() — detect PVD and locate root directory.
 * Returns 1 on success, 0 if no valid ISO found.
 */
int iso9660_init(void) {
    g_ready = 0;
    g_cached_lba = 0xFFFFFFFF;

    if (!atapi_detected()) return 0;

    /* Read PVD at sector 16 */
    if (!iso_read_sector(ISO_PVD_SECTOR)) return 0;

    /* Check signature: byte 0 = descriptor type (1 = PVD), bytes 1..5 = "CD001" */
    if (g_sec_buf[0] != 0x01) return 0;
    if (g_sec_buf[1] != 'C' || g_sec_buf[2] != 'D' ||
        g_sec_buf[3] != '0' || g_sec_buf[4] != '0' || g_sec_buf[5] != '1')
        return 0;

    /* Root directory record is at offset 156 in the PVD (34 bytes) */
    const uint8_t *rdr = g_sec_buf + 156;
    g_root_lba  = rd32(rdr + DR_EXTENT_LO);
    g_root_size = rd32(rdr + DR_SIZE_LO);

    if (g_root_lba == 0 || g_root_size == 0) return 0;

    g_ready = 1;
    return 1;
}

int iso9660_detected(void) { return g_ready; }

/*
 * iso_find_in_dir() — scan a directory for a named entry.
 * dir_lba, dir_size: location of the directory on the CD.
 * name: the component to search for (will be uppercased for compare).
 * out_lba, out_size: filled on success.
 * out_is_dir: 1 if the found entry is a directory.
 * Returns 1 if found, 0 if not.
 */
static int iso_find_in_dir(uint32_t dir_lba, uint32_t dir_size,
                            const char *name,
                            uint32_t *out_lba, uint32_t *out_size,
                            int *out_is_dir)
{
    uint32_t bytes_left = dir_size;
    uint32_t cur_lba    = dir_lba;
    uint32_t sec_off    = 0; /* byte offset within current sector */

    while (bytes_left > 0) {
        if (!iso_read_sector(cur_lba)) return 0;

        sec_off = 0;
        while (sec_off < ISO_SECTOR_SIZE && bytes_left > 0) {
            uint8_t rec_len = g_sec_buf[sec_off + DR_LEN];
            if (rec_len == 0) {
                /* advance to next sector */
                break;
            }
            uint8_t name_len = g_sec_buf[sec_off + DR_NAMELEN];
            const char *entry_name = (const char *)(g_sec_buf + sec_off + DR_NAME);

            /* skip "." and ".." entries (name_len==1, name[0]==0 or 1) */
            if (name_len == 1 && (entry_name[0] == 0x00 || entry_name[0] == 0x01)) {
                sec_off    += rec_len;
                bytes_left -= rec_len;
                continue;
            }

            if (iso_name_match(entry_name, name_len, name)) {
                *out_lba    = rd32(g_sec_buf + sec_off + DR_EXTENT_LO);
                *out_size   = rd32(g_sec_buf + sec_off + DR_SIZE_LO);
                *out_is_dir = (g_sec_buf[sec_off + DR_FLAGS] & FLAG_DIRECTORY) ? 1 : 0;
                return 1;
            }

            sec_off    += rec_len;
            bytes_left -= rec_len;
        }
        cur_lba++;
        /* if we broke out of inner loop due to zero record, sector is consumed */
        if (sec_off == 0 && bytes_left > 0) {
            /* sector was entirely consumed above already */
        }
    }
    return 0;
}

/*
 * iso9660_find() — locate a file by absolute path, e.g. "/boot/havendos.elf"
 * Fills out_lba (starting sector) and out_size (bytes).
 * Returns 1 on success, 0 on failure.
 */
int iso9660_find(const char *path, uint32_t *out_lba, uint32_t *out_size) {
    if (!g_ready) return 0;

    /* split path into components */
    char parts[8][64];
    int  nparts = 0;

    const char *p = path;
    if (*p == '/') p++; /* skip leading slash */

    while (*p && nparts < 8) {
        int i = 0;
        while (*p && *p != '/' && i < 63)
            parts[nparts][i++] = *p++;
        parts[nparts][i] = 0;
        if (i > 0) nparts++;
        if (*p == '/') p++;
    }

    if (nparts == 0) return 0;

    uint32_t cur_lba  = g_root_lba;
    uint32_t cur_size = g_root_size;
    int      is_dir   = 1;

    for (int i = 0; i < nparts; i++) {
        if (!is_dir) return 0; /* tried to descend into a file */
        uint32_t found_lba, found_size;
        int      found_dir;
        if (!iso_find_in_dir(cur_lba, cur_size, parts[i],
                             &found_lba, &found_size, &found_dir))
            return 0;
        cur_lba  = found_lba;
        cur_size = found_size;
        is_dir   = found_dir;
    }

    if (is_dir) return 0; /* path pointed to a directory, not a file */
    *out_lba  = cur_lba;
    *out_size = cur_size;
    return 1;
}

/*
 * iso9660_read_file() — read up to buf_size bytes of a file.
 * path: absolute path on the ISO.
 * buf:  destination buffer.
 * buf_size: size of buf (reads up to this many bytes).
 * Returns bytes read, or -1 on error.
 */
int iso9660_read_file(const char *path, uint8_t *buf, uint32_t buf_size) {
    uint32_t lba, size;
    if (!iso9660_find(path, &lba, &size)) return -1;

    uint32_t to_read = size < buf_size ? size : buf_size;
    uint32_t done    = 0;

    while (done < to_read) {
        if (!iso_read_sector(lba)) return (int)done;
        uint32_t chunk = to_read - done;
        if (chunk > ISO_SECTOR_SIZE) chunk = ISO_SECTOR_SIZE;
        memcpy(buf + done, g_sec_buf, chunk);
        done += chunk;
        lba++;
        g_cached_lba = 0xFFFFFFFF; /* force re-read next sector */
    }
    return (int)done;
}
