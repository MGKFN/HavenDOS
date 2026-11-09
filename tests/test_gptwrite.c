/* Adding a partition to somebody's Windows disk without losing the disk.
 *
 * This is the suite the 1.0 plan calls not negotiable, and it is written
 * before fs/gpt_write.c rather than after it.
 *
 * ── WHY IT IS SHAPED LIKE THIS ────────────────────────────────────────
 *
 * The disk under test is not a toy. It is a 512 GB drive laid out the way
 * Windows 11 lays one out — protective MBR, EFI System Partition,
 * Microsoft Reserved, a ~510 GB NTFS volume and a recovery partition —
 * with a 1 GB unallocated gap between the NTFS volume and recovery,
 * exactly as Disk Management leaves one after you shrink C: by 1 GB. That
 * is the disk this has to work on, so that is the disk it is tested on.
 *
 * The disk is SPARSE: a sector exists only once something has written it.
 * That is what makes the central claim checkable. A dense image can only
 * prove "the bytes that were there did not change"; a sparse one also
 * proves "nothing appeared anywhere it should not have", because a write
 * to a sector that was previously nothing is just as visible as a write
 * to a sector that held something. Every write is logged with its LBA, so
 * the test asserts the exact set of sectors touched rather than taking the
 * header's word for it.
 *
 * It runs against a 4 TB disk as well. 512 GB is 2^30 sectors and fits in
 * 32 bits; 4 TB is 2^33 and does not. A truncation bug would put the
 * backup header somewhere inside somebody's data, and on the 512 GB disk
 * alone it would never show.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* memset/memcpy/memcmp used to be defined here with uint32_t lengths,
   because <string.h> resolves to the kernel's own include/string.h on this
   include path and the two sets of prototypes disagreed. fs/gpt_write.c
   now includes include/string.h itself -- it had to, so that it and
   fs/fat32.c can live in one translation unit for
   tests/test_installadd.c -- and include/string.h declares them with
   size_t, which is what the host's libc provides. So there is nothing to
   declare here, and nothing to implement: the real ones are linked in. */

#include "../fs/gpt.c"
#include "../fs/gpt_write.c"

/* ── scoreboard ─────────────────────────────────────────────────────── */
static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-58s got %ld\n", what, got); }
}

#include "gptdisk.inc"


int main(void)
{
    gpt_part_t parts[GPT_MAX_PARTS];
    gpt_gap_t  gaps[8];
    uint64_t   wr[8];
    int        nwr = 0, idx = 0;

    printf("Adding a partition to a Windows disk, without losing the disk\n");
    printf("=============================================================\n");

    /* ── 512 GB, the real target ─────────────────────────────────── */
    build_disk(1073741824ULL);                       /* 512 GB */

    head("the fixture is the disk we actually care about");
    ck("it reads as a GPT", gpt_present(d_read, 0) == 1, 0);
    ck("five partitions, the Windows shape plus an OEM area",
       gpt_scan(d_read, 0, parts, GPT_MAX_PARTS) == 5,
       gpt_scan(d_read, 0, parts, GPT_MAX_PARTS));
    ck("the first is the ESP", parts[0].is_esp, 0);
    ck("the table verifies the way firmware checks it", table_verifies(), 0);

    head("the free space is found, and it is the gap Disk Management left");
    {
        int n = gpt_find_gaps(d_read, 0, gaps, 8);
        ck("gaps are found", n >= 1, n);
        gpt_gap_t g;
        ck("a 1 GB request is satisfied",
           gpt_find_free(d_read, 0, 2097152ULL, &g) == GPTW_OK, 0);
        ck("and it is the gap between the data volume and recovery",
           g.first_lba == WIN_FREE_FIRST && g.last_lba == WIN_FREE_LAST,
           (long)(g.first_lba - WIN_FREE_FIRST));
        ck("a request bigger than any gap is refused, not rounded down",
           gpt_find_free(d_read, 0, 900ULL*1024*1024*2, &g) == GPTW_E_NOSPACE, 0);
    }

    /* ── THE CENTRAL CLAIM ───────────────────────────────────────── */
    head("adding the partition touches FOUR sectors and no others");
    snapshot();
    g_logging = 1; g_nwlog = 0;
    {
        int r = gpt_add_partition(d_read, d_write, 0,
                                  WIN_FREE_FIRST, WIN_FREE_LAST,
                                  GPT_TYPE_HAVENDOS, "HavenDOS",
                                  &idx, wr, &nwr);
        ck("the write is accepted", r == GPTW_OK, r);
        ck("it claimed a free entry slot, not an occupied one", idx == 5, idx);
        ck("and it did not mistake the OEM entry for a free slot just\n"
           "         because its type GUID starts with a zero byte",
           idx != 4, idx);
        ck("exactly four sectors were written", nwr == 4, nwr);
        ck("and the driver's own count agrees with the disk's log",
           g_nwlog == 4, g_nwlog);

        uint64_t last = g_disk_sectors - 1;
        uint64_t bk_ent = last - ENTSECS;
        /* entry 4 is the 5th of 4-per-sector, so sector 1 of the array */
        /* entry 5 is the 6th of four-per-sector, so sector 1 of the array */
        ck("backup entry sector",  wr[0] == bk_ent + 1, (long)wr[0]);
        ck("backup header",        wr[1] == last,       (long)wr[1]);
        ck("primary entry sector", wr[2] == 2 + 1,      (long)wr[2]);
        ck("primary header",       wr[3] == 1,          (long)wr[3]);
        ck("the primary header is written LAST, so a power cut leaves the\n"
           "         copy firmware reads either wholly old or wholly new",
           wr[3] == 1 && nwr == 4, 0);
    }
    g_logging = 0;

    head("every other sector on the disk is byte-identical");
    {
        int bad = identical_except(wr, nwr);
        ck("not one sector outside those four changed", bad == 0, bad);
        /* said another way, and worth saying: 31 of the 32 entry sectors
           in each copy were never written at all */
        ck("31 of 32 primary entry sectors untouched",
           nwr == 4 && wr[2] == 3, 0);
    }

    head("and the disk is still a disk");
    ck("the table still verifies", table_verifies(), 0);
    {
        int n = gpt_scan(d_read, 0, parts, GPT_MAX_PARTS);
        ck("six partitions now", n == 6, n);
        ck("the ESP is unchanged",
           parts[0].is_esp && parts[0].first_lba == 2048
           && parts[0].last_lba == 616447, 0);
        ck("the Windows data volume is unchanged",
           parts[2].first_lba == 648192, (long)parts[2].first_lba);
        ck("recovery is unchanged",
           parts[3].last_lba == WIN_FREE_LAST + 1 + 1048575,
           (long)parts[3].last_lba);
        ck("the OEM partition is unchanged",
           parts[4].first_lba == 1024 && parts[4].last_lba == 2047,
           (long)parts[4].first_lba);
        ck("and ours is where we asked for it",
           parts[5].first_lba == WIN_FREE_FIRST
           && parts[5].last_lba == WIN_FREE_LAST, 0);
    }

    /* ── the refusals ────────────────────────────────────────────── */
    head("what it refuses, every one before a byte is written");
    build_disk(1073741824ULL);
    snapshot();
    {
        struct { const char *what; uint64_t a, b; int want; } R[] = {
          { "a range overlapping the Windows volume by one sector",
            648192ULL, 700000ULL, GPTW_E_OVERLAP },
          { "a range whose LAST sector clips the ESP",
            1000ULL, 2048ULL, GPTW_E_OVERLAP },
          { "a range that swallows a whole partition",
            600000ULL, 700000ULL, GPTW_E_OVERLAP },
          /* The two exact-boundary cases, which are the whole reason the
             overlap test is <= and not <. Both sit in free space except
             for a SINGLE sector of a neighbour, and free space on the
             other side -- so no other partition catches them, and an
             off-by-one here silently eats one sector of the Windows
             volume or one sector of the recovery partition. */
          { "a range starting on the data volume's very last sector",
            0, 0, GPTW_E_OVERLAP },
          { "a range ending on recovery's very first sector",
            0, 0, GPTW_E_OVERLAP },
          { "a range below FirstUsableLBA",
            2ULL, 100ULL, GPTW_E_GEOM },
          { "a range past LastUsableLBA",
            1073741000ULL, 1073741900ULL, GPTW_E_GEOM },
          { "a backwards range", 700000ULL, 600000ULL, GPTW_E_PARAM },
          { "a zero-length range", 0ULL, 0ULL, GPTW_E_GEOM },
        };
        R[3].a = WIN_FREE_FIRST - 1; R[3].b = WIN_FREE_LAST;
        R[4].a = WIN_FREE_FIRST;     R[4].b = WIN_FREE_LAST + 1;
        for(unsigned i=0;i<sizeof R/sizeof R[0];i++){
            int r = gpt_add_partition(d_read, d_write, 0, R[i].a, R[i].b,
                                      GPT_TYPE_HAVENDOS, "HavenDOS",
                                      &idx, wr, &nwr);
            ck(R[i].what, r == R[i].want, r);
        }
        ck("and after every refusal the disk is untouched",
           identical_except(0, 0) == 0, identical_except(0, 0));
    }

    head("a table it does not understand is a table it will not write to");
    {
        build_disk(1073741824ULL);
        g_dsec[0].b[0] = 0;                       /* nothing; find LBA 1 */
        sec_t *h = find(1);
        h->b[16] ^= 0xFF;                        /* break the header CRC */
        snapshot();
        int r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                                  WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                                  "HavenDOS", &idx, wr, &nwr);
        ck("a bad primary header CRC is refused", r == GPTW_E_HDRCRC, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        build_disk(1073741824ULL);
        find(3)->b[0] ^= 0xFF;                   /* corrupt the entry array */
        snapshot();
        r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                              WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                              "HavenDOS", &idx, wr, &nwr);
        ck("a bad entry-array CRC is refused", r == GPTW_E_ENTCRC, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        build_disk(1073741824ULL);
        memset(find(g_disk_sectors-1)->b, 0, 512);   /* destroy the backup */
        snapshot();
        r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                              WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                              "HavenDOS", &idx, wr, &nwr);
        ck("a missing backup header is refused", r == GPTW_E_BACKUP, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        build_disk(1073741824ULL);
        /* A backup header that still SAYS the right things -- same entry
           CRC, same count, same size -- but whose own checksum no longer
           covers its contents. Zeroing the backup is caught by three
           different guards; this is caught only by actually verifying its
           CRC, which is the guard that matters. */
        find(g_disk_sectors-1)->b[56] ^= 0xFF;       /* inside the CRC, not compared */
        snapshot();
        r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                              WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                              "HavenDOS", &idx, wr, &nwr);
        ck("a backup header whose own CRC is stale is refused",
           r == GPTW_E_BACKUP, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        build_disk(1073741824ULL);
        memset(find(1)->b, 0, 512);                  /* no GPT at all */
        snapshot();
        r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                              WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                              "HavenDOS", &idx, wr, &nwr);
        ck("a disk with no GPT is refused", r == GPTW_E_NOTGPT, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);
    }

    head("a full entry array is refused rather than overwriting entry 0");
    {
        build_disk(1073741824ULL);
        /* fill every slot, then recompute both CRCs so the table is valid */
        static uint8_t ent[ENTSECS*512];
        for(uint32_t s=0;s<ENTSECS;s++) memcpy(ent+s*512, find(2+s)->b, 512);
        for(int i=5;i<NENT;i++){
            uint8_t *e = ent + i*ENTSZ;
            memcpy(e, T_DATA, 16);
            for(int k=0;k<16;k++) e[16+k] = (uint8_t)(0x90+k+i);
            p64(e+32, 100ULL); p64(e+40, 200ULL);   /* overlapping, but present */
        }
        uint32_t crc = gpt_crc32(ent, NENT*ENTSZ);
        uint64_t last = g_disk_sectors-1, bk = last-ENTSECS;
        for(uint32_t s=0;s<ENTSECS;s++){
            memcpy(make(2+s)->b, ent+s*512, 512);
            memcpy(make(bk+s)->b, ent+s*512, 512);
        }
        for(int which=0;which<2;which++){
            uint8_t *h = find(which?last:1)->b;
            p32(h+88, crc); p32(h+16, 0);
            uint8_t t[512]; memcpy(t,h,512);
            p32(h+16, gpt_crc32(t, g32(h+12)));
        }
        snapshot();
        int r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                                  WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                                  "HavenDOS", &idx, wr, &nwr);
        ck("no free slot is refused", r == GPTW_E_NOSLOT, r);
        ck("and nothing was written", identical_except(0,0) == 0, 0);
    }

    head("the dry run writes nothing and tells the truth");
    {
        build_disk(1073741824ULL);
        snapshot();
        int r = gpt_add_partition(d_read, NULL, 0, WIN_FREE_FIRST,
                                  WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                                  "HavenDOS", &idx, wr, &nwr);
        ck("a dry run of a legal write reports OK", r == GPTW_OK, r);
        ck("it names the four sectors it would have written", nwr == 4, nwr);
        ck("and the disk is untouched", identical_except(0,0) == 0, 0);
        ck("it still claims the slot it would have used", idx == 5, idx);

        r = gpt_add_partition(d_read, NULL, 0, 648192ULL, 700000ULL,
                              GPT_TYPE_HAVENDOS, "HavenDOS", &idx, wr, &nwr);
        ck("a dry run of an illegal write reports the same refusal",
           r == GPTW_E_OVERLAP, r);
        ck("and the disk is still untouched", identical_except(0,0) == 0, 0);
    }

    /* ── 4 TB: the LBAs that do not fit in 32 bits ───────────────── */
    head("a 4 TB disk, where the backup header is past the 32-bit horizon");
    {
        build_disk(8589934592ULL);               /* 4 TB */
        ck("the fixture is really past 2^32",
           g_disk_sectors - 1 > 0xFFFFFFFFULL, 0);
        ck("it reads as a GPT", gpt_present(d_read, 0) == 1, 0);
        ck("five partitions", gpt_scan(d_read,0,parts,GPT_MAX_PARTS) == 5, 0);
        snapshot();
        g_logging = 1; g_nwlog = 0;
        int r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                                  WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                                  "HavenDOS", &idx, wr, &nwr);
        g_logging = 0;
        ck("the write is accepted", r == GPTW_OK, r);
        ck("the backup header went to the real last LBA, not a truncated one",
           nwr == 4 && wr[1] == g_disk_sectors - 1, (long)(nwr?wr[1]:0));
        ck("four sectors, as before", nwr == 4, nwr);
        ck("every other sector identical", identical_except(wr, nwr) == 0, 0);
        ck("the table still verifies", table_verifies(), 0);
        ck("six partitions", gpt_scan(d_read,0,parts,GPT_MAX_PARTS) == 6, 0);
        ck("ours is at the 4 TB offset we asked for",
           parts[5].first_lba == WIN_FREE_FIRST, 0);
    }

    head("an entry array whose length is not a whole number of sectors");
    {
        /* 128 entries of 128 bytes is 16384 bytes, which is exactly 32
           sectors -- so a CRC taken over the sector count and a CRC taken
           over the entry count are the same number, and an implementation
           that uses the wrong one is indistinguishable. 130 entries is
           16640 bytes, which is 32.5 sectors. The spec says the CRC is
           over n_ent * entry_size; this is the fixture that proves we do
           that and not the other thing. */
        build_disk_n(1073741824ULL, 130);
        ck("the odd-sized table reads as a GPT", gpt_present(d_read, 0) == 1, 0);
        ck("and verifies", table_verifies(), 0);
        snapshot();
        g_logging = 1; g_nwlog = 0;
        int r = gpt_add_partition(d_read, d_write, 0, WIN_FREE_FIRST,
                                  WIN_FREE_LAST, GPT_TYPE_HAVENDOS,
                                  "HavenDOS", &idx, wr, &nwr);
        g_logging = 0;
        ck("the write is accepted", r == GPTW_OK, r);
        ck("still four sectors", nwr == 4, nwr);
        ck("every other sector identical", identical_except(wr, nwr) == 0, 0);
        ck("and the table still verifies, which it only can if the entry\n"
           "         CRC was taken over 130 entries and not over 33 sectors",
           table_verifies(), 0);
        ck("six partitions", gpt_scan(d_read,0,parts,GPT_MAX_PARTS) == 6, 0);
        build_disk(1073741824ULL);
    }

    head("backing the table up before touching it");
    {
        build_disk(1073741824ULL);
        static uint8_t buf[70*512];
        int n = gpt_backup_table(d_read, 0, buf, sizeof buf);
        ck("the backup succeeds", n > 0, n);
        ck("it is both copies, headers and entries",
           n == (int)((1 + ENTSECS) * 2 * 512), n);
        ck("and it starts with the primary header",
           memcmp(buf, "EFI PART", 8) == 0, 0);
        ck("a buffer too small is refused rather than truncated",
           gpt_backup_table(d_read, 0, buf, 100) == GPTW_E_PARAM, 0);
    }


    /* ════════════════════════════════════════════════════════════════
       v0.8.8.8: TAKING ONE BACK OUT

       gpt_remove_partition() is the narrow opposite of the call above
       and is held to exactly the same claim: four sectors, named in
       advance, and every other sector on the disk byte-identical
       afterwards. It is the more dangerous of the two -- adding a
       partition in free space harms nothing if it is wrong, removing
       one takes somebody's filesystem off the table -- so the
       refusals are tested before the success.
       ════════════════════════════════════════════════════════════════ */
    head("removing a partition you cannot name is REFUSED");
    {
        gpt_part_t p2[GPT_MAX_PARTS];
        int i2;
        uint8_t wrongtype[16];

        /* A FRESH disk with a partition of our own on it. The sections
           between here and the add at the top rebuild the disk several
           times over -- for the 4 TB case and the odd entry array --
           so the partition added up there is long gone, and the first
           run of this block spent its checks discovering that rather
           than testing a removal. */
        build_disk(1073741824ULL);
        ck("a HavenDOS partition is added to remove",
           gpt_add_partition(d_read, d_write, 0,
                             WIN_FREE_FIRST, WIN_FREE_LAST,
                             GPT_TYPE_HAVENDOS, "HavenDOS",
                             &idx, wr, &nwr) == GPTW_OK, 0);

        for(i2 = 0; i2 < 16; i2++) wrongtype[i2] = GPT_TYPE_HAVENDOS[i2];
        wrongtype[0] ^= 0xFF;

        snapshot();
        ck("a wrong TYPE GUID is refused",
           gpt_remove_partition(d_read, d_write, 0, idx,
                                wrongtype, WIN_FREE_FIRST, 0, 0)
               == GPTW_E_MISMATCH, 0);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        snapshot();
        ck("a wrong FIRST LBA is refused",
           gpt_remove_partition(d_read, d_write, 0, idx,
                                GPT_TYPE_HAVENDOS, WIN_FREE_FIRST + 2048, 0, 0)
               == GPTW_E_MISMATCH, 0);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        snapshot();
        ck("a slot holding somebody ELSE's partition is refused\n"
           "         (the ESP, named with OUR type and OUR start LBA)",
           gpt_remove_partition(d_read, d_write, 0, 0,
                                GPT_TYPE_HAVENDOS, WIN_FREE_FIRST, 0, 0)
               == GPTW_E_MISMATCH, 0);
        ck("and the ESP is still there", identical_except(0,0) == 0, 0);

        snapshot();
        ck("an empty slot is refused as EMPTY, not silently accepted",
           gpt_remove_partition(d_read, d_write, 0, 20,
                                GPT_TYPE_HAVENDOS, WIN_FREE_FIRST, 0, 0)
               == GPTW_E_EMPTY, 0);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        snapshot();
        ck("a slot index past the end of the array is refused",
           gpt_remove_partition(d_read, d_write, 0, 9999,
                                GPT_TYPE_HAVENDOS, WIN_FREE_FIRST, 0, 0)
               == GPTW_E_PARAM, 0);
        ck("and nothing was written", identical_except(0,0) == 0, 0);

        head("a dry run checks everything and writes nothing");
        snapshot();
        {
            uint64_t dw[4]; int dn = 0;
            ck("the dry run says it would succeed",
               gpt_remove_partition(d_read, 0, 0, idx,
                                    GPT_TYPE_HAVENDOS, WIN_FREE_FIRST,
                                    dw, &dn) == GPTW_OK, 0);
            ck("it names four sectors in advance", dn == 4, dn);
            ck("the same four the add named",
               dn == 4 && dw[0] == wr[0] && dw[1] == wr[1]
                       && dw[2] == wr[2] && dw[3] == wr[3], 0);
        }
        ck("and the disk is untouched", identical_except(0,0) == 0, 0);

        head("removing it touches FOUR sectors and no others");
        snapshot();
        g_logging = 1; g_nwlog = 0;
        {
            uint64_t rw[4]; int rn = 0;
            int r2 = gpt_remove_partition(d_read, d_write, 0, idx,
                                          GPT_TYPE_HAVENDOS, WIN_FREE_FIRST,
                                          rw, &rn);
            ck("the removal is accepted", r2 == GPTW_OK, r2);
            ck("exactly four sectors were written", rn == 4, rn);
            ck("and the disk's own log agrees", g_nwlog == 4, g_nwlog);
            ck("the primary header is written LAST here too",
               rn == 4 && rw[3] == 1, (long)rw[3]);
            g_logging = 0;
            ck("not one sector outside those four changed",
               identical_except(rw, rn) == 0, identical_except(rw, rn));
        }
        g_logging = 0;

        ck("the table still verifies the way firmware checks it",
           table_verifies(), 0);
        {
            int n3 = gpt_scan(d_read, 0, p2, GPT_MAX_PARTS);
            int still = 0, k3;
            ck("five partitions again", n3 == 5, n3);
            for(k3 = 0; k3 < n3; k3++)
                if(p2[k3].first_lba == WIN_FREE_FIRST) still = 1;
            ck("ours is gone", !still, still);
            ck("the ESP is untouched",
               p2[0].is_esp && p2[0].first_lba == 2048
               && p2[0].last_lba == 616447, 0);
        }

        head("the DATA is left alone -- only the table entry is cleared");
        {
            /* Re-adding an entry over the same range must give back a
               partition in the same place. That is the whole reason
               the caller has to know the range: a removal is
               recoverable precisely because nothing was wiped. */
            int idx2 = -1;
            ck("the same range can be claimed again",
               gpt_add_partition(d_read, d_write, 0,
                                 WIN_FREE_FIRST, WIN_FREE_LAST,
                                 GPT_TYPE_HAVENDOS, "HavenDOS",
                                 &idx2, 0, 0) == GPTW_OK, 0);
            ck("and it lands in the same slot it vacated", idx2 == idx, idx2);
        }
    }

    printf("\n%d/%d passed\n", total-fails, total);
    return fails ? 1 : 0;
}
