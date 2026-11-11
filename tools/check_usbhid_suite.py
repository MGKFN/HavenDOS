#!/usr/bin/env python3
"""check_usbhid_suite.py -- prove tests/test_usbhid.c can fail.

v0.9.2.1. usb/hid.c now drives every boot keyboard and boot mouse
interface of a device, not only the first -- because a wireless
receiver's mouse is on interface 1, and on Kyan's HP on 11 November the
receiver did nothing. The suite was also run against the 0.9.2 driver
by hand and failed 14 of its 29 checks; this harness keeps it honest
against the defects a later edit could reintroduce.

Its first run missed one: SET_PROTOCOL sent to the core's `ifnum`
instead of the interface being bound passed, because after a claim the
two agree. Only a RESCAN tells them apart, and the suite now checks the
requests a rescan sends.

Run:  python3 tools/check_usbhid_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC = "usb/hid.c"
TEST = "tests/test_usbhid.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function",
      "-fsanitize=address,undefined"]

BUGS = [
    ("only the first interface is driven again (0.9.2's bug)",
     "the receiver's mouse, on interface 1, is never looked at",
     """        for (k = 0; k < sv.n && g_nhid < HID_MAX; k++)""",
     """        for (k = 0; k < sv.n && k < 1 && g_nhid < HID_MAX; k++)"""),

    ("the class requests go to the interface the core chose",
     "SET_PROTOCOL to interface 0 twice; the mouse stays in report "
     "protocol and its bytes are not a boot report",
     """                    HID_REQ_SET_PROTOCOL, 0, num, 0, 0) < 0) {""",
     """                    HID_REQ_SET_PROTOCOL, 0, d->ifnum, 0, 0) < 0) {"""),

    ("an interface is claimed even when its endpoints are already held",
     "every rescan reconfigures a keyboard somebody is typing on",
     """    e = usb_find_ep_if(d, num, USB_EP_INTR, 1);
    if (!e) {""",
     """    e = 0;
    if (!e) {"""),

    ("the endpoint is looked for on any interface",
     "the mouse is bound to the keyboard's endpoint and types its "
     "movement as keys",
     """        e = usb_find_ep_if(d, num, USB_EP_INTR, 1);
    }""",
     """        e = usb_find_ep(d, USB_EP_INTR, 1);
    }"""),

    ("a non-boot interface is driven anyway",
     "an NKRO or vendor report read as a boot report is noise",
     """    if (sub != HID_SUBCLASS_BOOT) {""",
     """    if (0) {"""),

    ("a mouse interface does not claim the pointer",
     "drivers/mouse.c draws no cursor for a device it was never told of",
     """    if (h->proto == HID_PROTO_MOUSE) mouse_usb_attach();""",
     """    /* pointer not claimed */"""),

    ("the survey reads the wrong configuration",
     "index 0 is not the configuration in use on a device that has "
     "several; here, asking index 1 finds nothing and falls back",
     """        if (usb_survey_config(d, d->cfgidx, hid_survey_cb, &sv) < 0 || !sv.n) {""",
     """        if (usb_survey_config(d, (uint8_t)(d->cfgidx + 1), hid_survey_cb, &sv) < 0 || !sv.n) {"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the driver changed",
    """int usb_hid_count(void) { return g_nhid; }""",
    """/* how many */
int usb_hid_count(void) { return g_nhid; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "usbhid", SRC, TEST, CC, BUGS, HARMLESS, timeout=120,
        files=(SRC, TEST),
        preamble="Proving tests/test_usbhid.c can fail."))
