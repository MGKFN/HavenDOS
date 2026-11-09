# HavenDOS v0.8.5.2 Beta "Atlas" — the firmware was still driving

v0.8.5.1 fixed the two reasons the xHCI would not work. It then stopped
working for a third reason, and this one only exists on real hardware.

**Every keystroke arrived twice**, which made the installer impossible to
drive on the one machine this whole line of work is for.

## Why

Before an operating system touches an xHCI, **the BIOS owns it.** An SMM
handler drives the controller and presents an attached USB keyboard at
ports 0x60/0x64 as though it were a PS/2 keyboard. That is why a USB
keyboard works in the GRUB menu. On a laptop with no PS/2 port of either
kind — which is the HP — it is the only reason anything could be typed in
v0.8.3 and v0.8.4, when HavenDOS had no USB driver at all.

The xHCI specification has a procedure for taking it over: the **USB
Legacy Support capability**. Set the OS-owned semaphore, wait for the
firmware to drop the BIOS-owned one, then disable its SMI sources so it
cannot be pulled back in.

**v0.8.5.1 never performed it.** So the firmware kept emulating while this
kernel also drove the controller, and one keypress became two:

```
  firmware -> SMM -> port 0x60 -> kbd_poll()      -> one character
  xHCI     -> HID             -> usb_hid_poll()   -> the same character
```

Both paths feed the same queue, by design — that is how a USB key and a
PS/2 key became indistinguishable in v0.8.5. Here it meant doubled input.

Two things made it invisible until a person booted it:

- **GRUB uses only the first path**, so the menu behaved perfectly. The
  doubling started exactly when HavenDOS took over, which is the shape of
  evidence that says "this is your code".
- **QEMU has no SMM legacy emulation.** Every key arrived once, typing
  worked at full speed, and the release looked finished.

## What changed

`xhci_take_from_bios()` walks the extended capability list, finds the
legacy capability, performs the handoff, and disables the firmware's SMI
sources whether or not it let go. It runs **before** the controller is
reset — resetting a controller SMM is still driving is the race the
handoff exists to avoid.

`dmesg` now says which happened, in one of four ways: *taken from the
BIOS*, *the BIOS was not holding it*, *no legacy capability*, or **BIOS
would NOT hand it over; keys may arrive twice**.

### And an escape hatch, because firmware is firmware

`nousbkbd` on the kernel command line turns **our** USB HID input off and
leaves the firmware's emulation to do the typing, exactly as it did
before any of the USB work existed. The ISO carries two menu entries that
set it:

```
HavenDOS -- Install to Disk (firmware keyboard)
HavenDOS -- Shell Mode (firmware keyboard)
```

A firmware that refuses to let go is a thing that happens, and on a
machine whose only keyboard is USB the difference between one keyboard
and none is the difference between installable and not. These entries are
indices 5 and 6, after the four the harnesses count on.

## Testing

**1929 checks across 29 suites**, up from 1909. `tests/test_xhci.c` is
**150**, up from 130. `check_xhci_suite.py` is **54 of 54**, up from 45.

The simulator gained a firmware, and it is worth listing what it had been
quietly getting wrong — because every one of these made a real bug
undetectable:

- **It had no BIOS at all.** The controller was simply free for the
  taking, which is not how any machine that boots firmware behaves.
- **It had one extended capability.** Real controllers start their list
  with Supported Protocol entries and legacy support is further down, so
  a walk that reads only the first entry, or forgets the next-pointer is
  counted in dwords, finds nothing. The list has two entries now, legacy
  second.
- **Its firmware dropped the ownership bit instantly.** Hardware takes a
  moment, so a driver that asks and does not wait sees a refusal it
  caused itself. Three reads now.
- **Its write-1-to-clear register cleared on any write.** That made "the
  latched SMIs are never acknowledged" impossible to detect — the
  simulator was doing the driver's job for it.

Two injections are recorded as **genuinely redundant** rather than
reported as misses, the same way the Link TRB's Toggle Cycle bit was in
v0.8.5:

- Removing the capability walk's loop guard is undetectable because the
  walk cannot spin: a zero next-pointer ends the list and any other value
  is *added* to the offset, so it strictly increases and the bounds check
  already terminates it. The guard caps 16384 iterations at 64. It
  prevents a slow walk, not a hang — and the comment above it claimed
  otherwise until now.
- Removing `v |= XLEG_SMI_STATUS` is undetectable because it is a no-op
  given the read-modify-write around it: the driver reads the register
  first, so a latched bit is already 1 in the value written back, and
  writing 1 to a write-1-to-clear bit is what clears it.

## Known limitations

- **The handoff has never run against real firmware.** QEMU's xHCI has no
  legacy capability at all, so `dmesg` there reads *no legacy capability*
  and the interesting path is proved by the suites only. The HP is the
  first machine to execute it — as it is for the above-4G BAR mapping
  from v0.8.5.1.
- **If the firmware refuses**, keys will still double. Use the *(firmware
  keyboard)* menu entries; `dmesg` will have said so.
- Everything carried from v0.8.5.1.

## What to do with it

The partition is gone, so this is a fresh install, not an `update`.

1. Write `HavenDOS-0.8.5.2.iso` to a USB stick and boot it.
2. Pick **Install to Disk**. If keys double, reboot and pick **Install to
   Disk (firmware keyboard)** instead.
3. Afterwards, `dmesg | grep xHCI` says which way the handoff went. That
   line is the whole of this release.
