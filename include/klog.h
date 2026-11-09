#ifndef KLOG_H
#define KLOG_H
#include "types.h"

/* ── the kernel log ────────────────────────────────────────────────────
 *
 * Before this existed, HavenDOS had THREE boot screens and two of them were
 * fiction.  The POST screen pretended to be firmware that had already run
 * before GRUB.  The verbose boot log was an array of 24 string literals, of
 * which four were real; it told every machine that ATA was absent, that
 * FAT16 was on a VirtIO partition, that the heap was 4 MB at 0x00200000 (it
 * is a 256 KB static array) and that the GDT had 256 descriptors (it has 3).
 * `dmesg` printed its own copy of the same inventions with timestamps a
 * millisecond apart that nothing had measured.
 *
 * Meanwhile the real information existed and was scattered across private
 * buffers nothing displayed: pci_get_log(), uhci_get_log(), virtio_blk_log().
 *
 * So there is now one log.  Every subsystem writes a line as it initialises,
 * from inside the code that actually knows the numbers, and the boot screen
 * and `dmesg` are both views of it.  A line nobody measured is a line that
 * does not get written.
 *
 * Echo is ON from the first line, so a kernel that dies halfway through init
 * has already said how far it got -- which is the entire reason for having
 * this rather than an animation that plays after everything succeeded.
 *
 * ── AND IT IS OFF ONCE INIT IS OVER, v0.8.8 ───────────────────────────
 *
 * That argument is about INIT and stops applying the moment init ends, so
 * kernel/main.c calls klog_set_echo(0) after "System ready".
 *
 * It was safe to leave on for as long as every klog() call came from a
 * driver bringing itself up, in a fixed order, before anything was on the
 * screen.  v0.8.8 made that false: net/net_tick.c logs a DHCP retry at ten
 * seconds and again at forty, a carrier transition whenever the cable
 * moves, and a hot-plug rescan several lines -- all from kernel_poll(),
 * which runs from the keyboard's idle loop, which is to say at exactly the
 * moments the screen belongs to somebody else.
 *
 * The first boot after wiring the clients up printed a DHCP line over the
 * top of dmesg's own pager prompt.  Found by booting it.
 *
 * The ring keeps every line either way and `dmesg` is the view of it, which
 * is what a printk ring is for.
 */

#define KLOG_OK    0
#define KLOG_WARN  1
#define KLOG_ERR   2
#define KLOG_INFO  3

/* Record one line, and print it if echo is on.  Supports %d %u %x %s %c %%,
   the same set vga_printf takes. */
void klog(int level, const char *fmt, ...);

/* Live echo to the screen.  Turned off by `quiet` on the kernel command
   line, or by the saved config once the disk is readable -- which is late,
   because reading the config needs most of the drivers this log describes. */
void klog_set_echo(int on);
int  klog_echo_on(void);

/* ── a sink, instead of the scrolling echo ───────────────────────────
   While a sink is installed it receives every recorded line and the
   echo is bypassed. kernel/bootsplash.c uses it so the loading screen
   can show the latest line as its caption and turn its ring once per
   line.

   A POINTER AND NOT A DIRECT CALL. klog.c called bootsplash_active()
   directly first, and every native suite that links klog.c stopped
   linking, because a test harness has no loading screen -- which
   would have meant the same stub pasted into twenty test files. This
   file does not need to know what a loading screen is.

   The ring buffer keeps every line regardless, so a sink is a view
   and never a filter: klog_dump() replays the lot afterwards. */
void klog_set_sink(void (*fn)(int level, const char *text));

/* Replay, for dmesg. */
int         klog_count(void);
uint32_t    klog_ms(int i);
int         klog_level(int i);
const char *klog_text(int i);
int         klog_dropped(void);
void        klog_dump(void);        /* replay the lot */
void        klog_print_line(int i); /* replay one, for a paged view */

/* Milliseconds since boot, from whichever of the two clocks is doing better.
   See kernel/pit.c -- both undercount in different circumstances and this is
   the better lower bound. */
uint32_t klog_now_ms(void);

#endif
