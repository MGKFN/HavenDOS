# HavenDOS v0.9.2.2 Beta "Janus": the monitor takes the signal; now the picture

*11 November 2026. 64 test suites, 6695/6695 checks.*

## What the HP said on 0.9.2.1

**The LG locked on to HavenDOS's signal.** The monitor came out of
standby and showed a lit, black screen. The laptop printed:

```
Lit. 69 writes, every one read back as it should.
  pipe B frame counter 1 -> 4 in 50 ms: scanning out.
  no underrun on pipe B.
```

That proves almost the whole path:
- DPLL0 is locked at 148.5 MHz.
- DDI B's clock is ungated.
- Transcoder B runs 1920x1080 at 60 Hz.
- The PHY's signal levels and lanes are up.
- DDI_BUF_CTL B left idle.
- The monitor accepted the TMDS link and the timing.

It is the first time a HavenDOS display path drove a screen the firmware
didn't set up.

**What's missing is the picture.** The monitor should show the laptop's
1024x768 console in the middle of a black frame. It shows black from
edge to edge. (I enhanced the photo's contrast: there is no structure
anywhere on it.) Every plane register read back exactly as written, so
this release adds the tools to find out why instead of guessing.

Kyan also found the USB problem: **the keyboard was on a hub.**

## `gfx color` and `gfx b`

```
gfx color [blue|red|green|white|off]   pipe B's background colour
gfx b                                  pipe B as it runs, read only
```

`gfx color` writes PIPE_BOTTOM_COLOR B, the colour the pipe shows where
no plane is. It's one write, to pipe B, through the same allow-list and
read-back, and it refuses if pipe B isn't running. The monitor then
answers the question:

| on the monitor | means |
|---|---|
| all blue | the plane is not being shown at all |
| blue with a black 1024x768 box | the plane is there, and fetching black |
| still black | pixels are not reaching the monitor, though the timing does |
| the console, blue around it | it works; `gfx color off` |

`gfx b` prints pipe B live:
- whether the scanline and frame counter are moving;
- TRANSCONF, DDI_FUNC, DDI_BUF and PIPESRC;
- the plane's CTL, SURF, **SURFLIVE** (what it is actually scanning, as
  opposed to what was armed), POS, SIZE and STRIDE;
- its buffer allocation, watermark and MBUS credits;
- the arbiter, background, gamma and CSC;
- the DBUF slices and MBUS_CTL;
- the pipe interrupt status (ISR and IIR) and DPCLKA.

`gfx color` prints it too, so one photograph has both.

## USB: a hub is now told it is a hub

**On xHCI a hub's slot context carries a Hub flag**, plus its port count,
Multi-TT and TT think time. **None of these was ever set.** The
controller was told every hub was a plain device.
- A **high-speed** device behind a hub doesn't care, which is why a USB
  stick behind one was always fine.
- A **low- or full-speed** device does: a keyboard, a mouse receiver. Its
  traffic has to go through the hub's transaction translator as split
  transactions, and the controller only schedules those for a slot it
  knows is a hub. **A keyboard on a hub lit up and typed nothing.**

`usb/hub.c` now reads the TT think time from the hub descriptor
(wHubCharacteristics bits 6:5), alongside the port count. Before
touching any port, it calls the new `usb_hub_info()`. On xHCI
(`x_hub_info`) that takes the slot context out of the device context,
sets the Hub flag, Number of Ports, MTT (a high-speed hub with one TT per
port) and the think time, and hands it back. It uses a Configure Endpoint
command whose only add flag is A0, so no endpoint is touched; this is
what Linux does in `xhci_update_hub_device()`. UHCI, where a hub is
transparent, has nothing to be told. A controller that refuses is logged,
and the walk goes on.

**One thing still missing:** a device plugged into a hub *after* boot
isn't noticed until `usb rescan`, because the hub's own change endpoint
isn't polled.

## Tests

- **`test_xemode` 199 → 209**: the colour as one write, its refusal on
  a dark pipe, the live readout. `check_xemode_suite.py` catches 30/30.
- **`test_hub` 92 → 99**: the hub is marked with 4 ports and think time
  2, *before* any port is powered or reset. A refusal doesn't stop the
  walk, a SuperSpeed hub gets think time 0, and UHCI isn't asked.
  `check_hub_suite.py` catches 18/18. Its first run missed one: the
  fixture's think time (1) equalled the power-switching bits (1), so
  reading bits 1:0 passed. It is now 2.
- **`test_xhci` 334 → 344**: against the simulated controller, the hub's
  slot gets Hub, MTT, 4 ports and think time 2 in one Configure Endpoint
  with A0 alone. Route, speed, root port and Context Entries are
  untouched. A full-speed hub gets no MTT and no think time.
  `check_xhci_suite.py` catches 93/93.
- **Against 0.9.2.1's `usb/hub.c`, `test_hub` fails 5 checks.**
- **QEMU:**
  - `gfx_boot.py` 15/15: `gfx b` and `gfx color` refuse without an
    engine.
  - `hid_boot.py` 3/3.
  - `usb_storage.py` 52/52. That includes a stick behind QEMU's hub on
    xHCI, which is now marked as a hub, and QEMU's xHCI accepts the
    A0-only Configure Endpoint.

## What it needs from the HP

`docs/GFX-ON-HARDWARE-TEST.md`, steps 7 and 8:
1. `gfx on`, then `gfx color blue`. Photograph the monitor AND the laptop.
2. The keyboard on its hub, plugged in before boot. Does it type?
