/* ================================================================
   shell/migratecmd.c -- the `migrate` command (v0.9.6)
   ================================================================
     migrate           what can be done, from where this system runs
     migrate <n>       move this FAT16 install to place <n>, as FAT32
     migrate finish    from the new install: remove the old FAT16 one

   Held to the standard `install` and `uninstall` set: it says what it
   will do before it asks, names partitions by number and size, takes a
   typed word, and every refusal names its reason.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "migrate.h"
#include "instdisk.h"
#include "version.h"
#include "task.h"

extern int keyboard_getchar(void);
extern const char *inst_disk_bus_name(int i);

static void line(const char *s, uint8_t fg)
{ vga_set_color(fg, VGA_BLACK); vga_puts(s); vga_puts("\n"); }
static void say(const char *s, uint8_t fg){ vga_set_color(fg, VGA_BLACK); vga_puts(s); }

static void num(uint64_t v)
{
    char b[24]; int n = 0, i;
    if(!v){ vga_puts("0"); return; }
    while(v){ b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for(i = n - 1; i >= 0; i--){ char c[2]; c[0] = b[i]; c[1] = 0; vga_puts(c); }
}
static void mb(uint64_t sectors){ num(sectors / 2048ull); vga_puts(" MB"); }
static void serial(uint32_t s){ char b[10]; mig_serial_str(s, b); vga_puts(b); }

static int ask(const char *word)
{
    char typed[16];
    int k = 0;
    say("  Type ", VGA_YELLOW); say(word, VGA_WHITE);
    say(" to go ahead, anything else to stop: ", VGA_YELLOW);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    for(;;){
        int c = keyboard_getchar();
        if(c == '\n' || c == '\r') break;
        if(c < 0) continue;
        if(c == '\b'){ if(k){ k--; vga_puts("\b \b"); } continue; }
        if(k < (int)sizeof typed - 1 && c >= 32 && c < 127){
            char s2[2]; s2[0] = (char)c; s2[1] = 0;
            typed[k++] = (char)c; vga_puts(s2);
        }
    }
    typed[k] = 0;
    vga_puts("\n");
    if(strcmp(typed, word) != 0){ line("  Stopped. Nothing was written.", VGA_LIGHT_GREY); return 0; }
    return 1;
}

static const char *const STEP_NAME[] = {
    "checking", "the partition table", "formatting / the kernel",
    "copying files", "the boot menu", "done"
};
static void progress(int step, int total, const char *what)
{
    (void)total; (void)step;
    say("  .. ", VGA_DARK_GREY); line(what, VGA_LIGHT_GREY);
}

static void failed(int rc, const mig_result_t *r)
{
    vga_puts("\n");
    say("  FAILED: ", VGA_LIGHT_RED); line(mig_strerror(rc), VGA_LIGHT_RED);
    if(r){
        say("  stopped at: ", VGA_LIGHT_GREY);
        line(STEP_NAME[r->step < 0 || r->step > 5 ? 0 : r->step], VGA_WHITE);
        if(r->esp_rc){
            say("  EFI partition code: ", VGA_LIGHT_GREY); num((uint64_t)(-r->esp_rc));
            vga_puts("\n");
        }
    }
}

static void show_target(int i, const mig_target_t *t)
{
    say("   ", VGA_WHITE); num((uint64_t)(i + 1)); vga_puts("  ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    if(t->kind == MIG_T_GAP){
        vga_puts("free space        "); mb(t->sectors);
        say("   unallocated", VGA_LIGHT_GREEN);
    } else {
        vga_puts("partition "); num((uint64_t)(t->slot + 1)); vga_puts("       ");
        mb(t->sectors);
        if(t->blank) say("   empty -- never used", VGA_LIGHT_GREEN);
        else         say("   no filesystem, but old data in it", VGA_YELLOW);
        if(t->name[0]){ say("  \"", VGA_DARK_GREY); say(t->name, VGA_DARK_GREY); say("\"", VGA_DARK_GREY); }
    }
    vga_puts("\n");
}

static void overview(const mig_survey_t *s)
{
    say("  This system: ", VGA_LIGHT_GREY);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts(s->sys_fs == 16 ? "FAT16" : "FAT32");
    vga_puts(", partition "); num((uint64_t)(s->sys_slot + 1));
    vga_puts(", "); mb(s->sys_sectors);
    vga_puts(", disk "); num((uint64_t)s->disk); vga_puts(" (");
    vga_puts(inst_disk_bus_name(s->disk)); vga_puts(")\n");
}

static void cmd_survey_and_list(void)
{
    mig_survey_t s;
    int rc = mig_survey(&s);
    if(rc != MIG_OK){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(rc), VGA_LIGHT_RED); return; }
    overview(&s);
    vga_puts("\n");

    if(s.sys_fs == 32){
        if(s.n_old){
            line("  The move is done and you are running the new FAT32 install.", VGA_LIGHT_GREEN);
            say("  The old FAT16 one is still there: partition ", VGA_WHITE);
            num((uint64_t)(s.old_slot + 1)); vga_puts(", "); mb(s.old_sectors); vga_puts(".\n\n");
            if(!s.booted_by_serial){
                line("  This boot did not come from the new menu entry, so 'finish' will", VGA_YELLOW);
                line("  refuse. Reboot and pick the FIRST HavenDOS entry.", VGA_YELLOW);
            } else {
                line("  When you are happy everything is here, remove the old one with:", VGA_LIGHT_GREY);
                line("      migrate finish", VGA_LIGHT_CYAN);
            }
        } else {
            line("  This system is already FAT32. Nothing to move.", VGA_LIGHT_GREEN);
        }
        return;
    }

    if(s.n_moved){
        line("  A FAT32 copy of this system is already on this disk.", VGA_YELLOW);
        line("  Reboot and pick the FIRST HavenDOS entry in the menu.", VGA_WHITE);
        return;
    }

    line("  Move HavenDOS to a new FAT32 partition on this disk. Your users,", VGA_LIGHT_GREY);
    line("  passwords, settings and files come with it. Nothing is removed:", VGA_LIGHT_GREY);
    line("  the old install stays in the boot menu until 'migrate finish'.", VGA_LIGHT_GREY);
    vga_puts("\n");
    if(!s.n_targets){
        line("  There is nowhere to move it to. It needs either:", VGA_YELLOW);
        line("    * 512 MB or more of UNALLOCATED space on this disk, or", VGA_WHITE);
        line("    * an EMPTY partition (made, never formatted) of 512 MB or more", VGA_WHITE);
        line("  Windows' Disk Management can make either: shrink a volume, then", VGA_DARK_GREY);
        line("  leave the space unallocated (or make a volume and pick \"Do not", VGA_DARK_GREY);
        line("  format this volume\").", VGA_DARK_GREY);
    } else {
        line("  Places it can go:", VGA_LIGHT_CYAN);
        for(int i = 0; i < s.n_targets; i++) show_target(i, &s.targets[i]);
    }
    if(s.n_refused){
        vga_puts("\n");
        line("  Left alone, and why:", VGA_DARK_GREY);
        for(int i = 0; i < s.n_refused; i++){
            say("      partition ", VGA_DARK_GREY); num((uint64_t)(s.refused[i].slot + 1));
            vga_puts("  "); mb(s.refused[i].sectors); vga_puts("  ");
            line(s.refused[i].why, VGA_DARK_GREY);
        }
    }
    if(s.n_targets){
        vga_puts("\n");
        line("  Then:  migrate <number>    e.g.  migrate 1", VGA_LIGHT_CYAN);
    }
}

static void cmd_move(int n)
{
    mig_survey_t s;
    mig_result_t r;
    int rc = mig_survey(&s);
    if(rc != MIG_OK){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(rc), VGA_LIGHT_RED); return; }
    if(s.sys_fs != 16){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(s.n_old ? MIG_E_TWO : MIG_E_ALREADY32), VGA_LIGHT_RED); return; }
    if(s.n_moved){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(MIG_E_TWO), VGA_LIGHT_RED); return; }
    if(n < 1 || n > s.n_targets){
        line("  No such place. Type 'migrate' to see the list.", VGA_LIGHT_RED);
        return;
    }
    const mig_target_t *t = &s.targets[n - 1];

    overview(&s);
    vga_puts("\n");
    line("  This will:", VGA_LIGHT_CYAN);
    if(t->kind == MIG_T_GAP){
        say("    * add a HavenDOS partition in the free space (", VGA_WHITE); mb(t->sectors); vga_puts(")\n");
    } else {
        say("    * take EMPTY partition ", VGA_WHITE); num((uint64_t)(t->slot + 1));
        vga_puts(" ("); mb(t->sectors); vga_puts(") and mark it HavenDOS\n");
        line("      It has no filesystem on it -- anything in it is lost, and it", VGA_YELLOW);
        line("      disappears from Windows (Windows leaves HavenDOS's type alone).", VGA_YELLOW);
    }
    line("    * format it FAT32 and put this system's kernel on it", VGA_WHITE);
    line("    * copy everything from this install but /boot: users, settings, files", VGA_WHITE);
    line("    * add it to the boot menu as the FIRST HavenDOS entry", VGA_WHITE);
    line("      (Windows stays the default; this install stays, as the third entry)", VGA_DARK_GREY);
    vga_puts("\n");
    line("  This FAT16 install is not changed. Windows is not touched.", VGA_LIGHT_GREEN);
    vga_puts("\n");
    if(!ask(t->kind == MIG_T_PART ? "ERASE" : "MOVE")) return;
    vga_puts("\n");

    /* No other task runs while the table and the volumes are changing:
       the FAT16 driver is shared, and a background job writing a file in
       the middle of the copy is a file the copy did not see. */
    task_lock();
    rc = mig_run(&s, n - 1, progress, &r);
    task_unlock();
    if(rc != MIG_OK){ failed(rc, &r); return; }

    vga_puts("\n");
    line("  Done. HavenDOS is on its new FAT32 partition.", VGA_LIGHT_GREEN);
    say("    partition ", VGA_LIGHT_GREY); num((uint64_t)(r.slot + 1));
    vga_puts(", "); mb(r.sectors); vga_puts(", volume "); serial(r.serial); vga_puts("\n");
    say("    copied ", VGA_LIGHT_GREY); num((uint64_t)r.n_files); vga_puts(" files in ");
    num((uint64_t)r.n_dirs); vga_puts(" folders, "); num((uint64_t)(r.bytes / 1024u)); vga_puts(" KB\n");
    if(r.n_skipped){
        say("    NOT copied (too big for the copy buffer): ", VGA_YELLOW);
        num((uint64_t)r.n_skipped); vga_puts("\n");
        for(int i = 0; i < r.n_skipped && i < 4; i++){ say("      ", VGA_YELLOW); line(r.skipped[i], VGA_YELLOW); }
        line("    They are still on the old install; copy them by hand if you need them.", VGA_DARK_GREY);
    }
    vga_puts("\n");
    line("  Next:", VGA_LIGHT_CYAN);
    line("    1. reboot and pick the FIRST HavenDOS entry in the menu", VGA_WHITE);
    line("    2. log in, check your files are there", VGA_WHITE);
    line("    3. then run:  migrate finish   (removes the old FAT16 install)", VGA_WHITE);
    line("  If the new one does not boot, the old one is still in the menu.", VGA_DARK_GREY);
}

static void cmd_finish(void)
{
    mig_survey_t s;
    mig_result_t r;
    int rc = mig_survey(&s);
    if(rc != MIG_OK){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(rc), VGA_LIGHT_RED); return; }
    overview(&s);
    if(s.sys_fs != 32){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(MIG_E_NOT32), VGA_LIGHT_RED); return; }
    if(!s.booted_by_serial){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(MIG_E_NOTBOOTED), VGA_LIGHT_RED); return; }
    if(!s.n_old){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(MIG_E_NOOLD), VGA_LIGHT_RED); return; }
    if(s.n_old > 1){ say("  Cannot: ", VGA_LIGHT_RED); line(mig_strerror(MIG_E_MANYOLD), VGA_LIGHT_RED); return; }

    vga_puts("\n");
    line("  This will:", VGA_LIGHT_CYAN);
    line("    * take the old FAT16 install out of the boot menu", VGA_WHITE);
    say("    * DELETE the old FAT16 partition ", VGA_WHITE); num((uint64_t)(s.old_slot + 1));
    vga_puts(" ("); mb(s.old_sectors); vga_puts(", volume "); serial(s.old_serial); vga_puts(")\n");
    line("      Everything on it goes. This install (FAT32) is what stays.", VGA_YELLOW);
    line("      The space becomes unallocated; Windows can use it.", VGA_DARK_GREY);
    vga_puts("\n");
    if(!ask("REMOVE")) return;
    vga_puts("\n");

    task_lock();
    rc = mig_finish(&s, progress, &r);
    task_unlock();
    if(rc != MIG_OK){
        failed(rc, &r);
        line("  Run 'migrate finish' again to complete it; each step is safe to repeat.", VGA_LIGHT_GREY);
        return;
    }
    vga_puts("\n");
    line("  Done. The old FAT16 install is gone.", VGA_LIGHT_GREEN);
    say("    ", VGA_LIGHT_GREY); num((uint64_t)r.freed_mb);
    vga_puts(" MB is unallocated now. The boot menu has one HavenDOS entry: this one.\n");
}

void cmd_migrate(const char *arg)
{
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("\n");
    line("Move HavenDOS " HAVENDOS_VER " to FAT32", VGA_LIGHT_CYAN);
    vga_puts("\n");
    /* No rescan: the system disk was found at boot, and a rescan would
       re-probe every controller for a question about one disk. */

    while(arg && *arg == ' ') arg++;
    if(!arg || !*arg){ cmd_survey_and_list(); return; }
    if(!strcmp(arg, "finish")){ cmd_finish(); return; }
    if(arg[0] >= '1' && arg[0] <= '9' && (arg[1] == 0 || arg[1] == ' ')){
        cmd_move(arg[0] - '0'); return;
    }
    line("  usage: migrate | migrate <number> | migrate finish", VGA_LIGHT_GREY);
}
