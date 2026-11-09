#pragma once
#include "types.h"

/* USB mass storage -- Bulk-Only Transport with SCSI over it.
 *
 * usb/msc.c carries the argument for the shape of this: why the recovery
 * paths are the substance, why the CBW tag is load-bearing, and what is
 * deliberately not done (one LUN, no writes to a non-512-byte block
 * device, no hot-plug).
 *
 * fs/blkdev.c is the only caller of the read/write pair -- a USB disk is
 * a disk, and nothing above the block layer knows which bus it is on.
 * That is the whole point of the layer and the thing three separate bus
 * switches got wrong before 0.8.6.
 */

/* Walk the USB device table and bring up every Bulk-Only mass storage
 * device on it. Returns how many are usable.
 *
 * MUST be re-run after usb_enumerate(), which rebuilds the table this
 * reads: each device here holds a COPY of its usb_dev_t, so a stale one
 * addresses whatever moved into that slot. msc_rescan() is that call. */
int      msc_probe(void);
int      msc_rescan(void);

int      msc_count(void);          /* probes lazily if nothing has yet  */
uint64_t msc_sectors(int i);       /* in 512-byte sectors               */
int      msc_readonly(int i);
int      msc_removable(int i);     /* the SCSI RMB bit                  */
uint32_t msc_blocksize(int i);     /* the DEVICE's block size           */
const char *msc_vendor(int i);
const char *msc_product(int i);

/* 512-byte sectors, translated to device blocks inside. 1 on success. */
int msc_read_sector (int i, uint64_t lba, uint8_t *buf);
int msc_write_sector(int i, uint64_t lba, const uint8_t *buf);
