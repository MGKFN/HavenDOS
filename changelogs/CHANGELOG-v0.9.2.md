# HavenDOS v0.9.2 Beta "Janus": lighting the monitor

*11 November 2026. 63 test suites, 6634/6634 checks.*

## What the HP said on 0.9.1

`gfx edid` worked first time. It made 8 GMBUS writes, refused none, and
read back both EDID blocks without a NAK. The monitor is an **LG
24GN65R "LG ULTRAGEAR"** (GSM 5C19, week 7 of 2024). Its modes:

| mode | clock | note |
|---|---|---|
| 1920x1080 @ 143.98 Hz | 346.5 MHz | preferred. Over 340 MHz, so it needs HDMI 2.0 scrambling |
| 1920x1080 @ 60.00 Hz | 148.5 MHz | CEA VIC 16, **0.9.2's target** |
| 1920x1080 @ 119.98 Hz | 285.5 MHz | needs no scrambling; next after 60 |
| 2560x1440 @ 74.97 Hz | 304.25 MHz | a downscaled mode |

All four of 0.9.1's report fixes held on the HP.

## The release: `gfx plan`, `gfx on`, `gfx off`

```
gfx plan    what `gfx on` would write, every operation, or why it
            refuses. Writes nothing except the EDID read.
gfx on      light the HDMI monitor. It shows the plan's summary and
            asks y/N, then runs it and prints every write with what
            read back, plus pipe B's frame counter.
gfx off     turn pipe B, DDI B and their PLL off again.
```

`gfx on` programs the HP's second display path:

```
DPLL0 -> DDI B (combo PHY B) <- transcoder B <- pipe B <- plane 1
```

It points pipe B's plane at the framebuffer the panel already shows, so
the monitor **mirrors** the laptop. The picture is 1024x768, unscaled,
centred in a 1920x1080 frame at 60 Hz. The signalling is **DVI** on the
HDMI cable: the same pixels, no infoframes, no audio. HDMI mode needs an
AVI infoframe, and that comes once the picture is proved.

The sequence is i915's `hsw_crtc_enable()` for HDMI on a display-13
combo PHY, step by step, in `drivers/xemode.c`:

1. **Power.** DDI B IO well and DBUF slices 3/4.
2. **The PLL.** `combo_pll_enable`.
3. **Clocks.** The port clock is selected and ungated in two separate
   writes, as i915 requires. Then the transcoder clock.
4. **Pipe and transcoder.** Timings, PIPE_MISC, PIPE_CHICKEN, line time,
   MBUS credits, the buffer allocation and the watermarks.
5. **The function.** TRANS_DDI_FUNC_CTL, Wa_22012358565, then TRANSCONF.
6. **The PHY.** Signal levels from i915's HDMI table, lane power, then
   DDI_BUF_CTL.
7. **The plane.** PLANE_SURF goes last because it arms the plane.

Where i915 *computes* a value this release has no model for (the
watermarks, the MBUS credits, the context latency), the value is
**copied from pipe A**. The firmware programmed those for a plane of
exactly the size being mirrored.

## What makes it safe to try on the only laptop there is

1. **A plan first.** `gfx plan` lists every operation. `gfx on` runs
   the same list, after a y/N.
2. **Refusals before the first write.** There are 15 named reasons, each
   checked on a fresh read: DC5/DC6 armed, MBUS joined, pipe A's buffer
   in B's slices, no free PLL, PHY B uncalibrated, pipe B already lit,
   and so on. A refused plan has written nothing, and the screen shows
   the register behind the refusal.
3. **One write function, behind an allow-list.** `xm_wr()` is the only
   place a register is written, and it asks `xm_allowed()` first.
   - Pipe B, transcoder B, DDI B and PHY B may be written whole.
   - A *shared* register may change only B's bits. For DPCLKA that means
     port B's select and gate. For CHICKEN_TRANS B it means the
     frame-start field. A power request may only be *added*.
   - Pipe A's registers are not on the list.

   `tools/check_gfx_readonly.py` fails the build if:
   - a second write appears;
   - `xm_wr()` writes before it asks;
   - `xm_allowed()` so much as names an offset of pipe A's path.
4. **A read-back after every write.** The first register that doesn't
   hold what it should stops the sequence. The screen names the step,
   the register, what was wanted and what came back.
5. **Never at boot.**

## A 0.9.1 misreading, fixed: the HP's link is 270 MHz, not 269.7

On a 38.4 MHz reference, display 13 stores the PLL's DCO fraction at
**half** its value (WA #22010492432), and it must be doubled to read.
0.9.1 didn't double it, so it showed DPLL1 as 269.7 MHz and the panel as
59.93 Hz.

The HP's own register proves the fix. Its fraction is `0x3800`, exactly
half the `0x7000` in i915's HBR table entry. **DPLL1 is 270 MHz and the
panel runs at 60.00 Hz.** The writer applies the same workaround in the
other direction: DPLL0's 148.5 MHz fraction is written as 1024, not 2048.

DPLL0's stale value on the HP (`018001D4`) turns out *not* to be halved.
Doubled, it is past 15 bits, so whatever wrote it skipped the
workaround. It's unused, and it now decodes as 100.1 MHz.

## Also

- **The `gfx edid` screen named the wrong mode.** It said "0.9.2 lights
  pipe B with the first one", which on the LG is 346.5 MHz. It now names
  the mode `gfx on` will actually light, using the same `edid_pick()`
  the driver uses: progressive, 340 MHz or under, large enough for the
  plane, nearest 60 Hz, then largest. When the preferred mode is over
  340 MHz, it says that needs HDMI 2.0 scrambling.
- **The GMBUS test model followed the HP.** 0.9.1's model "consumed"
  SW_RDY in GMBUS1 and asserted it. The HP read GMBUS1 back as written
  (`468000A1/468000A1`). The model now does what the silicon did, and a
  knob keeps the other behaviour. The driver never compared that
  register, so it works with both, and the suite proves that.
- **The VBT parser reads two more fields.** The HDMI vswing entry (byte
  7, bits 4:0, BDB 158+) and lane reversal (flags1 bit 1, BDB 184+).
  `gfx on` uses both.

## Tests

- **`test_xemode` (195, new).** It starts from the HP's 96 real
  registers and fills the rest in i915's shapes, marked synthetic. The
  model behaves like hardware:
  - status bits move: PLLs lock only once powered and enabled, wells
    follow their requests, and DDI B leaves idle only when enabled;
  - PHY group writes fan out to all four lanes, and reading a group
    register returns garbage;
  - every write is seen, and pipe A's registers are compared word for
    word before and after.

  It checks the exact values, i915's ordering, all 15 refusals in 26 cases (each
  with zero writes), the allow-list register by register, a PLL that
  never locks, a register that won't hold, a buffer stuck idle, a plan
  aimed at pipe A, and `gfx off` with B on the panel's PLL.
- **`tools/check_xemode_suite.py`: 27/27 caught.** Its first run missed
  two, both gaps in the suite:
  - every clock tested had only one divider that fit, so "take the
    first divider" passed; 25.175 MHz is tested now;
  - every mode was +h+v, so swapped sync bits were invisible; a CVT-RB
    +h−v mode is tested now.
- **`test_gfx` 199 → 203, `test_edid` 101 → 106.**
  `check_gfx_suite.py` is 41/41 (three new injections: the fraction
  doubling and the two VBT fields). `check_edid_suite.py` is 24/24.
- **QEMU:** `harness/gfx_boot.py` passes 13/13. With no engine, `gfx plan`,
  `gfx on` and `gfx off` say so and never ask y/N.

## What it needs from the HP

**`docs/GFX-ON-HARDWARE-TEST.md`.** Ten minutes. Photograph both screens
at `gfx on`. **This code has never run on an Iris Xe**: the model proves
it does what it says, in i915's order, with nothing outside B touched.
Whether the silicon agrees is the HP's answer.
