#!/usr/bin/env python3
"""check_msc_suite.py -- prove tests/test_msc.c can fail.

tests/test_msc.c drives a simulated stick, and a simulator is kinder than
hardware every single time this project has used one:

  0.8.5   the simulated HID device always had a report ready, so an
          endpoint could never NAK -- which was the ENTIRE mechanism of
          the bug that made the keyboard work at one character per
          second. The suites were green and the harness said 23 of 23.
  0.8.5.1 the same simulator stalled CLEAR_FEATURE(ENDPOINT_HALT), so
          usb_clear_halt() returned early and a missing reset hid behind
          a passing test.
  0.8.5.2 it had no BIOS owning the controller.
  0.8.5.3 its write-1-to-clear register cleared on any write.

So the defects below are planted in usb/msc.c and the suite has to
notice. The ones that matter most are the recovery paths, because a
protocol with a status phase has a way to desynchronise and a driver one
command out of step reports the PREVIOUS command's outcome -- which on a
write is the difference between "it worked" and data somewhere else.

Run:  python3 tools/check_msc_suite.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "usb/msc.c"
TEST = "tests/test_msc.c"
CC = ["gcc", "-std=gnu99", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BUGS = [
    # ── the desynchronisation paths, which are the substance ─────────
    ("the CSW tag is not checked",
     "a status wrapper for a DIFFERENT command is believed. Every answer "
     "from then on is one command out of step, and on a write that is "
     "the difference between 'it worked' and data somewhere else",
     """    if (ld32le(g_csw + 4) != tag) {""",
     """    if (0) {"""),

    ("the tag is not incremented",
     "every command carries the same tag, so a stale CSW matches and the "
     "check that exists to catch a desync cannot fire",
     """    if (++m->tag == 0) m->tag = 1;
    tag = m->tag;""",
     """    tag = m->tag;"""),

    ("the CSW signature is not checked",
     "a data packet arriving where a status wrapper belongs is read as "
     "one: byte 12 of somebody's file becomes the command's status",
     """    if (ld32le(g_csw + 0) != CSW_SIG) {""",
     """    if (0) {"""),

    ("a stalled data phase ends the command",
     "THE PATH THAT MATTERS MOST. A device refusing a command stalls the "
     "data endpoint and still has a CSW for you; returning here leaves "
     "that wrapper in the pipe for the NEXT command to read as its own",
     """        if (r == USB_ESTALL) {
            /* THE PATH THAT MATTERS. A device refusing a command stalls
               the data endpoint and then still has a CSW for you. Clear
               the halt and carry on to phase 3; treating this as the end
               is what leaves the status wrapper in the pipe for the next
               command to read as its own. */
            usb_clear_halt(&m->dev, in ? m->ep_in : m->ep_out, in);
        } else if (r < 0) {
            msc_reset(m);
            return r;
        }""",
     """        if (r < 0) {
            return r;
        }"""),

    ("the halt is not cleared after a stalled data phase",
     "the endpoint stays halted, so every later transfer fails for a "
     "reason that has nothing to do with the device -- and the CSW read "
     "that follows fails too",
     """            usb_clear_halt(&m->dev, in ? m->ep_in : m->ep_out, in);""",
     """            (void)in;"""),

    ("a stalled status phase is not retried",
     "the specification's own two-attempt rule. One stall on the status "
     "endpoint ends the command and leaves the device mid-operation",
     """    if (got == USB_ESTALL) {
        /* The specification's own two-attempt rule: a stalled status
           phase is cleared and the CSW requested once more. Exactly
           once -- a loop here would spin on a device that stalls
           everything. */
        usb_clear_halt(&m->dev, m->ep_in, 1);
        memset(g_csw, 0, CSW_LEN);
        got = usb_bulk(&m->dev, m->ep_in, 1, g_csw, CSW_LEN, m->mps_in);
    }""",
     """    if (got == USB_ESTALL) { }"""),

    ("the status phase is retried for ever",
     "a device that stalls everything spins the kernel. The bound is "
     "what makes a broken stick a failed command rather than a hang",
     """        usb_clear_halt(&m->dev, m->ep_in, 1);
        memset(g_csw, 0, CSW_LEN);
        got = usb_bulk(&m->dev, m->ep_in, 1, g_csw, CSW_LEN, m->mps_in);
    }""",
     """        while (got == USB_ESTALL) {
            usb_clear_halt(&m->dev, m->ep_in, 1);
            memset(g_csw, 0, CSW_LEN);
            got = usb_bulk(&m->dev, m->ep_in, 1, g_csw, CSW_LEN, m->mps_in);
        }
    }"""),

    ("a short CSW is accepted",
     "thirteen bytes is the whole wrapper; twelve means the status byte "
     "is whatever was in the buffer before",
     """    if (got != CSW_LEN) { msc_reset(m); return USB_EXFER; }""",
     """    if (got < 1) { msc_reset(m); return USB_EXFER; }"""),

    ("a phase error is not a reset",
     "the device is saying the phases did not line up. There is nothing "
     "to recover and the class says to reset; carrying on is carrying on "
     "from a state neither end agrees about",
     """    if (g_csw[12] == CSW_PHASE) {""",
     """    if (0) {"""),

    ("the reset does not clear the halts",
     "Mass Storage Reset tells the DEVICE to abandon the command; the "
     "halts are the host side of the same thing. One without the other "
     "leaves half the pipe thinking the command is still live",
     """    usb_clear_halt(&m->dev, m->ep_in,  1);
    usb_clear_halt(&m->dev, m->ep_out, 0);""",
     """    (void)0;"""),

    # ── the data phase ───────────────────────────────────────────────
    ("the data phase uses the IN endpoint in both directions",
     "a write goes out the endpoint that only reads. This is a bug this "
     "driver was written with and caught by this suite on its first run",
     """        r = usb_bulk(&m->dev, in ? m->ep_in : m->ep_out, in, data, dlen,
                     in ? m->mps_in : m->mps_out);""",
     """        r = usb_bulk(&m->dev, m->ep_in, in, data, dlen,
                     in ? m->mps_in : m->mps_out);"""),

    ("the CBW direction flag is inverted",
     "the device is told host-to-device for a read. A stick obeys the "
     "wrapper, not the endpoint, so it waits for data that never comes",
     """    g_cbw[12] = in ? 0x80 : 0x00;""",
     """    g_cbw[12] = in ? 0x00 : 0x80;"""),

    ("the CBW's transfer length is always zero",
     "the device is told to expect no data and returns none, so a read "
     "succeeds with the buffer untouched: the caller gets whatever was "
     "in it before",
     """    st32le(g_cbw + 8,  (uint32_t)(dlen < 0 ? 0 : dlen));""",
     """    st32le(g_cbw + 8,  0);"""),

    ("the command block length is not sent",
     "bCBWCBLength is how the device knows how much of the 16-byte "
     "field is a command. Zero means it reads none of it",
     """    g_cbw[14] = (uint8_t)cmdlen;""",
     """    g_cbw[14] = 0;"""),

    # ── SCSI ─────────────────────────────────────────────────────────
    ("READ CAPACITY is parsed little-endian",
     "SCSI is big-endian on the wire and everything else in this file is "
     "not. A 512 GB stick reports as something else entirely, and the "
     "mistake only shows up past the first few sectors",
     """    { uint32_t last = ld32be(d + 0);
      uint32_t bs   = ld32be(d + 4);""",
     """    { uint32_t last = ld32le(d + 0);
      uint32_t bs   = ld32le(d + 4);"""),

    ("READ CAPACITY's last-LBA is used as a count",
     "off by one at the end of every disk: a driver that reads one "
     "sector past the end, which on a stick is a stall",
     """      m->blocks = last + 1u;           /* LAST lba, so one more */""",
     """      m->blocks = last;"""),

    ("the LBA in READ(10) is written little-endian",
     "the command block is big-endian too. Sector 1 becomes sector "
     "16777216, which on a small stick is out of range and on a large "
     "one is somebody's data",
     """    st32be(cmd + 2, lba);""",
     """    st32le(cmd + 2, lba);"""),

    ("TEST UNIT READY is asked once",
     "a stick almost always refuses its FIRST command with UNIT "
     "ATTENTION. Believing the first answer reports a working stick as "
     "not present",
     """    for (i = 0; i < 10; i++) {""",
     """    for (i = 0; i < 1; i++) {"""),

    ("the sense data is never read after a refusal",
     "the device keeps reporting the same condition for every later "
     "command, so one refusal turns into a device that refuses "
     "everything",
     """        { uint8_t key = 0, asc = 0, ascq = 0;
          scsi_request_sense(m, &key, &asc, &ascq);
          if (key == 0x02 && asc == 0x3A) {""",
     """        { uint8_t key = 0, asc = 0, ascq = 0;
          if (key == 0x02 && asc == 0x3A) {"""),

    ("a device that never becomes ready is waited on for ever",
     "the boot stops on a broken stick",
     """    for (i = 0; i < 10; i++) {
        memset(cmd, 0, sizeof cmd);
        cmd[0] = SCSI_TEST_UNIT_READY;""",
     """    for (i = 0; ; i++) {
        memset(cmd, 0, sizeof cmd);
        cmd[0] = SCSI_TEST_UNIT_READY;"""),

    ("an empty card-reader slot is retried ten times",
     "NOT READY / medium not present is a slot with nothing in it, not a "
     "device waking up. Retrying it adds a second to every boot with a "
     "card reader attached",
     """              return USB_ENODEV;""",
     """              (void)0;"""),

    ("MODE SENSE failing makes the device read-only",
     "plenty of sticks do not implement it. Treating a missing optional "
     "command as a fault makes working hardware unwritable",
     """    if (r == CSW_PASS && (d[2] & 0x80)) {""",
     """    if (r != CSW_PASS || (d[2] & 0x80)) {"""),

    ("the write-protect bit is ignored",
     "a stick with its tab across is offered as an install target, and "
     "the installer finds out part-way through",
     """    if (r == CSW_PASS && (d[2] & 0x80)) {
        m->readonly = 1;""",
     """    if (r == CSW_PASS && (d[2] & 0x80)) {
        m->readonly = 0;"""),

    # ── block size ───────────────────────────────────────────────────
    ("a non-512 block size is writable",
     "read-modify-write on REMOVABLE media: a torn 4096-byte block loses "
     "seven sectors nobody asked to write. drivers/nvme.c does it "
     "because an NVMe drive cannot be pulled out of the machine",
     """      if (bs != 512u) m->readonly = 1;""",
     """      if (bs != 512u) m->readonly = 0;"""),

    ("a block size that is not a multiple of 512 is accepted",
     "520-byte blocks cannot be addressed in 512-byte sectors at all, "
     "and the arithmetic below silently reads the wrong offsets",
     """      if (bs % 512u) {""",
     """      if (0) {"""),

    ("a block size past the bounce buffer is accepted",
     "an 8192-byte read into a 4096-byte static buffer",
     """      if (bs == 0 || bs > MSC_BLOCK_MAX) {""",
     """      if (bs == 0) {"""),

    ("the sector count is the block count",
     "a 2048-byte-block device reports a quarter of its size, so the "
     "filesystem refuses the second half of it",
     """      m->sectors = (uint64_t)m->blocks * (uint64_t)m->per;""",
     """      m->sectors = (uint64_t)m->blocks;"""),

    ("the offset into a device block is not applied",
     "every 512-byte sector in a 4096-byte block returns the FIRST 512 "
     "bytes of it, so seven sectors out of eight are the wrong data and "
     "every one of them looks like a successful read",
     """      uint32_t off = (uint32_t)(lba & (uint64_t)(m->per - 1u)) * 512u;""",
     """      uint32_t off = 0;"""),

    ("an LBA past the end is sent to the device",
     "a READ(10) past the end is a stall on real hardware, and a "
     "recovery the host did not need to pay for",
     """    if (lba >= m->sectors) return 0;

    if (m->per == 1)""",
     """    if (m->per == 1)"""),

    # ── probing ──────────────────────────────────────────────────────
    ("a non-Bulk-Only protocol is driven anyway",
     "CBI and the control/bulk variants need a different transport "
     "entirely; driving them with CBWs talks nonsense at them",
     """        if (d->if_protocol != MSC_PROTO_BOT) {""",
     """        if (0) {"""),

    ("any interface class is treated as storage",
     "a keyboard gets a Command Block Wrapper sent to it",
     """        if (d->if_class != MSC_CLASS) continue;""",
     """        if (0) continue;"""),

    ("one bulk endpoint is enough",
     "the early refusal include/usb.h describes. Without both, the "
     "failure arrives part-way through a transfer on a device that was "
     "never going to work",
     """        if (!ei || !eo) {""",
     """        if (!ei && !eo) {"""),

    ("GET_MAX_LUN stalling is treated as a failure",
     "a stall there means 'one LUN' and is legal. Refusing the device "
     "for it rejects working hardware",
     """          m->maxlun = (r == 1) ? ml : 0;""",
     """          m->maxlun = (r == 1) ? ml : (uint8_t)(r & 0xFF);
          if (r != 1) continue;"""),

    ("a rescan appends instead of replacing",
     "each device holds a COPY of its usb_dev_t, so the old entries "
     "address whatever moved into their slots after an enumeration",
     """    g_nmsc  = 0;
    g_probed = 1;
    memset(g_msc, 0, sizeof g_msc);""",
     """    g_probed = 1;"""),

    ("an invalid device-table entry is probed",
     "a slot the core left behind is read as a device",
     """        if (!d || !d->valid) continue;""",
     """        if (!d) continue;"""),

    ("the INQUIRY strings are not trimmed",
     "SCSI pads with spaces and a screen should not. Cosmetic, and it is "
     "the name a person recognises their own stick by",
     """    for (i = 7;  i >= 0 && m->vendor[i]  == ' '; i--) m->vendor[i]  = 0;""",
     """    (void)0;"""),

    ("a CD-ROM is offered as writable",
     "peripheral device type 5 is an optical drive. It is a legal mass "
     "storage device and not an install target",
     """      if (type == 0x05) m->readonly = 1; }""",
     """      if (type == 0x05) m->readonly = 0; }"""),

    ("INQUIRY's removable bit is dropped",
     "removable is what keeps a stick behind the fixed disks in "
     "fs/blkdev.c's registration order, and that order is what stops a "
     "stick renumbering somebody's Windows disk",
     """    m->removable = (d[1] & 0x80) ? 1 : 0;""",
     """    m->removable = 0;"""),

    ("a write to a read-only device is attempted",
     "the driver's own refusal, above the one in fs/blkdev.c. Both exist "
     "because 'by construction' is what the three bus switches this "
     "release deleted were also true by",
     """    if (!m->valid || m->readonly) return 0;""",
     """    if (!m->valid) return 0;"""),

    ("a null buffer reaches the transfer",
     "the bounds check at the top is the only thing between a caller's "
     "mistake and a memcpy from zero",
     """    if (i < 0 || i >= g_nmsc || !buf) return 0;
    m = &g_msc[i];
    if (!m->valid) return 0;
    if (lba >= m->sectors) return 0;""",
     """    if (i < 0 || i >= g_nmsc) return 0;
    m = &g_msc[i];
    if (!m->valid) return 0;
    if (lba >= m->sectors) return 0;"""),
]

HARMLESS = (
    "a comment is added (must not be caught)",
    "nothing about the code changed, so a catch here means the loop is "
    "restoring badly or the suite is checking text",
    """int msc_rescan(void){ return msc_probe(); }""",
    """/* Re-probe after an enumeration; see the note on msc_probe(). */
int msc_rescan(void){ return msc_probe(); }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "msc", SRC, TEST, CC, BUGS, HARMLESS, timeout=60,
        preamble="Proving tests/test_msc.c can fail.\n"
                 "Every time this project has used a simulator, the "
                 "simulator was kinder than the hardware."))
