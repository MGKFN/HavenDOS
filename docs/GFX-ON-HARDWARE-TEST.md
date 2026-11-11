# 0.9.2 hardware test: light the HDMI monitor

**Ten minutes. The HP, the LG 24GN65R on its HDMI input, the cable in.**

0.9.2 is the first HavenDOS release that changes what a screen shows.
`gfx on` programs a whole display path on the HDMI socket (DPLL0, DDI B,
transcoder B, pipe B and its plane) and points it at the picture the
laptop panel already shows. If it works, the monitor shows the same
1024x768 HavenDOS screen, unscaled, in the middle of a black 1920x1080
frame, at 60 Hz.

**The panel's path is not touched.** Pipe A, transcoder A, DDI A and
DPLL1 aren't on the allow-list behind the one function that writes, so
the laptop screen should stay exactly as it is whatever happens on the
monitor. If the panel *does* go dark, that is the most important thing
this test can find. Hold the power button to recover, and say so first.

Nothing in 0.9.2 runs at boot. Only typing `gfx on` and answering `y`
lights anything, and a reboot undoes all of it.

## Before you start

1. Monitor on, input set to **HDMI**, cable in.
2. Boot 0.9.2 on the HP as usual.

## 1. `gfx`: one line to check (photo)

Look for these two lines:

- `DPLL1 ON, locked, 270 MHz`: 0.9.1 said **269.7**, which was a
  misreading. On a 38.4 MHz reference the PLL's fraction is stored at
  half its value, and 0.9.1 didn't double it. HBR is 270 MHz exactly.
- The panel line should now say **60.00 Hz**, not 59.93.

## 2. `gfx plan`: every page (photos)

This reads the monitor's EDID again and prints what `gfx on` *would*
write, without writing it. (The EDID read is the only write.) Check:

- `mode   1920x1080 at 60.00 Hz, 148.5 MHz, from the EDID`. That is the
  LG's 1080p60. Its preferred 144 Hz mode needs HDMI 2.0 scrambling,
  which isn't in yet.
- `DPLL0  148.5 MHz  cfg 001001D0 00000E84  (port A stays on DPLL1)`.
- About 77 numbered operations. Photograph them all. They are the
  exact list `gfx on` will run.

**If it says `Refused:`**, photograph that page and stop there. The line
under it shows the register that caused the refusal. Nothing was written.

## 3. `gfx on`: the moment (photos of BOTH screens)

1. Type `gfx on`. It prints the summary and asks
   `Light the HDMI monitor now? The panel is not touched. y/N`.
2. Press `y`.
3. Photograph:
   - **the laptop screen**: the result, and every page of the write log;
   - **the monitor**, whatever it shows: the picture, "no signal", a
     blank screen, or noise.

How to read the result:

| on screen | means |
|---|---|
| `Lit. N writes, every one read back` + `scanning out.` | the engine did everything asked and pipe B is counting frames |
| `... NOT MOVING.` | every write landed, but pipe B isn't running. Look at the monitor anyway |
| `Stopped: a status bit never came.` + `DPLL: locked` | the PLL didn't lock. The dividers are suspect |
| `Stopped: ...` + `DDI B active` | the port never left idle. The PHY or its power is suspect |
| `Stopped: a register did not keep what was written.` | the line under it gives the register, what was written and what came back |
| `Stopped: the allow-list refused a write.` | should be impossible. It's a bug in the plan, caught before it was written |

Whatever the monitor shows tells us something:

- **The HavenDOS screen, small and centred:** it works. Type a few
  commands and watch both screens.
- **"No signal":** the PHY or the PLL is wrong. The write log says which
  values went where.
- **Noise or rolling:** the timing is wrong.
- **Black, but the monitor says it has a signal:** the plane, its buffer
  or the watermarks are wrong. The pipe is running with nothing in it.

## 4. `gfx` again, while it is lit (photo)

Pipe B, transcoder B and DDI B should now show as on in the inspector.

## 5. `gfx off`, then `gfx on` once more

`gfx off` turns pipe B, DDI B and DPLL0 off again (it leaves power wells
and buffer slices on, which is harmless). Photograph the result. Then
`gfx on` and `y` again, to show it can be lit twice in one boot.

If `gfx on` stopped part-way in step 3, run `gfx off` before anything
else and photograph that too.

## What to send

Photos of steps 1-5, with the monitor's photo next to the laptop's for
step 3. Say in words whether the **panel** flickered or changed at any
point.
