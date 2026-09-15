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
 * Requires: FAT16 partition mounted (disk install), ATAPI CD present.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/atapi.h"
#include "../include/iso9660.h"
#include "../include/fat16.h"
#include "../include/io.h"

/* ── Current version string ─────────────────────────────────── */
#define HAVENDOS_VERSION "0.5.9.14"

/* ── Buffer sizes ───────────────────────────────────────────── */
/* Max kernel ELF size we'll handle: 2MB (plenty of room) */
#define MAX_ELF_SIZE   (2 * 1024 * 1024)

/* Static buffer for kernel ELF — lives in BSS, not stack */
static uint8_t g_elf_buf[MAX_ELF_SIZE];

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

/* Very simple version compare: "0.5.9.13" vs "0.5.9.14"
   Returns:  1 if a > b
             0 if a == b
            -1 if a < b  */
static int ver_cmp(const char *a, const char *b) {
    /* parse up to 4 numeric components separated by '.' */
    int av[4] = {0,0,0,0};
    int bv[4] = {0,0,0,0};
    int i = 0;
    const char *p = a;
    while (*p && i < 4) {
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n*10 + (*p++ - '0'); }
        av[i++] = n;
        if (*p == '.') p++;
    }
    i = 0; p = b;
    while (*p && i < 4) {
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n*10 + (*p++ - '0'); }
        bv[i++] = n;
        if (*p == '.') p++;
    }
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
    step("Restoring /boot/havendos.bak → /boot/havendos.elf ...");

    /* Read backup into our buffer */
    int bak_sz = fat16_read("/boot/havendos.bak", (char *)g_elf_buf, MAX_ELF_SIZE);
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
    /* Write back */
    if (fat16_write("/boot/havendos.elf", (const char *)g_elf_buf,
                    (uint32_t)bak_sz) != 0) {
        err("Failed to write restored ELF to disk.");
        return 0;
    }
    ok("Restored successfully from backup.");
    return 1;
}

/* ── Main update entry point ────────────────────────────────── */
void cmd_update(void) {
    vga_puts("\n");
    UC_HEADER;
    vga_puts("  +------------------------------------------+\n");
    vga_puts("  |       HavenDOS Update Manager           |\n");
    vga_puts("  +------------------------------------------+\n");
    UC_NORM;
    vga_puts("\n");

    /* ── Prerequisite: must be running from disk install ── */
    if (!fat16_detected()) {
        err("No FAT16 partition found.");
        UC_DIM;
        vga_puts("  You must be running from a disk install to use 'update'.\n");
        vga_puts("  Boot the new ISO and use 'install' instead.\n\n");
        UC_NORM;
        return;
    }

    /* ── Step 1: Detect ATAPI CD-ROM ── */
    step("Scanning for ATAPI CD-ROM drive...");
    if (!atapi_init()) {
        err("No CD-ROM drive detected.");
        UC_DIM;
        vga_puts("  Attach the HavenDOS update ISO as a CD drive in UTM,\n");
        vga_puts("  then run 'update' again.\n\n");
        UC_NORM;
        return;
    }
    ok("CD-ROM found.");

    /* ── Step 2: Mount ISO 9660 ── */
    step("Reading ISO 9660 filesystem...");
    if (!iso9660_init()) {
        err("Failed to read ISO. Is a HavenDOS ISO inserted?");
        vga_puts("\n");
        return;
    }
    ok("ISO 9660 mounted.");

    /* ── Step 3: Read /version.txt from ISO ── */
    step("Reading version information from ISO...");
    uint8_t ver_buf[128];
    memset(ver_buf, 0, sizeof(ver_buf));
    int vr = iso9660_read_file("/version.txt", ver_buf, 127);
    if (vr < 1) {
        /* Try uppercase path */
        vr = iso9660_read_file("/VERSION.TXT", ver_buf, 127);
    }
    if (vr < 1) {
        warn("No /version.txt on ISO. Assuming update is safe to apply.");
        strcpy(g_iso_version, "unknown");
    } else {
        strncpy(g_iso_version, (const char *)ver_buf, 63);
        g_iso_version[63] = 0;
        strip_nl(g_iso_version);
    }

    UC_NORM; vga_puts("  Running : "); update_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts(HAVENDOS_VERSION); vga_puts("\n");
    UC_NORM; vga_puts("  On ISO  : ");
    update_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts(g_iso_version); vga_puts("\n\n"); UC_NORM;

    /* Version check */
    if (strcmp(g_iso_version, "unknown") != 0) {
        int cmp = ver_cmp(g_iso_version, HAVENDOS_VERSION);
        if (cmp == 0) {
            UC_WARN;
            vga_puts("  Same version already installed.\n\n");
            UC_NORM;
            if (!confirm("  Apply same version anyway")) {
                vga_puts("  Update cancelled.\n\n");
                return;
            }
        } else if (cmp < 0) {
            UC_WARN;
            vga_puts("  ISO version is OLDER than installed.\n\n");
            UC_NORM;
            if (!confirm("  Downgrade to older version")) {
                vga_puts("  Update cancelled.\n\n");
                return;
            }
        } else {
            /* Newer — normal path */
            if (!confirm("  Install this update")) {
                vga_puts("  Update cancelled.\n\n");
                return;
            }
        }
    } else {
        if (!confirm("  Apply update from ISO")) {
            vga_puts("  Update cancelled.\n\n");
            return;
        }
    }

    vga_puts("\n");

    /* ── Step 4: Check /boot/havendos.elf exists on ISO ── */
    step("Locating kernel on ISO...");
    uint32_t iso_elf_lba, iso_elf_size;
    if (!iso9660_find("/boot/havendos.elf", &iso_elf_lba, &iso_elf_size) &&
        !iso9660_find("/BOOT/HAVENDOS.ELF", &iso_elf_lba, &iso_elf_size)) {
        err("Cannot find /boot/havendos.elf on ISO.");
        vga_puts("\n");
        return;
    }
    UC_NORM; vga_puts("  Kernel size: ");
    char szbuf[16]; utoa(iso_elf_size / 1024, szbuf, 10);
    vga_puts(szbuf); vga_puts(" KB\n");

    if (iso_elf_size > MAX_ELF_SIZE) {
        err("Kernel too large for update buffer (>2MB). Aborting.");
        vga_puts("\n");
        return;
    }

    /* ── Step 5: Backup current kernel ── */
    step("Backing up current kernel to /boot/havendos.bak ...");

    if (fat16_exists("/boot/havendos.elf")) {
        /* Read existing ELF */
        int cur_sz = fat16_read("/boot/havendos.elf", (char *)g_elf_buf, MAX_ELF_SIZE);
        if (cur_sz < 4) {
            warn("Could not read current kernel for backup — continuing anyway.");
        } else {
            /* Delete old backup if present */
            if (fat16_exists("/boot/havendos.bak"))
                fat16_delete("/boot/havendos.bak");

            if (fat16_write("/boot/havendos.bak", (const char *)g_elf_buf,
                            (uint32_t)cur_sz) != 0) {
                warn("Backup write failed. Continuing without backup.");
            } else {
                char bkbuf[16]; utoa((uint32_t)cur_sz / 1024, bkbuf, 10);
                UC_NORM; vga_puts("  Backed up "); vga_puts(bkbuf);
                vga_puts(" KB → /boot/havendos.bak\n");
            }
        }
    } else {
        warn("No existing kernel found — first install path.");
    }

    /* ── Step 6: Stream kernel from ISO into buffer ── */
    step("Reading new kernel from ISO...");
    vga_puts("\n");

    memset(g_elf_buf, 0, iso_elf_size);

    uint32_t bytes_done    = 0;
    uint32_t cur_lba       = iso_elf_lba;
    uint32_t bytes_left    = iso_elf_size;
    uint8_t  sec_buf[2048];

    while (bytes_left > 0) {
        if (!atapi_read_sector(cur_lba, sec_buf)) {
            vga_puts("\n");
            err("CD-ROM read error during download.");

            /* Auto-restore */
            vga_puts("\n");
            UC_WARN; vga_puts("  Attempting automatic rollback...\n"); UC_NORM;
            if (do_restore()) {
                UC_OK; vga_puts("  System restored. Please reboot.\n\n"); UC_NORM;
            } else {
                UC_ERR;
                vga_puts("  CRITICAL: Rollback failed. System may not boot.\n");
                vga_puts("  Boot from ISO and run 'install' to recover.\n\n");
                UC_NORM;
            }
            return;
        }

        uint32_t chunk = bytes_left > 2048 ? 2048 : bytes_left;
        memcpy(g_elf_buf + bytes_done, sec_buf, chunk);
        bytes_done += chunk;
        bytes_left -= chunk;
        cur_lba++;

        draw_progress(bytes_done, iso_elf_size);
    }

    vga_puts("\n\n");
    ok("Kernel read complete.");

    /* ── Step 7: Verify ELF magic in buffer ── */
    step("Verifying ELF integrity...");
    if (g_elf_buf[0] != 0x7F || g_elf_buf[1] != 'E' ||
        g_elf_buf[2] != 'L'  || g_elf_buf[3] != 'F') {
        err("ELF magic check FAILED. ISO kernel may be corrupt.");

        /* Auto-restore */
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
        return;
    }
    ok("ELF magic valid (7F 45 4C 46).");

    /* ── Step 8: Write new kernel to FAT16 ── */
    step("Writing new kernel to disk...");

    /* Delete old ELF first (FAT16 write overwrites but let's be clean) */
    if (fat16_exists("/boot/havendos.elf"))
        fat16_delete("/boot/havendos.elf");

    if (fat16_write("/boot/havendos.elf", (const char *)g_elf_buf, iso_elf_size) != 0) {
        err("FAT16 write failed.");

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
        return;
    }

    /* ── Step 9: Verify written file by reading ELF magic back ── */
    step("Verifying disk write...");
    uint8_t check[4] = {0,0,0,0};
    int chk = fat16_read("/boot/havendos.elf", (char *)check, 4);
    if (chk < 4 || check[0] != 0x7F || check[1] != 'E' ||
        check[2] != 'L' || check[3] != 'F') {
        err("Disk verify FAILED. Written file is corrupt.");

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
        return;
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
}

/* ── Rollback command: restore from backup manually ─────────── */
void cmd_rollback(void) {
    vga_puts("\n");
    UC_WARN;
    vga_puts("  HavenDOS Rollback\n");
    UC_NORM;
    vga_puts("\n");

    if (!fat16_detected()) {
        err("No FAT16 partition. Must be on a disk install.");
        vga_puts("\n");
        return;
    }

    if (!fat16_exists("/boot/havendos.bak")) {
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
