# HavenDOS v0.9.6.6 Beta "Daedalus" — what the HP showed in 0.9.6.5

The sixth maintenance update of 0.9.6, from Kyan's test of 0.9.6.5 on
the HP on 17 November. Its message:

- the window system "is cool, it starts to look like a real OS now";
- Alt+Tab and the mouse work;
- the monitor works, but on the unscaled fallback;
- the Shell's buttons don't work;
- full screen should fill the monitor;
- the trackpad still doesn't work ("still the memory window error");
- a mouse, iPhone or stick unplugged and plugged back in doesn't come back;
- the resolution is only applied once the Shell has been opened;
- long lists like the Start menu's All Apps don't scroll.

These are fixes. Full screen filling the screen was his choice of what
full screen should mean, asked on the same day. The features he asked
for alongside them come before HavenDOS 1 as written exceptions E-2 to E-5
in THS-RM-1005 REV C:

- mouse selection and copy/paste in the editor;
- a right-click menu in Files;
- pins on the taskbar and smaller icons;
- real audio through the speakers.

## Fixed

### The Shell's X did nothing

The window's X sends the app an ESC, which closes almost every app. The
Shell's ESC clears the line it is typing, so its X did nothing at all.
The title bar also said "ESC closes" above it.

- **The Shell's X now runs `exit`.** The window remembers that the ESC
  came from the X (`chrome_take_close()`), and the Shell asks.
- **The title bar says "exit closes"** in the Shell.
- A real ESC on the keyboard can never be taken for a click: the
  remembered X is forgotten as soon as any key arrives.

### Full screen fills the screen

An 80×25 app (most apps, every game) used to stay an 80×25 box in the
middle of the screen when made full screen, at the size it already had.
Kyan: "full screen should render the app on full monitor".

- **Now the whole console becomes 80×25 with the biggest characters
  that fit,** keeping their shape. On the HP's 1024×768 they are 12×24
  instead of 8×16, so the app uses 960×600 of the screen instead of
  640×400. What the app had drawn moves with it.
- **F11 or Alt+Enter brings the window back** at the grid you chose,
  with its title bar and the taskbar.
- **It goes back by itself** when the app closes, and before Alt+Tab or
  a taskbar click shows another app. Every other app is laid out for
  your grid.
- Apps that use any size (Settings, the Shell) are unchanged: their
  full screen was already the whole screen.

### A mouse plugged back in never moved again

When something is plugged in or pulled out, HavenDOS walks the whole USB
bus again. That rebuilds its table of devices, but **keyboards and
mice were never bound again afterwards.** Their driver kept pointers into
the old table. So:

- a mouse unplugged and plugged back in was found ("device arrived") but
  never moved;
- plugging in a stick or the iPhone could leave the keyboard or mouse
  driver polling whatever device had taken its place.

The rescan now binds keyboards and mice again, right after the bus walk
and before the sticks and the network adapter. QEMU shows it: a mouse
pulled out and plugged back in gets a second "USB mouse" line, and a
stick plugged in after that comes back as a disk with the mouse still
bound. Without the fix the same test shows one "USB mouse" line and the
mouse is dead.

### The trackpad: its controller had no memory window

On every boot `dmesg` said "I2C 00:15.0: BAR0 is unassigned -- the
firmware gave it no memory window". The HP's firmware leaves the
trackpad's two I2C controllers with no address to talk to them through.
Windows and Linux assign one themselves; HavenDOS never had.

**`drivers/pci_assign.c` assigns one.** Where it goes, and why it is
safe:

- **Above 4 GB, right after a device the firmware placed there itself.**
  On the HP every chipset device sits at 0x60_xxxxxxxx (the USB
  controller at 0x60_02120000). An address right after one of them is
  inside the range the firmware already makes the chipset answer.
- **Never below 4 GB,** where the interrupt controllers, the timer and
  the flash chip live without being listed anywhere this code can see.
- **No other device's registers are touched.** Measuring another
  device's window would mean writing to it while it runs (the screen,
  the keyboard's USB controller). It isn't needed: a window is always
  aligned to its own size, so where it starts is enough to bound how
  big it can be.
- **Written with the controller's memory decoding off, then read back.**
  If the address doesn't stick, the controller is put back to zero and
  left off, and the reason is logged.

`dmesg` now says either "I2C 00:15.0: BAR0 assigned at ..." followed by
what the controller is, or exactly why no window could be given. **This
is the step the trackpad has been waiting on since 0.8.8.4. Whether the
trackpad then answers is what the HP will show.**

### Long lists didn't scroll with the wheel

The desktop and Files read the mouse themselves, so 0.9.6.3's wheel
never reached them.

- **The Start menu:** the wheel, or two fingers on a trackpad, moves
  through the open category three apps a notch. All Apps is longer
  than the screen.
- **Files:** the wheel moves through the list.

### The resolution was only applied once the Shell opened

A resolution you saved was applied only at the Shell's prompt. That rule
dates from before apps were windows, when a big grid would have
stranded 80×25 apps in a corner. Since 0.9.6.5 they are boxed and
centred, so **the saved grid is now applied at boot,** before the
login screen. It applies on the framebuffer console only, and only for a
grid that holds 80×25 (a smaller one is still replaced, as since
0.8.8.5). `dmesg` says "Console: 128x48, as saved". This needs an
installed system to see: a live ISO keeps no settings.

## Not fixed yet, and what would help

- **The monitor on the unscaled fallback.** 0.9.6.4 made the fallback
  say why it was taken. **A photo of `dmesg | grep monitor`** shows the
  step where the scaled picture stopped.
- **"The cursor doesn't appear everywhere."** **Which app or screen?** One
  cause is known: the text editor only looks at the mouse after a key.
  That is rebuilt with E-2 (mouse selection) in 0.9.7.
- **The iPhone after a replug.** The network adapter now comes back, and
  DHCP starts again. Whether the phone shares its connection again
  without typing `pair` is for the HP to say.

## Tests

- **`test_pci_assign`, new, 44 checks.** It runs on a simulated bus laid
  out like the HP's, from its own `lspci`, with the true size of every
  device's window. The planner never sees those sizes; the checks
  compare against them.
  - Both I2C controllers get windows that touch nobody, above 4 GB,
    inside the chipset's cluster.
  - Every write is made with decoding off, and no other device's
    register is written.
  - Each refusal is named.
- **`check_pci_assign_suite`, new:** 12 planted defects, all caught. Its
  first run caught 10; the two that got through led to two new cases in
  the suite.
- **`test_kpoll` 159:** the rescan order now includes binding keyboards
  and mice. `check_hotplug_suite` has 15 defects, all caught, two of
  them new.
- **`test_console` 337:** 80×25 filling the HP's 1024×768 at 12×24,
  pixel for pixel, and back.
- **`test_window` 34:** full screen grows and always comes back (F11,
  another app in front, the app closing), with the app's cells moved
  both ways.
- **`test_i2c` 55:** an unassigned window is asked for once; a refusal
  means no bus.
- 7472/7472 checks across 72 suites.
- **QEMU:**
  - `fixes_0966`, new, 9/9: the Shell's X, full screen filling, a mouse
    and a stick unplugged and plugged back in. Against a build without
    the rescan fix, the mouse checks fail.
  - `window_apps` 11/11 and `mouse_apps` 14/14.
