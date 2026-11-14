# HavenDOS v0.9.5 Beta "Hephaestus": the apps, rebuilt

*14 November 2026.*

Kyan sent eight photos of an installed 0.9.4.1 and a list: help and man
are out of date, `users` is useless, the guide is out of date, the themes
haven't changed in two years, Settings isn't useful, and the three system
apps are almost the same. Also, the multitasking promised for 0.8.x never
happened: add it or drop the promise.

This release does all of that except multitasking. That is 0.9.6:
cooperative tasks with an Alt+Tab switcher, which is what Kyan chose. The
promise is gone from every screen that made it.

## One command reference: `help`, `man` and the Manual app

There used to be two hand-kept lists. `shell/help.c` was a category
table. `shell/man.c` had 46 pages for about 150 commands. They had
drifted from each other and from the shell:

- man said `ls` read "RAMFS" and that `help 2` showed a second page.
- man was drawn on a fixed 80x25 canvas. It cut every page off at row
  22, and on a big console it sat in the top-left corner.
- help paged every 20 lines whatever the screen height. It had no way
  to ask about one command.

Now there is one table, **`shell/cmddb.c`**: 128 entries covering all 150
command names. Each entry has a name, aliases, a topic, a synopsis, a
one-line summary, a description, examples and related commands.

- **`help`** is an index of nine topics: files, text, shell, system,
  disks, hardware, network, users and apps.
  - `help <topic>` lists the commands in one topic.
  - `help <command>` shows one command's usage in a few lines.
  - `help all` lists everything.
  - A word it doesn't know gets "did you mean".
  - It pages and wraps by the console's real size.
- **`man <command>`** is a full-screen page laid out for the console. It
  scrolls with Up/Down, PgUp/PgDn and Home/End, so nothing is cut off.
  `man <topic>` lists a topic.
- **`man`** on its own is a browser of every command by topic: arrows to
  choose, Enter to open, a letter to jump. The desktop's **Manual** app
  is now this browser, not a one-word prompt.
- **The build checks it** (`tools/check_help.py`, rewritten). It fails
  the build when:
  - a name or alias in the table doesn't dispatch;
  - a command that dispatches is missing from the table;
  - a summary is wider than the 64-column console;
  - a "see also" points at nothing;
  - `help.c` or `man.c` start keeping their own lists again;
  - one of the guide's example lines starts with a command that doesn't
    exist.
- New suite `test_cmddb` (22 checks): lookups by name and alias, topic
  synonyms, and the shape of every entry.

Writing the table meant checking each description against the code. That
turned up commands whose behaviour didn't match any description:

- **`h` ran help.** A default alias `h=help` hid the H language's own
  command, so typing `h` never reached it. The default is gone, and a
  copy saved in `ETC/ALIASES.TXT` by an older release is dropped when it
  loads.
- **`find` only searched the current folder.** Its matching was "ends
  with" or "contains", so `find *note*` found nothing. It's now a real
  glob (`*` and `?`, any case) over the current folder and every folder
  below it. FAT16 lists only its root, so on FAT16 it stays one level.
- **`run` and `source` looked in the root.** `run count.bsh` in your
  home folder didn't find the file. Paths are now resolved from the
  working directory, like every other file command.
- **`ps` printed three processes that don't exist**, with made-up CPU
  figures, and "(multitasking not yet active - planned v0.8.x)". It now
  lists what is actually there: the kernel, the periodic jobs and the
  shell.
- **`df` printed "/dev/vda VirtIO block" and "ramfs 2MB" on every
  machine.** It now reports the system volume: type, size, used, free.
  FAT16 has new `fat16_total_clusters()` and `fat16_free_clusters()` for
  this.
- **`ver`** shows the channel and codename.
- **`chmod` and `chown` work only on the live session's RAM
  filesystem.** On a FAT install they used to say "File not found". They
  now say that FAT16 and FAT32 have nowhere to store locks, and change
  nothing.
- **`color` lasted until the next command.** Every command reset the
  colour to white. The shell's text colour is now applied before and
  after each command and saved. The background stays black: most
  commands draw their own colours on black, and any other background
  came out striped. `color reset` undoes it.
- **`ps1` is saved.** `ps1 reset` restores the default prompt.
- **`neofetch`** names the disk HavenDOS runs from using the block
  layer, and the network adapter using netdev. It used to check VirtIO,
  then ATA, then NVMe "read-only", and had never heard of AHCI, USB disks
  or USB network adapters.

## `users`, and the login log behind it

On 0.9.4 `users` printed "16s admin" on every line (`%-16s` wasn't
supported until 0.9.4.1), and even fixed it showed only a name and a
role. It now shows:

- every account with its role;
- whether it has a password, and whether that password is still the
  **DEFAULT** written in HavenDOS's source (admin and bootos), in yellow,
  with what to do about it;
- its home folder;
- its last login;
- whether it's hidden from the login screen;
- which account is you.

`users <name>` shows one account, with auto-login and its recent
activity.

For "last login" to mean anything, the log needed fixing.
`shell/login.c`'s audit log was a 512-byte buffer with no times in it.
It started empty every boot and overwrote the file, so `log` only ever
showed this boot.

- Every line is now timestamped: `2026-11-14 10:32  kyan  login OK`.
- The file is read back at boot and appended to.
- It's capped at 2 KB, and drops whole old lines.
- Lines in the old undated format are still understood.
- `log` pages by screen height.

Fixed alongside:

- **`passwd` was never saved.** It said "Password changed." and the old
  password came back at the next boot. It now saves.
- **`adduser`** creates the account's home folder, as Setup does.

`test_accounts`: 27 -> 50 checks (log format, rotation, read-back,
undated lines, passwd persistence, password states, roles).

## Themes, redone

Every one of the twenty old themes had a **black** desktop. Switching
theme changed the taskbar and little else. The login screen gave four
of them a pattern and the desktop gave ten, using two separate switch
statements.

- **Twelve themes** come with every install: Haven, Midnight, Terminal,
  Amber CRT, Ocean, Forest, Sunset, Grape, Slate, Paper, Retro and High
  Contrast.
- **themes-extra** adds eight more: Neon, Arctic, Volcano, Desert,
  Lagoon, Mono, Royal and Rose.
- **Each theme has its own look.**
  - Most have their own desktop colour, and every theme but High
    Contrast has a wallpaper: grid, rain, hatch, wave, trees, bands,
    scatter, columns or check.
  - `desk_fg` is a new field, so text on the desktop is readable on that
    colour. The desktop used to write in `bar_fg`, which only worked
    because every desktop was black.
- **One function draws the wallpaper** (`ui_pattern_at`), so the
  desktop, the login screen, Setup's preview and Settings' preview all
  match.
- **No background is a blinking colour.** Colours 8-15 as a background
  blink in VGA text mode.
- **The theme is saved by name** in `.theme`. A number from 0.9.4 or
  older is moved once to the theme most like it.
- **`theme`** lists every theme with a strip drawn in that theme's own
  colours, and switches by number or name.
- `test_theme`: 41 -> 44 checks (save by name, migration, readability,
  wallpaper, the on/off switch).
- `shell/toast.c` had its own copy of `theme_t` with the fields in a
  different order. It now uses the real header.

## Settings, rebuilt

The old Settings app was five tabs in a fixed 72x21 window:

- a theme list;
- visibility switches for three built-in accounts nobody uses;
- two boot flags;
- a hostname and a screensaver number, typed in with no feedback;
- an About tab that said "HavenDOS 0.6.1".

It couldn't change your password, your desktop icons, your console size,
your prompt or your network address.

The new one (`shell/settings.c`) has a sidebar of sections and a page for
each, laid out for the console. Every change is saved the moment it's
made, and every setting is read by the code it describes.

- **Appearance**: the theme, with a miniature of the desktop as a
  preview; wallpaper on/off; the desktop's memory and uptime strip
  on/off.
- **Display**: the console size (fit, max, 80x25 up to 160x50, reset),
  through the same tested path as `resolution`.
- **Desktop**: a checklist of every app for its desktop icon and
  favourite. This edits the same `DESKTOP.CFG` and `FAVORITE.CFG` the
  start menu does.
- **Shell**: the prompt (five presets) and the text colour, with a
  sample.
- **Accounts**: every account with role, password state and last login.
  Change a password, change a role (it won't demote the last
  administrator), delete, hide a built-in account, turn auto-login on or
  off, or create a new account.
- **Network**: adapter, link, address, gateway, DNS and where the
  address came from. Ask DHCP again, set an address by hand, set the
  DNS server.
- **Startup**: the boot log or loading screen, the hardware summary,
  the screensaver delay, the auto-login countdown and the hostname.
- **About**.

`settings <section>` opens on a section. The **Themes** app, which was a
third theme picker, now opens Settings > Appearance.

## One System app

The **System** app (`shell/sysapp.c`) replaces:

- Dashboard;
- System Monitor;
- System Info;
- Info+ (the sysinfo-plus package);
- the full-screen dashboard drawn inside `shell.c`;
- the `sysinfo` text dump, which still said "80x25 VGA Text Mode" on
  framebuffer machines.

Between them they also printed "Multitasking: planned v0.8.x".

It has five tabs:

- **Overview**: version, uptime, clock, CPU, memory and storage bars,
  display, network, user, theme, packages, how it's running.
- **Memory**: physical memory and the kernel heap.
- **Storage**: the system volume, and every disk with its bus and size,
  marking the one HavenDOS runs from.
- **Network**: adapters, MAC, link, address, gateway, DNS, where the
  address came from, and traffic counters.
- **Devices**: every PCI device, decoded; every USB device; the I2C
  pointer.

Other details:

- It refreshes every second.
- It lays itself out for the console, and uses the whole panel when
  opened from the desktop.
- `system <tab>` opens on a tab.
- `dashboard`, `htop`, `sysmon`, `sysinfo` and `sysinfo-plus` all open
  it.
- Desktop icons and favourites for the four old apps become one System
  icon.
- sysinfo-plus is gone from the package catalogue.

## The guide, rewritten

The 0.6-era guide said your files live in RAMFS "and are lost on reboot
(by design)". It printed the built-in accounts' passwords and sent
people to `sysinfo` and `dashboard`.

`shell/guide.c` is ten new pages, written against 0.9.5 and shown in the
same scrolling viewer as `man`:

1. Welcome
2. The desktop
3. The shell
4. Files and disks
5. Scripts
6. Settings and the look
7. Accounts
8. The network
9. Installing and updating
10. When something goes wrong

Left/Right turns pages and `guide <n>` opens on a page.

## Smaller

- The desktop's info strip uses the theme's desktop text colour.
  Light-grey text on Paper's light-grey desktop was invisible.
- `harness/helios_boot.py` pages `help all`, now the long screen.
  `harness/capture.py` knows the System app.
- `tools/check_bus_label.py` exemptions for the USB device list in the
  System app and the `usb` entry in the reference, which name the bus
  and not a disk.

## Not in this release

- **Multitasking** is 0.9.6: cooperative kernel tasks that switch when
  an app waits for a key or yields, background jobs, and Alt+Tab
  between open apps, each keeping its state.
- **File locks on FAT volumes.** `chmod` says so instead of failing
  silently.
- A new install still pins only the Shell to the desktop. Settings >
  Desktop now makes adding icons a checklist.
