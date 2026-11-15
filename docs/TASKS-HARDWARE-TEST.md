# 0.9.6 on the HP: several apps at once

Five minutes. QEMU has already run everything below on a PS/2 keyboard.
What it cannot show is the **USB keyboard's Alt key** and the HP's
framebuffer. Boot the normal (first) GRUB entry and sign in.

## 1. Alt+Tab, held

1. Open the Shell (Tab on the desktop), then type `open notes`.
2. In Notes, press A, type a few words and press Enter.
3. **Hold Alt and tap Tab once. Keep holding Alt.** A panel called
   "Open apps" should appear in the middle of the screen, listing Notes
   (in front), Shell and Desktop, with Shell highlighted.
4. Tap Tab again while still holding Alt. The highlight moves to Desktop.
5. **Let go of Alt.** You should be on the desktop.

| what you see | what it means |
|---|---|
| the panel comes and goes as above | it works, on USB too |
| the panel appears but letting go of Alt does nothing; Enter switches | the USB keyboard's Alt release is not reaching us. Photograph `dmesg . grep kbd` after booting with `kbddiag` |
| no panel; a Tab appears in Notes | Alt is not being seen at all. Same photograph |
| no panel, nothing happens | Alt+Tab arrived and the switcher did not draw. Photograph the screen, then `ps` |

## 2. The bar

On the desktop the bottom bar should show **Shell** and **Notes** chips
between your name and the alerts. Click **Notes**: it comes back with
your text, exactly as you left it.

## 3. A song behind

1. Back on the desktop, open Media > Music Player and press 2.
2. While it plays, Alt+Tab to the desktop.

The song should keep playing, and its chip should be **green**. A song
that stops or stutters badly when you switch is the thing to report.
This also checks the speaker: the Music Player never played a note
before 0.9.6.

## 4. The lock

In a Shell, type `lock`, press a key, then **Alt+Tab**. The panel
should say "The screen is locked: sign in first", and you should stay
on the lock screen. Type your password to get back.

## 5. If anything is odd

    ps
    dmesg . grep task

`ps` shows every task and its state; `dmesg` shows a task that was
stopped for overrunning its stack, if any was. **`notasks` on the boot
line** (press `e` in GRUB) turns all of this off and runs one app at a
time, exactly as 0.9.5.1 did.
