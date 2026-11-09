#!/usr/bin/env python3
"""check_hotplug_suite.py -- prove the hot-plug section of
tests/test_kpoll.c can fail.

usb/hotplug.c is the client with the most to get wrong in v0.8.8: a
bitmap compared across polls, and a seven-call sequence whose ORDER is
its entire content.  Every step of that order exists because of a
specific bug found on real hardware, which means an injection that
reorders it is not hypothetical -- it is a regression to a release
this project has already paid for:

  - usbnet_release() before the walk is v0.8.7. A NIC holds an INDEX
    into the core's device table and usb_enumerate() rebuilds it.
  - lockdown_forget()/usbmux_close() are v0.8.7.7. A connection whose
    transport has been torn down reports itself up.
  - msc_rescan() after the walk is v0.8.6. Each mass-storage device
    holds a COPY of its usb_dev_t.
  - blk_scan() last of the three is v0.8.6 again: a disk that has gone
    away must not stay in the installer's picker.

And two injections are about the thing a bitmap client can do that
nothing else can: rescan in a loop, for ever, off one plug event.

Run:  python3 tools/check_hotplug_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/hotplug.c"
TEST = "tests/test_kpoll.c"
CC = ["gcc", "-std=gnu99", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

CHECKS = [["python3", "tools/check_kpoll_guard.py"]]

BUGS = [
    ("the baseline is taken before anything is read",
     "every occupied port reads as a brand-new arrival on the first "
     "poll, so a machine that boots with a stick in it rescans the "
     "bus a second later for no reason -- and v0.8.6.1's slot leak "
     "means a rescan is not free: on xHCI the controller owns the "
     "device and the third rescan loses one",
     """void usb_kpoll_register(void)
{
    usb_hotplug_baseline();
    kpoll_register("usb ports", 1000, usb_hotplug_work);
}""",
     """void usb_kpoll_register(void)
{
    kpoll_register("usb ports", 1000, usb_hotplug_work);
}"""),

    ("the bitmap is not re-read after the rescan",
     "enumeration resets ports, so a reading taken before it can "
     "disagree with the settled one -- and the next poll sees that "
     "disagreement as another change. One plug event becomes a rescan "
     "every second until the readings happen to agree",
     """    usb_hotplug_baseline();

    /* A NIC that has just arrived needs an address""",
     """    /* A NIC that has just arrived needs an address"""),

    # ── NOT A DEFECT, and recorded rather than deleted ───────────────
    #
    # "the remembered bitmap is updated in the loop as well" was
    # written as an injection, twice, and reported MISSED both times.
    #
    # THE FIRST TIME IT WAS A REAL FINDING AND NOT THIS ONE.
    # usb/hotplug.c originally wrote g_seen[c] = now inside the change
    # loop AND re-read the whole bitmap from usb_hotplug_baseline()
    # after the rescan. Two writes, and each masked the other: delete
    # either alone and the behaviour is identical, so an injection into
    # EITHER was missed. That is v0.8.6.7's failing-only-in-pairs
    # shape, and four of tests/test_netdev.c's seven holes were the
    # same thing. The redundant write was removed and the injection
    # into the surviving one is caught -- it is number 2 in the list
    # above.
    #
    # THE SECOND TIME, putting the redundant write BACK, it is simply
    # not a defect. The post-rescan baseline runs afterwards and
    # overwrites whatever the loop recorded, so the mid-change value
    # can never survive. The comment in usb/hotplug.c explains why the
    # ordering is the way round it is; the redundant write is dead
    # rather than wrong.
    #
    # Left here as a note because v0.8.6.3's harness proved two of its
    # ten injections not to be defects at all, and an injection deleted
    # without a word is one somebody writes again.

    ("the NIC is not released before the bus walk",
     "v0.8.7's bug. A NIC left registered across a rescan holds an "
     "index into a table usb_enumerate() has rebuilt, so it "
     "addresses whatever moved into its old slot",
     """    usbnet_release();
    lockdown_forget();""",
     """    lockdown_forget();"""),

    ("the lockdownd connection is not told the transport is going",
     "v0.8.7.7. It holds no device index of its own, so nothing "
     "breaks loudly -- the connection simply reports itself up over a "
     "pipe that no longer exists, which is the worst of the two "
     "outcomes",
     """    usbnet_release();
    lockdown_forget();
    usbmux_close();""",
     """    usbnet_release();
    usbmux_close();"""),

    ("the block layer is rebuilt before the storage devices are",
     "msc_rescan() re-probes the devices the block layer then lists, "
     "so doing them the other way round registers disks from stale "
     "usb_dev_t copies. The order is the order the layers stack",
     """    g_last.devices = usb_enumerate();
    g_last.disks   = msc_rescan();
    g_last.blkdevs = blk_scan();""",
     """    g_last.devices = usb_enumerate();
    g_last.blkdevs = blk_scan();
    g_last.disks   = msc_rescan();"""),

    ("the NIC is re-probed before the bus is walked",
     "there is no bus to probe yet. The adapter comes back absent and "
     "`ifconfig` reports no NIC on a machine with a phone plugged in",
     """    g_last.devices = usb_enumerate();
    g_last.disks   = msc_rescan();
    g_last.blkdevs = blk_scan();
    g_last.nics    = usbnet_probe();""",
     """    g_last.nics    = usbnet_probe();
    g_last.devices = usb_enumerate();
    g_last.disks   = msc_rescan();
    g_last.blkdevs = blk_scan();"""),

    ("a back-end with no port_connected op reports every port empty",
     "a controller whose optional ops are absent -- which is what the "
     "op being optional MEANS -- would read as a mass removal on the "
     "first poll, and rescan the bus once a second for ever",
     """    if(!h->ops->port_connected) return 0;""",
     """    /* missing op not handled */"""),

    ("a controller with more root ports than the bitmap is truncated silently",
     "reading 32 of 40 ports is the kind of truncation that looks "
     "like a dead socket, and the person holding the stick has no way "
     "to tell it from a broken port",
     """    if(n > HP_MAX_PORTS){ g_truncated++; n = HP_MAX_PORTS; }""",
     """    if(n > HP_MAX_PORTS){ n = HP_MAX_PORTS; }"""),

    ("a NIC that arrives does not get the DHCP sequence restarted",
     "v0.8.7.2's whole subject. The adapter comes up holding the "
     "compile-time fallback -- 10.0.2.15 behind a gateway at 10.0.2.2, "
     "QEMU's network and nobody else's -- with ifconfig reporting UP "
     "over the top of it",
     """    if(g_last.nics > 0) net_dhcp_retry_reset();""",
     """    /* no address asked for */"""),

    ("the change is counted but the rescan is not run",
     "the counters move, `usb` reports the arrival, and the device is "
     "not in any table. A screen that says something happened when "
     "nothing did is worse than silence",
     """    g_rescans++;
    usb_rescan_all();""",
     """    g_rescans++;"""),
]

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """uint32_t usb_hotplug_changes(void){ return g_changes; }""",
    """/* port changes observed since boot */
uint32_t usb_hotplug_changes(void){ return g_changes; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "hotplug", SRC, TEST, CC, BUGS, HARMLESS, timeout=60, checks=CHECKS,
        preamble="Proving the hot-plug section of tests/test_kpoll.c can "
                 "fail.\nusb/hotplug.c is a bitmap and a seven-call "
                 "sequence whose order IS the content."))
