# Mouse and monitor on the HP (v0.9.6.3, v0.9.6.4)

**0.9.6.4:** if the monitor comes up *unscaled* (a box in the middle,
with a notification saying so), run `dmesg` and photograph the line
"monitor: scaling stopped at ...". It names the exact register.

Photograph anything that looks wrong, and write down what you did just
before it.

## 1. The monitor fills the screen

1. Update and reboot with the LG plugged into the HDMI socket. **Don't
   type `gfx on`.**
2. **Within a few seconds of the desktop appearing:**
   - the monitor should show HavenDOS filling its height, with thin
     black bars left and right;
   - a notification should say "Monitor on: fitted, shape kept".
   Photograph the monitor.
3. Settings > Display > HDMI monitor > Picture: click it twice to switch
   between **fitted** and **stretched**. The monitor should change
   straight away. Photograph both.
4. Is the text sharp enough to read on the monitor? Say which setting
   reads better.
5. Unplug the HDMI cable, wait 3 seconds, and plug it back in. The
   monitor should light again by itself. If it doesn't, run `dmesg` and
   photograph the lines that start with "monitor:".
6. Settings > Display > Light it by itself: set it to **off**, unplug
   and replug. The monitor should stay dark until `gfx on`. Then turn
   the setting back on.

## 2. The mouse in Settings

Use the trackpad, then a USB mouse if you have one.
1. Open Settings. Click **Display** in the sidebar: its page should
   open.
2. Click a row once: it is selected. Click it again: it changes or
   opens.
3. Scroll with two fingers on the trackpad, or with the mouse wheel:
   the selection should move up and down.
4. Right-click: back to the sidebar. Right-click again: Settings closes.
5. Click **ESC close** in the grey bar at the bottom: Settings closes.

## 3. The mouse in other apps

Open a few apps (Calculator, Clock, Reminders, a game) and try:
- clicking the key hints in the bottom bar;
- the wheel;
- right-click to leave.

Note any app where the pointer leaves marks behind, or where a click
does something you didn't expect.

## 4. Windows (v0.9.6.5)

1. Open Settings from the Start menu: a title bar on top
   (`_  [ ]  X`), the taskbar on the bottom row.
2. Minimize: the desktop comes back, and Settings is a chip on the
   taskbar. Click its chip in an app's taskbar to bring it back.
3. Full screen: the bars go. **Alt+Enter** (or F11) brings them back.
4. X closes it, like ESC.
5. Open Settings from the desktop's **right-click menu** too, and check
   the mouse works there (this was the 0.9.6.4 bug).
