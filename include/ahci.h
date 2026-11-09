/* ================================================================
   include/ahci.h -- SATA, the way every machine since about 2010
                     actually presents its disks
   ================================================================
   drivers/ata.c talks to a legacy IDE controller through ports 0x1F0
   and 0x170. That works on an i440FX machine and on real hardware
   whose firmware is set to "Legacy IDE" or "Compatibility" mode. It
   does not work on anything else, and "anything else" is the default
   on every laptop and every QEMU q35 machine: the SATA controller
   comes up in AHCI mode, where there are no legacy ports at all and
   the whole interface is a 4 KB window of memory-mapped registers.

   What that cost, before this file existed: on a q35 machine the
   installer reported "No Disks Found" -- correctly, it could not see
   the disk -- and the only way to test the dual-boot installer in
   QEMU was to ask for an i440FX machine. On real hardware it meant
   going into the firmware and switching the SATA mode, which on many
   laptops makes Windows refuse to boot until it is switched back.

   ── How a command happens ──────────────────────────────────────

   Per port, three pieces of memory that the controller reads by DMA:

     the command list   32 slots of 32 bytes, 1 KB aligned.  Each slot
                        says how long the command FIS is, whether the
                        transfer is a write, how many scatter-gather
                        entries there are, and where the command table
                        is.

     the FIS receive    256 bytes, 256-byte aligned.  The controller
     area               writes incoming FISes here.  We never read it
                        except to know the controller is alive; the
                        completion we actually wait on is a bit in
                        PxCI going back to zero.

     a command table    128 bytes of command FIS and ATAPI packet,
                        then the scatter-gather list.  128-byte
                        aligned.  One per slot.

   To issue a command: fill a command table with a Host-to-Device
   Register FIS naming the ATA command and the LBA, point one
   scatter-gather entry at the data buffer, fill in the command
   header, then set the slot's bit in PxCI. The controller clears
   that bit when it is done. Errors arrive in PxTFD and PxSERR.

   ── What this driver deliberately does not do ──────────────────

   One command at a time, polled. The hardware can have 32 in flight
   and raise an interrupt when each finishes; this driver uses slot 0
   and spins on PxCI. That is the same choice drivers/nvme.c and
   drivers/uhci.c make, for the same reason -- IRQ delivery is not
   reliable under UTM SE's interpreter, and an installer is not a
   workload that needs queue depth.

   Native Command Queuing, port multipliers, hot-plug, staggered
   spin-up and the Enclosure Management Bridge are not implemented.
   ================================================================ */
#ifndef AHCI_H
#define AHCI_H
#include "types.h"

/* Every implemented port with a disk behind it is its own drive, and
   the installer's disk table lists it separately. A machine with a
   SATA disk and a SATA optical drive has two. */
#define AHCI_MAX_DRIVES   8

/* Scan PCI for AHCI controllers, bring up every port that has a SATA
   disk on it, and fill the drive table. Safe to call more than once.
   Returns the number of drives found. */
int      ahci_init(void);

int      ahci_count(void);
uint64_t ahci_sectors(int idx);          /* in 512-byte sectors        */
const char *ahci_model(int idx);         /* IDENTIFY words 27..46      */
int      ahci_is_atapi(int idx);         /* an optical drive, not a disk */

/* One sector at a time, the same shape as every other block driver in
   this tree. 1 on success, 0 on failure. */
int      ahci_read_sector (int idx, uint64_t lba, uint8_t *buf);
int      ahci_write_sector(int idx, uint64_t lba, const uint8_t *buf);

/* What the boot log prints. Never a literal: tools/check_boot_log.py
   fails the build on invented output. */
const char *ahci_debug(void);

#endif /* AHCI_H */
