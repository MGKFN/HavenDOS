/* ════════════════════════════════════════════════════════════════════
   test_diskname.c -- a disk is named for the bus it is really on.

   Four releases carried the same bug: a disk on one bus announced as
   being on another, on screens that ask a person to type YES before
   something irreversible happens.

     v0.8.3.1  the picker said NVMe, the confirmation said VirtIO.
     v0.8.5.3  on a laptop with one NVMe disk, the welcome screen said
               "WARNING: No disks found" directly above "1 VirtIO disk
               found -- will be auto-selected", and the desktop
               installer announced a SATA disk as "(VirtIO)".

   The fix each time was "name it in one place". What this suite tests is
   that the one place is RIGHT and that nothing is left that could
   disagree with it -- the second half is tools/check_bus_label.py's job
   and this is the first.

   ── v0.8.6: THIS SUITE NO LONGER HOLDS A COPY ────────────────────────

   It used to. The disk table lived in shell/iso_installer.c behind a
   wall of VGA and keyboard code, so the function under test could not be
   compiled on its own and this file carried a hand-kept copy of it,
   policed by tools/check_diskname_sync.py. That check existed because
   this tree's own rule says a suite holding a private copy of the
   function it is testing proves nothing -- and a build check guarding a
   copy is a lesser answer than not having one.

   fs/blkdev.c is the real function now, it compiles natively, and this
   file LINKS it. The ops tables below are simulated buses registered
   through blk_register(), so the table, the bounds checks and the words
   under test are the shipping ones. A green run here means the code that
   runs on the laptop is the code that passed.

   BLK_NO_HW keeps blk_scan() and the five real back-ends out, since
   there are no drivers to call here. Everything else in the file is
   compiled exactly as the kernel compiles it.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define BLK_NO_HW 1
/* v0.8.8: kpoll_enter()/kpoll_leave() for the guarded entry points
   this suite compiles -- see tests/kpollguard.inc. */
#include "kpollguard.inc"

#include "../fs/blkdev.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}

/* ── simulated buses ──────────────────────────────────────────────────
   One ops table per bus, carrying the SAME WORD the kernel's own tables
   carry -- which is the one thing this file must still state twice, so
   the assertions below are written against the specification and not
   against whatever fs/blkdev.c happens to say today. A typo in the
   kernel's table is a failure here rather than a pair of files agreeing
   with each other, which is the trap the copy this file used to hold
   fell into.

   tools/check_diskname_sync.py checks that the kernel's tables use these
   same words, so the two directions are both covered. */
static int sim_rd(int unit, uint64_t lba, uint8_t *buf){
    (void)unit; (void)lba; (void)buf; return 1;
}
static int sim_wr(int unit, uint64_t lba, const uint8_t *buf){
    (void)unit; (void)lba; (void)buf; return 1;
}
static int sim_ro_yes(int unit){ (void)unit; return 1; }

static const blk_ops_t ops_ide    = { "IDE",    sim_rd, sim_wr, 0, 0 };
static const blk_ops_t ops_sata   = { "SATA",   sim_rd, sim_wr, 0, 0 };
static const blk_ops_t ops_nvme   = { "NVMe",   sim_rd, sim_wr, 0, 0 };
static const blk_ops_t ops_virtio = { "VirtIO", sim_rd, sim_wr, 0, 0 };
static const blk_ops_t ops_usb    = { "USB",    sim_rd, sim_wr, 0, 1 };
/* A bus with no write function at all, for the refusal below. */
static const blk_ops_t ops_rdonly = { "USB",    sim_rd, 0,      0, 1 };
/* A bus that can write, holding a disk that cannot. */
static const blk_ops_t ops_perdisk= { "NVMe",   sim_rd, sim_wr, sim_ro_yes, 0 };

static const blk_ops_t *ops_for(uint8_t kind){
    switch(kind){
        case BLK_BUS_ATA:    return &ops_ide;
        case BLK_BUS_AHCI:   return &ops_sata;
        case BLK_BUS_NVME:   return &ops_nvme;
        case BLK_BUS_VIRTIO: return &ops_virtio;
        default:             return &ops_usb;
    }
}

static void only(uint8_t kind, uint64_t sectors){
    blk_reset();
    blk_register(ops_for(kind), kind, 0, sectors, 0, 0);
}

int main(void)
{
    head("every bus has its own name");
    only(BLK_BUS_ATA, 1000);
    ck("legacy ATA is IDE",    !strcmp(blk_bus_name(0), "IDE"), 0);
    only(BLK_BUS_AHCI, 1000);
    ck("AHCI is SATA",         !strcmp(blk_bus_name(0), "SATA"), 0);
    only(BLK_BUS_NVME, 1000);
    ck("NVMe is NVMe",         !strcmp(blk_bus_name(0), "NVMe"), 0);
    only(BLK_BUS_VIRTIO, 1000);
    ck("VirtIO is VirtIO",     !strcmp(blk_bus_name(0), "VirtIO"), 0);
    only(BLK_BUS_USB, 1000);
    ck("USB is USB",           !strcmp(blk_bus_name(0), "USB"), 0);

    head("the two machines this was found on");
    /* Kyan's HP: one NVMe disk, 488386 MB, nothing else on the bus. The
       welcome screen called this "no disks found" AND "1 VirtIO disk". */
    only(BLK_BUS_NVME, 1000215216ull);
    ck("an NVMe-only laptop is not VirtIO",
       strcmp(blk_bus_name(0), "VirtIO") != 0, 0);
    ck("...it is NVMe", !strcmp(blk_bus_name(0), "NVMe"), 0);
    {
        int ide=-1, sata=-1, nvme=-1, virtio=-1, usb=-1;
        blk_bus_counts(&ide,&sata,&nvme,&virtio,&usb);
        ck("and the counts say so",
           nvme==1 && ide==0 && sata==0 && virtio==0 && usb==0, nvme);
        ck("so a summary cannot conclude 'no disks'",
           ide+sata+nvme+virtio+usb == 1, ide+sata+nvme+virtio+usb);
    }
    /* The older HP: one AHCI disk, 122104 MB. The confirmation screen
       called it VirtIO on the screen gated behind typing YES. */
    only(BLK_BUS_AHCI, 250069680ull);
    ck("an AHCI-only laptop is not VirtIO",
       strcmp(blk_bus_name(0), "VirtIO") != 0, 0);
    ck("...it is SATA", !strcmp(blk_bus_name(0), "SATA"), 0);

    head("a disk index that is not a disk");
    only(BLK_BUS_ATA, 1000);
    ck("index 1 of 1 is 'none'",   !strcmp(blk_bus_name(1), "none"), 0);
    ck("a negative index is 'none'",!strcmp(blk_bus_name(-1), "none"), 0);
    ck("...and NOT VirtIO, which is what a default: arm would say",
       strcmp(blk_bus_name(1), "VirtIO") != 0, 0);
    blk_reset();
    ck("index 0 with no disks is 'none'",
       !strcmp(blk_bus_name(0), "none"), 0);
    {
        int ide=-1, sata=-1, nvme=-1, virtio=-1, usb=-1;
        blk_bus_counts(&ide,&sata,&nvme,&virtio,&usb);
        ck("and every count is zero",
           !ide && !sata && !nvme && !virtio && !usb,
           ide+sata+nvme+virtio+usb);
    }

    head("several disks on different buses");
    blk_reset();
    blk_register(&ops_nvme,   BLK_BUS_NVME,   0, 100, 0, 0);
    blk_register(&ops_sata,   BLK_BUS_AHCI,   0, 100, 0, 0);
    blk_register(&ops_virtio, BLK_BUS_VIRTIO, 0, 100, 0, 0);
    blk_register(&ops_ide,    BLK_BUS_ATA,    0, 100, 0, 0);
    blk_register(&ops_usb,    BLK_BUS_USB,    0, 100, 0, 0);
    ck("each is named for its own bus",
       !strcmp(blk_bus_name(0),"NVMe") &&
       !strcmp(blk_bus_name(1),"SATA") &&
       !strcmp(blk_bus_name(2),"VirtIO") &&
       !strcmp(blk_bus_name(3),"IDE") &&
       !strcmp(blk_bus_name(4),"USB"), 0);
    {
        int ide=0, sata=0, nvme=0, virtio=0, usb=0;
        blk_bus_counts(&ide,&sata,&nvme,&virtio,&usb);
        ck("the counts add up to the table",
           ide+sata+nvme+virtio+usb == blk_count(),
           ide+sata+nvme+virtio+usb);
        ck("one of each",
           ide==1 && sata==1 && nvme==1 && virtio==1 && usb==1, 0);
    }

    head("counts tolerate a caller that wants only some of them");
    {
        int nvme = -1;
        blk_bus_counts(0, 0, &nvme, 0, 0);
        ck("asking for one does not fault", nvme == 1, nvme);
        blk_bus_counts(0, 0, 0, 0, 0);
        ck("asking for none does not fault", 1, 0);
    }

    head("a bus this layer has not been taught about counts as nothing");
    /* The default: arm that answered VirtIO for a disk nobody had taught
       it about is the shape of all four shipped bugs -- so a kind
       outside the five must count as none of them rather than fall into
       the commonest one. Found missing by
       tools/check_diskname_suite.py. */
    blk_reset();
    blk_register(&ops_usb, 99, 0, 100, 0, 0);   /* not a BLK_BUS_* */
    {
        int ide=-1, sata=-1, nvme=-1, virtio=-1, usb=-1;
        blk_bus_counts(&ide,&sata,&nvme,&virtio,&usb);
        ck("the disk is in the table", blk_count() == 1, blk_count());
        ck("but it counts on no bus",
           !ide && !sata && !nvme && !virtio && !usb,
           ide+sata+nvme+virtio+usb);
        ck("...and NOT as VirtIO, which is what a default: arm would do",
           virtio == 0, virtio);
    }


    /* ── v0.8.6: the words are not the only thing one place now owns ──
       The label was guarded since v0.8.3.1 and the DISPATCH was not, and
       that is how fs/fat32.c went two releases unable to see an NVMe or
       an AHCI disk while every screen named them correctly. These check
       the rest of what moved. */
    head("a disk's read-only state comes from its bus, not from a switch");
    blk_reset();
    blk_register(&ops_rdonly, BLK_BUS_USB, 0, 100, 0, 0);
    ck("a bus with no write function is read-only", blk_readonly(0), 0);
    ck("...and blk_write refuses rather than faulting",
       blk_write(0, 0, (const uint8_t *)"x") == 0, 0);
    ck("...while reading still works", blk_read(0, 0, (uint8_t *)"x") == 1, 0);

    blk_reset();
    blk_register(&ops_perdisk, BLK_BUS_NVME, 0, 100, 0, 0);
    ck("a writable bus can still hold a read-only disk", blk_readonly(0), 0);
    ck("...and that disk refuses a write too",
       blk_write(0, 0, (const uint8_t *)"x") == 0, 0);

    blk_reset();
    blk_register(&ops_nvme, BLK_BUS_NVME, 0, 100, 0, 0);
    ck("an ordinary disk on an ordinary bus is writable", !blk_readonly(0), 0);
    ck("...and the write reaches the bus",
       blk_write(0, 0, (const uint8_t *)"x") == 1, 0);

    head("an index that is not a disk is read-only, not writable");
    blk_reset();
    ck("no disks: index 0 is read-only", blk_readonly(0), 0);
    ck("a negative index is read-only",  blk_readonly(-1), 0);
    ck("...and a write to it does nothing",
       blk_write(0, 0, (const uint8_t *)"x") == 0, 0);
    ck("...and so does a read", blk_read(0, 0, (uint8_t *)"x") == 0, 0);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
