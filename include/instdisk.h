#pragma once
#include "types.h"
#include "gpt.h"

/* ── The installer's view of the disks, in one header ────────────────
 *
 * This header exists because its absence cost four releases of the same
 * bug.
 *
 * shell/iso_installer.c owns the disk table. Every other file reached it
 * by writing its own `extern int inst_disk_count(void);` at the top of
 * whatever function needed it -- 33 such declarations across the tree.
 * With no header to hang it from, there was nowhere to put the ONE
 * function that turns a disk into a human-readable bus name, so four
 * screens worked it out independently:
 *
 *   v0.8.3.1  the picker said NVMe and the confirmation said VirtIO.
 *             Fixed by writing bus_name() "in one place" -- inside
 *             iso_installer.c, where nothing else could call it.
 *   v0.8.5.3  the welcome screen said "No disks found" and "1 VirtIO
 *             disk found" at the same time, on a machine whose only
 *             disk is NVMe; and the desktop installer announced a SATA
 *             disk as "(VirtIO)" on the screen that asks you to type
 *             YES before it writes.
 *
 * So the rule is now mechanical: inst_disk_bus_name() is the only thing
 * in this tree allowed to produce one of those words, and
 * tools/check_bus_label.py fails the build if any other file writes one
 * as a literal.
 */

int      inst_disk_scan(void);
int      inst_disk_count(void);
uint64_t inst_disk_sectors(int i);
uint8_t  inst_disk_bus(int i);
uint8_t  inst_disk_dev(int i);
int      inst_disk_read(int i, uint64_t lba, uint8_t *b);
int      inst_disk_write(int i, uint64_t lba, const uint8_t *b);
int      inst_disk_is_ata(int i);
int      inst_disk_is_nvme(int i);
int      inst_disk_is_readonly(int i);
int      inst_disk_has_gpt(int i);
int      inst_disk_gpt_parts(int i, gpt_part_t *out, int max);

/* The bus this disk is on, as a word to show a person: "IDE", "SATA",
 * "NVMe" or "VirtIO". The ONLY place in the tree that produces one.
 * Out-of-range returns "none" rather than inventing a bus for a disk
 * that is not there -- which is what a welcome screen counting two
 * booleans used to do. */
const char *inst_disk_bus_name(int i);

/* How many disks are on each bus. For a summary line that has to say
 * what was found without listing it: a screen that tests two booleans
 * cannot describe a machine with only NVMe. Pass 0 for any you do not
 * need. */
void inst_disk_bus_counts(int *ide, int *sata, int *nvme, int *virtio);
