#pragma once
#include "types.h"

/* ════════════════════════════════════════════════════════════════════
   include/bootsplash.h -- the loading screen.  v0.8.8.4.

   A logo, a turning ring, and the name of the thing the kernel is
   doing right now, instead of a blank screen.

   ── WHY IT IS NOT A PROGRESS BAR AND NOT A TIMER ────────────────────

   shell/bootanim.c's header is the warning this file was written
   against. That file used to be "260 lines of theatre": a POST screen
   for a test SeaBIOS had already done, a boot log of twenty string
   literals "four of them untrue on any given machine", and two
   mandatory keypresses. All of it described a machine nobody had
   measured.

   So the ring here does not spin on a timer and there is no
   percentage. IT IS DRIVEN BY klog(). Every line the kernel records
   advances it one position and replaces the caption, so the ring
   turns exactly as fast as the machine is actually getting things
   done -- and when it stops turning, something has stopped.

   A percentage would need a total, and the only honest source for
   "how many steps is a boot" is a previous boot of the same machine,
   which the first boot does not have and a changed configuration
   invalidates. A ring makes no such claim.

   ── AND THE PROPERTY IT MUST NOT BREAK ──────────────────────────────

   include/klog.h: the live echo exists so that "a kernel that dies
   halfway through init has already said how far it got". A splash
   that hid the log would destroy that.

   It does not hide it. The caption IS the last line klog recorded, in
   full, so a kernel that wedges leaves the name of the step it wedged
   in on the screen -- the same information the scrolling log would
   have left, in one line instead of twenty-four. The ring buffer
   still has every line for `dmesg`, and ANY KEYPRESS drops the splash
   and replays the whole log immediately, which is what a person
   debugging a boot actually wants.
   ════════════════════════════════════════════════════════════════════ */

/* Take over the screen. Turns klog's scrolling echo off and starts
   routing lines here instead. */
void bootsplash_begin(void);

/* One recorded line. Called from klog() and from nowhere else: the
   ring advances and the caption changes. */
void bootsplash_note(int level, const char *text);

/* Is the splash up? klog() asks, to decide where a line goes. */
int  bootsplash_active(void);

/* Drop the splash. If `replay` is set, the whole buffered log is
   printed where it would have been -- so turning the splash off never
   loses a line. Used when the saved verbose setting turns out to be
   on, and when a keypress asks for the log. */
void bootsplash_end(int replay);
