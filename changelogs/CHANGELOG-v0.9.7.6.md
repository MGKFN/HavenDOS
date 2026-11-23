# HavenDOS v0.9.7.6 Stable "Mnemosyne" — the 0.9.7 line is Stable, and USB devices come back after a replug

Kyan, 23 November, after testing 0.9.7.5 on the HP:

- "only one USB works after the unplugs, but plugging back in works";
- "0.9.7 releases are stable now".

> **Not tested on a real machine.** Built and tested on the build
> machine and in QEMU. The USB fix is based on what Kyan described.
> There was no photo of the failure itself, so the fix covers the most
> likely cause, and the log now names the port if it is something else.

## Stable

- **0.9.7.6 is the first Stable release.**
  - On GitHub it is a full release, so it is the one "Latest" points to.
    0.9.7.0 to 0.9.7.5 stay pre-releases.
  - The early-access warnings are gone: the boot splash, the notice after
    it, the setup wizard and `ver`.
- **Updating from a Beta is fine:** a system on the Beta channel accepts a
  Stable update.
- **After updating, a system follows Stable.** This is when it has no
  `/boot/channel.inf`. Every later 0.9.7 release is Stable as well. To
  keep getting Betas, use `update channel beta`.

## Fixed: a USB device that does not come back after a replug

**What happened.** A plug event walks the whole bus, and the walk resets
every port, including ones nobody touched. Some devices are slow to
answer after a reset, such as a wireless keyboard/mouse receiver, or a
stick that is still settling after being pushed in. A device like that:

- did not answer in time;
- was written off at once with "no answer";
- stayed gone until the next plug event walked the bus again.

There was a retry, but it only fired when a walk found *nothing*. A
walk that found the stick and lost the keyboard found one device, so
nothing tried again. That matches "only one works; plugging it back in
works".

**Now:**

- **During the walk:** a connected port that does not answer is tried
  twice more, 100 ms and then 250 ms later. The USB spec gives a device
  100 ms to settle after it connects. A port that empties in the
  meantime is left alone. The log says `answered on try 2 of 3`, or
  `3 tries, no answer`.
- **After the walk:** an occupied root port with no device behind it
  walks the bus again, 1 s and then 2 s later. If the device still does
  not answer, the log names the port: `usb: xHCI hc0 port N: something
  is plugged in and does not answer -- unplug it and plug it back in`.
  Each plug event gets two retries, so a broken device is never retried
  endlessly.

If it still goes wrong, `dmesg | grep usb` now shows which port, and
whether its device answered.

## Retro PC

`emu/` is RetroPC 0.1.0 (1cecc78), the same as 0.9.7.5's. Retro PC as a
Windows and Linux program now has its own repo and releases:
[MGKFN/RetroPC](https://github.com/MGKFN/RetroPC). This release no longer
carries the RetroPC downloads.

## Tests

- `make test`: 77 suites, 7755 checks.
- QEMU: `harness/replug_0973.py` 11/11 on xHCI. It covers:
  - mouse, stick and keyboard unplugged and plugged back, slowly and
    within one second;
  - ten more replugs than the controller has slots.
- test_xhci 358: two checks now expect the retry. An Enable Slot refused
  once is recovered by the second try in the same walk, and the leaked
  slots are still swept by the next walk. "A refused command makes no
  device" now refuses every try.
- **test_kpoll** 177 (+9):
  - a walk that finds some devices but not all is walked again, and both
    are back;
  - a device that never answers costs two walks per plug event, and a new
    plug event gets two more;
  - a walk that finds nothing is retried twice, 1 s and 2 s apart.

  Run against 0.9.7.5's `usb/hotplug.c`, seven of these fail.
- **test_hub** 103 (+4):
  - a root device that does not answer at first is in the table after its
    third try, having waited 100 ms and 250 ms;
  - one that never answers gets three tries;
  - one unplugged after its first try gets no more.

  Against 0.9.7.5's `usb/core.c`, three fail.
