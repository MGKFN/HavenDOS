/* ════════════════════════════════════════════════════════════════════
   blkbus.inc -- the rest of what fs/blkdev.c's blk_scan() calls.

   v0.8.6 put one block layer under the three bus-dispatch switches that
   fs/fat16.c, fs/fat32.c and shell/iso_installer.c each used to carry.
   The suites that compile fs/fat16.c or fs/fat32.c therefore compile
   fs/blkdev.c too -- which is the point, because the bus-selection
   logic those suites exist to test MOVED there. tests/test_fat16nvme.c
   in particular asks "can an installed system on an NVMe drive read
   itself back", and the answer is now blk_boot_dev()'s.

   Each suite already stubs the raw sector functions for the buses it
   simulates, with its own knobs: a file-backed image, a model GPT disk,
   a count of AHCI ports. This file supplies only the REMAINING entry
   points blk_scan() needs -- the counts, the sizes and the inits -- in
   terms of those stubs, so a suite's own knobs still decide which buses
   exist and the real blk_scan() does the registering.

   Include it AFTER the suite's own stubs and BEFORE fs/blkdev.c.

   The sector counts are nominal: nothing in fs/fat16.c or fs/fat32.c
   reads a disk's size, and a suite that cares states its own by defining
   BLKSTUB_SECTORS first.
   ════════════════════════════════════════════════════════════════════ */

#ifndef BLKSTUB_SECTORS
#define BLKSTUB_SECTORS  0x10000000ull    /* 128 GiB in 512-byte sectors */
#endif

/* The suite's own stubs, declared so this file does not rely on having
   been included after each of them. An implicit declaration here would
   compile and would be an int-returning guess at a function whose real
   stub returns something else. */
int virtio_blk_ready(void);
int virtio_blk_read (uint64_t lba, uint8_t *buf);
int virtio_blk_write(uint64_t lba, const uint8_t *buf);
int ata_detected(void);
int ahci_count(void);

/* ── VirtIO ───────────────────────────────────────────────────────────
   The multi-drive face, in terms of the singleton one every suite
   already stubs. One drive when the suite says a disk is there. */
int      virtio_scan_drives(void){ return virtio_blk_ready() ? 1 : 0; }
uint64_t virtio_drive_sectors(int i){
    (void)i; return virtio_blk_ready() ? BLKSTUB_SECTORS : 0;
}
uint8_t  virtio_drive_bus(int i){ (void)i; return 0; }
uint8_t  virtio_drive_dev(int i){ (void)i; return 0; }
int virtio_drive_read(int i, uint64_t l, uint8_t *b){
    (void)i; return virtio_blk_read(l, b);
}
int virtio_drive_write(int i, uint64_t l, const uint8_t *b){
    (void)i; return virtio_blk_write(l, b);
}

/* ── NVMe ─────────────────────────────────────────────────────────────
   Writable, because the one suite that drives this bus writes through
   it. A namespace this layer cannot write is drivers/nvme.c's own
   answer and tests/test_nvmerw.c is where that is tested. */
uint64_t nvme_sectors(int i){ (void)i; return BLKSTUB_SECTORS; }
int      nvme_writable(int i){ (void)i; return 1; }

/* ── AHCI ─────────────────────────────────────────────────────────────
   ahci_init() returns the suite's port count rather than probing, so a
   suite that sets g_ahci_n sees exactly that many. */
int      ahci_init(void){ return ahci_count(); }
uint64_t ahci_sectors(int i){ (void)i; return BLKSTUB_SECTORS; }

/* ── legacy ATA ─────────────────────────────────────────────────────── */
void     ata_init(void){ }
uint32_t ata_sectors(void){
    return ata_detected() ? (uint32_t)0x08000000u : 0u;   /* 64 GiB */
}

/* ── USB mass storage ─────────────────────────────────────────────────
   No stick in MOST of these suites. usb/msc.c has its own suite, and a
   removable disk appearing in a FAT16 mount test would be testing two
   things at once.

   Zero is also the interesting value here: blk_scan() registers USB
   LAST so a stick cannot renumber a fixed disk, and these suites assert
   indices. tests/test_blkdev.c is where that ordering is checked with a
   stick present.

   A suite that needs a real stick defines BLKSTUB_USB_OWN before
   including this and supplies the six below itself --
   tests/test_fat16borrow.c does, because a writable borrow of a
   removable disk cannot be tested without one. The alternative was a
   second copy of this whole file, which is the shape v0.8.6 deleted
   three of. */
#ifndef BLKSTUB_USB_OWN
int      msc_count(void){ return 0; }
uint64_t msc_sectors(int i){ (void)i; return 0; }
int      msc_readonly(int i){ (void)i; return 1; }
int msc_read_sector(int i, uint64_t l, uint8_t *b){
    (void)i; (void)l; (void)b; return 0;
}
int msc_write_sector(int i, uint64_t l, const uint8_t *b){
    (void)i; (void)l; (void)b; return 0;
}
#endif /* BLKSTUB_USB_OWN */

#include "../fs/blkdev.c"
