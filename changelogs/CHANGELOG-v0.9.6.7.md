# HavenDOS v0.9.6.7 Beta "Daedalus" — the boot no longer waits on the trackpad

The seventh maintenance update of 0.9.6, from Kyan's photo of 0.9.6.6 on
the HP, 17 November. Fixes only.

## What the photo shows

The boot splash stopped with:

```
I2C 00:1F.5 at FE010000: not a DesignWare core (type 00000000)
press any key for the boot log
```

No key did anything.

That line is the last of the I2C step, so the boot had got past
assigning the trackpad's controllers their memory window, which 0.9.6.6
added. Since 0.8.8.4 they had never come up, so **the search for the
trackpad had never run on the HP.** 0.9.6.6 made it run for the first
time, and it held the boot.

- **Each I2C wait was 2,000,000 register reads.** In QEMU that is
  nothing. On the HP it is one to two seconds.
- **The search tries 112 addresses on each controller.** If the
  controller never moves the bus (for example, if its clock is not
  running), every address waits its full time. That is many minutes.
- **It ran before USB.** On the HP the keyboard is USB, so "press any
  key" could not be answered until the search was over.

## Fixed

- **The keyboard comes first.** I2C and the trackpad search now run after
  USB and the USB keyboard and mouse.
- **A wait has a clock.** An I2C wait now ends after 50 ms of the timer,
  a hundred times the longest transfer it makes, as well as after the
  spin count.
- **A controller that times out is left alone.** A NAK is an answer
  ("nobody here") and the search goes on. A timeout means the controller
  is not moving the bus at all, so the first one marks it stuck and every
  later transfer is refused at once. The log says so once:

  ```
  I2C 00:15.0: a transfer timed out -- the controller is not moving the
  bus, so it is left alone this boot
  ```

  `i2c` lists it as "timed out at boot -- left alone".
- **`noi2c` leaves the whole step out:** no controllers, no search. Press
  `e` on a boot entry in the boot menu and add it to the `multiboot` line.
- **The boot log says when the step starts:** "I2C: looking for the
  trackpad's controllers", then "I2C: searching 2 controllers for the
  trackpad".

## Getting 0.9.6.7 onto an installed 0.9.6.6 that will not boot

1. **In Windows:** run `Update-HavenDOS.cmd` from this bundle as usual.
   It puts the new kernel on the EFI partition.
2. **Boot this release's ISO from a USB stick** and choose "Update
   installed system from the ESP". That runs 0.9.6.7 itself, with the
   installed partition mounted.
3. **Run `update`.**

Then boot normally. If the trackpad's controllers time out, the log now
says so and the boot carries on.

## What to photograph

`dmesg | grep I2C` after it boots. The lines say:

- where each controller's window went;
- whether each one is a DesignWare core;
- whether its first transfer worked, timed out, or found the trackpad.

## Tests

- **`test_i2c` 62**, up from 55. A simulated controller that never moves
  the bus:
  - a probe on it times out within a bounded number of register reads
    while the timer runs, and within the spin count when it doesn't;
  - the bus is then stuck and not ready, and the next transfer is refused
    with no register read at all;
  - a whole scan stops at the first timeout;
  - a NAK leaves the bus alone.
- **`check_i2c_suite`:** 18 planted defects, all caught, four of them new.
  A fifth, a stop guard in the scan, was MISSED. That was correct, since
  a stuck bus already refuses every probe without reading anything, so
  the guard was removed rather than kept untested.
- 7479/7479 checks across 72 suites.
- **QEMU:** `fixes_0966` 9/9 and `hid_boot` 3/3. QEMU has no DesignWare
  controller, so the I2C path itself only runs on the HP.
