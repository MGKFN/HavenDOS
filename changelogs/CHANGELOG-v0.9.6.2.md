# HavenDOS v0.9.6.2 Beta "Daedalus" — the monitor's buffer

The second maintenance update of 0.9.6, made from Kyan's `gfx try` photos
of 16 November. It contains fixes only (THS-RM-1004 section 12).

## Fixed

### The HDMI monitor showed one colour edge to edge, never the picture

`gfx try` on the HP settled it. Each of the four trials painted the
monitor in its own colour (green, red, white, blue-green) and none showed
a picture. The underrun rate hardly moved:

| | before | deep watermark | 64×64 plane | both | DBOX credits |
|---|---|---|---|---|---|
| underrun / 20000 | 1504 | 1346 | 1342 | 1485 | 1497 |

A plane one sixteenth the size starved just as badly, so the cause was
never bandwidth. The plane was reading a buffer that does not exist.

Alder Lake-P has four display-buffer slices of 1024 blocks:
- pipe A (the panel) uses slices 1–2;
- pipe B (the HDMI socket) uses slices 3–4.

On this display generation each pipe's `PLANE_BUF_CFG` is counted from
the start of **its own half**. Linux subtracts `mbus_ddb_offset()`, which
is 2048 for slices 3–4. 0.9.6.1 wrote pipe B's buffer as blocks
2048–4031, counted from slice 1. Pipe B read that as 2048 blocks past
the start of its half, which is past the end of the 2048 blocks it has.
Every fetch came back empty, whatever the plane's size or watermark.

`gfx on` now writes **0–1983** (`PLANE_BUF_CFG` 07BF0000): the same
1984 blocks, counted the way the hardware counts them. 64 blocks are
still left free for a cursor.

## Changed

- **`gfx try` has a fifth trial, YELLOW, which is the fix itself.** It
  moves the buffer to 0–1983 on the running pipe. Lighting the monitor
  again isn't needed. Trial 0 still puts everything back afterwards,
  the buffer included.
- **`gfx b` names the bug when it sees it.** If pipe B's buffer reaches
  past block 2047, it prints four red lines saying why the plane
  underruns and what fixes it.
- **`gfx on` measures the underrun rate** over 20,000 reads after it
  lights the monitor, instead of reading the register once. It says
  either "the plane keeps up" or "the plane is starving".

## How to test

1. Update and reboot. Plug the monitor into the HDMI socket.
2. Run `gfx on`.
   - **Expected:** the HavenDOS screen in the middle of the monitor, and
     `underrun in 0 of 20000 reads`.
   - Photograph the monitor and the last lines on the laptop.
3. If the monitor is still a flat colour, run `gfx b` and `gfx try`, and
   photograph them as before. Trial 5 is yellow.

A monitor that was lit by 0.9.6.1 in the same boot keeps the old buffer
until `gfx off`. `gfx try` trial 5 fixes it without that.

## Tests

- `test_xemode` 236 (floor 226 → 236): the buffer value, trial 5,
  `xm_buf_outside()`, and `gfx b`'s new lines;
- `check_xemode_suite` 33/33: three new injections, one of them
  0.9.6.1's absolute numbers put back;
- 7258/7258 checks across 69 suites.
