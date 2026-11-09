#pragma once
/* ════════════════════════════════════════════════════════════════════
   include/keys.h -- the key codes the drivers push.  v0.8.8.4.

   drivers/keyboard.c and usb/hid.c both push the same values, so a
   key from a PS/2 keyboard and a key from a USB keyboard are
   indistinguishable by the time anything reads them. usb/hid.c's own
   header makes the point: the codes are "not invented here".

   They are 0x100 | <PS/2 set-1 scancode>, which is why they look
   arbitrary -- 0x148 is Up because Up is scancode 0x48.

   ── WHY THIS HEADER EXISTS NOW ──────────────────────────────────────

   The codes were bare hex at every use site. That was survivable
   while they were only arrows, because 0x148 appears next to a
   comment saying Up every time. KEY_SUPER is different: it is read in
   the desktop's main loop, in the shell, and in two drivers, and
   "0x15B" with no comment in the fifth place is how a key quietly
   stops working on one path only.

   Existing bare uses are not swept up here. A rename touching fifty
   lines across six files to make a comment redundant is how a release
   acquires a diff nobody can review, and the arrows are not the thing
   that changed. New code uses the names.
   ════════════════════════════════════════════════════════════════════ */

#define KEY_EXT(sc)   (0x100 | (sc))

#define KEY_F1        0x13B
#define KEY_F2        0x13C
#define KEY_F3        0x13D
#define KEY_F4        0x13E

#define KEY_HOME      0x147
#define KEY_UP        0x148
#define KEY_PGUP      0x149
#define KEY_LEFT      0x14B
#define KEY_RIGHT     0x14D
#define KEY_END       0x14F
#define KEY_DOWN      0x150
#define KEY_PGDN      0x151
#define KEY_INS       0x152
#define KEY_DEL       0x153

/* ── the Windows key ─────────────────────────────────────────────────
   PS/2 sends E0 5B for the left one and E0 5C for the right, and
   drivers/keyboard.c discarded both: they fell past the switch in the
   extended block and were dropped, so the key did nothing anywhere in
   the system.

   On USB it is a MODIFIER, not a key in the report's key array -- bit
   3 of the modifier byte for left GUI, bit 7 for right -- which is a
   different place in a different structure, so usb/hid.c has to look
   for it separately. A keyboard where the Windows key works on PS/2
   and not on USB would be exactly the kind of thing that goes
   unnoticed until it is on a laptop.

   BOTH are reported as KEY_SUPER. Nothing in this system wants to
   tell left from right, and a desktop that opened the start menu from
   one side of the spacebar only would be a worse bug than no key at
   all. */
#define KEY_SUPER     0x15B
