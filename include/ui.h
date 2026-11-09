#pragma once
#include "types.h"

/* The shared pager.
 *
 * Added in v0.8.5 for `lspci -vv`, which scrolled off the top of a real
 * laptop's screen with no way to read it; declared there with a bare
 * `extern` because it had exactly one caller. It has two now -- `usb -vv`
 * had the same problem on the same machine -- so the declaration lives in
 * one place rather than being copied into the second file. (v0.8.5.1)
 *
 * The page height comes from vga_rows(), not a constant: the 90x60 modes
 * exist today and v0.8.8 adds more. shell/help.c still has its own pager
 * hardcoded to 20 lines, which is already wrong in those modes.
 *
 *   ui_page_reset()  start a fresh page count
 *   ui_page_line()   count one line just printed; returns 0 if the user
 *                    asked to stop, in which case print nothing further
 */
void ui_page_reset(void);
int  ui_page_line(void);
