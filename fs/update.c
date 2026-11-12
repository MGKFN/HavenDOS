/*
 * update.c — HavenDOS ISO-based in-place upgrade with rollback
 *
 * How it works:
 *   1. Detect ATAPI CD-ROM drive (polling, no IRQs)
 *   2. Mount ISO 9660 filesystem on the CD
 *   3. Read /version.txt from the ISO — compare to running version
 *   4. If newer: ask user to confirm
 *   5. Backup /boot/havendos.elf → /boot/havendos.bak on FAT16
 *   6. Stream /boot/havendos.elf from ISO → FAT16
 *   7. Verify ELF magic on written file
 *   8. On verify failure: auto-restore from .bak
 *   9. Prompt reboot
 *
 * The backup at /boot/havendos.bak is the rollback point.
 * GRUB config already boots havendos.elf; a future recovery menu
 * entry will boot havendos.bak as fallback.
 *
 * Requires: a HavenDOS partition mounted -- FAT16, or FAT32 since 0.9.3
 * (see include/sysvol.h) -- and an update source (include/update_src.h).
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/atapi.h"
#include "../include/iso9660.h"
#include "../include/fat16.h"
#include "../include/io.h"
#include "../include/update_src.h"
#include "../include/sysvol.h"
extern uint32_t upd_image_crc(const uint8_t *buf, uint32_t len);
extern int fat16_detected(void);  /* v0.7.5: was implicit */

/* v0.7.19 FIX — the second reason 'update' never worked.
   Everything here lives in /boot, which is a SUBDIRECTORY.  fat16_exists(),
   fat16_read(), fat16_write() and fat16_delete() only ever look in the root:
   they hand the whole string to name_to_83() and search the root directory,
   so "/boot/havendos.elf" became a single 8.3 name that does not exist there.
   fat16_exists() therefore reported no current kernel (so no backup was
   taken), and fat16_write() then failed outright — which is exactly what the
   update manager printed: "No existing kernel found" followed by "FAT16 write
   failed", followed by a rollback that had no backup to roll back to.
   The _path variants walk the directory chain and are what this file needs. */
/* v0.8.6.3: these four are in include/fat16.h now, which fs/fat16.c
   also includes -- so the compiler compares the declaration with the
   definition instead of trusting four lines typed out here. */

/* ── Current version string ─────────────────────────────────────
   v0.7.18 FIX — this was frozen at "0.5.9.14" while the OS shipped 0.7.17.   (version-literal-ok: historical note, not a live version)
   ver_cmp() below compares the update ISO against it, so "same version
   already installed" could never fire and any ISO from 0.5.9.15 onward —
   INCLUDING a downgrade — compared as newer and was applied without a
   warning.  It now tracks the real version from include/version.h. */
#include "../include/version.h"
#define HAVENDOS_VERSION HAVENDOS_VER_NUM

/* ── Buffer sizes ───────────────────────────────────────────── */
/* The staging buffer for one kernel image.

   v0.8.4 — this was 2 MB and described as "plenty of room". The kernel is
   2.80 MB. It outgrew the cap some releases ago and nothing noticed,
   because nothing had successfully run `update` since: an update from a
   CD would have stopped at "Kernel too large for update buffer", which
   reads like a corrupt ISO rather than like a kernel that got bigger.

   Note the two sizes involved, because they are not the same file:

     havendos_pass1.elf   1.32 MB   what the INSTALLER writes, embedded
                                    in the final kernel as kernel_elf_data[]
     havendos.elf         2.80 MB   what is on the ISO and what `update`
                                    copies -- it carries the above inside it

   8 MB is chosen to outlast several releases of growth rather than to fit
   today, and it costs nothing at boot: the buffer comes from the physical
   allocator only while `update` or `rollback` is running.
   tools/check_update_size.py fails the build if the kernel ever passes it
   again. */
#define MAX_ELF_SIZE   (8 * 1024 * 1024)

/* v0.7.18 — this was `static uint8_t g_elf_buf[MAX_ELF_SIZE]`: two megabytes
   of BSS reserved at every boot for a buffer only the update and rollback
   commands ever touch.  It is now taken from the physical allocator when one
   of those commands runs and handed straight back afterwards, so an ordinary
   boot never pays for it.  The wrappers at the bottom of this file own the
   lifetime, which is what keeps the many early returns in cmd_update() from
   each needing their own release. */
extern void *pmm_alloc_contig(uint32_t nblocks);
extern void  pmm_free_contig(void *p, uint32_t nblocks);
#define ELF_BUF_BLOCKS (MAX_ELF_SIZE / 4096)
static uint8_t *g_elf_buf = 0;

/* Version string buffer */
static char g_iso_version[64];

/* ── External dependencies ──────────────────────────────────── */
extern int  keyboard_getchar(void);
extern void sleep_ms(uint32_t ms);

/* ── Internal helpers ───────────────────────────────────────── */

static void update_color(uint8_t fg, uint8_t bg) {
    vga_set_color((vga_color_t)fg, (vga_color_t)bg);
}

#define UC_HEADER  update_color(VGA_LIGHT_CYAN,  VGA_BLACK)
#define UC_OK      update_color(VGA_LIGHT_GREEN, VGA_BLACK)
#define UC_WARN    update_color(VGA_YELLOW,       VGA_BLACK)
#define UC_ERR     update_color(VGA_LIGHT_RED,    VGA_BLACK)
#define UC_NORM    update_color(VGA_WHITE,        VGA_BLACK)
#define UC_DIM     update_color(VGA_DARK_GREY,    VGA_BLACK)

static void step(const char *msg) {
    UC_DIM; vga_puts("  >> "); UC_NORM; vga_puts(msg); vga_puts("\n");
}
static void ok(const char *msg) {
    UC_OK;  vga_puts("  OK   "); UC_NORM; vga_puts(msg); vga_puts("\n");
}
static void warn(const char *msg) {
    UC_WARN; vga_puts("  WARN "); UC_NORM; vga_puts(msg); vga_puts("\n");
}
static void err(const char *msg) {
    UC_ERR;  vga_puts("  ERR  "); UC_NORM; vga_puts(msg); vga_puts("\n");
}

/* Wait for Y/N key, return 1 for Y, 0 for N */
static int confirm(const char *prompt) {
    UC_WARN; vga_puts(prompt); vga_puts(" [Y/N]: "); UC_NORM;
    for (;;) {
        int c = keyboard_getchar();
        if (c == 'y' || c == 'Y') { vga_puts("Y\n"); return 1; }
        if (c == 'n' || c == 'N') { vga_puts("N\n"); return 0; }
        if (c == 27 || c == 'q') { vga_puts("N\n"); return 0; } /* ESC = no */
        sleep_ms(10);
    }
}

/* Very simple version compare: "0.5.9.13" vs "0.5.9.14"   version-literal-ok
   Returns:  1 if a > b
             0 if a == b
            -1 if a < b  */
/* Parse up to 4 numeric components out of a version string.
   Returns how many were found.

   v0.7.19 FIX — this is why 'update' never worked.

   The ISO carries its version in /version.txt, which make_version.py writes in
   DISPLAY form, with a leading v, while the running system compares against
   HAVENDOS_VER_NUM, which is bare.  The old parser started
   at the first character and only consumed digits, so for a v-prefixed string
   the inner loop matched nothing, n stayed 0, and `if (*p == '.')` was false
   because *p
   was still the 'v' — so p never advanced.  The outer loop then ran four times
   on the same character and produced all zeroes.

   A genuinely NEWER ISO therefore compared as OLDER, and the update manager
   asked "Downgrade to older version?" for every upgrade.  Answering no — the
   sensible answer to that question — cancelled it.

   The lead-in is skipped, and a component that consumes no digits now breaks
   out instead of spinning. */
static int ver_parse(const char *s, int out[4]) {
    for (int j = 0; j < 4; j++) out[j] = 0;
    if (!s) return 0;
    const char *p = s;
    /* Skip 'v', 'V' or any other non-digit lead-in. */
    while (*p && (*p < '0' || *p > '9')) p++;
    int i = 0;
    while (*p && i < 4) {
        int n = 0, any = 0;
        while (*p >= '0' && *p <= '9') { n = n*10 + (*p++ - '0'); any = 1; }
        if (!any) break;              /* never spin on a non-digit */
        out[i++] = n;
        if (*p == '.') p++;
        else break;                   /* trailing text ends the version */
    }
    return i;
}

int ver_cmp(const char *a, const char *b) {
    int av[4], bv[4];
    int na = ver_parse(a, av);
    int nb = ver_parse(b, bv);
    /* A string with no digits at all is not a version. Treat it as equal to
       nothing rather than as 0.0.0.0, which would make every real version
       look newer than it. */
    if (!na && !nb) return 0;
    if (!na) return -1;
    if (!nb) return  1;
    for (int j = 0; j < 4; j++) {
        if (av[j] > bv[j]) return  1;
        if (av[j] < bv[j]) return -1;
    }
    return 0;
}

/* Strip trailing whitespace/newlines from a string */
static void strip_nl(char *s) {
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t'))
        s[--n] = 0;
}

/* Progress bar: filled / total, width 40 */
static void draw_progress(uint32_t done, uint32_t total) {
    int w = 38;
    int fill = 0;
    if (total > 0) {
        uint32_t tw = total / (uint32_t)w;
        fill = (tw > 0) ? (int)(done / tw) : w;
        if (fill > w) fill = w;
    }
    vga_puts("\r  [");
    update_color(VGA_LIGHT_GREEN, VGA_BLACK);
    for (int i = 0; i < fill; i++) vga_puts("=");
    UC_DIM;
    for (int i = fill; i < w; i++) vga_puts("-");
    UC_NORM;
    vga_puts("] ");
    /* percentage */
    /* avoid 64-bit division — scale down both sides first */
    uint32_t pct = 0;
    if (total > 0) {
        /* done/total * 100: divide total by 100 first to keep 32-bit */
        uint32_t t100 = total / 100;
        pct = (t100 > 0) ? (done / t100) : ((done >= total) ? 100 : 0);
        if (pct > 100) pct = 100;
    }
    char pbuf[8]; 
    utoa(pct, pbuf, 10);
    vga_puts(pbuf); vga_puts("%  ");
}

/* ── Restore from backup ─────────────────────────────────────── */
static int do_restore(void) {
    step("Restoring /boot/havendos.bak to /boot/havendos.elf ...");

    /* Read backup into our buffer */
    int bak_sz = sysvol_read("/boot/havendos.bak", (char *)g_elf_buf, MAX_ELF_SIZE);
    if (bak_sz < 4) {
        err("Backup read failed or too small.");
        return 0;
    }
    /* Verify backup is ELF */
    if (g_elf_buf[0] != 0x7F || g_elf_buf[1] != 'E' ||
        g_elf_buf[2] != 'L'  || g_elf_buf[3] != 'F') {
        err("Backup is not a valid ELF. Cannot restore.");
        return 0;
    }
    /* Write back.  See the note on return conventions at the first write. */
    if (sysvol_write("/boot/havendos.elf", (const char *)g_elf_buf,
                    (uint32_t)bak_sz) <= 0) {
        err("Failed to write restored ELF to disk.");
        return 0;
    }
    ok("Restored successfully from backup.");
    return 1;
}

/* ── Main update entry point ────────────────────────────────── */
/* v0.8.4 — this used to be four steps bound to an ATAPI CD-ROM: detect the
   drive, mount ISO 9660, read /version.txt, stream /boot/havendos.elf.  The
   laptop HavenDOS is aimed at has no optical drive, no USB mass storage in
   this kernel and no supported NIC, so `update` could not run on it at all
   and every release reached it by re-running the additive installer beside
   a live Windows partition.

   Those four steps now live behind the interface in include/update_src.h,
   with an EFI System Partition implementation beside the CD-ROM one, and
   ONE function choosing between them.  See that header for why there are
   two implementations from the first day rather than one. */
static void cmd_update_inner(void) {
    const upd_source_t *src = 0;
    upd_manifest_t      man;
    int                 mc;
    int                 wrote_kernel = 0;   /* has anything been overwritten yet */

    vga_puts("\n");
    UC_HEADER;
    vga_puts("  +------------------------------------------+\n");
    vga_puts("  |       HavenDOS Update Manager           |\n");
    vga_puts("  +------------------------------------------+\n");
    UC_NORM;
    vga_puts("\n");

    /* ── Prerequisite: must be running from disk install ── */
    /* v0.9.3: FAT16 or FAT32 -- an install above 2047 MB is FAT32, and
       this line used to tell a disk install it was not one. */
    if (sysvol_kind() == SYSVOL_NONE) {
        err("No HavenDOS partition is mounted (no FAT16, no FAT32).");
        UC_DIM;
        vga_puts("  You must be running from a disk install to use 'update'.\n");
        vga_puts("  Boot the new ISO and use 'install' instead.\n\n");
        UC_NORM;
        return;
    }

    /* ── Step 1: find an update source ──
       upd_source_select() is the only place in the tree that decides
       this, and upd_source_label() is the only place that names it. */
    step("Looking for an update...");
    src = upd_source_select();
    if (!src) {
        err("No update found.");
        UC_DIM;
        vga_puts("  Put the new kernel and update.inf in\n");
        vga_puts("    " UPD_ESP_DIR "\n");
        vga_puts("  on the EFI System Partition, or attach an update ISO\n");
        vga_puts("  as a CD drive, then run 'update' again.\n\n");
        UC_NORM;
        return;
    }
    UC_NORM; vga_puts("  Source  : ");
    update_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts(upd_source_label(src)); vga_puts("\n"); UC_NORM;

    /* ── WHY NOT THE STICK, v0.8.6.5 ──────────────────────────────
       A stick is probed before the ESP, so reaching any other source
       means the USB probe said no -- and "it only found the ESP" is
       four different failures wearing one face. The probe counted what
       it saw; this prints it, once, in the one place somebody is
       already looking. */
    if (src->kind != UPD_SRC_USB) {
        upd_usb_why_t w;
        upd_usb_why(&w);
        UC_DIM;
        vga_puts("            no update staged on a USB stick -- ");
        if (w.removable == 0) {
            vga_puts("no removable disk\n");
            vga_puts("            on the bus. Try 'usb rescan', then "
                     "'disk'.\n");
        } else if (w.volumes == 0 && w.xvolumes == 0) {
            vga_puts("a stick is there with\n");
            vga_puts("            no FAT32 and no exFAT volume on it. "
                     "'fat32'\n");
            vga_puts("            says what it actually is; FAT16 is "
                     "not read here.\n");
        } else if (w.mounted == 0 && w.xmounted == 0) {
            /* Which TYPE would not mount, because they are different
               bugs with different fixes -- 'fat32' and 'exfat' both
               name their refusal, and saying which command to run is
               the difference between a report and a round trip. */
            if (w.volumes) {
                vga_puts("its FAT32 volume\n");
                vga_puts("            would not mount. 'fat32' names "
                         "the refusal.\n");
            } else {
                vga_puts("its exFAT volume\n");
                vga_puts("            would not mount. 'exfat' names "
                         "the refusal.\n");
            }
        } else {
            vga_puts("no manifest on it at\n");
            vga_puts("            " UPD_ESP_MANIFEST "\n");
        }
        UC_NORM;
    }

    /* From here on every exit goes through done: so the source is always
       released.  The ESP source holds fs/fat32.c's single mount slot while
       it is open, which is the system's own, so leaking it would leave the
       machine unable to read its own FAT32 volumes until a reboot. */

    /* ── Step 2: read the manifest ── */
    step("Reading update manifest...");
    mc = src->manifest(&man);
    if (mc != UPDM_OK) {
        err(upd_manifest_strerror(mc));
        UC_DIM;
        if (src->kind == UPD_SRC_ESP) {
            vga_puts("  Expected a manifest at\n");
            vga_puts("    " UPD_ESP_MANIFEST "\n");
        }
        vga_puts("\n");
        UC_NORM;
        goto done;
    }

    strncpy(g_iso_version, man.version, 63);
    g_iso_version[63] = 0;
    strip_nl(g_iso_version);

    UC_NORM; vga_puts("  Running : "); update_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(HAVENDOS_VERSION); vga_puts("\n");
    UC_NORM; vga_puts("  Offered : ");
    update_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts(g_iso_version); vga_puts("\n");
    UC_NORM; vga_puts("  Size    : ");
    { char szb[16]; utoa(man.size / 1024u, szb, 10);
      vga_puts(szb); vga_puts(" KB"); }
    if (man.have_crc) {
        UC_DIM; vga_puts("   (checksummed)"); UC_NORM;
    } else {
        UC_DIM; vga_puts("   (no checksum here)"); UC_NORM;
    }
    vga_puts("\n\n");

    /* ── Version check ── */
    if (strcmp(g_iso_version, "unknown") != 0) {
        int cmp = ver_cmp(g_iso_version, HAVENDOS_VERSION);
        if (cmp == 0) {
            UC_WARN;
            vga_puts("  Same version already installed.\n\n");
            UC_NORM;
            if (!confirm("  Apply same version anyway")) {
                vga_puts("  Update cancelled.\n\n");
                goto done;
            }
        } else if (cmp < 0) {
            UC_WARN;
            vga_puts("  Offered version is OLDER than installed.\n\n");
            UC_NORM;
            if (!confirm("  Downgrade to older version")) {
                vga_puts("  Update cancelled.\n\n");
                goto done;
            }
        } else {
            if (!confirm("  Install this update")) {
                vga_puts("  Update cancelled.\n\n");
                goto done;
            }
        }
    } else {
        if (!confirm("  Apply update of unknown version")) {
            vga_puts("  Update cancelled.\n\n");
            goto done;
        }
    }

    vga_puts("\n");

    if (man.size > MAX_ELF_SIZE) {
        /* Say both numbers. The 0.8.3.1 lesson: a refusal that names a
           limit without naming what was measured against it sends the
           reader to check the wrong thing. */
        err("Kernel too large for the update buffer.");
        UC_DIM;
        vga_puts("  offered ");
        { char b[16]; utoa(man.size / 1024u, b, 10); vga_puts(b); }
        vga_puts(" KB, buffer holds ");
        { char b[16]; utoa((uint32_t)MAX_ELF_SIZE / 1024u, b, 10); vga_puts(b); }
        vga_puts(" KB\n\n");
        UC_NORM;
        goto done;
    }

    /* ── Step 3: Backup current kernel ──
       Before the new image is read, because both use the same staging
       buffer, and before anything is written, because that is the point
       of a backup. */
    step("Backing up current kernel to /boot/havendos.bak ...");

    if (sysvol_exists("/boot/havendos.elf")) {
        int cur_sz = sysvol_read("/boot/havendos.elf", (char *)g_elf_buf, MAX_ELF_SIZE);
        if (cur_sz < 4) {
            warn("Could not read current kernel for backup - continuing anyway.");
        } else {
            if (sysvol_exists("/boot/havendos.bak"))
                sysvol_delete("/boot/havendos.bak");

            /* v0.7.19 FIX — the third reason 'update' never worked.

               Both fat16_write() and fat16_write_path() -- and so sysvol_write() -- return the NUMBER OF BYTES
               WRITTEN on success and a negative value on failure.  Every call here tested
               `!= 0`, so a successful write of a 1.2 MB kernel returned 1230708 and was
               read as a failure.  The update manager printed "FAT16 write failed" and
               rolled back a write that had in fact just succeeded.

               Failure is < 0.  A zero return means zero bytes written, which for a kernel
               is also not success, so both are treated as failure below. */
            if (sysvol_write("/boot/havendos.bak", (const char *)g_elf_buf,
                            (uint32_t)cur_sz) <= 0) {
                warn("Backup write failed. Continuing without backup.");
            } else {
                char bkbuf[16]; utoa((uint32_t)cur_sz / 1024, bkbuf, 10);
                UC_NORM; vga_puts("  Backed up "); vga_puts(bkbuf);
                vga_puts(" KB -> /boot/havendos.bak\n");
            }
        }
    } else {
        warn("No existing kernel found - first install path.");
    }

    /* ── Step 4: Read the new kernel into the staging buffer ── */
    step("Reading new kernel...");
    vga_puts("\n");

    memset(g_elf_buf, 0, man.size);

    if (!src->read_image(&man, g_elf_buf, MAX_ELF_SIZE, draw_progress)) {
        vga_puts("\n");
        err("Read error while fetching the new kernel.");
        UC_DIM;
        vga_puts("  Nothing has been written. The running kernel is untouched.\n\n");
        UC_NORM;
        goto done;
    }

    vga_puts("\n\n");
    ok("Kernel read complete.");

    /* ── Step 5: Verify ELF magic in the buffer ── */
    step("Verifying ELF integrity...");
    if (g_elf_buf[0] != 0x7F || g_elf_buf[1] != 'E' ||
        g_elf_buf[2] != 'L'  || g_elf_buf[3] != 'F') {
        err("ELF magic check FAILED. The image is not a kernel.");
        UC_DIM;
        vga_puts("  Nothing has been written. The running kernel is untouched.\n\n");
        UC_NORM;
        goto done;
    }
    ok("ELF magic valid (7F 45 4C 46).");

    /* ── Step 6: Verify the checksum, BEFORE the first byte is written ──
       The ATAPI path checked four bytes of ELF magic and trusted the rest,
       because an ISO is a single object that was either read or not.  A
       file copied onto an EFI System Partition by hand from Windows is
       not that: it can be truncated by a full volume, half-written by a
       stick pulled out early, or simply the wrong file.  A kernel that is
       a valid ELF and the wrong bytes passes every check this command had
       before 0.8.4, and does not boot. */
    if (man.have_crc) {
        step("Verifying checksum...");
        if (upd_image_crc(g_elf_buf, man.size) != man.crc) {
            err("Checksum MISMATCH - the image is damaged, or is not the");
            err("one the manifest describes.");
            UC_DIM;
            vga_puts("  Nothing has been written. The running kernel is untouched.\n\n");
            UC_NORM;
            goto done;
        }
        ok("Checksum matches the manifest.");
    } else {
        warn("This source carries no checksum - ELF magic only.");
    }

    /* ── Step 7: Write the new kernel to the system volume ── */
    step("Writing new kernel to disk...");

    if (sysvol_exists("/boot/havendos.elf"))
        sysvol_delete("/boot/havendos.elf");

    wrote_kernel = 1;
    if (sysvol_write("/boot/havendos.elf", (const char *)g_elf_buf,
                         man.size) <= 0) {
        err(sysvol_kind() == SYSVOL_FAT32 ? "FAT32 write failed."
                                          : "FAT16 write failed.");
        goto rollback;
    }

    /* ── Step 8: Verify the written file by reading it back ── */
    step("Verifying disk write...");
    {
        uint8_t check[4] = {0,0,0,0};
        int chk = sysvol_read("/boot/havendos.elf", (char *)check, 4);
        if (chk < 4 || check[0] != 0x7F || check[1] != 'E' ||
            check[2] != 'L' || check[3] != 'F') {
            err("Disk verify FAILED. Written file is corrupt.");
            goto rollback;
        }
    }
    ok("Disk write verified.");

    /* ── Done ── */
    vga_puts("\n");
    UC_HEADER;
    vga_puts("  +------------------------------------------+\n");
    if (strcmp(g_iso_version, "unknown") != 0) {
        vga_puts("  |     Update complete! HavenDOS ");
        vga_puts(g_iso_version);
        vga_puts("      |\n");
    } else {
        vga_puts("  |         Update complete!                |\n");
    }
    vga_puts("  |  Reboot to run the new version.         |\n");
    vga_puts("  +------------------------------------------+\n");
    UC_NORM;
    vga_puts("\n");
    UC_DIM;
    vga_puts("  Backup kept at /boot/havendos.bak for rollback.\n");
    vga_puts("  Run 'reboot' when ready.\n\n");
    UC_NORM;
    goto done;

rollback:
    /* Only reached once something has actually been overwritten.  Before
       0.8.4 a failed download also ran this, which wrote the backup back
       over a kernel nothing had touched -- a pointless write to the boot
       partition on the one path where the machine was already fine. */
    if (!wrote_kernel) goto done;
    vga_puts("\n");
    UC_WARN; vga_puts("  Attempting automatic rollback...\n"); UC_NORM;
    if (do_restore()) {
        UC_OK; vga_puts("  System restored from backup. Reboot to continue.\n\n"); UC_NORM;
    } else {
        UC_ERR;
        vga_puts("  CRITICAL: Rollback failed.\n");
        vga_puts("  Boot from ISO and run 'install' to recover.\n\n");
        UC_NORM;
    }

done:
    if (src && src->release) src->release();
}


/* ── Rollback command: restore from backup manually ─────────── */
static void cmd_rollback_inner(void) {
    vga_puts("\n");
    UC_WARN;
    vga_puts("  HavenDOS Rollback\n");
    UC_NORM;
    vga_puts("\n");

    if (sysvol_kind() == SYSVOL_NONE) {
        err("No HavenDOS partition mounted. Must be on a disk install.");
        vga_puts("\n");
        return;
    }

    if (!sysvol_exists("/boot/havendos.bak")) {
        err("No backup found at /boot/havendos.bak.");
        UC_DIM; vga_puts("  Run 'update' first to create a backup.\n\n"); UC_NORM;
        return;
    }

    vga_puts("  This will restore the previous kernel from /boot/havendos.bak.\n\n");
    if (!confirm("  Restore previous version")) {
        vga_puts("  Rollback cancelled.\n\n");
        return;
    }

    vga_puts("\n");
    if (do_restore()) {
        vga_puts("\n");
        UC_OK; vga_puts("  Rollback complete. Run 'reboot' to boot previous version.\n\n");
        UC_NORM;
    } else {
        vga_puts("\n");
        UC_ERR;
        vga_puts("  Rollback failed. Boot from ISO and run 'install' to recover.\n\n");
        UC_NORM;
    }
}


/* ── buffer lifetime ─────────────────────────────────────────────────
   Acquire once on entry, release on exit, so every early return inside the
   command bodies is covered without threading cleanup through all of them. */
static int elf_buf_acquire(void) {
    if (g_elf_buf) return 1;
    g_elf_buf = (uint8_t *)pmm_alloc_contig(ELF_BUF_BLOCKS);
    return g_elf_buf != 0;
}
static void elf_buf_release(void) {
    if (!g_elf_buf) return;
    pmm_free_contig(g_elf_buf, ELF_BUF_BLOCKS);
    g_elf_buf = 0;
}

void cmd_update(void) {
    if (!elf_buf_acquire()) {
        UC_ERR;
        vga_puts("\n  Not enough free memory to stage a kernel image.\n\n");
        UC_NORM;
        return;
    }
    cmd_update_inner();
    elf_buf_release();
}

void cmd_rollback(void) {
    if (!elf_buf_acquire()) {
        UC_ERR;
        vga_puts("\n  Not enough free memory to stage a kernel image.\n\n");
        UC_NORM;
        return;
    }
    cmd_rollback_inner();
    elf_buf_release();
}
