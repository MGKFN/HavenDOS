#pragma once
#include "types.h"

/* Where an update comes from.
 *
 * ── WHY THIS FILE EXISTS ─────────────────────────────────────────────
 *
 * fs/update.c read the new kernel from an ATAPI CD-ROM and from nowhere
 * else.  Four steps of it -- detect the drive, mount ISO 9660, read
 * /version.txt, stream /boot/havendos.elf -- were bound to that one bus.
 *
 * The machine this project is aimed at has no optical drive, no USB mass
 * storage in HavenDOS, and a MediaTek Wi-Fi part nothing here drives.  So
 * `update` could not run on it at all, and every release reached it by
 * re-running the additive installer beside a live Windows partition.  That
 * is a loaded gun pointed at somebody's Windows install, fired once per
 * release, for no reason other than the source of a file.
 *
 * The EFI System Partition is already there, it is FAT32, fs/fat32.c
 * already reads it, fs/fat32vol.c already finds it by partition type GUID,
 * and Windows can write to it with `mountvol`.  Every part of that route
 * existed before this file did.
 *
 * ── THE RULE THIS FILE IS SHAPED BY ──────────────────────────────────
 *
 * 0.8.3.1's post-mortem: three of its six bugs were one mistake -- the
 * installer has one block layer and the filesystem has another, and only
 * one of them got taught.  Four screens each worked the bus label out
 * independently and only one of them knew about NVMe, so the picker said
 * NVMe and the screen gated behind typing YES said VirtIO.
 *
 * So this interface is born with TWO implementations rather than one, and
 * exactly one function decides which is used:
 *
 *     upd_source_select()   decides.  Nothing else chooses a source.
 *     upd_source_label()    names it.  Every screen asks this.
 *
 * An abstraction with a single implementation is a wrapper and proves
 * nothing.  With two, the compiler keeps them honest.
 *
 * ── WHAT THE ESP SOURCE READS ────────────────────────────────────────
 *
 *   \EFI\HAVENDOS\update\update.inf     the manifest, plain text
 *   \EFI\HAVENDOS\update\havendos.elf   the kernel it describes
 *
 * Nothing on the ESP is written, deleted or moved by an update.  The
 * volume is mounted READ ONLY through fat32_vol_mount_ro(), which hands
 * fs/fat32.c a NULL write function -- so the refusal lives in the driver
 * and not in the discipline of this file.
 *
 * ── AND WHY THERE IS A CHECKSUM ──────────────────────────────────────
 *
 * The ATAPI path verified four bytes of ELF magic and nothing else: it
 * trusted the ISO, because an ISO is a single object that was either read
 * or not.  A file copied into the ESP by hand from Windows is not that.
 * It can be truncated by a full volume, half-written by a removed stick,
 * or simply the wrong file.  The manifest carries the length and a CRC-32
 * of the image, and both are checked against the staged buffer BEFORE the
 * first byte is written over the running kernel.
 *
 * gpt_crc32() is the CRC-32 the GPT already uses -- reflected 0xEDB88320,
 * the same one zlib computes -- so the value in a manifest can be checked
 * by anyone with python3 and no HavenDOS source.  A checksum only our own
 * code can compute would be our code agreeing with itself.
 */

/* ── the manifest ────────────────────────────────────────────────────
 *
 * A text file, because the person writing it is at a Windows command
 * prompt with Notepad and no tooling of ours:
 *
 *     HAVENDOS-UPDATE 1
 *     version=0.8.4
 *     size=631284
 *     crc32=1a2b3c4d
 *     kernel=havendos.elf
 *
 * Blank lines and lines starting with # are ignored.  CR is tolerated,
 * because the file will be written on Windows and will have CRLF endings
 * whatever anyone intends.
 */

#define UPD_VER_MAX     32
#define UPD_NAME_MAX    32

typedef struct {
    char     version[UPD_VER_MAX];   /* exactly as written, leading v or not */
    char     kernel[UPD_NAME_MAX];   /* file name beside the manifest        */
    uint32_t size;                   /* bytes the image must be              */
    uint32_t crc;                    /* CRC-32 it must have                  */
    int      have_version;
    int      have_kernel;
    int      have_size;
    int      have_crc;
} upd_manifest_t;

/* Parser return codes.  Each refusal is its own code because the failure
   screen prints its own sentence for each -- the lesson from 0.8.3.1,
   where every failure in the installer printed "check disk is writable
   and large enough", including the one caused by a disk that was too
   LARGE. */
#define UPDM_OK          0
#define UPDM_E_MAGIC   (-1)   /* first line is not HAVENDOS-UPDATE        */
#define UPDM_E_FORMAT  (-2)   /* a manifest format this kernel is too old for */
#define UPDM_E_FIELD   (-3)   /* a required field is missing              */
#define UPDM_E_VALUE   (-4)   /* a field is present and will not parse    */
#define UPDM_E_READ    (-5)   /* the manifest file would not read at all  */
#define UPDM_E_NONE    (-6)   /* there is no manifest here to read        */

/* Parse `len` bytes of manifest text.  Pure: no I/O, no allocation, no
   dependency on anything in the kernel, which is what lets the suite
   compile it on the host.  `out` is always fully initialised, including
   on a refusal, so a caller that ignores the code cannot read a stale
   version string out of it. */
int upd_manifest_parse(const char *text, uint32_t len, upd_manifest_t *out);

const char *upd_manifest_strerror(int code);

/* ── a source ────────────────────────────────────────────────────────  */

#define UPD_SRC_NONE   0
#define UPD_SRC_ESP    1
#define UPD_SRC_CDROM  2
/* v0.8.6.3: a FAT32 volume on a REMOVABLE disk, carrying a manifest at
   the same path the ESP uses. Probed BEFORE the ESP -- see the note on
   upd_source_select() in fs/update_src.c for why a stick wins against a
   staged update that was never cleaned up. */
#define UPD_SRC_USB    3

/* ── what the USB probe saw, v0.8.6.5 ─────────────────────────────────
 * "update only found the ESP" is four different failures wearing one
 * face, and the screen named none of them. Filled in by every call to
 * the USB probe; fs/update.c prints it when the source it chose is not
 * the stick.
 *
 *   removable  == 0   nothing removable on the bus. The stick was not
 *                     enumerated at all -- `usb rescan`, then `disk`.
 *   volumes    == 0   a removable disk is there with no FAT32 AND no
 *                     exFAT volume on it. FAT16 lands here; `fat32`
 *                     names what it actually found.
 *   mounted    == 0   a volume of a type we read that would not mount.
 *   nomanifest  > 0   the volume mounted and there is no
 *                     /EFI/HAVENDOS/update/update.inf on it.
 *
 * ── exFAT, v0.8.8.2 ─────────────────────────────────────────────────
 *
 * This used to be FAT32 only, and `update` said so: "exFAT and FAT16
 * are not read here". That was honest and it was still the wrong
 * answer, because **a USB stick of 32 GB or more is formatted exFAT by
 * default**, which is the size of stick somebody reaches for to carry
 * a kernel. "USB discovery for 32 GB drives still broken for updating"
 * was reported four releases running, and every one of those was this
 * line refusing by design.
 *
 * fs/exfat.c arrived in v0.8.8.1 and is READ-ONLY, which is exactly
 * and only what an update SOURCE needs -- the writing is to /boot on
 * the internal disk through fs/fat16.c, and nothing here writes to the
 * stick. So the driver that could not be used for installing can be
 * used for this without relaxing anything.
 *
 * The two kinds are counted SEPARATELY. "a stick with no volume we can
 * read" and "an exFAT stick whose volume would not mount" have
 * different fixes, and v0.8.6.5's lesson was that "it only found the
 * ESP" was four failures wearing one face. */
typedef struct {
    int removable;    /* removable disks the block layer holds          */
    int volumes;      /* FAT32 volumes found on them                    */
    int mounted;      /* ...of those, the ones that mounted             */
    int nomanifest;   /* ...of those, the ones carrying no manifest     */
    int xvolumes;     /* exFAT volumes found on them                    */
    int xmounted;     /* ...of those, the ones that mounted             */
} upd_usb_why_t;

void upd_usb_why(upd_usb_why_t *out);

/* read_image() fills `buf` with exactly m->size bytes and calls
 * progress(done, total) as it goes.  It returns 1 on success.  It does
 * NOT check the checksum: verification belongs to the caller, once, in
 * one place, against the staged buffer -- so that the ATAPI path and the
 * ESP path cannot drift in how carefully they check.  */
typedef struct {
    int         kind;                 /* UPD_SRC_*                        */
    int       (*probe)(void);         /* 1 if this source is present      */
    int       (*manifest)(upd_manifest_t *m);   /* UPDM_* code            */
    int       (*read_image)(const upd_manifest_t *m, uint8_t *buf,
                            uint32_t bufsz,
                            void (*progress)(uint32_t done, uint32_t total));
    void      (*release)(void);       /* unmount, put back what we borrowed */
} upd_source_t;

/* THE function that chooses.  Probes the ESP first and the CD-ROM second,
 * and returns 0 when neither is there.  Nothing else in the tree decides
 * where an update comes from. */
const upd_source_t *upd_source_select(void);

/* THE function that names it.  Takes what select() returned -- or 0 --
 * and is what every screen prints.  Four screens each deciding this for
 * themselves is the bug 0.8.3.1 shipped. */
const char *upd_source_label(const upd_source_t *s);

/* Where the ESP source looks, exported so the installer, the rescue
   script and the failure screens all name the same path. */
#define UPD_ESP_DIR       "/EFI/HAVENDOS/update"
#define UPD_ESP_MANIFEST  "/EFI/HAVENDOS/update/update.inf"
