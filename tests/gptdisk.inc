/* ================================================================
   tests/gptdisk.inc -- the Windows-shaped disk, shared by two suites
   ================================================================
   A sparse in-memory GPT disk with a protective MBR, an EFI System
   Partition, a Microsoft Reserved partition, a large basic-data
   partition standing in for the NTFS one, an OEM partition whose type
   GUID begins with a zero byte, and a gap of unallocated space -- the
   shape of a 512 GB Windows 11 laptop that somebody has shrunk a
   partition on.

   It lives in its own file because two suites need it:
   tests/test_gptwrite.c, which checks that adding a partition writes
   four sectors and no others, and tests/test_installadd.c, which runs
   the whole additive install against it and checks the same thing
   about every sector on the disk.

   It was extracted from test_gptwrite.c rather than copied into the
   second suite. A hand-copied fixture drifts, and a drifted fixture
   certifies a disk shape that is no longer the one being tested --
   the same reason tests/test_chrome.c renders the real draw function
   out of a generated .inc instead of keeping its own copy of it.

   The sector array is g_dsec and not g_sec: fs/fat32.c uses g_sec for
   its own 512-byte directory scratch buffer, and test_installadd.c
   compiles both in one translation unit.

   Expects, from its includer: the scoreboard (ck/head), fs/gpt.c for
   gpt_crc32, and <string.h> or the kernel's memcmp/memcpy.
   ================================================================ */
/* ── a sparse disk ──────────────────────────────────────────────────── */
/* Overridable: tests/test_gptwrite.c writes a handful of sectors and 8192
   is plenty, while tests/test_installadd.c runs a whole install -- two
   811 KB loaders onto the ESP, a 256-sector FAT twice over, a 300 KB
   kernel -- and needs tens of thousands. Both arrays below are sized from
   this, so it is the only number to change. */
#ifndef MAXSEC
#define MAXSEC 8192
#endif
#ifndef MAXWLOG
#define MAXWLOG 256
#endif
typedef struct { uint64_t lba; uint8_t b[512]; } sec_t;
static sec_t  g_dsec[MAXSEC];
static int    g_ndsec;
static uint64_t g_disk_sectors;

/* every write since the last arm, in order */
static uint64_t g_wlog[MAXWLOG];
static int      g_nwlog;
static int      g_logging;

/* An index, because find() is on the path of every read and every write.
   The linear scan it replaces made tests/test_installadd.c take 45
   seconds -- a whole install is tens of thousands of accesses against
   tens of thousands of sectors -- and its injection harness runs that
   suite twenty-four times.

   Open addressing, linear probing, power-of-two table at least twice
   MAXSEC so it never fills. Each slot holds an index into g_dsec plus
   one, so zero means empty and the table needs no initialisation beyond
   memset. */
#define IDXSZ (2u << (                                                   \
    (MAXSEC) <= 8192u    ? 13 : (MAXSEC) <= 16384u  ? 14 :               \
    (MAXSEC) <= 32768u   ? 15 : (MAXSEC) <= 65536u  ? 16 :               \
    (MAXSEC) <= 131072u  ? 17 : (MAXSEC) <= 262144u ? 18 : 19))
static int g_idx[IDXSZ];

static unsigned idx_hash(uint64_t lba){
    /* splitmix-ish: the LBAs here are clustered (consecutive runs plus two
       at the very end of a 1-billion-sector disk), so the low bits alone
       would collide in long chains. */
    uint64_t x = lba + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (unsigned)(x & (IDXSZ - 1));
}

static sec_t *find(uint64_t lba){
    unsigned h = idx_hash(lba);
    for(;;){
        int v = g_idx[h];
        if(v == 0) return 0;
        if(g_dsec[v - 1].lba == lba) return &g_dsec[v - 1];
        h = (h + 1) & (IDXSZ - 1);
    }
}

static void idx_insert(uint64_t lba, int slot){
    unsigned h = idx_hash(lba);
    while(g_idx[h] != 0) h = (h + 1) & (IDXSZ - 1);
    g_idx[h] = slot + 1;
}
static sec_t *make(uint64_t lba){
    sec_t *s = find(lba);
    if(s) return s;
    if(g_ndsec >= MAXSEC){ printf("    (sparse disk full)\n"); exit(2); }
    s = &g_dsec[g_ndsec];
    idx_insert(lba, g_ndsec);
    g_ndsec++;
    s->lba = lba;
    memset(s->b, 0, 512);
    return s;
}
static int d_read(void *ctx, uint64_t lba, void *buf){
    (void)ctx;
    if(lba >= g_disk_sectors) return -1;
    sec_t *s = find(lba);
    if(s) memcpy(buf, s->b, 512);
    else  memset(buf, 0, 512);
    return 0;
}
static int d_write(void *ctx, uint64_t lba, const void *buf){
    (void)ctx;
    if(lba >= g_disk_sectors) return -1;
    if(g_logging){
        /* g_nwlog keeps counting past the array: the COUNT is what a
           dry-run assertion needs, and a reader of g_wlog must bound
           itself by MAXWLOG as well as by g_nwlog. */
        if(g_nwlog < MAXWLOG) g_wlog[g_nwlog] = lba;
        g_nwlog++;
    }
    /* v0.9.3: a sector that is not in the model READS as zeroes, so a
       zero written to one is already there. Not storing it keeps the
       model the size of what is on the disk rather than the size of what
       was formatted: a FAT32 volume's FATs are tens of thousands of zero
       sectors, and storing them filled MAXSEC on the first 32 GB install.
       The write log above still records the write -- the "which sectors
       did it touch" checks are about writes, not about contents. */
    if(!find(lba)){
        const uint8_t *z = (const uint8_t *)buf;
        int k, nz = 0;
        for(k = 0; k < 512; k++) if(z[k]){ nz = 1; break; }
        if(!nz) return 0;
    }
    memcpy(make(lba)->b, buf, 512);
    return 0;
}

/* A snapshot of the whole disk, for the identity check. */
static sec_t  g_snap[MAXSEC];
static int    g_nsnap;
static void snapshot(void){
    memcpy(g_snap, g_dsec, sizeof(sec_t) * (size_t)g_ndsec);
    g_nsnap = g_ndsec;
}

/* Compare now against the snapshot, ignoring the LBAs in `allowed`.
   Returns the number of violations and prints the first few. */
static int identical_except(const uint64_t *allowed, int n_allowed)
{
    int bad = 0;
    for(int i=0;i<g_nsnap;i++){
        int skip=0;
        for(int k=0;k<n_allowed;k++) if(g_snap[i].lba==allowed[k]) skip=1;
        if(skip) continue;
        sec_t *now = find(g_snap[i].lba);
        if(!now || memcmp(now->b, g_snap[i].b, 512) != 0){
            if(bad < 4)
                printf("      sector %llu CHANGED and was not permitted to\n",
                       (unsigned long long)g_snap[i].lba);
            bad++;
        }
    }
    /* and nothing new outside the permitted set */
    for(int i=0;i<g_ndsec;i++){
        int was=0;
        for(int k=0;k<g_nsnap;k++) if(g_snap[k].lba==g_dsec[i].lba) was=1;
        if(was) continue;
        int skip=0;
        for(int k=0;k<n_allowed;k++) if(g_dsec[i].lba==allowed[k]) skip=1;
        if(skip) continue;
        if(bad < 4)
            printf("      sector %llu APPEARED and was not permitted to\n",
                   (unsigned long long)g_dsec[i].lba);
        bad++;
    }
    return bad;
}

/* ── little-endian helpers for building the fixture ─────────────────── */
static void p32(uint8_t *p, uint32_t v){ for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void p64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t g32(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t g64(const uint8_t *p){
    uint64_t v=0; for(int i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v;
}

static const uint8_t T_ESP[16]={0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
                                0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t T_MSR[16]={0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,
                                0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE};
static const uint8_t T_DATA[16]={0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,
                                 0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
/* An OEM partition whose type GUID begins with a ZERO BYTE. Real disks
   carry these -- vendor recovery and diagnostic partitions use arbitrary
   GUIDs -- and it is the case that catches a "is this slot free?" test
   which looks at only the first byte of the type. Without one in the
   fixture, such a bug claims an occupied slot and nothing notices. */
static const uint8_t T_OEM[16] ={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                 0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};

#define NENT      128
#define ENTSZ     128
#define ENTSECS   (NENT*ENTSZ/512)        /* 32 */
#define MAX_ENTSECS 40

/* The layout under test, in sectors. Windows 11 on a 512 GB drive, with
   C: shrunk by 1 GB. */
typedef struct { const uint8_t *type; uint64_t a, b; const char *name; } partspec_t;

static uint64_t WIN_FREE_FIRST, WIN_FREE_LAST;

/* n_ent is a parameter because the entry-array CRC is specified over
   n_ent * entry_size, which on the usual 128 x 128 table is exactly 32
   sectors -- so a CRC computed over the SECTOR count instead gives the
   identical answer and a wrong implementation looks right. A count whose
   array is not sector-aligned is the only thing that tells them apart. */
static uint32_t g_nent = NENT;

/* gap_sectors: how much unallocated space to leave between the data
   volume and the recovery partition. 1 GB is what Disk Management leaves
   after the shrink tests/test_installadd.c is modelled on; that suite also
   needs a much larger gap, to check the installer caps what it takes, and
   a gap barely over the minimum, to check it refuses what is too small. */
/* v0.8.8.8. When set, the first partition is typed Linux filesystem
   instead of EFI System, so the disk has NO ESP. That is the case
   fs/install.c now has to handle by making one, and it cannot be
   reached from a fixture whose first partition is always an ESP. */
static int g_noesp = 0;

static void build_disk_gap(uint64_t total_sectors, uint32_t nent,
                           uint64_t gap_sectors)
{
    g_nent = nent;
    g_ndsec = 0; g_nwlog = 0;
    memset(g_idx, 0, sizeof g_idx);   /* the index goes with it */
    g_disk_sectors = total_sectors;
    uint64_t last = total_sectors - 1;
    uint32_t entsecs = (nent*ENTSZ + 511)/512;
    uint64_t bk_ent = last - entsecs;          /* backup entries start here */
    uint64_t first_usable = 2 + entsecs;
    uint64_t last_usable  = bk_ent - 1;

    /* The gap: 1 GB between the NTFS volume and recovery, which is what
       Disk Management leaves behind after a shrink. */
    uint64_t esp_a = 2048,        esp_b = 616447;          /* 300 MB */
    uint64_t msr_a = 616448,      msr_b = 648191;          /*  16 MB */
    uint64_t win_a = 648192;
    uint64_t rec_b = last_usable - 2048;                   /* recovery ends near the tail */
    uint64_t rec_a = rec_b - 1048575;                      /* 512 MB recovery */
    uint64_t gap_b = rec_a - 1;
    uint64_t gap_a = gap_b - (gap_sectors - 1);
    uint64_t win_b = gap_a - 1;
    WIN_FREE_FIRST = gap_a; WIN_FREE_LAST = gap_b;

    uint64_t oem_a = 1024, oem_b = 2047;        /* a small OEM area up front */
    /* Linux filesystem, 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
    static const uint8_t T_LXFS[16] = {
        0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
        0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
    partspec_t P[5] = {
        { g_noesp ? T_LXFS : T_ESP, esp_a, esp_b,
          g_noesp ? "Linux filesystem" : "EFI system partition" },
        { T_MSR,  msr_a, msr_b, "Microsoft reserved partition" },
        { T_DATA, win_a, win_b, "Basic data partition" },
        { T_DATA, rec_a, rec_b, "Basic data partition" },
        { T_OEM,  oem_a, oem_b, "OEM vendor partition" },
    };
    const int NPARTS = 5;

    /* protective MBR */
    { uint8_t *m = make(0)->b;
      m[446+0]=0x00; m[446+4]=0xEE;
      p32(m+446+8, 1); p32(m+446+12, 0xFFFFFFFFu);
      m[510]=0x55; m[511]=0xAA; }

    /* entry array, both copies */
    static uint8_t ent[MAX_ENTSECS*512];
    memset(ent, 0, (uint32_t)(entsecs*512));
    for(int i=0;i<NPARTS;i++){
        uint8_t *e = ent + i*ENTSZ;
        memcpy(e, P[i].type, 16);
        for(int k=0;k<16;k++) e[16+k] = (uint8_t)(0x40 + i*16 + k);  /* unique GUID */
        p64(e+32, P[i].a);
        p64(e+40, P[i].b);
        for(int k=0; P[i].name[k] && k<36; k++) e[56+k*2] = (uint8_t)P[i].name[k];
    }
    uint32_t ent_crc = gpt_crc32(ent, nent*ENTSZ);
    for(uint32_t s=0;s<entsecs;s++){
        memcpy(make(2 + (uint64_t)s)->b,      ent + s*512, 512);
        memcpy(make(bk_ent + (uint64_t)s)->b, ent + s*512, 512);
    }

    /* the two headers */
    for(int which=0; which<2; which++){
        uint8_t h[512];
        memset(h, 0, 512);
        memcpy(h, "EFI PART", 8);
        p32(h+8,  0x00010000u);            /* revision 1.0 */
        p32(h+12, 92);                     /* header size  */
        p32(h+16, 0);                      /* CRC, filled below */
        p64(h+24, which ? last : 1);       /* MyLBA        */
        p64(h+32, which ? 1 : last);       /* AlternateLBA */
        p64(h+40, first_usable);
        p64(h+48, last_usable);
        for(int k=0;k<16;k++) h[56+k] = (uint8_t)(0xA0 + k);   /* disk GUID */
        p64(h+72, which ? bk_ent : 2);     /* entry array LBA */
        p32(h+80, nent);
        p32(h+84, ENTSZ);
        p32(h+88, ent_crc);
        p32(h+16, gpt_crc32(h, 92));
        memcpy(make(which ? last : 1)->b, h, 512);
    }
}

static void build_disk_n(uint64_t total_sectors, uint32_t nent){
    build_disk_gap(total_sectors, nent, 2097152ull);      /* 1 GB */
}
static void build_disk(uint64_t total_sectors){ g_noesp = 0; build_disk_n(total_sectors, NENT); }

/* The same disk with no EFI System Partition on it. */
__attribute__((unused))
static void build_disk_noesp(uint64_t total_sectors){
    g_noesp = 1;
    build_disk_n(total_sectors, NENT);
    g_noesp = 0;
}

/* Does the table still verify, end to end, the way firmware would check it? */
static int table_verifies(void)
{
    uint8_t h[512], t[512];
    if(d_read(0, 1, h) != 0) return 0;
    if(memcmp(h, "EFI PART", 8) != 0) return 0;
    uint32_t hsz = g32(h+12), hcrc = g32(h+16);
    memcpy(t, h, 512); p32(t+16, 0);
    if(gpt_crc32(t, hsz) != hcrc) return 0;

    uint64_t elba = g64(h+72);
    uint32_t n = g32(h+80), esz = g32(h+84), ecrc = g32(h+88);
    static uint8_t ent[ENTSECS*512];
    uint32_t nsec = (n*esz + 511)/512;
    for(uint32_t s=0;s<nsec;s++)
        if(d_read(0, elba+s, ent + s*512) != 0) return 0;
    if(gpt_crc32(ent, n*esz) != ecrc) return 0;

    /* and the backup must agree about the entries */
    uint64_t alt = g64(h+32);
    uint8_t bh[512];
    if(d_read(0, alt, bh) != 0) return 0;
    if(memcmp(bh, "EFI PART", 8) != 0) return 0;
    memcpy(t, bh, 512); p32(t+16, 0);
    if(gpt_crc32(t, g32(bh+12)) != g32(bh+16)) return 0;
    if(g32(bh+88) != ecrc) return 0;
    return 1;
}
