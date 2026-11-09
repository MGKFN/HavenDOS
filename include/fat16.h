#pragma once
#include "types.h"
int  fat16_init(void);
int  fat16_detected(void);
int  fat16_on_ata(void);
/* The mounted volume's disk, as an index into fs/blkdev.c's table, or -1.
   Ask blk_bus_name() for the word; see the note in fs/fat16.c. */
int  fat16_dev(void);

/* ── borrowing the driver for a volume on another disk ────────────────
 * v0.8.6.1. This driver holds ONE volume, so inspecting a FAT16
 * partition on a USB stick means displacing the system's own and
 * putting it back -- the same shape as fs/fat32.c's
 * fat32_vol_mount_ro() / fat32_vol_release() pair, and the same hazard:
 * the volume most likely to turn up belongs to somebody else.
 *
 * A borrowed volume is READ ONLY at the driver: fat16_raw_write()
 * refuses while one is held, so the refusal does not depend on every
 * caller remembering.
 *
 * Returns 1 if a FAT16 volume mounted. ALWAYS pair with fat16_return(),
 * which re-mounts the system's own volume from the disk.
 *
 * ── THE WRITABLE BORROW ──────────────────────────────────────────────
 * v0.8.6.3 added a SECOND entry point rather than a flag on this one,
 * because the difference between them is whether this kernel may write
 * to a partition it does not own, and a caller has to ask for that by
 * name.
 *
 * fat16_borrow_rw() refuses a disk that is not removable, a disk or
 * volume that is read-only, the boot disk, and a FAT larger than the
 * cache. Those refusals are the feature; see fs/fat16.c for why each
 * one is there. It returns 0 and says which refusal fired in the log.
 *
 * Both are returned by the same fat16_return(), which flushes a
 * writable borrow's FAT to the STICK before switching the device back.
 * That order is load-bearing and is asserted by
 * tests/test_fat16borrow.c. */
int  fat16_borrow(int dev, uint64_t part_lba);
int  fat16_borrow_rw(int dev, uint64_t part_lba);
void fat16_return(void);
int  fat16_borrowed(void);
int  fat16_borrowed_rw(void);
int  fat16_read(const char *path, char *buf, uint32_t maxlen);
int  fat16_write(const char *path, const char *buf, uint32_t len);
int  fat16_delete(const char *path);
int  fat16_exists(const char *path);
int  fat16_is_dir(const char *path);
int  fat16_mkdir(const char *path);
/* Walk the ROOT directory, one callback per entry.
 *
 * This prototype said `int fat16_list(const char *dir, char *out,
 * uint32_t maxlen)` until v0.8.6.1 and the function has never had that
 * shape. Nothing caught it because fs/fat16.c did not include its own
 * header and the only caller -- fs/vfs.c -- wrote a local `extern` with
 * the right signature instead of using this one. That is the same
 * 33-local-externs pattern include/instdisk.h was created to kill, and
 * it hid a header that would have made any new caller pass three
 * arguments to a function that takes one.
 *
 * fs/fat16.c includes this header now, so the compiler compares them. */
void fat16_list(void (*cb)(const char *name, int is_dir, uint32_t size));

/* ── the subdirectory-aware variants ─────────────────────────────────
 * The five above only ever look in the ROOT directory. These walk a
 * path, which is what /boot/havendos.elf needs.
 *
 * Declared here as of v0.8.6.3. They existed since v0.7.5 and were
 * reached through four local `extern` lines at the top of fs/update.c
 * -- the very pattern the note above says include/instdisk.h was
 * created to kill, sitting in the same file as the note. A second
 * caller arrived (shell/fat16cmd.c's `fat16 cp`) and that is one more
 * copy of each signature than this tree tolerates.
 *
 * fat16_read_path() returns the number of bytes read, or <0. A return
 * equal to bufsz means the file may be longer than the buffer and
 * nothing distinguishes that from an exact fit, so a caller that cares
 * must size the buffer above what it will accept.
 *
 * fat16_write_path() returns the NUMBER OF BYTES WRITTEN, not 0/1 --
 * see the note on it in fs/fat16.c, which cost v0.7.5 a release. */
int  fat16_read_path  (const char *path, char *buf, uint32_t bufsz);
int  fat16_write_path (const char *path, const char *data, uint32_t size);
int  fat16_delete_path(const char *path);
int  fat16_exists_path(const char *path);

/* v0.8.6.3: `uint32_t fat16_file_size(const char *path)` was declared
   here and DEFINED NOWHERE. Nothing caught it because nothing called
   it; the first caller would have linked against a function that does
   not exist. Removed rather than implemented -- a prototype with no
   function behind it is the fat16_list() mistake with the sign
   flipped, and the next person to want it can write it. */
