# HELIOS ON THE HP — the five minutes that settle it

**v0.8.8.1. Read this, not HANDOFF.md, when you sit down at the
laptop.**

## START HERE: THE TWO NEW COMMANDS (0.8.8.3)

**1. `i2c scan` — the one that matters most.**

```
i2c
i2c scan
```

`i2c` should list two controllers at `00:15.0` and `00:15.1`. If it
says "No I2C controller found", run `dmesg` and read me the I2C line
— it will say either that the BAR would not map or that the core did
not identify itself as DesignWare, and those are different fixes.

`i2c scan` prints every address that answered. **Read me the list.**
That is the whole point of this release: an I2C device has no
enumeration and no vendor ID, the address normally comes from ACPI
which this kernel cannot read, and a scan is the only way to learn
it. Once I have the address the touchpad driver stops being guesswork.

It writes nothing — a zero-length read at each address, which is what
`i2cdetect -r` does.

**2. `rsa keygen` — how long a key takes on real silicon.**

```
rsa keygen 2048
```

In QEMU this is about two seconds. On the HP it should be faster.
**Read me the "took" line.** The handoff said 15–25 minutes for a
year and that figure measured code that no longer exists.

ESC stops it. The machine stays responsive while it runs — that is
the beta-1 timer tick doing its job, and this is the first thing to
use it.

Also worth a try:
```
rsa prime 2284453
rsa prime 1009
```

---

## THEN: THE FONT (0.8.8.2)

**Boot it and look at the text.** It was being drawn twice as tall as
it was wide on every framebuffer machine — 1280x800 at 80x25 came out
at a scale of 2 across and 4 down. The font is 8x16 now and both axes
take the same scale, so a character should be the shape of a
character.

If it still looks wrong, **say which**, because they are four
different fixes:

| what you see | what it means |
|---|---|
| still stretched tall | the font did not take; send a photo |
| correct but small, bars down the sides | expected on 16:9 — `resolution fit` fills it |
| correct and fills the screen | done |
| too few columns / huge text | `resolution fit`, then tell me the numbers |

`resolution` prints the grid. On a 1920x1080 panel, stock 80x25 will
letterbox to 1280x800 centred, and `resolution fit` should give about
120x33 filling the panel.

## THEN THE THREE FROM 0.8.8.1

Added in 0.8.8.1 and ahead of everything else below, because all three
are reasoned-and-tested rather than seen working.

**1. THE MOUSE. Just move it.**

Boot to the desktop and move the trackpad, then a USB mouse if one is
to hand. Both were dead; the cause was a readiness flag that only a
PS/2 reset could set, on a laptop with no PS/2 mouse.

**Read the boot log line first**, because it now tells you which path
is live:

| the log says | means |
|---|---|
| `no mouse answered the aux port (a USB mouse still works)` | no PS/2 mouse — **normal on this machine**. A USB mouse is the only pointer. |
| `mouse polled (8042 IRQ12 off)` | a PS/2 mouse DID answer. |

If the trackpad still does nothing and a USB mouse works, that is not
a regression — it means the trackpad is on I2C and needs a driver this
kernel does not have. **Say which of the two moved.** "Still dead" was
what cost the last round trip.

**2. exFAT. The big stick.**

```
exfat                      every exFAT volume the kernel can see
exfat ls 0:                the root directory
exfat cat 0:<a text file>   a file
```

If it **names** the volume and will not mount it, the refusal has a
name — `EXFAT_ENOTEXFAT`, `EXFAT_EGEOM` and the rest are different
bugs with different fixes. **Report the name.** If `exfat` says
nothing was found, run `disk` and `fat32` and report both.

**3. TLS. Any HTTPS request.**

The RSA key exchange runs on `net/bignum.c` as of this release and
**no harness in this tree completes a real handshake** — it is checked
against OpenSSL's own key natively and by the kernel link, and that is
all. If a handshake that used to work now fails, that is the thing to
report, and `obn_powmod` in `net/tls_crypto.c` is the independent
implementation kept on purpose to compare against.

**And one question still unanswered:** `resolution` "works but is
bugged" — what does the bugged part look like on screen? Grid
persistence and `resolution fit` are in this release and may or may
not be what that meant.

---

Everything in Helios passes 4062 suite checks and 32 boot checks in
QEMU. QEMU is a 1280x800 panel with a UHCI controller and no phone, and
`fbc_fit()`'s own header calls 1280x800 *the one size where the grid
asked for and the panel divided by the glyph agree*. So it is the one
panel on which every console change in this release could be a no-op
and still look perfect.

The HP is a real panel, a real xHCI controller, and a real clock. Five
things, in this order, none of which touch a disk.

Boot the **Live** entry. Nothing here installs, writes or updates.

---

## 1. WHAT PANEL DOES IT THINK IT HAS

```
resolution
```

**Read the first four lines and write them down.** They are the only
hardware fact in this test that nothing else can supply:

```
Display mode
  Current : 80x25
  Panel   : <W>x<H> pixels, framebuffer console
  Largest : <C>x<R>   ('resolution max')
```

- **If there is no `Panel` line at all**, the HP booted into VGA text
  memory and this whole release is untested on it — say so, and
  everything below about grids is moot. (The four text modes are then
  what you get, and `resolution 90x60` is the interesting one.)
- **If `Panel` says 1920x1080**, `Largest` must say **240x135**.
- **If `Panel` says anything else**, `Largest` is the panel divided by
  8 at the smallest scale that fits inside 320x180 and 32400 cells.
  Write down both numbers; the arithmetic is checked against six
  panels in `tests/test_console.c` and the HP is not one of them.
- **A `(capped by the cell budget)` note** after `Current` means this
  panel is larger than the console can express at one pixel per pixel.
  On 1920x1080 it should NOT appear. On anything above that it should.

## 2. A GRID NO VGA CARD CAN EXPRESS

```
resolution 100x50
```

then **ENTER** to confirm, within ten seconds.

- **Expected:** the text gets smaller, the screen stays legible, and
  after confirming it says `Mode set to 100x50`.
- **If the screen goes dark or will not sync:** do nothing for ten
  seconds. It reverts itself. That is v0.7.11's confirm-or-revert and
  it is the one guarantee in this release that has been in the tree for
  a year. If it does NOT revert, type `resolution reset` blind — the
  keyboard path is completely independent of the video mode.

Then check the shell still answers at it:

```
ver
help
```

`help` is the longest screen in the system and **it will page**. Press
SPACE through it. At 50 rows it should need fewer pages than at 25.

Then:

```
resolution max
```

ENTER to confirm. On a 1080p panel this is 240x135 and **the glyphs
will be 8 physical pixels tall, which is almost certainly too small to
read comfortably.** That is expected and it is recorded as an open
item: an 8x16 font option is RM-1003's fifth item for this release and
it did not ship. What matters is whether it RENDERS — whether the grid
is there and the characters are formed — not whether you would want to
use it.

```
resolution reset
```

brings back 80x25.

## 3. DOES IRQ0 ACTUALLY ARRIVE ON THIS MACHINE

This is the one that cannot be answered anywhere else, and it is the
most important line in this file.

```
kpoll
```

```
Periodic work
  IRQ0 ticks : <N>  at 100 Hz
  Enabled    : yes

  task           every     runs      late
  net dhcp       2000ms    <n>       <n>
  net carrier    1000ms    <n>       <n>
  usb ports      1000ms    <n>       <n>

  refusals, by reason
  <n>         not initialised
  <n>         already polling
  <n>         a driver holds a shared buffer
  <n>         switched off
```

**Wait ten seconds and run it again.**

- **`IRQ0 ticks` must have gone up by roughly 1000.** That is 100 Hz.
  This is the first thing in this project's history that depends on
  IRQ0 reaching a handler — `kernel/idt.c` records that the IRQ stub
  was missing its `push %esp` for an unknown number of releases and
  nothing noticed, because nothing depended on it.
- **If the tick count is ZERO or does not move:** IRQ0 is not arriving
  on this chipset. Everything else in part 3 and part 4 is then
  explained and nothing else needs investigating. Say so — it is a
  finding, not a failure of the release, and the polled clock in
  `kernel/pit.c` still carries the time the way it always has.
- **If it moves by far more or far less than 1000 per ten seconds**,
  write the number down: `pit_init(100)` and `KPOLL_HZ` are supposed to
  be the same number in two places and v0.7.5 is this tree's record of
  what happens when a divisor is written twice.
- **`already polling` must be 0.** Any other number is a client calling
  a polling sleep, and it is a bug.
- **`a driver holds a shared buffer` being non-zero is NORMAL** and
  expected — it means the poll was refused while a transfer or a sector
  write was open, which is exactly what it is for.
- **`late` climbing on a task** means that task is becoming due again
  before the poll reaches it. Occasional is fine; constant means the
  machine is spending its time somewhere else.

## 4. HOT-PLUG, WITHOUT TYPING ANYTHING

With the Kingston stick NOT plugged in:

```
usb
disk
```

Note what is listed. **Then plug the stick into a side socket and wait
two seconds. Do not type `usb rescan`.**

```
disk
usb
```

- **Expected:** the stick is in the list. Nothing was typed to find it.
- **`dmesg`** should carry `usb: <controller> port <n>: device arrived`
  and a line counting what the rescan found.
- **If nothing happened:** the HP's side sockets are wired to root
  ports (two boots on 25 October said so), so this should work — if it
  does not, check `kpoll` first. A tick count that is not moving
  explains it completely.
- **Then pull the stick out**, wait two seconds, and `disk` again. It
  must be gone. A disk that has gone away must not stay in the
  installer's picker.
- **A device behind a HUB will NOT be noticed.** That is not a bug in
  this test; the hub's change endpoint is not polled and the handoff
  says why.

## 5. DHCP ON ITS OWN

Only if you have something to tether or a cable.

With the phone's hotspot OFF, boot and wait. `dmesg` should show the
one-shot at boot failing, and then:

```
[  28.874]      dhcp: no address yet, asking again (try 1 of 5)
```

at roughly ten seconds after boot, then thirty seconds after that, then
sixty. **Five attempts and then one line saying it has stopped** —
not a line on every poll.

Then turn the hotspot ON and `usb rescan` (or just plug the phone in).
`ifconfig` should show a negotiated address within a minute without
anything being typed, and `dmesg` should say which try got it.

**The retry blocks the shell for about four seconds when it fires.**
That is expected and it is why there is a backoff: `dhcp_request()` is
a blocking call and taking it apart into a state machine is a bigger
change than this release should carry. If you are typing when it
happens you will notice; it does not start an attempt when a keystroke
is already waiting.

---

## WHAT WOULD CHANGE THE ROAD

Only one thing in this file can: **a tick count that does not move.**

If IRQ0 does not arrive on the HP, then the bottom half does not run on
the machine this project is actually built for, and pairing stage 3's
keygen — 15 to 25 minutes of modular arithmetic that was going to yield
through `kpoll_sleep_ms()` — is back to being a frozen machine with no
way to tell a slow prime search from a hang. That would be worth
knowing before the bignum is written, not after.

Everything else here is a measurement to write down.
