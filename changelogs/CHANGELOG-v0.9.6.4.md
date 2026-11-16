# HavenDOS v0.9.6.4 Beta "Daedalus" — the monitor never gets stuck

The fourth maintenance update of 0.9.6, from Kyan's photo of
16 November. Fixes only.

## Fixed

### `gfx on` was refused as "already lit" while the monitor was dark

What the photo shows:
- the warning "Monitor not lit -- `gfx on` says why";
- then `gfx on` refused with "pipe B is already lit -- `gfx off` first",
  before and after a `gfx off`.

Yet all three registers the refusal printed read *off*:
- TRANSCONF B 00000000;
- DDI_FUNC B 00030000 (only the sync polarities);
- DDI_BUF B 00000080 (idle).

What happened:
1. 0.9.6.3's watcher tried to light the monitor scaled, and the attempt
   stopped part-way, after the PLL, the clock and the scaler but before
   the pipe started.
2. That left pipe B **half set up**. The refusal counted the scaler as
   "lit" but never printed it.
3. Each later attempt was refused, and the watcher gave up after three.
4. `gfx off` only cleared parts that were *enabled*, so the leftovers
   stayed and `gfx on` stayed refused.

In 0.9.6.4:
- **`gfx on`, the watcher and Settings clear a half-set-up pipe B by
  themselves first**, saying so ("Pipe B was left half set up by an
  attempt that stopped: clearing it first").
- **If a scaled light stops, the monitor is lit unscaled instead**, the
  way 0.9.6.2 lit it on the HP. `gfx on` prints the step that stopped
  scaling, with the register, the value written and the value read back.
  The watcher puts the same line in `dmesg` and shows a notification
  saying it is unscaled. The monitor shows a picture either way.
- **The refusal names its reason.** It also prints PLANE_CTL B and both
  of pipe B's scalers, lists what is on, and says when pipe B isn't
  actually running.
- **`gfx off` clears everything of pipe B**: any scaler that is set, not
  just one that is enabled, with its window; and DDI_FUNC's leftover
  bits.
- **The scaler's read-back checks only what matters**: the enable bit,
  and the window's position and size. i915 reads none of these back,
  and the phases' spare bits are the hardware's to report however it
  likes. That may be what stopped it. If it was something else, the
  step is now on the screen.
- **`gfx b`** also shows pipe B's scaler, its window size, and pipe A's
  scaler for comparison.

## How to test

1. Update and reboot with the LG plugged in.
2. **Expected:** within a few seconds the monitor lights.
   - **Scaled:** the notification says "Monitor on: fitted, shape kept".
     Photograph the monitor.
   - **Unscaled:** the notification says so. Run `dmesg` and photograph
     the "monitor: scaling stopped at ..." line. That line says exactly
     which register the scaler stopped at.
3. Then `gfx off`, then `gfx on`. Photograph the end of its output.

## Tests

- `test_xemode` 285 (floor 285). It replays the HP's state: a scaled
  light stopped at the scaler, refused as busy, the refusal naming
  "scaler 1", `gfx off` clearing the scaler and DDI_FUNC 00030000, then
  lighting again.
- `check_xemode_suite` 47/47, with four new injections.
- 7367/7367 checks across 70 suites.
- QEMU: `mouse_apps` 14/14 and `gfx_boot` 15/15. QEMU has no Iris Xe,
  so the fallback itself only runs on the HP.
