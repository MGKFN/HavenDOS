/* ================================================================
   shell/uninstallcmd.c -- the `uninstall` command
   ================================================================
   v0.8.8.8. The screen in front of this is the last thing between a
   person and a partition table, so it says exactly what will happen
   BEFORE it asks, names the disk by bus and size rather than by an
   index nobody can check, and takes a typed word rather than a
   keypress.

   It runs the dry run first, always, and shows that result. The dry
   run is the same code path with the write functions withheld
   (fs/uninstall.c), so what it reports is what the real run will do
   and not a second description of it that could drift.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
/* keyboard_getchar() has no header either -- shell/desktop.c declares
   it locally too. Left as an extern here rather than widened into a
   new header in the same release that added include/pit.h; one
   prototype rescue at a time. */
extern int keyboard_getchar(void);
#include "uninstall.h"
#include "instdisk.h"
#include "version.h"

extern const char *inst_disk_bus_name(int i);

static void line(const char *s, uint8_t fg)
{ vga_set_color(fg, VGA_BLACK); vga_puts(s); vga_puts("\n"); }

static void num(uint64_t v)
{
    char b[24]; int n = 0, i;
    if(!v){ vga_puts("0"); return; }
    while(v){ b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for(i = n - 1; i >= 0; i--){ char c[2]; c[0] = b[i]; c[1] = 0; vga_puts(c); }
}

static void mb(uint64_t sectors){ num(sectors / 2048ull); vga_puts(" MB"); }

void cmd_uninstall(void)
{
    uninst_report_t u;
    int n, i, target = -1, found = 0, rc;
    char typed[16];

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("\n");
    line("Remove HavenDOS " HAVENDOS_VER " from this machine", VGA_LIGHT_CYAN);
    vga_puts("\n");

    inst_disk_scan();
    n = inst_disk_count();
    if(n <= 0){ line("  No disks found.", VGA_LIGHT_RED); return; }

    /* ── which disks have HavenDOS on them ──────────────────────── */
    for(i = 0; i < n; i++){
        rc = uninst_survey(i, &u);
        if(rc != UNINST_OK) continue;
        found++;
        if(target < 0) target = i;
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts("  disk "); num((uint64_t)i);
        vga_puts("  ("); vga_puts(inst_disk_bus_name(i)); vga_puts(", ");
        mb(inst_disk_sectors(i)); vga_puts(")\n");
    }

    if(!found){
        line("  HavenDOS is not installed on any disk in this machine.",
             VGA_LIGHT_GREY);
        line("  Nothing to do.", VGA_DARK_GREY);
        return;
    }
    if(found > 1){
        line("  HavenDOS is on more than one disk. This command removes it",
             VGA_YELLOW);
        line("  from the FIRST one listed only. Run it again for the rest.",
             VGA_YELLOW);
    }

    /* ── the dry run, and what it says ──────────────────────────── */
    rc = uninst_run(target, 1 /* dry run */, &u);
    if(rc != UNINST_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  Cannot: "); vga_puts(uninst_strerror(rc)); vga_puts("\n");
        return;
    }

    vga_puts("\n");
    line("  This will:", VGA_LIGHT_CYAN);

    if(u.did_restore_loader && u.did_delete_backup){
        line("    * put your ORIGINAL boot loader back", VGA_WHITE);
        line("      (\\EFI\\BOOT\\BOOTX64.HDB -> BOOTX64.EFI, then remove the backup)",
             VGA_DARK_GREY);
    } else if(u.did_restore_loader){
        line("    * remove \\EFI\\BOOT\\BOOTX64.EFI", VGA_WHITE);
        line("      (there was no loader there before HavenDOS, so none goes back)",
             VGA_DARK_GREY);
    }
    if(u.fallback_foreign){
        line("    * LEAVE \\EFI\\BOOT\\BOOTX64.EFI alone -- it is not ours",
             VGA_YELLOW);
    }
    if(u.did_remove_files)
        line("    * delete \\EFI\\HAVENDOS and everything in it", VGA_WHITE);
    if(u.did_remove_part){
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts("    * remove the HavenDOS partition from the table (");
        mb(u.part_sectors); vga_puts(")\n");
        line("      The DATA is not erased -- only the table entry.",
             VGA_DARK_GREY);
    }
    if(u.did_remove_esp){
        vga_set_color(VGA_WHITE, VGA_BLACK);
        vga_puts("    * remove the EFI System Partition HavenDOS created (");
        mb(u.esp_sectors); vga_puts(")\n");
    } else if(u.esp_lba){
        line("    * leave the EFI System Partition in place -- we did not make it",
             VGA_DARK_GREY);
    }

    vga_puts("\n");
    line("  Windows and anything else on this disk are not touched.",
         VGA_LIGHT_GREEN);
    vga_puts("\n");

    /* ── the one question ───────────────────────────────────────── */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("  Type UNINSTALL to go ahead, anything else to stop: ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    {
        int k = 0;
        for(;;){
            int c = keyboard_getchar();
            if(c == '\n' || c == '\r') break;
            if(c < 0) continue;
            if(c == '\b'){ if(k){ k--; vga_puts("\b \b"); } continue; }
            if(k < (int)sizeof typed - 1 && c >= 32){
                char s2[2]; s2[0] = (char)c; s2[1] = 0;
                typed[k++] = (char)c; vga_puts(s2);
            }
        }
        typed[k] = 0;
    }
    vga_puts("\n");

    if(strcmp(typed, "UNINSTALL") != 0){
        line("  Stopped. Nothing was written.", VGA_LIGHT_GREY);
        return;
    }

    /* ── do it ──────────────────────────────────────────────────── */
    rc = uninst_run(target, 0, &u);
    vga_puts("\n");
    if(rc != UNINST_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_puts("  FAILED: "); vga_puts(uninst_strerror(rc)); vga_puts("\n");
        line("  Some steps may have completed. Run `uninstall` again to",
             VGA_LIGHT_GREY);
        line("  finish, or see the manual steps in the changelog.",
             VGA_LIGHT_GREY);
        return;
    }

    line("  Done.", VGA_LIGHT_GREEN);
    if(u.did_restore_loader) line("    boot loader restored", VGA_LIGHT_GREY);
    if(u.did_remove_files)   line("    \\EFI\\HAVENDOS removed", VGA_LIGHT_GREY);
    if(u.did_remove_part)    line("    partition entry cleared", VGA_LIGHT_GREY);
    if(u.did_remove_esp)     line("    our EFI System Partition removed",
                                  VGA_LIGHT_GREY);
    vga_puts("\n");
    line("  This machine will boot the way it did before HavenDOS.",
         VGA_WHITE);
    line("  You are still running HavenDOS from RAM -- it is only gone",
         VGA_DARK_GREY);
    line("  from the disk. Reboot when you are ready.", VGA_DARK_GREY);
}
