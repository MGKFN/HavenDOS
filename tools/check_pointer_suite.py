#!/usr/bin/env python3
"""check_pointer_suite.py -- prove tests/test_kbdinit.c can fail about
the POINTER, which is the thing it had never been asked about.

v0.8.8.1's first finding, and it came from a laptop rather than from
anything in this tree: on the HP both the trackpad and an external USB
mouse were dead, and one cause explained both.

`mouse_ready` was set in exactly one place -- the end of mouse_init(),
i.e. the PS/2 reset having been acknowledged. usb/hid.c synthesises
PS/2-shaped packets and pushes them through mouse_feed(), so a USB
mouse tracked perfectly and never touched that flag. Every consumer
gates on it:

    shell/ui.c:317        if(!mouse_is_ready()) return;   <- no cursor
    shell/desktop.c:890   if(mouse_is_ready()) ui_draw_cursor();
    shell/desktop.c:1474  if(mouse_is_ready()){ ...clicks... }

So the trackpad did not ACK, the flag stayed 0, and a working USB
mouse was invisible.

It is the bus-dispatch mistake this tree has fixed three times
elsewhere -- one driver's private flag answering a question about the
whole subsystem, which is `nic_type` in net/net.c and the disk bus
label in four screens. The injections below are each a way of putting
one of those back.

AND THE BOOT LOG. kernel/main.c printed "ok PS/2: keyboard on IRQ1,
mouse polled" unconditionally, whether or not mouse_init() bailed at
its first command -- so the one screen that could have diagnosed this
reported a mouse that was not there. v0.8.8 edited that very line to
correct its IRQ claim and left the bigger one in it.

Run:  python3 tools/check_pointer_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "drivers/mouse.c"
TEST = "tests/test_kbdinit.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    ("readiness is the PS/2 driver's flag again",
     "the defect exactly as it shipped. On a laptop whose trackpad "
     "does not answer the PS/2 reset, a working USB mouse is tracked "
     "and invisible, because every consumer in the tree gates on this "
     "one function",
     """int  mouse_is_ready(void)              { return ps2_ready
                                                || usb_pointers > 0
                                                || i2c_pointers > 0; }""",
     """int  mouse_is_ready(void)              { return ps2_ready; }"""),

    ("readiness ignores the PS/2 side instead",
     "the same mistake pointing the other way: a machine with a "
     "working trackpad and no USB mouse loses its cursor. Half a "
     "subsystem answering for all of it is wrong in both directions",
     """int  mouse_is_ready(void)              { return ps2_ready
                                                || usb_pointers > 0
                                                || i2c_pointers > 0; }""",
     """int  mouse_is_ready(void)              { return usb_pointers > 0
                                                || i2c_pointers > 0; }"""),

    ("a USB mouse is a flag rather than a count",
     "two mice is a thing people do. With a flag, unplugging one takes "
     "the pointer away while the other is still plugged in",
     """void mouse_usb_attach(void)    { usb_pointers++; }""",
     """void mouse_usb_attach(void)    { usb_pointers = 1; }"""),

    ("the USB count is never reset",
     "usb_hid_init() sets its own device count back to zero and walks "
     "the bus again -- `usb rescan`, and now the hot-plug client every "
     "second. A count that survives that keeps claiming a cursor for a "
     "mouse that has been unplugged. Same reason usbnet_release() runs "
     "before usb_enumerate()",
     """void mouse_usb_forget_all(void){ usb_pointers = 0; }""",
     """void mouse_usb_forget_all(void){ }"""),

    ("the PS/2 reset is believed without an ACK",
     "mouse_init() returns early when the aux device does not "
     "acknowledge -- that early return is the only thing that stops a "
     "machine with no mouse reporting one, and with it gone the flag "
     "is set on hardware that never answered",
     """    mouse_cmd(0xFF);
    if(mouse_rd()!=0xFA){ return; }   /* no ACK → no mouse */""",
     """    mouse_cmd(0xFF);
    mouse_rd();"""),

    ("ps2_ready is set before the device has answered anything",
     "the same thing one step earlier, and it reads as perfectly "
     "ordinary initialisation order",
     """void mouse_init(void){
    /* Enable aux device */
    mw(); outb(0x64,0xA8);""",
     """void mouse_init(void){
    ps2_ready = 1;
    /* Enable aux device */
    mw(); outb(0x64,0xA8);"""),

    ("the two halves report each other",
     "mouse_ps2_ready() exists so the BOOT LOG can say what actually "
     "happened. Answering it with the combined state puts the old lie "
     "back: a machine with a USB mouse and no trackpad would log "
     "'mouse polled (8042 IRQ12 off)', which is about a port that "
     "answered nothing",
     """int  mouse_ps2_ready(void)             { return ps2_ready; }""",
     """int  mouse_ps2_ready(void)             { return mouse_is_ready(); }"""),

    # ── v0.8.9.6 ──────────────────────────────────────────────────────
    ("speed is counted in cells again",
     "four counts per cell: a 240-column grid takes three times the "
     "hand movement of 80x25 to cross -- 'heel sloom' on the HP",
     """    *num_x = cols;  *den_x = MOUSE_SPAN;""",
     """    *num_x = 1;  *den_x = MOUSE_DIVISOR;"""),

    ("the vertical span ignores the cell's shape",
     "a cell is twice as tall as it is wide, so the pointer moves twice "
     "as many pixels down as across for the same hand movement",
     """    *den_y = (int)((MOUSE_SPAN * ph + pw / 2) / pw);""",
     """    *den_y = MOUSE_SPAN * rows / cols;"""),

    ("an edge keeps the remainder pushed into it",
     "the first movement away from an edge pays off sub-cell debt "
     "before anything happens -- a cell of dead travel at every edge",
     """        if(mouse_y < 0)    { mouse_y = 0;    accum_y = 0; }""",
     """        if(mouse_y < 0)    { mouse_y = 0; }"""),

    ("acceleration on the signed value",
     "the v0.7.17 bug in its new home: every leftward movement is "
     "boosted whatever its speed, and the pointer drifts left",
     """    int mag = raw < 0 ? -raw : raw;""",
     """    int mag = raw;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """int  mouse_usb_count(void)             { return usb_pointers; }""",
    """/* how many USB pointing devices are registered right now */
int  mouse_usb_count(void)             { return usb_pointers; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "pointer", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        preamble="Proving tests/test_kbdinit.c can fail about the "
                 "POINTER.\nIt had 21 checks about the 8042's timing "
                 "and none about whether a cursor appears."))
