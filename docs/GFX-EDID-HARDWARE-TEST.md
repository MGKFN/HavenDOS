# 0.9.1 on the HP: reading the monitor

> **ANSWERED ON THE HP, 11 November 2026, on 0.9.1.** The EDID came back
> first time: 8 GMBUS writes, 0 refused, both blocks, no NAK. The monitor
> is an LG 24GN65R "LG ULTRAGEAR". It prefers 1080p144 at 346.5 MHz, which
> needs HDMI 2.0 scrambling, and offers 1080p60 at 148.5 MHz. GMBUS1 reads
> back as written, SW_RDY included. The next test is
> `docs/GFX-ON-HARDWARE-TEST.md`.

**Five minutes.** `gfx edid` asks the LG monitor to identify itself over
the HDMI cable's DDC wires, using the display engine's own I2C
controller (GMBUS).

This is the **first command in the display series that writes**. It
writes only the six GMBUS registers (`C5100` to `C5120`): the I2C
controller, nothing that draws the picture. The worst a wrong write can
do is leave that controller confused until the next boot. The panel
keeps working whatever happens, and the screen lists every write.

## Before you start

- Install 0.9.1 with `update`.
- The LG 24GN65R is on, set to the HDMI input, and plugged into the
  laptop's HDMI port.

## The photos

| step | type | what it shows |
|---|---|---|
| 1 | `gfx edid` (space to page; photo **every** page) | the monitor's maker, name, the modes it offers, the raw bytes, and every GMBUS write with what it read back |
| 2 | `gfx` | the summary again. Check the four lines 0.9.0 got wrong, below |
| 3 | switch the monitor **off**, then `gfx edid` | what "nothing answered" looks like on this machine |

## What a good `gfx edid` looks like

```
Monitor EDID -- GMBUS pin 2 (DDI B), I2C address 50
  N writes, every one to GMBUS (C5100-C5120); 0 refused
  maker GSM  product ....  made week .. of 20..  EDID 1.3
  name  LG ULTRAGEAR          (or similar)
  limits ..-.. Hz, ...
Modes (the first is the one the monitor prefers)
  1920x1080  60.00 Hz  148.5 MHz  total 2200x1125 +h+v      (probably)
  1920x1080  1xx.xx Hz ...  (block 1)
```

**GSM** is LG's manufacturer code. The exact modes are what this test
is for. The first line under Modes is what 0.9.2 will put on the
screen.

## And in `gfx`, the four fixes

- **Power:** a line `requested in the driver register: PW_1 PW_A PW_2
  PW_B PW_C`. 0.9.0 said "none" here, which was wrong.
- **Pipe A:** `pipe source 1024x768, scaled to 1920x1080 by the pipe
  scaler`. That is why HavenDOS has always looked stretched on the
  panel: the firmware runs 1024x768.
- **Transcoder A:** a new line `pixel clock ... MHz, ..... Hz`, the
  panel's real refresh rate.
- **PLLs:** `DPLL1 ON, locked, 269.7 MHz`, the panel's DP link clock.
- **Ports:** `eDP panel, AUX DDI A`, with no DDC pin any more.

## What a bad `gfx edid` means

| the screen says | it means | what to send |
|---|---|---|
| `Nothing answered at 50, twice` | no ACK on pin 2: the monitor is off, on another input, or the cable is out | the same screen with the monitor definitely on and on HDMI |
| `the controller never said it was ready` plus a GMBUS2 value | GMBUS hung: a sequencing bug in `drivers/xegmbus.c` | that screen, the write list included |
| `GMBUS0 read back something other than the pin` | the pin register didn't hold what was written, so nothing was sent | that screen |
| `the bus was already busy` | something else was mid-transfer and never finished | that screen, then a reboot and try again |
| `bytes came back, but they are not an EDID` | the wire works but the data is wrong (bytes lost or shifted) | **every** raw line |
| any `refused` count above 0 | the write guard stopped a write outside GMBUS. That should be impossible | everything on that screen |

## The 0.9.0 test is answered

Kyan's photos of 11 November settled it. `gfx` read the engine
correctly (the framebuffer cross-check passed), the panel runs on pipe
A / transcoder A / DDI A (eDP, 2 lanes, DPLL1), and HDMI is wired
directly to DDI B on GMBUS pin 2. The monitor is an LG 24GN65R.
