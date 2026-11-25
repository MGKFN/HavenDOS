/* shell/screenshot.c -- the PrtSc key (v0.9.10).
 *
 * Kyan, 25 Nov: "maybe add image and screenshot support" -- and then,
 * "how to send from HavenDOS to you??". The answer is this file. PrtSc,
 * anywhere, saves the whole screen as a PNG:
 *
 *   a FAT32 USB stick in   ->  USB:\Pictures\Shot-20261125-213105.png
 *                              (take the stick to Windows, attach the file)
 *   no stick               ->  Pictures\... on the system disk -- and on
 *                              a live boot that is RAM, gone at the reboot,
 *                              which the toast says
 *
 * The folder is "Pictures", 8.3-legal: a FAT16 install cannot hold a long
 * name (tools/check_migrate_names.py), and there the FILE gets one too --
 * "25213105.PNG", the day and the time.
 *
 * `screenshot [path]` in the Terminal does the same, after a moment so
 * the Terminal's own line is drawn first.
 *
 * The key is taken in drivers/keyboard.c's keyboard_getchar(), like
 * Alt+Tab -- never from the interrupt: encoding a PNG takes a while.
 */
#include "../include/png.h"
#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/rtc.h"
#include "../include/toast.h"
#include "../include/usbvol.h"
#include "../include/string.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern int   vfs_write(const char *name, const char *data, uint32_t size);
extern int   vfs_mkdir(const char *name);
extern int   boot_is_live(void);

static void two(char *p, int v){ p[0] = (char)('0' + v / 10 % 10); p[1] = (char)('0' + v % 10); }

/* "Shot-20261125-213105.png": 24 characters, inside the system disk's
   27-character name (the longer "HavenDOS-2026-..." lost its ".png") */
static void stamp_name(char *out)
{
    uint8_t h, m, s, d, mo; uint16_t y;
    rtc_get_time(&h, &m, &s); rtc_get_date(&d, &mo, &y);
    strcpy(out, "Shot-00000000-000000.png");
    out[5] = (char)('0' + y / 1000 % 10); out[6] = (char)('0' + y / 100 % 10);
    two(out + 7, y % 100); two(out + 9, mo); two(out + 11, d);
    two(out + 14, h); two(out + 16, m); two(out + 18, s);
}

/* v0.9.10: the signed-in user's Pictures -- HOME/<user>/Pictures, where
   the Terminal's `ls Pictures` finds it from the home folder; "Pictures"
   in the root before anyone has signed in */
void screenshot_dir(char *out)
{
    extern const char *current_username(void) __attribute__((weak));
    const char *u = current_username ? current_username() : 0;
    if (u && u[0]) { strcpy(out, "HOME/"); strncat(out, u, 24); strcat(out, "/Pictures"); }
    else strcpy(out, "Pictures");
}

static const char *g_last;
static char g_path[160];
const char *screenshot_last_path(void){ return g_last; }

/* Take it. path: where, or 0 for the stick or the system disk. Returns
   0, or -1 with a toast saying why. */
int screenshot_take(const char *path)
{
    int w, h, pitch;
    uint32_t *fb = vga_fb_pixels(&w, &h, &pitch);
    char name[40], msg[200];
    if (!fb) { toast_warning("Screenshots need the pixel screen (this one is text mode)"); return -1; }
    /* one read of the screen (slow memory: once), without the pointer */
    uint32_t *copy = (uint32_t *)kmalloc((uint32_t)w * (uint32_t)h * 4u);
    if (!copy) { toast_error("Screenshot: not enough memory"); return -1; }
    vga_pointer_hide_save();
    for (int y = 0; y < h; y++) memcpy32(copy + y * w, fb + y * pitch, (size_t)w);
    vga_pointer_show_last();
    uint32_t n = 0;
    uint8_t *png = png_encode(copy, w, h, w, &n);
    kfree(copy);
    if (!png) { toast_error("Screenshot: not enough memory to make the PNG"); return -1; }
    stamp_name(name);
    { extern int vfs_using_fat16(void);
      if (!(path && *path) && !usbv_writable() && vfs_using_fat16()) {
          /* DDhhmmss.PNG */
          memmove(name, name + 17, 2); memmove(name + 2, name + 20, 6); strcpy(name + 8, ".PNG");
      } }
    int onstick = 0;
    if (path && *path) strncpy(g_path, path, sizeof g_path - 1), g_path[sizeof g_path - 1] = 0;
    else if (usbv_writable()) {
        vfs_mkdir("/usb/Pictures");
        strcpy(g_path, "/usb/Pictures/"); strcat(g_path, name); onstick = 1;
    } else {
        screenshot_dir(g_path);
        vfs_mkdir(g_path);
        strcat(g_path, "/"); strcat(g_path, name);
    }
    int ok = vfs_write(g_path, (const char *)png, n) == (int)n;
    kfree(png);
    if (!ok) {
        strcpy(msg, "Screenshot not saved -- could not write "); strcat(msg, g_path);
        toast_error(msg);
        return -1;
    }
    g_last = g_path;
    if (onstick) { strcpy(msg, "Screenshot saved to the USB stick: Pictures\\"); strcat(msg, name); }
    else if (boot_is_live()) { strcpy(msg, "Screenshot in Pictures\\ -- live boot, so in RAM: put a USB stick in to keep it"); }
    else { strcpy(msg, "Screenshot saved: Pictures\\"); strcat(msg, name); }
    toast_success(msg);
    return 0;
}

/* `screenshot [path]` */
void cmd_screenshot(const char *args)
{
    extern void kpoll_sleep_ms(uint32_t ms);
    while (args && *args == ' ') args++;
    vga_flush();
    kpoll_sleep_ms(300);
    char full[96]; full[0] = 0;
    if (args && *args) { extern void make_path(char *dst, const char *name); make_path(full, args); }
    if (screenshot_take(full[0] ? full : 0) == 0) {
        vga_puts("Saved: "); vga_puts(g_last); vga_putchar('\n');
    } else vga_puts("Screenshot failed (see the message on screen).\n");
}
