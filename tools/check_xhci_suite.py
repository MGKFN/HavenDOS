#!/usr/bin/env python3
"""check_xhci_suite.py -- prove tests/test_xhci.c can fail.

xHCI is the most intricate driver in this tree and the one the test
laptop cannot work without: that machine has no UHCI, so input,
removable storage and its only route to a network all sit behind this
one controller. It is also a driver whose bugs are quiet. A cycle bit
that never toggles does not crash; it stops the controller dead at the
wrap point, after exactly one ring's worth of work, which looks like a
device that stopped answering. A context read at the wrong offset does
not fault; it makes the controller refuse commands with a parameter
error and say nothing else.

So the bugs below are the ones a real implementation ships, and the
suite has to notice every one of them.

Run:  python3 tools/check_xhci_suite.py
"""
import os
import subprocess
import sys
import tempfile

SRC = "usb/xhci_hc.c"
TEST = "tests/test_xhci.c"
CC = ["gcc", "-std=gnu99", "-no-pie", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

BAR = "drivers/pci_bar.c"
# ── v0.8.7: usb/core.c is injected into from here too ─────────────────
# Alternate-setting support is half in the driver and half in the core,
# and tests/test_xhci.c is the suite that drives both -- it is the only
# one with a controller that executes commands and a device that answers
# SET_INTERFACE. check_hub_suite.py also injects into usb/core.c, for
# the route arithmetic, and is caught by tests/test_hub.c; these defects
# are not reachable from that suite.
CORE = "usb/core.c"
LINK = [TEST, BAR]

# v0.8.5.1 -- the harness now injects into TWO files. The bug that stopped
# the HP from booting USB was not in the driver at all; it was in
# pci_bar_mem(), which truncated a 64-bit BAR and handed the driver an
# address 384 GiB away from the controller. A harness that can only
# damage xhci_hc.c could never have planted it. Each entry below names
# the file it belongs in.

BUGS = [
    # ── bring-up ─────────────────────────────────────────────────────
    ("the context size is assumed to be 32 bytes",
     "every field of every context is then at the wrong offset",
     "    x->ctxsz    = (hcc1 & (1u << 2)) ? 64 : 32;   /* CSZ -- trap 1 */",
     "    x->ctxsz    = 32;"),

    ("the context size is assumed to be 64 bytes",
     "the same mistake the other way, and just as silent",
     "    x->ctxsz    = (hcc1 & (1u << 2)) ? 64 : 32;   /* CSZ -- trap 1 */",
     "    x->ctxsz    = 64;"),

    ("CNR is not waited for after the reset",
     "every register written while it is set is dropped",
     "    if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_CNR) {\n"
     "        *why = \"controller-not-ready never cleared after reset\";\n"
     "        return USB_ETIMEOUT;\n"
     "    }",
     "    ;"),

    ("the scratchpad buffers the controller asked for are not given",
     "a controller that demanded them and did not get them does not start",
     "        g_dcbaa[hcno][0] = (uint64_t)pa(g_spadar[hcno]);",
     "        ;"),

    ("MaxSlotsEn is never written",
     "every Enable Slot is then refused, and nothing says why",
     "    mmio_write32(x->op, XOP_CONFIG, x->maxslots);",
     "    ;"),

    ("the run bit is never set",
     "the controller is configured perfectly and halted",
     "    mmio_write32(x->op, XOP_USBCMD,\n"
     "                 mmio_read32(x->op, XOP_USBCMD) | USBCMD_RS);",
     "    ;"),

    ("the ports are never powered",
     "a port with PP clear reports nothing connected, whatever is in it",
     "        if (!(v & PORTSC_PP)) portsc_write(x, (int)i, PORTSC_PP);",
     "        ;"),

    ("ERSTBA is written before ERDP",
     "the controller starts posting before software has a dequeue pointer",
     "    mmio_write32(x->rt, XRT_ERDP(0),     pa(g_evtr[hcno]) | ERDP_EHB);\n"
     "    mmio_write32(x->rt, XRT_ERDP(0) + 4, 0);\n"
     "    mmio_write32(x->rt, XRT_ERSTBA(0),     pa(g_erst[hcno]));",
     "    mmio_write32(x->rt, XRT_ERSTBA(0),     pa(g_erst[hcno]));\n"
     "    mmio_write32(x->rt, XRT_ERDP(0),     pa(g_evtr[hcno]) | ERDP_EHB);\n"
     "    mmio_write32(x->rt, XRT_ERDP(0) + 4, 0);"),

    # ── rings and the cycle bit ──────────────────────────────────────
    # NOT injected, and worth writing down rather than reporting a false
    # MISS. Removing TC from the Link TRB that ring_init() lays down is
    # not detectable, and the reason is that it is genuinely redundant:
    # ring_push() and cmd_push() both REWRITE that TRB's control word
    # with LINK | TC | cycle at the moment the ring wraps, which is the
    # first moment the controller can follow it. The TC set at init has
    # never been read by anything. The line stays -- a ring that is
    # correct before anybody pushes to it costs nothing -- but it is
    # recorded here as untested rather than pretended to be tested.
    # Same reasoning as the redundant guards recorded in
    # tools/check_esp_suite.py.

    ("software never toggles its cycle at a transfer ring wrap",
     "the controller stops at the wrap, having done one ring of work",
     "        *idx = 0;\n        *cycle ^= 1;",
     "        *idx = 0;"),

    ("the Link TRB is not given the current cycle",
     "the controller will not follow a link it does not own",
     "        lnk->control = TRB_TYPE(TRB_LINK) | TRB_TC | (*cycle ? TRB_C : 0);",
     "        lnk->control = TRB_TYPE(TRB_LINK) | TRB_TC;"),

    ("the command ring never toggles its cycle",
     "the same bug on the ring every command goes through",
     "        x->cmd_idx = 0;\n        x->cmd_cycle ^= 1;",
     "        x->cmd_idx = 0;"),

    ("the event ring cycle never flips",
     "after one lap round the ring every event looks stale and is ignored",
     "            x->evt_idx = 0;\n            x->evt_cycle ^= 1;",
     "            x->evt_idx = 0;"),

    ("ERDP is written back without the handler-busy bit",
     "the controller believes the ring is still full and stops posting",
     "        mmio_write32(x->rt, XRT_ERDP(0), pa(&r[x->evt_idx]) | ERDP_EHB);",
     "        mmio_write32(x->rt, XRT_ERDP(0), pa(&r[x->evt_idx]));"),

    # ── slots, contexts and addressing ───────────────────────────────
    ("the slot context is not added to the input context",
     "Address Device with no A0 flag is refused with a parameter error",
     "    ctx_set32(ic, 1, 0x3u);                       /* A0 | A1            */",
     "    ctx_set32(ic, 1, 0x2u);"),

    ("the endpoint 0 ring is given a dequeue cycle state of zero",
     "the controller owns the ring and software does not, so nothing runs",
     "    ctx_set32(iec, 2, pa(g_epr[hcno][slot][ri]) | 1u);   /* DCS = 1    */",
     "    ctx_set32(iec, 2, pa(g_epr[hcno][slot][ri]));"),

    ("endpoint 0's packet size is never corrected",
     "a full-speed device whose ep0 is 8 bytes is talked to in 64-byte "
     "packets",
     "        if (real && real != out->maxpacket0) {",
     "        if (0) {"),

    ("the doorbell is rung with the endpoint number, not the DCI",
     "endpoint 1 IN is DCI 3; ringing doorbell 1 rings endpoint 0",
     "    ring_doorbell(x, (uint32_t)slot, (uint32_t)dci);",
     "    ring_doorbell(x, (uint32_t)slot, (uint32_t)(ep & 0x0F));"),

    ("DCI is computed without the direction bit",
     "an IN and an OUT endpoint of the same number collide on one ring",
     "    return (int)(((uint32_t)(ep & 0x0F) * 2u) + (in ? 1u : 0u));",
     "    return (int)((uint32_t)(ep & 0x0F) * 2u);"),

    ("Configure Endpoint does not raise Context Entries",
     "the command is refused because the slot context still says one entry",
     "                          ((uint32_t)maxdci << 27));",
     "                          (1u << 27));"),

    # v0.8.7 rewrote x_configure() to send drop flags and to free the
    # rings it dropped, so the tail of the function moved and this
    # injection stopped matching. A stale injection reports nothing and
    # is therefore worse than a missing one -- check_nvmerw_suite.py had
    # not built for five releases and said so to nobody.
    ("Configure Endpoint is never issued",
     "every endpoint beyond the default pipe stays non-existent",
     "    r = cmd_run(x, hcno, pa(g_inctx[hcno]), 0, 0,\n"
     "                TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)slot << 24), 0);",
     "    r = USB_OK;"),

    ("bus mastering is never enabled",
     "the controller accepts register writes and then never completes "
     "a command",
     "        pci_enable(p, 1);",
     "        pci_enable(p, 0);"),

    ("a failed command is reported as success",
     "the driver carries on with a device the controller refused",
     "    if (cc != CC_SUCCESS) return USB_ECMD;",
     "    ;"),

    ("PORTSC is written back with every bit it was read with",
     "PED is write-1-to-DISABLE, so tidying the port turns it off",
     "    v &= ~PORTSC_RO_MASK;",
     "    ;"),
    # ══ v0.8.5.1: the BAR, which is where the real bug was ═══════════
    # Every one of these is a bug that shipped in v0.8.5 or is one line
    # away from it. The first is the actual defect that stopped the HP
    # from bringing up USB.
    (BAR, "a 64-bit BAR's high half is ignored, and the low half returned",
     "THE v0.8.5 BUG: the xHCI is at 0x60_02120000 and the driver was "
     "handed 0x02120000, so it read a capability register out of "
     "somebody else's memory and refused to start",
     """    if (((b >> 1) & 3u) == 2u) {            /* 64-bit: high half follows */
        if (n >= 5) return 0;               /* no register left to hold it */
        if (hi_out) *hi_out = d->bar[n + 1];
    }""",
     "    ;"),

    (BAR, "pci_bar_mem() truncates a BAR above 4 GB instead of refusing",
     "the caller gets a plausible-looking address that is 384 GiB wrong; "
     "returning 0 is what lets a driver say so",
     "    if (hi) return 0;",
     "    if (0) return 0;"),

    (BAR, "the high dword is read from this BAR instead of the next one",
     "an off-by-one in the one place the layout matters: bar[n] is the low "
     "half, bar[n+1] is the high half",
     "        if (hi_out) *hi_out = d->bar[n + 1];",
     "        if (hi_out) *hi_out = d->bar[n];"),

    (BAR, "the BAR's low four flag bits are not masked off",
     "the type and prefetch bits are left in the address, so every "
     "register is read 4 bytes past where it is",
     "    return b & ~0xFu;\n}\n\nint pci_bar_is_64",
     "    return b;\n}\n\nint pci_bar_is_64"),

    (BAR, "the 64-bit type is tested as 01b rather than 10b",
     "a 64-bit BAR is then treated as 32-bit -- which is the v0.8.5 "
     "behaviour arrived at a different way",
     "    return (((d->bar[n] >> 1) & 3u) == 2u);",
     "    return (((d->bar[n] >> 1) & 3u) == 1u);"),

    (BAR, "a 64-bit BAR in the last slot is accepted",
     "there is no bar[6] to hold its high half, so this reads past the "
     "end of the array and calls the result an address",
     "        if (n >= 5) return 0;               /* no register left to hold it */",
     "        ;"),

    ("the window above 4 GB is mapped cacheable",
     "highfb_map() is write-through: READS of a device register may come "
     "from cache, so a driver polling USBSTS for the reset to finish can "
     "read one stale dword forever and look like it hung",
     "            if (!highfb_map_io(bar_hi, bar_lo, XHCI_MMIO_BYTES, &virt)) {",
     "            if (!highfb_map(bar_hi, bar_lo, XHCI_MMIO_BYTES, &virt)) {"),

    ("a failed mapping is ignored and the truncated address used anyway",
     "this is the dangerous recovery: it turns a clean refusal into "
     "register writes at an address that belongs to something else",
     """                klog(KLOG_WARN, "xHCI %2x:%2x.%u is at %8x%8x, above 4 GB, "
                                "and could not be mapped",
                     (uint32_t)p->bus, (uint32_t)p->dev, (uint32_t)p->fn,
                     bar_hi, bar_lo);
                continue;""",
     "                ;"),

    ("the window is mapped and then the physical address is used",
     "the mapping succeeds and is thrown away, which is the same wild "
     "access as not mapping at all",
     "            bar = virt;",
     "            bar = bar_lo;"),

    ("only one page of the BAR is mapped",
     "the capability and operational registers are reachable and the "
     "doorbell array is not, so the controller comes up and never "
     "notices a single transfer",
     "#define XHCI_MMIO_BYTES  0x10000u",
     "#define XHCI_MMIO_BYTES  0x1000u"),

    # These two were added WITHOUT writing a test for them first, to find
    # out whether the suite above was actually strong or merely aimed at
    # the bug already known. See the changelog.
    ("a BAR whose low half is exactly zero is skipped",
     "a controller at 0x60_00000000 is legal and has bar[0] == 0 after "
     "masking; testing the low half alone discards it",
     "        if (!bar_lo && !bar_hi) continue;",
     "        if (!bar_lo) continue;"),

    ("the high half is tested for truth instead of the 64-bit type",
     "a 32-bit BAR sitting immediately below a second BAR whose value "
     "happens to be non-zero is then mapped as if it were 64-bit",
     "        if (bar_hi) {",
     "        if (p->bar[1]) {"),
    # ══ v0.8.5.1: polling a keyboard without blocking ════════════════
    ("the back-end offers no non-blocking poll",
     "THE v0.8.5 BEHAVIOUR: usb_intr_poll() falls back to the blocking "
     "transfer, which a NAKing keyboard never completes -- one character "
     "per second on a laptop whose only keyboard is USB",
     "    .intr_poll      = x_intr_poll,\n",
     ""),

    ("a transfer is queued on every poll rather than one at a time",
     "the ring fills with live TRBs in sixteen polls and the controller "
     "starts completing transfers whose callers are long gone",
     "    if (!x->ep_busy[slot][ri]) intr_arm(x, hcno, slot, ri, len);",
     "    intr_arm(x, hcno, slot, ri, len);"),

    ("the outstanding-transfer flag is never set",
     "same thing by another route: nothing records that a transfer is "
     "already queued",
     "    x->ep_busy[slot][ri] = 1;\n}",
     "}"),

    ("the next transfer is not queued after one completes",
     "the first keypress is read and the keyboard then goes silent for "
     "good, because nothing is ever waiting for the second",
     """        /* Re-arm immediately, whatever came back: a device that reports
           nothing is the normal case and must stay polled. */
        intr_arm(x, hcno, slot, ri, len);""",
     "        ;"),

    # ── v0.8.7.5: one owner for the event ring ───────────────────────
    ("an event nobody is waiting for is filed anyway",
     "the whole 0.8.7.4 defect: a timed-out transfer's event is handed "
     "to the next caller, on another endpoint and another device, as "
     "its own result -- six descriptor reads, six \"(3)\"",
     "        if (!x->ep_busy[slot][ri] && !x->ep_sync[slot][ri]) continue;",
     ""),

    ("a command completion met by the pump is discarded",
     "cmd_run() then waits for a result that has already been thrown "
     "away, which is how the old evt_wait() lost events of the wrong "
     "type",
     """        if (type == TRB_CMD_COMPLETE) {
            x->cmd_status  = st;
            x->cmd_param   = lo;
            x->cmd_control = c;
            x->cmd_done    = 1;
            continue;
        }""",
     "        if (type == TRB_CMD_COMPLETE) continue;"),

    ("an event is judged before it is consumed",
     "an event that is neither a transfer nor a command -- a port change "
     "-- then sits at the head of the ring without ever being consumed, "
     "and blocks every other consumer on the controller for good. This "
     "is the old evt_drain() shape, restored.",
     "        if (((c & TRB_C) ? 1 : 0) != x->evt_cycle) break;    /* ring empty */",
     "        if (((c & TRB_C) ? 1 : 0) != x->evt_cycle) break;\n"
     "        if ((int)TRB_TYPE_OF(c) != TRB_XFER_EVENT &&\n"
     "            (int)TRB_TYPE_OF(c) != TRB_CMD_COMPLETE) return took;"),

    ("the synchronous caller never claims its endpoint",
     "its own completion is then unowned and dropped, so every control "
     "transfer times out",
     "    x->ep_sync[slot][ri] = 1;\n    ring_doorbell(x, (uint32_t)slot, 1);",
     "    ring_doorbell(x, (uint32_t)slot, 1);"),

    ("a timed-out transfer keeps its claim on the endpoint",
     "the event that arrives later is filed against a waiter that has "
     "gone, and the next caller on that endpoint reads it",
     "    x->ep_sync[slot][ri] = 0;      /* whatever arrives now is nobody's */\n"
     "    ep_stop(x, hcno, slot, ri);",
     "    ep_stop(x, hcno, slot, ri);"),

    ("a timed-out endpoint is never stopped",
     "the controller goes on retrying a transfer the caller has already "
     "been told failed, holding its buffer",
     "    ep_stop(x, hcno, slot, ri);\n    return USB_ETIMEOUT;",
     "    return USB_ETIMEOUT;"),

    ("a stopped endpoint is not told where to resume",
     "it stays Stopped with its dequeue pointer parked on a TRB nobody "
     "will ever collect, so the endpoint never works again",
     """    deq = pa(&r[x->ep_idx[slot][ri]]) | (x->ep_cycle[slot][ri] ? 1u : 0u);
    cmd_run(x, hcno, deq, 0, 0,
            TRB_TYPE(TRB_SET_TR_DEQ) | ((uint32_t)slot << 24) | (dci << 16),
            0);""",
     "    (void)deq; (void)r;"),

    ("Enable Slot reads its slot id off the ring instead of the completion",
     "the pump drains the whole ring, so the entry behind the cursor can "
     "be any event at all",
     "    slot = (int)((x->cmd_control >> 24) & 0xFFu);",
     "    slot = (int)((g_evtr[hcno][(x->evt_idx == 0) ? (XHCI_EVT_TRBS - 1)\n"
     "                              : (x->evt_idx - 1)].control >> 24) & 0xFFu);"),

    ("the completion flag is not cleared when the data is taken",
     "every subsequent poll returns the same stale report, so one "
     "keypress repeats forever",
     """        x->ep_done[slot][ri] = 0;

        if (cc == 6) return USB_ESTALL;""",
     "        if (cc == 6) return USB_ESTALL;"),

    ("a stall re-arms the halted endpoint instead of reporting it",
     "the transfer is queued onto an endpoint the controller has halted, "
     "where it sits until something resets it -- and the caller never "
     "learns there was a stall to clear",
     """        if (cc == 6) return USB_ESTALL;   /* the core clears the halt, and
                                             reset_toggle() re-arms us */""",
     "        if (cc == 6) { intr_arm(x, hcno, slot, ri, len); return 0; }"),

    ("resetting an endpoint leaves it marked busy",
     "the reset discarded the queued transfer, so the endpoint waits "
     "forever for a completion that cannot come and the device goes "
     "quiet after its first stall",
     """        if (ri >= 0) {
            x->ep_busy[slot][ri] = 0;
            x->ep_done[slot][ri] = 0;
        }""",
     "        (void)ri;"),

    ("the asynchronous transfer shares the synchronous bounce buffer",
     "a transfer the controller has not finished with still owns its "
     "buffer; the next transfer overwrites it underneath",
     "              pa(g_abuf[hcno][slot][ri]), 0, (uint32_t)len,",
     "              pa(g_bounce[hcno]), 0, (uint32_t)len,"),
    # ══ v0.8.5.2: taking the controller from the firmware ════════════
    ("the controller is never taken from the BIOS",
     "THE v0.8.5.1 BUG: SMM keeps its USB keyboard emulation running "
     "while this driver also drives the controller, so every keystroke "
     "arrives twice -- once at port 0x60 and once through HID -- and the "
     "installer cannot be driven",
     "    xhci_take_from_bios(x, bar, hcc1);",
     "    ;"),

    ("the handoff is performed AFTER the controller is reset",
     "resetting a controller SMM still owns is the race the handoff "
     "exists to avoid; the firmware is driving hardware that vanished "
     "underneath it",
     """    xhci_take_from_bios(x, bar, hcc1);

    /* Stop it, then reset it, then WAIT FOR CNR -- trap 4. */
    mmio_write32(x->op, XOP_USBCMD,
                 mmio_read32(x->op, XOP_USBCMD) & ~USBCMD_RS);
    for (i = 0; i < 1000; i++) {
        if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_HCH) break;
        sleep_ms(1);
    }
    mmio_write32(x->op, XOP_USBCMD, USBCMD_HCRST);""",
     """    /* Stop it, then reset it, then WAIT FOR CNR -- trap 4. */
    mmio_write32(x->op, XOP_USBCMD,
                 mmio_read32(x->op, XOP_USBCMD) & ~USBCMD_RS);
    for (i = 0; i < 1000; i++) {
        if (mmio_read32(x->op, XOP_USBSTS) & USBSTS_HCH) break;
        sleep_ms(1);
    }
    mmio_write32(x->op, XOP_USBCMD, USBCMD_HCRST);
    xhci_take_from_bios(x, bar, hcc1);"""),

    ("the OS-owned semaphore is never set",
     "the firmware is never actually asked for the controller, so it "
     "never gives it up",
     "        mmio_write32(bar, off, v | XLEG_OS_OWNED);",
     "        mmio_write32(bar, off, v);"),

    ("the driver does not wait for the BIOS to let go",
     "firmware does not drop its ownership bit on the cycle it is "
     "asked; a driver that reads once and gives up concludes it was "
     "refused when it was not",
     """        for (i = 0; i < 1000; i++) {
            v = mmio_read32(bar, off);
            if (!(v & XLEG_BIOS_OWNED) && (v & XLEG_OS_OWNED)) break;
            sleep_ms(1);
        }""",
     "        ;"),

    ("the firmware's SMI sources are left enabled",
     "a firmware that can still take an SMI on an ownership change or a "
     "BAR write keeps running its emulation whatever the semaphore says",
     "    v &= ~XLEG_SMI_ENABLES;",
     "    ;"),

    # NOT injected either, and for a reason worth writing down. Deleting
    # `v |= XLEG_SMI_STATUS` is not detectable, because the line is a
    # no-op given the read-modify-write around it: the driver READS
    # USBLEGCTLSTS first, so any latched status bit is already a 1 in the
    # value it writes back -- and writing a 1 to a write-1-to-clear bit
    # is exactly what clears it. A bit that reads as 0 has nothing to
    # retire. The line stays because it states the intent at the point
    # the reader needs it, but it is recorded here as untested rather
    # than pretended to be tested.

    # NOT injected, and recorded rather than reported as a false MISS.
    # Removing the `guard` from xecp_find()'s walk is not detectable,
    # because the walk cannot actually spin: a next-pointer of zero ends
    # the list, and any other value is added to `off`, so `off` strictly
    # increases and `off < XHCI_MMIO_BYTES` terminates it within 16384
    # iterations whatever the firmware put there. The guard caps that at
    # 64, which is a sane bound on a list that is realistically three
    # entries long -- but it prevents a slow walk, not a hang, and the
    # comment above it used to claim otherwise. Same reasoning as the
    # Link TRB Toggle Cycle bit above.

    ("the capability list is walked for the wrong ID",
     "legacy support is ID 1; anything else finds nothing and silently "
     "skips the handoff",
     "    uint32_t off = xecp_find(bar, hcc1, XECP_ID_LEGACY);",
     "    uint32_t off = xecp_find(bar, hcc1, 2);"),

    ("xECP is read as a byte offset rather than a dword offset",
     "both the list head and every next-pointer are counted in DWORDS; "
     "reading either as bytes lands in the middle of nothing",
     "    uint32_t off = ((hcc1 >> 16) & 0xFFFFu) * 4u;",
     "    uint32_t off = ((hcc1 >> 16) & 0xFFFFu);"),

    ("the next-pointer is not scaled to dwords",
     "the first capability on a real controller is not the legacy one, "
     "so the walk has to follow the chain correctly to find it at all",
     "        uint32_t next = ((cap >> 8) & 0xFFu) * 4u;",
     "        uint32_t next = ((cap >> 8) & 0xFFu);"),

    ("only the first capability in the list is examined",
     "a real controller's list starts with Supported Protocol entries "
     "and legacy support is further down",
     "        if (!next) return 0;\n        off += next;",
     "        return 0;"),

    # ── v0.8.6.1: the slot leak, found on the HP and not here ────────
    ("a slot is never given back",
     "THE SHIPPED BUG. TRB_DISABLE_SLOT was defined in this file and "
     "never sent, so every `usb rescan` took fresh slots and freed "
     "none. The HP has 8 slots and 3 devices: boot took 1-3, the first "
     "rescan took 4-6, the third would have found no slot at all. Every "
     "check in the suite enumerated ONCE, which is how 150 of them "
     "passed over it",
     "    .detach         = x_detach,\n",
     ""),

    ("the slot is freed locally but the controller is not told",
     "clearing slot_used[] without sending Disable Slot is the leak "
     "with the symptom hidden: this side thinks the slot is free and "
     "the controller still holds it, so Enable Slot keeps handing out "
     "new ones until there are none",
     """    cmd_run(x, hcno, 0, 0, 0,
            TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24), 0);
""",
     ""),

    ("the endpoint rings are not released with the slot",
     "ep_dci[slot][] marks a ring in use. A reused slot finds its rings "
     "already allocated, and ring_alloc() hands back one with the "
     "PREVIOUS device's cursor in it -- a transfer posted wherever the "
     "last one left off",
     """    for (i = 0; i < XHCI_EP_RINGS; i++) {
        x->ep_dci[slot][i]    = 0;""",
     """    for (i = 0; i < 0; i++) {
        x->ep_dci[slot][i]    = 0;"""),

    ("a slot this side does not hold is disabled anyway",
     "a failed attach, or a device from a previous controller init, "
     "turns into a Disable Slot for whatever now occupies that number",
     "    if (!x->slot_used[slot]) return;",
     "    if (0) return;"),

    # An injection removing x_detach()'s `slot > maxslots` bound is NOT
    # here, and the reason is worth writing down rather than leaving a
    # gap: it cannot express a reachable defect. The slot_used[] check
    # below it returns first for any slot the controller does not hold,
    # and no normal path ever marks one beyond maxslots. The bound is
    # there to keep the ARRAY access in range -- a memory-safety guard,
    # not a behaviour -- and the only injection that would exercise it
    # needs a slot past XHCI_MAX_SLOTS, which is undefined behaviour in
    # the suite rather than a result.
    #
    # An injection that cannot express a real defect is worse than no
    # injection: it reports a catch the suite did not earn, which is
    # exactly what this harness's first version did eleven times over.

    # ── SuperSpeed, v0.8.6.7 ─────────────────────────────────────────
    ("bMaxPacketSize0 is copied through at SuperSpeed",
     "at SuperSpeed that byte is an EXPONENT -- 9 means 2^9 = 512. "
     "Copied as a byte count it programs a NINE-byte control endpoint, "
     "the pipe that was working at 512 stops working, and enumeration "
     "ends in USB_EXFER. That is hc0 port 12 on the HP, every boot "
     "since v0.8.5.3, and any USB 3 stick in a socket that trains at "
     "SuperSpeed",
     """        uint16_t real = mps0_from_desc(speed, desc[7]);""",
     """        uint16_t real = desc[7];"""),

    ("the SuperSpeed default packet size is truncated to a byte",
     "512 does not fit in a uint8_t, and this field was one for four "
     "releases -- so every SuperSpeed device started with a recorded "
     "ep0 size of ZERO, which the correction path then compared "
     "against and always disagreed with",
     """    out->maxpacket0 = default_mps(speed);""",
     """    out->maxpacket0 = (uint8_t)default_mps(speed);"""),

    ("a SuperSpeed exponent of nonsense is programmed anyway",
     "1u << 200 is not a packet size. The descriptor is somebody "
     "else's firmware, and 0 means 'keep what we had' rather than "
     "'program whatever arrived'",
     """        if (b != 9) return 0;
        return 512;""",
     """        return (uint16_t)(1u << b);"""),

    # ── v0.8.7: Configure Endpoint is called TWICE now ────────────────
    #
    # Everything above this point met a driver that configured a slot
    # once, after enumeration, so every endpoint the controller had been
    # told about was one that was still wanted. SET_INTERFACE makes it
    # twice, and three things that were true by construction stop being
    # true. The suite could not see any of them until the simulated
    # controller was taught to honour the DROP flags -- which is the
    # Disable Slot situation of v0.8.6.1 verbatim, one level down.

    ("Configure Endpoint sends no DROP flags",
     "dword 0 of the input control context is D31..D2, the endpoints "
     "the controller is being told to forget. It was left at zero for "
     "four releases, correctly, because nothing configured a slot "
     "twice. A switch from one alternate setting to another then "
     "leaves the previous setting's endpoint contexts live, pointing "
     "at rings this side has re-laid underneath them",
     """    ctx_set32(in_ctrl_ctx(x, hcno), 0, drop);
""",
     """"""),

    ("a setting with no endpoints issues no command at all",
     "the early return as it stood for four releases. A switch to an "
     "alternate setting with no endpoints -- which is ALT 0 OF THE "
     "APPLE ETHERNET INTERFACE -- produced add == 1 and returned "
     "USB_OK without issuing anything, so the old endpoints stayed "
     "configured AND the kernel reported success",
     """    if (add == 1u && drop == 0u) return USB_OK;""",
     """    if (add == 1u) return USB_OK;"""),

    ("the dropped rings are never given back",
     "the v0.8.6.1 slot leak one level down. XHCI_EP_RINGS is five -- "
     "ep0 and four more -- and alt 1 and alt 2 of the phone's Ethernet "
     "interface use four different endpoint addresses between them, so "
     "two switches exhaust them and ring_alloc() starts returning -1",
     """    if (drop) {
        for (i = 0; i < XHCI_EP_RINGS; i++) {
            int dci = (int)x->ep_dci[slot][i];
            if (dci < 2 || dci > 31)          continue;
            if (!(drop & (1u << dci)))        continue;
            ring_free(x, slot, i);
        }
    }
""",
     """"""),

    ("a reused ring is resumed part-way along instead of re-laid",
     "ring_alloc() returns the EXISTING ring for a DCI the slot already "
     "holds, and x_configure() writes the TR Dequeue Pointer back to "
     "the ring BASE -- so the controller dequeues from a TRB the driver "
     "wrote on the previous setting while the driver enqueues where it "
     "left off. This function's own comment has claimed since v0.8.5.1 "
     "that a configure re-lays the rings; clearing ep_busy and ep_done "
     "is not re-laying them",
     """        ring_relay(x, hcno, slot, ri);
""",
     """"""),

    ("the default pipe is dropped along with the rest",
     "DCI 1 is endpoint 0. It belongs to the DEVICE, not to an "
     "interface, and an alternate setting never has it -- so a drop set "
     "built without excluding it takes away the control pipe every "
     "later request goes through",
     """        if (dci < 2 || dci > 31)      continue;
        if (want & (1u << dci))       continue;
        drop |= (1u << dci);""",
     """        if (dci < 1 || dci > 31)      continue;
        if (want & (1u << dci))       continue;
        drop |= (1u << dci);"""),

    ("the drop set is every DCI the new setting lacks",
     "rather than every ring this slot actually HOLDS. Dropping an "
     "endpoint the controller was never told about is a command it is "
     "entitled to refuse, and a refused Configure Endpoint leaves the "
     "device with no endpoints at all",
     """    for (i = 0; i < XHCI_EP_RINGS; i++) {
        int dci = (int)x->ep_dci[slot][i];
        if (dci < 2 || dci > 31)      continue;
        if (want & (1u << dci))       continue;
        drop |= (1u << dci);
    }""",
     """    for (i = 2; i < 32; i++) {
        if (want & (1u << i))         continue;
        drop |= (1u << i);
    }"""),

    # ── NOT A DEFECT, and recorded rather than deleted ───────────────
    #
    # "Context Entries is raised to 31 instead of the highest DCI in
    # use" was written as an injection and reported MISSED. It was then
    # read properly: Context Entries is the index of the LAST VALID
    # endpoint context, and a value above the highest in use makes the
    # controller consider endpoint contexts whose state is Disabled --
    # which it ignores. The command is accepted and nothing
    # misbehaves, so there is nothing for a suite to catch.
    #
    # Left here as a note because v0.8.6.3's harness proved two of its
    # ten injections "not to be defects at all", and an injection
    # deleted without a word is one somebody writes again.

    # ══ v0.8.7: the core's half of alternate settings ═════════════════
    #
    # Five-element entries, so the file is named. These are in
    # usb/core.c, and tests/test_xhci.c is the only suite that can catch
    # them: it has a controller that executes commands AND a device that
    # answers SET_INTERFACE.

    (CORE, "the walk ignores the alternate setting it was given",
     "back to `alt == 0`, which is what it did for four releases. The "
     "Apple Ethernet interface's alt 0 has NO ENDPOINTS, so this is not "
     "a narrower behaviour -- it is a device that enumerates, reports "
     "the right class triple, and has nothing to send through",
     """                if ((int)alt == want_alt && !seen_iface &&""",
     """                if (alt == 0 && !seen_iface &&"""),

    (CORE, "SET_INTERFACE is addressed to the DEVICE",
     "the recipient is bits 4:0 of bmRequestType and it has to be the "
     "INTERFACE. Real firmware stalls the device-directed form, and "
     "that stall looks exactly like a device with no alternate "
     "settings -- so the symptom sends whoever hits it reading the "
     "wrong datasheet",
     """    r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                    USB_REQ_SET_INTERFACE, alt, ifnum, 0, 0);""",
     """    r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_SET_INTERFACE, alt, ifnum, 0, 0);"""),

    (CORE, "SET_INTERFACE has wValue and wIndex the wrong way round",
     "wValue is the alternate setting and wIndex is the interface. "
     "Swapped, a request for interface 2 alt 1 asks for interface 1 alt "
     "2 -- a VALID request to the wrong place, which is the shape of "
     "the route-string bug in usb/hub.c: the symptom is a device doing "
     "something reasonable that nobody asked for",
     """                    USB_REQ_SET_INTERFACE, alt, ifnum, 0, 0);""",
     """                    USB_REQ_SET_INTERFACE, ifnum, alt, 0, 0);"""),

    (CORE, "the new setting is written to the live device before the "
           "request is sent",
     "the walk has to run first, because the endpoints are in the "
     "descriptor. If it writes straight into *d and the device then "
     "REFUSES, the kernel describes a setting the device never left and "
     "the next bulk transfer goes to an endpoint address it does not "
     "have -- on xHCI a transfer retried until it times out, on UHCI a "
     "stall, and on neither of them a message saying the interface did "
     "not switch",
     """    next = *d;
    r = fetch_config(d, d->cfgidx);""",
     """    r = fetch_config(d, d->cfgidx);"""),

    (CORE, "the controller is never told about the new endpoints",
     "on xHCI every endpoint beyond the default pipe returns USB_ENODEV "
     "until a Configure Endpoint has been accepted -- and the drop half "
     "of it is what takes the PREVIOUS setting's endpoints away",
     """    if (h->ops->configure) {
        /* On xHCI this DROPS the previous setting's endpoints as well
           as adding this one's. Dropping is new in v0.8.7 for exactly
           this call: nothing before it configured a slot twice. See
           x_configure(). */
        r = h->ops->configure(h, d);""",
     """    if (0) {
        r = h->ops->configure(h, d);"""),

    (CORE, "the configuration descriptor is re-read by VALUE, not INDEX",
     "bConfigurationValue and the descriptor index are different "
     "numbers: a device is free to number its configurations 1, 3, 4, 7 "
     "while they sit at indices 0, 1, 2, 3. The phone's configuration "
     "at index 2 has value 3, so this fetches index 3 -- a different "
     "configuration, or a stall",
     """    r = fetch_config(d, d->cfgidx);""",
     """    r = fetch_config(d, d->config);"""),

    (CORE, "usb_select_config does not bound the index",
     "d->nconfigs is what the device said it has, and an index past it "
     "is a driver bug rather than something to trouble the device with. "
     "A stall on GET_DESCRIPTOR is indistinguishable from a device that "
     "has stopped answering",
     """    if (idx >= d->nconfigs)      return USB_EPARAM;""",
     """    ;"""),

    (CORE, "a reconfigure the controller refused leaves endpoints on offer",
     "the device HAS switched -- it accepted SET_INTERFACE -- and the "
     "controller has not. There is no state both sides can be described "
     "by, so the only honest answer is that there are no usable "
     "endpoints. A list the controller was never told about is a driver "
     "sending bulk transfers to pipes that do not exist",
     """            d->nep = 0;
            return r;""",
     """            return r;"""),

    (CORE, "an unconfigured device is allowed to switch interface",
     "a device with no configuration has no interfaces. The device "
     "would refuse, but asking means the kernel has already had to "
     "decide what its own table says in the meantime",
     """    if (d->config == 0) return USB_ENODEV;""",
     """    ;"""),

    # ══ v0.8.9.9: two interfaces on one phone ═════════════════════════
    (CORE, "a second claim on the same configuration selects it again",
     "the HP's bug: `pair` re-selected configuration index 3 for usbmux, "
     "SET_CONFIGURATION reset the Ethernet interface to alt 0, and every "
     "frame DHCP sent was refused -- 0 asked, 0 offered, 0 acknowledged",
     """    if (d->config == 0 || d->cfgidx != cfgidx) {
        r = do_set_config(h, d, cfgidx, (int)ifnum);""",
     """    if (1) {
        r = do_set_config(h, d, cfgidx, (int)ifnum);"""),

    (CORE, "the other interface's endpoints are not carried over",
     "the table holds only the newest interface, so the controller is "
     "told to drop the first driver's pipes -- the same symptom by a "
     "different road",
     """        if (d->ep[i].iface == ifnum) continue;""",
     """        continue;"""),

    (CORE, "the carried-over endpoints are not marked keep",
     "the controller re-adds a running pipe, re-laying its ring under an "
     "outstanding receive and resetting a sequence the device did not "
     "reset -- the next packet is taken as a retransmission and dropped",
     """        next.ep[next.nep].keep = 1;""",
     """        next.ep[next.nep].keep = 0;"""),

    (CORE, "SET_INTERFACE is skipped for a non-zero alternate",
     "Ethernet's alt 1 is where its endpoints are; without the request "
     "the device stays on alt 0, which has none",
     """    if (alt != 0) {
        r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD |""",
     """    if (0) {
        r = usb_control(d, USB_DIR_OUT | USB_TYPE_STANDARD |"""),

    (CORE, "the keep marks are left on after the claim",
     "the NEXT configure -- a SET_INTERFACE, a re-claim -- would skip "
     "endpoints that really do need re-adding",
     """    for (i = 0; i < d->nep; i++) d->ep[i].keep = 0;
    return USB_OK;
}""",
     """    return USB_OK;
}"""),

    (CORE, "endpoints are not tagged with their interface",
     "usb_find_ep_if() finds nothing, so neither driver on the phone can "
     "find its own bulk pair",
     """                e->iface    = d->ifnum;   /* v0.8.9.9: whose it is */""",
     """                e->iface    = 0;"""),

    (SRC, "x_configure re-adds endpoints marked keep",
     "the running pipe of the OTHER driver is re-initialised behind its "
     "back: its ring re-laid, its outstanding receive thrown away",
     """        if (e->keep && ring_slot_for_dci(x, slot, dci) >= 0) continue;""",
     """        ;"""),
]


def build_and_run(timeout):
    exe = os.path.join(tempfile.gettempdir(), "hd_txhci_inj")
    b = subprocess.run(CC + ["-o", exe] + LINK, capture_output=True, text=True)
    if b.returncode != 0:
        return "build-failed", b.stderr
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", ""
    return ("pass" if r.returncode == 0 else "fail"), r.stdout


def main():
    if not os.path.exists(SRC):
        sys.stderr.write("run me from the source root\n")
        return 2
    pristine = {f: open(f, encoding="utf-8").read() for f in (SRC, BAR, CORE)}
    original = pristine[SRC]

    print("Injecting %d real xHCI bugs into %s and the BAR decoder.\n"
          "This is the controller the test laptop's keyboard, storage and\n"
          "network all sit behind.\n" % (len(BUGS), SRC))

    st, out = build_and_run(120)
    if st != "pass":
        print("  the UNMODIFIED driver does not pass (%s) -- fix that first" % st)
        print(out[-2500:])
        return 2
    print("  baseline: the unmodified driver passes\n")

    missed, stale = [], []
    try:
        for i, entry in enumerate(BUGS, 1):
            if len(entry) == 5:
                fname, label, why, old, new = entry
            else:
                fname, (label, why, old, new) = SRC, entry
            # EVERY file goes back to pristine before each injection.
            # The first version of this loop restored only the file it was
            # about to damage, so once a BAR bug had been injected the BAR
            # file stayed broken for all eleven injections after it -- and
            # every one of them reported "caught" because the suite was
            # failing on the LEFTOVER bug, not the planted one. One of
            # them was in fact a genuine miss, hidden by that. A harness
            # that reports a catch it did not earn is worse than no
            # harness. (v0.8.5.1)
            for f, text in pristine.items():
                open(f, "w", encoding="utf-8").write(text)
            base = pristine[fname]
            n = base.count(old)
            if n != 1:
                stale.append((label, n))
                print("  %2d. %-58s INJECTION STALE (%d)" % (i, label, n))
                continue
            open(fname, "w", encoding="utf-8").write(base.replace(old, new, 1))
            st, _ = build_and_run(60)
            if st == "pass":
                missed.append((label, why))
                verdict = "MISSED"
            elif st == "build-failed":
                verdict = "caught (did not compile)"
            elif st == "hang":
                verdict = "caught (hung)"
            else:
                verdict = "caught"
            print("  %2d. %-58s %s" % (i, label, verdict))
    finally:
        for f, text in pristine.items():
            open(f, "w", encoding="utf-8").write(text)

    print()
    if stale:
        print("  %d injection(s) no longer match the source:" % len(stale))
        for label, n in stale:
            print("    - %s (%d matches)" % (label, n))
    if missed:
        print("  %d bug(s) the suite did NOT catch:" % len(missed))
        for label, why in missed:
            print("    - %s\n        %s" % (label, why))
        return 1
    if stale:
        return 1
    print("  all %d caught" % len(BUGS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
