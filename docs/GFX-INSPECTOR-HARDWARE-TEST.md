# 0.9.0 on the HP: the display engine, read only

**Ten minutes, and nothing on the laptop changes.** `gfx` reads the
Iris Xe's registers and writes none of them. The panel keeps working
whatever it finds. The worst it can do is print something wrong, and
the point of this test is to find out whether it does.

QEMU has no Intel display engine, so **this is the first time any of
this code has run against real hardware.** Every register offset in it
comes from the Linux i915 driver. The photos settle whether it reads
them right.

## Before you start

- Install 0.9.0 with `update`, as usual.
- **Plug the monitor into the laptop's HDMI port** and switch it on.
  (If it only connects over USB-C, use USB-C and say so. That changes
  what 0.9.1 has to do.)
- Boot HavenDOS normally, with the lid open.

## The four photos

Type each command, then press the **space bar** to page. Photograph
**every page**, not just the first.

| step | type | what it shows |
|---|---|---|
| 1 | `gfx` | the summary: power, pipes, transcoders, ports, PLLs, and what it all means |
| 2 | `gfx vbt` | which socket the firmware says is wired to which port |
| 3 | `gfx regs` | every raw register value, in the order it was read |
| 4 | **unplug the HDMI cable**, then `gfx` again | only the **Ports** page is needed |

Step 4 is what shows whether hot-plug detection works: the DDI B line
should change between the two runs.

## What a good result looks like

The last section of `gfx`, under **So**, should read something like:

```
The panel runs on pipe A, transcoder A, DDI A.
Pipe B and transcoder B are free for the monitor.
HDMI is wired to DDI B.
0.9.1 reads the monitor's EDID over GMBUS pin 2.
```

The port and pin may differ: whatever the HP's VBT says is the answer.
The rest of the summary also has two self-checks:

- **Pipes:** the pipe A line should end with
  `= the framebuffer HavenDOS draws on`. That line compares the plane
  register with the address GRUB handed over. If they agree, the
  inspector is reading the right registers.
- **Self-check:** it should say `0 written` and
  `unclaimed-access flag clear before and after`. That is the engine's
  own report that every register read was in a powered-up block.

## What a bad result means

| the screen says | it means | what to send |
|---|---|---|
| `Stopped: an Intel display, but not display version 13` | the device ID is not in the table | the `8086:xxxx` from the first line |
| `Stopped: its register BAR would not map` | the kernel's eight high-memory windows are full | `dmesg` (the lines that say "mapped at") |
| `Stopped: every register reads FFFFFFFF` | the GPU is powered down, or BAR0 is wrong | `lspci -v` |
| `PW_1 is off: the display core is asleep` | the firmware left the display in a power-saving state | all of `gfx` and `gfx regs` |
| `UNCLAIMED READ` in red | a register was read in a powered-down block: a gate in `drivers/xedisp.c` is wrong | **`gfx regs`, every page**: the offset is in there |
| pipe A shows an address but no `= the framebuffer ...` | the plane is scanning out something other than what GRUB gave us, or BAR2 is read wrong | the pipe A lines, and the `firmware framebuffer` line |
| a port says `detect off` | the firmware never enabled hot-plug detection there; turning it on is a write, which is 0.9.1's job | nothing extra, just note it |
| `The VBT names no HDMI connector` | the VBT is missing or the parser misread it | `gfx vbt` |

## Two questions for Kyan

1. **The monitor's make and model, and its native resolution.** The
   sticker on the back or the OSD menu will have it. 0.9.1 will read it
   from the monitor too, but it helps to know what to expect.
2. **HDMI or USB-C?** The photo of 11 November shows an HDMI cable next
   to the laptop, so HDMI is the assumption here.

## Still open from earlier releases

These have still never been photographed on the HP. One photo each
whenever convenient:

- `dmesg`: the `Framebuffer memory: write-combining (PAT)` line
  (0.8.9.6).
- The pointer: does it reach all four edges, and is the speed OK?
- `i2chid`: the trackpad.
