# HavenDOS v0.9.6.1 Beta "Daedalus" — the first fixes from the HP

The first maintenance update of 0.9.6, made from Kyan's photos of 16
November. It has fixes only, as the 0.9.x rules in THS-RM-1004 section 12
require. The one new command, `gfx try`, is a diagnostic for the external
monitor.

## Fixed

### `resolution` refused on the desktop, every time

0.9.6 refused to change the grid whenever any other app was open, so that
open apps would not come back scrambled. On the desktop that meant always,
because the desktop and the Shell it opens are already two apps. The shell
also never got back to the resolution you chose for it.

Now the grid can change whatever is open:
- **Other apps are re-laid out when you switch back to them.** Every cell
  keeps its row and column at the new width. What no longer fits is
  dropped, and the new edge is blank.
- **An app made for 80×25 is centred again** in the new grid, and what it
  drew moves with it.
- **The cursor stays inside** the screen.
- **Nothing is asked of the app:** it sees its own screen in the same place,
  and lays itself out for the new grid the next time it draws.

The re-layout is `ctx_reflow()` in `drivers/vga.c`, run from
`vga_ctx_load()` whenever a console made for one grid is loaded at another.

### The "Welcome / Create new account" screen came back on every boot

It was meant only for a machine nobody had made an account on. It checked a
flag held in memory, and memory is empty at every power-on, so the check was
always true, even after setup had made your account. It now shows only when
no account exists besides the three built in (admin, bootos and guest).

### BOOT Chat is out of the menus

Its server is shut down until further notice. `chat` on its own now says
so, and names what replaces it in 27H2: IRC on Libera.Chat, Haven News, and
LAN chat. `chat user@host` still connects to a server you run yourself.

### `gfx on` said "no underrun" when there was one

It read the interrupt register that latches events, but an underrun that is
masked there never latches. It now says one read proves nothing, and points
to `gfx b`.

## For the external monitor: `gfx b` and `gfx try`

**On the HP (16 November), `gfx color blue` turned the LG blue edge to
edge.** So the HDMI link, the timing and pipe B all work; what is missing is
the picture layer (the plane). `gfx b` showed:
- the plane is on (PLANE_CTL 94000008) and armed (SURFLIVE 0, the same as
  PLANE_SURF);
- **PIPE_ISR bit 31 set: pipe B's FIFO underran.** The plane is trying to
  fetch the screen and can't keep up, so nothing it fetches is shown.

What 0.9.6.1 adds to find out why:

- **`gfx b`** now samples bit 31 20,000 times and prints how many reads saw
  it set. That's a rate rather than one lucky read.
- **`gfx try`** runs four trials. Each changes one thing on pipe B's plane,
  measures the underrun rate, and paints the background its own colour, so
  a photo of the monitor shows which trial it was:

  | Trial | Background | Change |
  | --- | --- | --- |
  | 1 | green | deep watermark: WM0 gets the whole buffer, every other level off |
  | 2 | red | a 64×64 plane, a sixteenth of the bandwidth |
  | 3 | white | both together |
  | 4 | blue-green | the MBUS DBOX B credits 12 → 8, as i915 sets them on Alder Lake-P |

  Everything is put back at the end, or straight away at Q. Only pipe B's
  own registers are written, through the same allow-list as `gfx on`.

## Tests

- **`make test`: 7248/7248 across 69 suites.**
  - `test_console` 309 → 326: re-layout at 80×25 → 90×30 → 80×25, a
    centred viewport, and a console larger than the new grid.
  - `test_xemode` 209 → 226: every trial against the model of the HP's
    engine. PLANE_SURF is written last; trial 0 puts back every register;
    nothing outside pipe B is written; the underrun rate counts bit 31 only.
- **`check_console_suite.py`: 38/38 caught**, including three new
  injections for the re-layout.
- **New `harness/resolution_tasks.py`: 8/8.** In Shell Mode it opens Notes
  and writes a line, then sets `resolution 90x30` with Notes open. Notes
  comes back with its line moved as one piece, and is still intact at
  80×25. **The 0.9.6 ISO fails the same harness, 5/7.**
- `appguard_boot.py` 19/19, which includes the login screen.

## Not yet proved on the HP

- `gfx try`: that's what it's for.
- The resolution change on the HP's 128×48 framebuffer grid.
- Alt+Tab with the USB keyboard, the Welcome fix, and `migrate`, all still
  waiting for photos.
