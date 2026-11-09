/* Reading a GUID Partition Table.
 *
 * This is checked because of what depends on it: fs/install.c zeroes LBA
 * 0..63 and the last 33 sectors, which on a GPT disk is the header, the
 * entry array and the backup -- and on a dual-boot machine is somebody's
 * other operating system, both copies.  gpt_present() is the thing standing
 * between the installer and that disk.
 *
 * A false NEGATIVE here is the dangerous direction: a real GPT disk reported
 * as "no GPT" is a disk the installer will happily erase.  So the tests lean
 * on that case -- a valid table must always be recognised, and the CRC check
 * must not reject one that is genuinely fine.
 */
#include <stdio.h>
#include <stdint.h>

/* These were defined here with uint32_t lengths because the kernel's
   include/string.h and the host's <string.h> disagreed about size_t.
   fs/gpt.c now includes include/string.h itself -- it had to, so that it
   can share a translation unit with fs/fat32.c in
   tests/test_installadd.c -- and those prototypes match the host's libc,
   which links the real ones in. */
static int streq(const char *a, const char *b){
    while(*a && *a==*b){a++;b++;} return *a==*b;
}

#include "../fs/gpt.c"

static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *w, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-54s got %ld\n", w, got); }
}

/* A disk in memory: 512-byte sectors, read through the same callback shape
   the kernel uses. */
#define NSECT 2048
static uint8_t disk[NSECT*512];
static int rd(void *ctx, uint64_t lba, void *buf){
    (void)ctx;
    if(lba >= NSECT) return -1;
    memcpy(buf, disk + lba*512, 512);
    return 0;
}

static void put32(uint8_t *p, uint32_t v){ for(int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void put64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

static const uint8_t ESP[16]={0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
                              0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t MSD[16]={0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,
                              0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};

/* Build a valid three-partition GPT, the shape a Windows laptop has. */
static void build_gpt(void)
{
    memset(disk,0,sizeof disk);
    /* protective MBR */
    disk[446+4]=0xEE; disk[510]=0x55; disk[511]=0xAA;

    uint8_t *ent = disk + 2*512;
    /* Laid out to FIT the 2048-sector test disk.  The first draft used a
       real laptop's LBAs (2048..206847 and beyond) on a 1 MB image, so two
       of the three partitions ended past the end and gpt_scan's
       last<first sanity check correctly dropped one.  The test data was
       wrong, not the parser -- worth keeping the check that caught it. */
    struct { const uint8_t *g; uint64_t a,b; const char *n; } P[3] = {
        {ESP,   34,  1000, "EFI system partition"},
        {MSD, 1001,  1500, "Basic data partition"},
        {MSD, 1501, NSECT-34, "Recovery"},
    };
    for(int i=0;i<3;i++){
        uint8_t *e = ent + i*128;
        memcpy(e, P[i].g, 16);
        memset(e+16, 0xAB, 16);                /* unique GUID, any value */
        put64(e+32, P[i].a);
        put64(e+40, P[i].b);
        for(int k=0; P[i].n[k] && k<36; k++) e[56+k*2] = (uint8_t)P[i].n[k];
    }
    uint32_t ent_crc = gpt_crc32(ent, 128*128);

    uint8_t *h = disk + 512;
    memcpy(h, "EFI PART", 8);
    put32(h+8, 0x00010000u);
    put32(h+12, 92);
    put64(h+24, 1); put64(h+32, NSECT-1);
    put64(h+40, 34); put64(h+48, NSECT-34);
    put64(h+72, 2);
    put32(h+80, 128); put32(h+84, 128);
    put32(h+88, ent_crc);
    put32(h+16, 0);
    put32(h+16, gpt_crc32(h, 92));
}

int main(void)
{
    gpt_part_t p[GPT_MAX_PARTS];

    head("CRC-32 matches the standard the UEFI spec points at");
    ck("crc32(\"\") is 0",            gpt_crc32("",0)==0u, (long)gpt_crc32("",0));
    ck("crc32(\"123456789\")",        gpt_crc32("123456789",9)==0xCBF43926u,
       (long)gpt_crc32("123456789",9));
    ck("crc32(\"a\")",                gpt_crc32("a",1)==0xE8B7BE43u,
       (long)gpt_crc32("a",1));

    head("a valid table is recognised and read");
    build_gpt();
    ck("gpt_present says yes", gpt_present(rd,0)==1, gpt_present(rd,0));
    int n = gpt_scan(rd,0,p,GPT_MAX_PARTS);
    ck("three partitions found", n==3, n);
    if(n==3){
        ck("first starts at 34",    p[0].first_lba==34, (long)p[0].first_lba);
        ck("first ends at 1000",    p[0].last_lba==1000, (long)p[0].last_lba);
        ck("first is the ESP",      p[0].is_esp==1, p[0].is_esp);
        ck("second is Microsoft",   p[1].is_msft==1, p[1].is_msft);
        ck("the ESP is not flagged Microsoft", p[0].is_msft==0, p[0].is_msft);
        ck("name decoded from UTF-16",
           streq(p[0].name,"EFI system partition"), 0);
        ck("third name decoded", streq(p[2].name,"Recovery"), 0);
    }

    head("an empty or foreign disk is NOT reported as GPT");
    memset(disk,0,sizeof disk);
    ck("a zeroed disk has no GPT", gpt_present(rd,0)==0, gpt_present(rd,0));
    ck("and scans to zero partitions", gpt_scan(rd,0,p,GPT_MAX_PARTS)==0, 0);
    memset(disk,0,sizeof disk);
    disk[446+4]=0x0E; disk[510]=0x55; disk[511]=0xAA;   /* plain FAT16 MBR */
    ck("an MBR-only disk has no GPT", gpt_present(rd,0)==0, gpt_present(rd,0));

    head("a corrupt header is rejected rather than half-parsed");
    build_gpt();
    disk[512+88] ^= 0xFF;                       /* break the entry-array CRC */
    ck("a wrong entry CRC still leaves the header CRC wrong",
       gpt_scan(rd,0,p,GPT_MAX_PARTS)==0, gpt_scan(rd,0,p,GPT_MAX_PARTS));
    build_gpt();
    disk[512+16] ^= 0xFF;                       /* break the header CRC */
    ck("a wrong header CRC is refused", gpt_scan(rd,0,p,GPT_MAX_PARTS)==0, 0);
    build_gpt();
    disk[512+12] = 0;  disk[512+13] = 0;        /* absurd header size */
    ck("an absurd header size is refused", gpt_scan(rd,0,p,GPT_MAX_PARTS)==0, 0);

    head("the dangerous direction: a real table must never read as absent");
    /* gpt_present only checks the signature, deliberately: it is the gate the
       installer uses, and it must say yes even for a table this parser would
       decline to walk. A disk with a GPT signature is a disk to leave alone. */
    build_gpt();
    disk[512+16] ^= 0xFF;                       /* header CRC broken */
    ck("a GPT with a bad CRC is STILL reported present",
       gpt_present(rd,0)==1, gpt_present(rd,0));
    build_gpt();
    put32(disk+512+84, 999);                    /* nonsense entry size */
    ck("a GPT with a nonsense entry size is STILL present",
       gpt_present(rd,0)==1, gpt_present(rd,0));

    head("a read error is reported, not treated as 'no GPT'");
    ck("gpt_scan returns -1 when the first read fails",
       gpt_scan(0,0,p,GPT_MAX_PARTS)==-1, 0);

    head("an entry that runs off the end of the disk is dropped");
    /* Not pedantry: a table that describes a partition the disk cannot hold
       is a table we do not understand, and the installer's answer to that
       has to be "leave it alone", not "parse the parts that look fine". */
    build_gpt();
    put64(disk + 2*512 + 40, 8);          /* last_lba < first_lba */
    ck("the impossible entry is skipped", gpt_scan(rd,0,p,GPT_MAX_PARTS)==2,
       gpt_scan(rd,0,p,GPT_MAX_PARTS));
    ck("but the disk is still reported as GPT", gpt_present(rd,0)==1, 0);

    head("the entry array is bounded");
    build_gpt();
    ck("asking for one partition returns one",
       gpt_scan(rd,0,p,1)==1, gpt_scan(rd,0,p,1));
    ck("max<=0 is refused", gpt_scan(rd,0,p,0)==-1, 0);

    printf("\n%d/%d passed\n", total-fails, total);
    return fails?1:0;
}
