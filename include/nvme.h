#pragma once
#include "types.h"

/* NVM Express.
 *
 * This is the driver that lets HavenDOS see the disk in a machine built
 * after about 2015.  It was deliberately READ-ONLY until v0.8.4, and the
 * reason is worth keeping where it cannot be missed, because it is also
 * the reason the write path arrived when it did:
 *
 *   Until this file existed, the one thing protecting a dual-boot machine
 *   from HavenDOS was that HavenDOS could not find its disk.  The installer
 *   in fs/install.c zeroes LBA 0..63 and the last 33 sectors on purpose --
 *   that is both GPT headers and the EFI partition entries.  On a laptop
 *   with Windows on it, running that is not "a partition overwritten", it
 *   is the partition table gone, both copies.
 *
 *   So the driver exposed no write path at all, and the installer refused
 *   to list an NVMe namespace as a target.  The condition set on that
 *   refusal was: writing comes with the GPT-aware, additive installer, not
 *   before it.  That installer shipped in v0.8.3 "Lodger", proved against
 *   a Windows-shaped disk under UEFI firmware -- four sectors written
 *   outside the new partition, every other sector byte-identical -- so the
 *   condition is met and nvme_write() exists.
 *
 *   The destructive whole-disk installer still cannot reach an NVMe drive.
 *   It refuses any disk carrying a GPT, which every NVMe drive in a
 *   machine made this decade has.
 *
 * READ-MODIFY-WRITE is not optional here and is the whole hazard of the
 * write path.  A namespace whose blocks are 4096 bytes cannot have 512
 * bytes written to it; the smallest thing that can be written is a block.
 * Writing one without reading it first destroys the seven sectors beside
 * the one being written -- silently, with every return code saying
 * success.  nvme_write() reads any block it is not completely filling.
 *
 * Polled throughout: the completion queue's phase bit is checked in a loop
 * rather than waiting on an interrupt, for the same reason the UHCI driver
 * polls -- IRQs are unreliable under UTM SE's interpreter.
 */

#define NVME_MAX_DEVS 4

int      nvme_init(void);            /* finds and brings up controllers */
int      nvme_count(void);           /* usable namespaces found         */
uint64_t nvme_sectors(int idx);      /* namespace size, in 512B sectors */
uint32_t nvme_sector_size(int idx);
int      nvme_read(int idx, uint64_t lba, uint32_t count, void *buf);
int      nvme_write(int idx, uint64_t lba, uint32_t count, const void *buf);
/* 1 if this namespace can be written: the controller came up AND the
   namespace does not report itself write protected (NSATTR bit 0). The
   installer asks before offering the disk, not after starting on it. */
int      nvme_writable(int idx);
/* Given a 4096-byte Identify Namespace structure, is the namespace write
   protected?  Exposed so it can be tested without simulating the admin
   queue the identify command travels on. */
int      nvme_ns_write_protected(const uint8_t *idns);
/* Parse a 4096-byte Identify Namespace page into the three numbers every
   read and write is computed from: size in blocks, block size in bytes,
   and whether it is write protected. 1 on success, 0 if the namespace
   does not exist. Exposed so it can be tested without a controller. */
int      nvme_ns_parse(const uint8_t *idns, uint64_t *nsze_out,
                       uint32_t *lba_bytes_out, int *ro_out);

/* For `disk` and the boot log. */
const char *nvme_model(int idx);
int         nvme_bar_was_high(int idx);   /* BAR0 was above 4 GB          */
