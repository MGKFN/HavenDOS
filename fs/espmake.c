/* ================================================================
   fs/espmake.c -- see include/espmake.h for why this exists.
   ================================================================
   The rules fs/esp.c runs under apply here with more force, because
   this file writes a PARTITION TABLE. Nothing is written until every
   refusal has been checked; the dry run is the same code path with
   the write functions withheld rather than a second path that can
   drift; and the GPT edit goes through fs/gpt_write.c, whose blast
   radius is four sectors by construction.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "espmake.h"
#include "fat32fmt.h"

extern int inst_disk_read(int idx, uint64_t lba, uint8_t *buf);
extern int inst_disk_write(int idx, uint64_t lba, const uint8_t *buf);

const uint8_t ESPMK_TYPE_EFI[16] = {
    /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B: the first three fields are
       little-endian on disk, the last two are byte order as written. */
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

/* Microsoft basic data, EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
static const uint8_t T_MSDATA[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};
/* Microsoft reserved, E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
static const uint8_t T_MSRES[16] = {
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
    0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE
};
/* Windows recovery, DE94BBA4-06D1-4D40-A16A-BFD50179D6AC */
static const uint8_t T_MSREC[16] = {
    0xA4,0xBB,0x94,0xDE, 0xD1,0x06, 0x40,0x4D,
    0xA1,0x6A, 0xBF,0xD5,0x01,0x79,0xD6,0xAC
};
/* Linux filesystem, 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
static const uint8_t T_LXFS[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
    0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4
};
/* Linux swap, 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F */
static const uint8_t T_LXSWAP[16] = {
    0x6D,0xFD,0x57,0x06, 0xAB,0xA4, 0xC4,0x43,
    0x84,0xE5, 0x09,0x33,0xC8,0x4B,0x4F,0x4F
};
/* Linux LVM, E6D6D379-F507-44C2-A23C-238F2A3DF928 */
static const uint8_t T_LXLVM[16] = {
    0x79,0xD3,0xD6,0xE6, 0x07,0xF5, 0xC2,0x44,
    0xA2,0x3C, 0x23,0x8F,0x2A,0x3D,0xF9,0x28
};

/* Named espmk_* and not guid_eq/guid_zero: fs/gpt.c already has both
   under those names as file-statics, and tests/test_espmake.c builds
   this file and that one into one translation unit so the whole
   survey-then-create path can be exercised against a real sgdisk
   table. Two file-statics with the same name is a redefinition, and
   renaming here is cheaper than weakening the test. */
static int espmk_guid_eq(const uint8_t *a, const uint8_t *b)
{ int i; for(i = 0; i < 16; i++) if(a[i] != b[i]) return 0; return 1; }

static int espmk_guid_zero(const uint8_t *a)
{ int i; for(i = 0; i < 16; i++) if(a[i]) return 0; return 1; }

/* ── the disk, as gpt.c and gpt_write.c want it ──────────────────── */
/* s_espmk_disk and not s_espmk_disk. fs/install.c has a `static int s_espmk_disk`
   too, and tests/test_installadd.c compiles both files into one
   translation unit -- where two uninitialised statics of the same
   name and type are TENTATIVE definitions that C merges into ONE
   OBJECT, with no error and no warning at any -W level. The installer
   would have been writing this file's private state and the symptom
   would have surfaced somewhere else entirely.

   Caught by tools/check_statics.py, which exists because v0.8.7 lost
   an afternoon to usb/core.c's device count and net/netdev.c's NIC
   count being the same int. */
static int s_espmk_disk;

static int rd_cb(void *ctx, uint64_t lba, void *buf)
{
    (void)ctx;
    /* inst_disk_read returns 1 on success; these callbacks want 0. */
    return inst_disk_read(s_espmk_disk, lba, (uint8_t *)buf) ? 0 : -1;
}
static int wr_cb(void *ctx, uint64_t lba, const void *buf)
{
    (void)ctx;
    return inst_disk_write(s_espmk_disk, lba, (const uint8_t *)buf) ? 0 : -1;
}

/* ── does this partition's first sector look like a filesystem? ───
   Deliberately generous: anything that matches is treated as "has
   something on it" and the partition is NOT offered as unused. The
   cost of a false positive is one fewer candidate in a list; the
   cost of a false negative is somebody's data. */
static int looks_formatted(uint64_t lba)
{
    uint8_t s[512];
    if(!inst_disk_read(s_espmk_disk, lba, s)) return 1;   /* unreadable: assume yes */

    if(s[510] == 0x55 && s[511] == 0xAA) return 1;  /* any boot sector      */
    if(s[0x36]=='F'&&s[0x37]=='A'&&s[0x38]=='T') return 1;     /* FAT12/16  */
    if(s[0x52]=='F'&&s[0x53]=='A'&&s[0x54]=='T') return 1;     /* FAT32     */
    if(s[3]=='N'&&s[4]=='T'&&s[5]=='F'&&s[6]=='S') return 1;   /* NTFS      */
    if(s[3]=='E'&&s[4]=='X'&&s[5]=='F'&&s[6]=='A'&&s[7]=='T') return 1;
    {   /* ext2/3/4's superblock is at byte 1024 of the volume, so its
           magic is not in sector 0 -- check sector 2 as well. */
        uint8_t t[512];
        if(inst_disk_read(s_espmk_disk, lba + 2, t)
           && t[0x38] == 0x53 && t[0x39] == 0xEF) return 1;
    }
    {   int i;
        for(i = 0; i < 512; i++) if(s[i]) return 0;
    }
    return 0;                                        /* all zeroes          */
}

int espmk_survey(int disk, espmk_survey_t *s)
{
    gpt_part_t parts[GPT_MAX_PARTS];
    gpt_gap_t  gaps[8];
    int n, i, ng;

    if(!s) return ESPMK_E_PARAM;
    memset(s, 0, sizeof *s);
    s->esp_index = -1;
    s_espmk_disk = disk;

    /* gpt_scan() returns the PARTITION COUNT, and "0" means both "this
       disk is not GPT" and "this disk is GPT with nothing on it". The
       first version of this file treated 0 as not-GPT and therefore
       refused a freshly initialised disk -- which is precisely the
       disk this whole file was asked for: "without anything on disk
       detect an unused partition and use that as free space".

       gpt_present() is the question that actually distinguishes them,
       and it is cheap. Found by tests/test_espmake.c handing it a
       disk straight out of `sgdisk -o`. */
    if(!gpt_present(rd_cb, 0)) return ESPMK_E_NOTGPT;

    n = gpt_scan(rd_cb, 0, parts, GPT_MAX_PARTS);
    if(n < 0)  return ESPMK_E_READ;

    s->has_gpt = 1;
    s->n_parts = n;

    for(i = 0; i < n; i++){
        const uint8_t *t = parts[i].type_guid;
        if(parts[i].is_esp || espmk_guid_eq(t, ESPMK_TYPE_EFI)){
            if(s->esp_index < 0){
                s->esp_index   = i;
                s->esp_lba     = parts[i].first_lba;
                s->esp_sectors = parts[i].last_lba - parts[i].first_lba + 1;
            }
            continue;
        }
        if(espmk_guid_eq(t, T_MSDATA) || espmk_guid_eq(t, T_MSRES) || espmk_guid_eq(t, T_MSREC)
           || parts[i].is_msft){
            s->has_windows = 1;
            continue;
        }
        if(espmk_guid_eq(t, T_LXFS) || espmk_guid_eq(t, T_LXSWAP) || espmk_guid_eq(t, T_LXLVM)){
            s->has_linux = 1;
            continue;
        }
        if(espmk_guid_eq(t, GPT_TYPE_HAVENDOS)) continue;   /* ours already    */

        /* Unrecognised. Only a candidate if it also carries nothing
           that looks like a filesystem. */
        if(!espmk_guid_zero(t) && s->n_unused < ESPMK_MAX_UNUSED
           && !looks_formatted(parts[i].first_lba)){
            s->unused_lba[s->n_unused]     = parts[i].first_lba;
            s->unused_sectors[s->n_unused] = parts[i].last_lba
                                           - parts[i].first_lba + 1;
            s->unused_index[s->n_unused]   = i;
            s->n_unused++;
        }
    }

    ng = gpt_find_gaps(rd_cb, 0, gaps, 8);
    if(ng > 0){
        s->n_gaps           = ng;
        s->best_gap_lba     = gaps[0].first_lba;
        s->best_gap_sectors = gaps[0].sectors;
    }
    return ESPMK_OK;
}

/* ── formatting the new partition ───────────────────────────────── */
static uint64_t s_fmt_base;
static int      s_fmt_dry;

static int fmt_wr(void *ctx, uint32_t part_lba, const uint8_t *buf)
{
    (void)ctx;
    if(s_fmt_dry) return 1;        /* counted, never written           */
    return inst_disk_write(s_espmk_disk, s_fmt_base + part_lba, buf);
}

/* The core: a range is given, an ESP is made in it. */
int espmk_create_at(int disk, uint64_t first, uint64_t sectors,
                    int dry_run, int *part_index)
{
    fat32_geom_t g;
    uint64_t     last;
    int          rc, idx = -1;
    uint8_t      v[512];

    if(!sectors) return ESPMK_E_PARAM;
    if(first & 2047ull) return ESPMK_E_PARAM;     /* must be 1 MB aligned */
    if(sectors < (uint64_t)ESPMK_MIN_MB * 2048ull) return ESPMK_E_NOSPACE;
    if(sectors > 0xFFFFFFFFull) return ESPMK_E_GEOM;

    s_espmk_disk = disk;
    last   = first + sectors - 1;

    if(!fat32_geom((uint32_t)sectors, &g)) return ESPMK_E_GEOM;

    rc = gpt_add_partition(rd_cb, dry_run ? 0 : wr_cb, 0,
                           first, last, ESPMK_TYPE_EFI,
                           "EFI System Partition", &idx, 0, 0);
    if(rc != GPTW_OK) return ESPMK_E_GPT;

    s_fmt_base = first;
    s_fmt_dry  = dry_run;
    /* The serial is derived from where the partition landed rather
       than from a clock: two installs onto the same gap produce the
       same volume, which is what makes this testable. */
    if(!fat32_format(fmt_wr, 0, &g, (uint32_t)first,
                     (uint32_t)(first * 2654435761ull), "HAVENBOOT"))
        return ESPMK_E_FORMAT;

    if(!dry_run){
        if(!inst_disk_read(disk, first, v))            return ESPMK_E_VERIFY;
        if(v[510] != 0x55 || v[511] != 0xAA)           return ESPMK_E_VERIFY;
        if(v[0x52] != 'F' || v[0x53] != 'A' || v[0x54] != 'T'
           || v[0x55] != '3' || v[0x56] != '2')        return ESPMK_E_VERIFY;
        if(v[0x16] || v[0x17])                         return ESPMK_E_VERIFY;
    }

    if(part_index) *part_index = idx;
    return ESPMK_OK;
}

int espmk_create(int disk, uint32_t want_mb, int dry_run,
                 uint64_t *lba_out, uint64_t *sect_out, int *part_index)
{
    espmk_survey_t sv;
    gpt_gap_t      gap;
    uint64_t       need, first, sectors;
    int            rc;

    if(want_mb < ESPMK_MIN_MB) want_mb = ESPMK_WANT_MB;

    rc = espmk_survey(disk, &sv);
    if(rc != ESPMK_OK) return rc;
    if(sv.esp_index >= 0) return ESPMK_E_HAVEESP;

    s_espmk_disk = disk;
    need = (uint64_t)want_mb * 2048ull;

    rc = gpt_find_free(rd_cb, 0, need, &gap);
    if(rc == GPTW_E_NOSPACE){
        /* Try the floor before giving up: a disk with 60 MB free can
           still hold a working ESP, and refusing a machine over 40 MB
           is a worse answer than a smaller boot partition. */
        need = (uint64_t)ESPMK_MIN_MB * 2048ull;
        rc = gpt_find_free(rd_cb, 0, need, &gap);
    }
    if(rc == GPTW_E_NOSPACE) return ESPMK_E_NOSPACE;
    if(rc != GPTW_OK)        return ESPMK_E_GPT;

    /* Align the start to 1 MB. Every partitioning tool does, firmware
       is happier for it, and on a 4Kn or 512e SSD a misaligned FAT
       turns every cluster write into a read-modify-write. */
    first = (gap.first_lba + 2047ull) & ~2047ull;
    if(first > gap.last_lba) return ESPMK_E_NOSPACE;
    sectors = gap.last_lba - first + 1;
    if(sectors > need) sectors = need;

    rc = espmk_create_at(disk, first, sectors, dry_run, part_index);
    if(rc != ESPMK_OK) return rc;

    if(lba_out)  *lba_out  = first;
    if(sect_out) *sect_out = sectors;
    return ESPMK_OK;
}

const char *espmk_strerror(int code)
{
    switch(code){
    case ESPMK_OK:         return "ok";
    case ESPMK_E_READ:     return "the disk would not read";
    case ESPMK_E_NOTGPT:   return "this disk has no GPT, so there is no table to add to";
    case ESPMK_E_HAVEESP:  return "this disk already has an EFI System Partition";
    case ESPMK_E_NOSPACE:  return "no unallocated gap big enough for a boot partition";
    case ESPMK_E_GPT:      return "the partition table refused the new entry";
    case ESPMK_E_GEOM:     return "the free space will not make a valid FAT32 volume";
    case ESPMK_E_FORMAT:   return "a write failed while formatting";
    case ESPMK_E_VERIFY:   return "the new partition did not read back as FAT32";
    case ESPMK_E_PARAM:    return "bad parameters";
    default:               return "unknown error";
    }
}
