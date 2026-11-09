# Trackpad and pairing — the hardware test for v0.8.8.11

Ten minutes on the HP. Two commands, and both of them are built to be
photographed rather than described.

Nothing here has ever run on hardware. Every byte of both protocols is
from its specification and every path is tested against a mock that
implements it — which proves the code does what the spec says and
proves nothing at all about what is actually soldered to your I2C bus.

---

## Before you start

Boot the **0.8.8.11 ISO** (not an installed copy, unless you have
updated it). Log in. You need:

- the HP, with its own trackpad — nothing plugged in
- an iPhone and its cable, for part 2
- a camera

---

## Part 1 — the trackpad, five minutes

### 1.1 Is the bus even there

```
lspci
```

Look for **00:15.0** and **00:15.1**, `8086:51E8` and `8086:51E9`,
class 0C/80. If they are not listed, the firmware has Serial-IO
disabled and nothing below will work — that is a BIOS setting, not a
HavenDOS problem. **Photograph the screen** either way.

### 1.2 What the driver found at boot

```
i2chid
```

**Photograph this screen.** It is the whole test. Three possible
answers:

**(a) It found a pointer.**

```
bus 0  addr 2c  descriptor register 0020
  vid 04f3  pid 3098  ver 0001
  report descriptor 412 bytes at 0002, input 32 bytes at 0003
  POINTER: absolute X/Y, 2 button(s), multitouch, report id 4
  25 field(s) parsed, report 56 bits
```

Then go to 1.3. This is the good case and it means the register probe
worked.

**(b) It found a device but no pointer in it.**

> `found, but no X and Y in its report descriptor`

That is a real I2C-HID device whose report descriptor does not
describe a pointer — most likely the internal keyboard. Photograph it;
the vendor and product id are what identify it.

**(c) It found nothing.** Then `i2chid` rescans and prints every
address that answered:

```
No HID device found. Scanning again to show what IS there:
  bus 0: 2 address(es) answered: 15 2c
```

**That is the most useful screen in this test.** It says the bus works
and the descriptor register is not one of the six tried (01, 20, 00,
02, 05, 10). Send me the photograph and the next release tries the
right one.

If it says `bus 0: nothing answered`, the bus is up and nothing is
listening — different problem, also worth the photograph.

### 1.3 Does it move the pointer

Open anything with a cursor:

```
desktop
```

Now **put one finger on the trackpad and move it.**

| what you see | what it means |
|---|---|
| the pointer tracks your finger | it works |
| the pointer moves but feels slow or jumpy | it works; `MOUSE_DIVISOR` needs tuning for this pad's resolution |
| the pointer jumps to a corner and stays | the coordinates are being read at the wrong bit offsets |
| nothing moves | the device was found but is not reporting — try pressing it first, some pads sleep until touched |

Then the one that matters:

> **Lift your finger. Put it down somewhere completely different on
> the pad. The pointer must NOT move.**

If the pointer jumps when you replace your finger, the
absolute-to-relative conversion is wrong and I want to know
immediately — that is the difference between a trackpad and a drawing
tablet and it is three lines of code.

Then **tap** it, and **click the physical button** if it has one.

---

## Part 2 — pairing, five minutes

### 2.1 Plug the phone in and unlock it

The phone must be **unlocked** — awake, passcode entered, on the home
screen. A locked phone answers `PasswordProtected` and that is a
correct refusal, not a bug.

```
usb
```

It should list the iPhone. Then:

```
usb lockdown
```

This is stage 1 and 2 and has worked since 0.8.7.9. If it fails here,
stop — pairing cannot work and the problem is not pairing.

### 2.2 Ask for trust

```
pair
```

It narrates every step. **Photograph the whole screen**, because the
interesting part is which step it reached:

```
  connecting to lockdownd...
  asking for DevicePublicKey...
  the device's key is 2048 bits.
  generating the root key (2048-bit)...
  root key: done in 4120 ms
  generating the host key (2048-bit)...
  host key: done in 3980 ms
  building the certificates...
  root 712, host 697, device 701 bytes.
  HostID C1A2B3C4-D5E6-47F8-9A0B-1C2D3E4F5A6B
  the request is 3705 bytes, 2 mux packets.
  sending it...

  the device is asking the user -- tap Trust
```

**Those two "done in" numbers are a measurement nobody has ever
taken.** The keygen has only ever been timed in a container, where one
Miller-Rabin round is 5.2 ms. If it is a minute each on the HP, that
is the figure the next release plans around. Any key aborts the
generation, so you are never stuck.

### 2.3 The phone

**Look at the phone.** It should be showing:

> **Trust This Computer?**

If it is: that is the whole point of this release. **Photograph the
phone as well.**

Tap **Trust**, then on the HP:

```
pair
```

again. Expected: `paired`.

### 2.4 What the answers mean

| HavenDOS says | what to do |
|---|---|
| `the device is asking the user -- tap Trust` | tap it, run `pair` again |
| `paired` | it worked |
| `the user tapped Don't Trust` | run `pair` again and tap Trust |
| `unlock the device with its passcode first` | unlock it properly, retry |
| `the device does not know this HostID` | expected on a second run: the record is not saved, so every `pair` invents a new HostID |
| `the device refused and named a reason` | `usb lockdown` then `dmesg` prints the reply verbatim — photograph that |
| `nothing recognisable in the reply` | same: `usb lockdown` then `dmesg` |

### 2.5 Known and expected

- **The pair record is not saved.** A reboot, or even a second `pair`,
  starts over with a new HostID and the phone asks again. Storing it
  is the next release.
- **Pairing does not give you a working connection yet.** The TLS
  session that follows needs client certificates, which `net/tls.c`
  does not do. So the phone trusting us is as far as this goes — and
  it is the step everything after it depends on.

---

## Part 3 — the USB mouse, one minute

This is a regression test for a freeze, not a feature test.

Plug in a USB mouse. Then:

```
notepad
```

**Right-click in it.**

Before 0.8.8.11 this froze the machine with a USB mouse — the wait
loop polled nothing and read no keys. Now the context menu should
appear. If it still hangs, ESC should get you out after two seconds;
if ESC does not work either, the fix is wrong and that is worth
knowing straight away.

Also worth a try: the pointer should move while nothing is reading the
keyboard, and a click should register in a click-only consumer:

```
chat
```

---

## What to send back

1. `lspci` (part 1.1)
2. `i2chid` (part 1.2) — **the most important one**
3. whether the pointer tracked, and whether the lift-and-replace test
   passed
4. `pair` in full (part 2.2), including the two keygen times
5. a photograph of the phone if the Trust dialog appeared
6. whether right-clicking in Notepad still freezes

Six photographs and three sentences settles everything in this
release that a container cannot answer.
