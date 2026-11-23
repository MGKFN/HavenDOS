# HavenDOS v0.9.7.4 Beta "Mnemosyne" — USB comes back every time

Kyan, 23 November, on 0.9.7.3 on the HP:

> unplugging and replugging works once after that nothing all usb stuff
> gone

> **Not tested on a real machine.** Tested in QEMU and on the build
> machine. Nothing in it has run on the HP yet.

## Fixed: USB devices gone for good after a replug or two

The USB controller gives each device a *slot*, and the HP's controller has
8 of them. A slot was handed back only when its device had been set up
completely.

- A replug is exactly when a device is half there. Its contacts settle
  while HavenDOS is reading it, the read fails, and that device's slot
  was never handed back.
- The webcam and Bluetooth take two slots on every scan, and the mouse
  and stick two more. A couple of failed replugs used up all 8. After
  that every device was refused: "all usb stuff gone", until a reboot.

Now:

- **Every failed setup hands its slot back**, at every step where it can
  fail.
- **Before each scan, any slot nobody owns is handed back.** If the
  controller refused a slot during the last scan, every slot is cleared
  first.
- The log says when either happens:
  - `xHCI: N slots were held by no device -- given back`
  - `xHCI: Enable Slot was refused last walk -- every slot disabled`

  `dmesg | grep xHCI` will show whether this was the HP's problem.

## Fixed: a scan that looked like a replug

- 0.9.7.3 noticed quick replugs through the port's *change* bit.
- But a scan resets each port, and a USB 2 device can disconnect and
  reconnect during that reset, which sets the bit again. The next check
  took that for a replug and scanned again. Each scan restarts the mouse,
  so it stuttered or froze until the storm guard stepped in.
- Now the change bits are cleared after each scan, so only a change that
  happens after the scan counts.

## Tests

- `make test`: 77 suites, 7740 checks.
  - test_xhci 358, 14 new:
    - a device whose descriptor read fails 24 times;
    - the 18-byte read failing;
    - Address Device refused;
    - slots the controller kept after Enable Slot was refused.

    With the old code these give exactly the HP's symptom: the device is
    lost after a few tries and all 8 slots are held by nothing.
  - test_kpoll 168: a scan that sets the change bit itself does not
    start another scan.
- QEMU harnesses:
  - replug_0973 11/11, now with ten more mouse replugs, more than the
    8 slots;
  - usb_storage 52/52.
