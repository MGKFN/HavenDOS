/*
 * bootanim.c — the splash, and honouring the saved verbose setting
 *
 * This file used to be 260 lines of theatre.  It drew a POST screen that
 * announced "TechHaven BIOS v2.1" and performed a power-on self test --
 * except HavenDOS is not firmware.  SeaBIOS POSTs the machine before GRUB
 * has even loaded the kernel, so this was a second POST for a test that had
 * already happened, and it ended in a blocking keyboard_waitchar().  Then a
 * "boot log" scrolled 24 lines of which twenty were string literals, four of
 * them untrue on any given machine, and waited for a second keypress.
 *
 * Two mandatory keypresses and half a second of sleep_ms padding, to tell
 * you things nobody had measured.
 *
 * What is left: a splash that makes no claims about hardware, and one place
 * that honours the saved "verbose boot" setting.  The actual boot log is
 * printed live by kernel/klog.c as each subsystem initialises, which is the
 * only arrangement that survives a kernel dying halfway through init.
 */
#include "../include/vga.h"
#include "version.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/klog.h"

extern int      keyboard_getchar(void);
extern void     sleep_ms(uint32_t ms);

static void cls(void){
    for(int y=0;y<vga_rows();y++)
        for(int x=0;x<vga_cols();x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

static void centre(const char *s, int y, vga_color_t fg){
    int len=0; while(s[len]) len++;
    int x=(vga_cols()-len)/2; if(x<0) x=0;
    vga_puts_at(s,x,y,fg,VGA_BLACK);
}

/* ── the splash ───────────────────────────────────────────────────────
 * Name, version, vendor.  It does not say what hardware is present, what
 * passed a test, or what it is pretending to be -- the log does all of
 * that, from the drivers, a few milliseconds later.
 *
 * It holds briefly so there is a brand moment rather than a flicker, and
 * any keypress cuts it short.  Nothing here blocks: a boot nobody is
 * watching still reaches the system.
 */
void boot_splash(void){
    cls();
    int mid = vga_rows()/2;
    centre("------------------------------------------", mid-3, VGA_DARK_GREY);
    centre(HAVENDOS_NAME,   mid-1, VGA_LIGHT_CYAN);
    centre(HAVENDOS_VER " " HAVENDOS_CHANNEL, mid, VGA_WHITE);
    centre("\"" HAVENDOS_CODENAME "\"", mid+1, VGA_DARK_GREY);
    centre(HAVENDOS_VENDOR, mid+2, VGA_DARK_GREY);
    centre("------------------------------------------", mid+4, VGA_DARK_GREY);
    vga_flush();

    /* 600 ms, in 20 ms steps so a keypress cuts it short.  The old screens
       cost two keypresses and would wait forever for them. */
    for(int i=0;i<30;i++){
        if(keyboard_getchar()>0) break;
        sleep_ms(20);
    }
    cls();
    vga_set_cursor(0,0);
    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
}

/* ── the saved setting ────────────────────────────────────────────────
 * Reading the config needs the filesystem, which needs the drivers this log
 * has already described -- so by the time the saved preference is readable,
 * the log has been on screen for most of a second.  That is the right way
 * round: the setting cannot be allowed to decide whether a failing boot gets
 * to say anything.  If verbose is off, the screen is cleared here and the
 * lines stay in the buffer for `dmesg`.
 */
void boot_animation(int had_loading_screen){
    extern int cfg_verbose_boot(void);
    extern int  bootsplash_active(void);
    extern void bootsplash_end(int replay);

    /* ── v0.8.8.4: the loading screen and the saved setting ───────────
       kernel/bootsplash.c is up by default from early in init, which
       is before this setting can be read -- the config lives on a
       filesystem that needs most of the drivers the log describes. So
       the decision is made here, as late as it can be made and as
       early as it is possible to make it.

       If the saved preference is verbose, the loading screen comes
       down and the WHOLE log is replayed, not just the lines from
       here on. The ring buffer kept every line while the screen was
       showing them one at a time as captions, so replaying costs
       nothing and loses nothing -- which is the only reason the
       loading screen was allowed to take the screen that early. */
    if(bootsplash_active()){
        if(cfg_verbose_boot()) bootsplash_end(1);
        return;              /* otherwise it stays up until "System ready" */
    }

    /* ── SHELL AND SAFE MODE KEEP THEIR LOG ───────────────────────────
       FOUND BY BOOTING IT: both boot harnesses broke the moment
       verbose_boot's default changed to 0, with "the boot log never
       completed" -- because this function read that one flag and
       cleared the screen, and in shell mode there was no loading
       screen to have replaced it with. The log simply vanished.

       The flag changed meaning in this release. It used to be "echo
       the log at all"; it is now "show the log INSTEAD OF the loading
       screen", and where there is no loading screen there is nothing
       for it to decide. Shell and safe mode are the diagnostic
       entries and always keep their log; kernel/main.c passes 0 for
       them and the function stops here.

       Two meanings on one flag is how a default change silently
       deleted the output of a boot. */
    if(!had_loading_screen) return;

    if(!cfg_verbose_boot()){
        klog_set_echo(0);
        cls();
        vga_set_cursor(0,0);
    }
}
