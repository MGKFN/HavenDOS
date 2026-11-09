# HavenDOS v0.8.8.5 "Helios beta 6" — the stick works, the console fits the panel, and the heap is 125x bigger

Channel Beta, codename "Helios", **beta 6**. 2 November 2026.

| release | called | what it was |
|---|---|---|
| v0.8.8 | **Helios beta 1** | the console grid, and the timer tick |
| v0.8.8.1 | **Helios beta 2** | the mouse, exFAT, the bignum |
| v0.8.8.2 | **Helios beta 3** | the stretch, the stick, the primality bug |
| v0.8.8.3 | **Helios beta 4** | the keygen in seconds, and an I2C bus |
| v0.8.8.4 | **Helios beta 5** | the app guard, the viewport, Minefield |
| v0.8.8.5 | **Helios beta 6** | this one |

---

## 0. What the HP said about beta 5

Three findings from one `dmesg`, and the first of them closes an item
that has been top of the open list since v0.8.6.

### THE STICK ENUMERATES

```
ok  USB xHCI hc0 port 12: 0951:1666 class 08, address 1, 2 endpoint(s)
ok  USB MSC Kingston DataTraveler 3.0: 29560 MB, 512-byte blocks, removable
```

`0951:1666` is Kingston; 29560 MB is the 32 GB stick. **Port 12 — the
SuperSpeed port that failed for five releases — enumerated, got an
address, and registered as a block device.**

What this does *not* yet say: nothing mounted a filesystem off it, and
nothing should have at boot. `FAT16: mounted, VFS ready` is the
installed system's own partition on the NVMe — `fat16_init()` runs
before `vfs_init()` and takes the first usable disk. **Reading the
stick is still unproved** and needs `fat32`, `exfat` and `update`
against it.

### THE APP GUARD HELD ON REAL SILICON

```
warn appguard: Shell faulted -- divide by zero at 001794E9
Kyan@havendos:~#
```

`crash div` in the Shell at t+201s. The guard caught `#DE`, restored
the desktop's stack, logged it, and the prompt came back. On an Alder
Lake, not just in QEMU — which is what confirms the `iret` reasoning
in `kernel/appguard.c`'s header holds on real hardware.

### I2C IS NOT A DRIVER PROBLEM

```
warn I2C 00:15.0: BAR0 is unassigned -- the firmware gave it no memory window
warn I2C 00:15.1: BAR0 is unassigned -- the firmware gave it no memory window
warn I2C 00:1F.5 at FE010000: not a DesignWare core (type 00000000)
```

Beta 4's instrumentation paying for itself. **The firmware assigned
the two Serial-IO controllers no memory window at all** — a common
arrangement on modern Intel laptops, where they are described only in
ACPI for the OS driver to place. And `00:1F.5` is the SPI controller
wearing the same class code; the `IC_COMP_TYPE` check is what stopped
the driver writing the I2C register map into it.

So the trackpad is unreachable because there is **nothing to talk
through**, which is tractable: assign the BAR, enable memory decode,
then probe. PCI resource assignment is something this kernel has never
done and is a bounded piece of work.

---

## 1. "Some apps still go off the screen" — measured, and it was the console

> *"some apps still go off the screen and aren't show fully maybe bc of
> 16:10???"*

**The 16:10 was exactly right.** 80x25 of an 8x16 cell is 640x400,
which is 16:10. On a 16:9 panel the console can only ever occupy a
letterboxed window in the middle — 1280x800 of 1920x1080, at the
largest whole scale that fits.

### `layout` — the machine audits itself

There are forty-six apps and guessing which three is how a release
fixes the wrong ones. So `vga_write()` — the one function every cell
goes through — counts every write it clips and keeps the worst
coordinate, and a new `layout` command opens every app in turn and
reports.

```
layout          open every app and report what draws off-screen
```

**43 of 43 auditable apps opened. Not one drew outside its console.**
The apps were never the problem.

It runs on the HP, which no harness ever will, and the result is in
`dmesg` as one line per app that overflowed.

### So the grid fits the panel now

`shell/shell_v076.c` has carried this as a known gap since the grid
became persistent:

> *"The auto-detect-at-boot that RM-1003 asks for still needs the
> viewport layer; this does not."*

**The viewport layer exists** (beta 5), so the reason not to do it is
gone: an app that lays out for 80x25 is boxed and centred rather than
stranded in a corner. On a machine that has never chosen a grid,
HavenDOS now picks the one that fits:

| panel | grid chosen |
|---|---|
| 1280x800 | 80x25 — no change, it already fits exactly |
| **1920x1080** | **120x33** |
| 2560x1440 | bigger, with the glyph still ≥16 px wide |
| 640x384 | **refused** — see below |

Glyphs stay the same physical size; there is simply more screen.
`resolution max` is still the 8-pixel cell nobody wanted. **A saved
grid is never overridden** — a zero in the config means "never
chosen".

> **The decision is a function, not an `if` in the boot path.**
> `vga_fb_boot_grid()` exists because a boot-path `if` can only be
> tested by booting, on a panel of the right size — and the panel QEMU
> hands this project is 1280x800, which `tests/test_console.c`'s own
> header calls *"the one panel on which every v0.8.8 console change
> could be a no-op and look perfect"*. That is exactly what happened:
> the first version appeared to do nothing and there was no way to
> tell that from being broken. It is twelve lines with no hardware in
> it now, driven against four simulated panels.

> **FOUND BY BOOTING IT:** on a 640x384 framebuffer the readable grid
> is 40x12 — *smaller* than the 80x25 every app lays out for. Taking
> it would have been this very complaint, backwards: nothing boxed,
> every app drawing off the right-hand edge. It only ever goes
> upwards now.

---

## 2. The memory ceiling, and it was never the address width

> *"more ram by maybe making it 64 bit?? Opening up an new world of
> possibilities"*

Two things were capping it, neither of them 64-bit:

**`kernel/heap.c` was a 256 KB static array in bss** that never touched
the physical allocator. That was the entire dynamic-memory budget of
the OS, on any machine. It takes an eighth of free memory from the PMM
now.

**`kernel/pmm.c` clamped at 2 GB** while `kernel/main.c` already clamps
the loader's figure to 4 GB — so half of what a flat 32-bit map can
reach was being thrown away.

Measured on real boots:

| machine | heap before | heap now | PMM before | PMM now |
|---|---|---|---|---|
| 256 MB | 256 KB | **31 MB** | 255 MB | 255 MB |
| 2 GB | 256 KB | **64 MB** | 2047 MB | 2047 MB |
| 4 GB | 256 KB | **64 MB** | 2047 MB | **3071 MB** |

The 64 MB cap is deliberate: `kmalloc` is first-fit over a singly
linked list, so an unbounded pool gets **slower** the more memory the
machine has. Size classes are their own release.

> Writing the new PMM ceiling the obvious way —
> `(4096UL*1024*1024/PMM_BLOCK_SIZE)` — is **wrong on this target**.
> `unsigned long` is 32 bits, the multiply happens before the divide,
> and 4096·1024·1024 is exactly 2³² — so it wraps to 0, the bitmap
> becomes a zero-length array and the machine gets no memory at all.
> It is grouped as `4096 * BLOCKS_PER_MB` to keep every intermediate
> inside 32 bits.

The boot log says which pool it got and warns when it is at the 4 GB
ceiling, so unreachable memory reads as a stated limit rather than
memory that vanished.

### `kernel/heap.c` was the oldest untested file in the tree

Seven bug fixes listed in its own header — double-free detection,
magic validation, bounds checks, a `krealloc()` that dereferenced
`p-12` before validating `p` — every one found by reading, and **no
suite at all**. It has 62 checks now and
`tools/check_heap_suite.py` plants 13 defects and catches all 13.

The suite found a real defect before any injection: **`heap_init()`
was not idempotent.** A call refused by the physical allocator still
described the pool a *previous* call was given, and reported success
with memory it did not have. The shipped kernel calls it once, which
is what kept it latent.

And the harness found **five of the suite's own checks could not
fail**:

| check | why it could not fail |
|---|---|
| `krealloc` copying the wrong length | shrinking copies the same bytes either way — only **growing** differs. It now looks for a neighbour's data in the grown buffer |
| `krealloc` freeing the original on failure | nothing ever made the inner `kmalloc` fail |
| a stray pointer into `krealloc` | the magic check was catching what the *bounds* check was for. The suite now builds a **convincing fake header** outside the pool |
| the 64 MB ceiling | on a 16 MB test pool, capped and uncapped both fell back to 256 KB — identical results |
| poisoning on free | nothing checked it at all |

---

## 3. Pairing stage 3 begins: a DER writer

Everything stage 3 needs is built on this. An X.509 certificate **is**
DER, a PKCS#1 signature is computed over DER, and the pair record
carries three certificates — so a quiet error here is a certificate
the phone rejects with no reason given, three layers below where the
symptom appears.

`net/der.c` calls nothing but `memcpy`, `memmove` and the bignum's
byte export, so `tests/test_der.c` links it directly: **no seam, no
stub, no simulated hardware**, and every expected byte string is
written out by hand from X.690's rules rather than computed with the
same arithmetic as the encoder.

**57 checks. `tools/check_der_suite.py` plants 16 defects and catches
all 16.**

### The two that actually break RSA

**The sign byte.** A DER INTEGER is *signed*. A 2048-bit modulus has
its top bit set by definition, so it must carry a leading `00` and
encode as **257 content bytes, not 256**. Drop it and every verifier
in the world rejects the certificate — and the hex dump looks
perfectly reasonable.

**The minimal length.** 200 bytes of content is `81 C8`. `82 00 C8`
parses as BER, is not DER, and a strict parser refuses it — while a
lenient one accepts it, so whether it works depends on which parser is
at the other end and nothing ever says which.

### And the one the harness found about the suite itself

The injection *"the sequence contents are copied, not moved"* — a
`memcpy` over regions that overlap whenever a header is shorter than
its contents — **was MISSED**, because glibc's `memcpy` happens to
survive that overlap.

**The kernel does not use glibc.** `kernel/string.c`'s `memcpy` is
`while(n--) *d++ = *s++;` — strictly forward, byte by byte, as its own
v0.7.12 note says — so the destination clobbers what it has not read
yet, deterministically. The suite links the kernel's `string.c` now
and tests the `memcpy` **that actually ships**.

---

## 4. Nothing long-running stops the clock, continued

Beta 5 fixed eight loops. The new app's event loop joins them, and the
`layout` audit injects keys through `keyboard_inject()` — the same
entry `usb/hid.c` uses — so every app can be driven to its exit.

> **Third time in one release for the same bug class.** The audit left
> 240 injected keys per app in the ring, which dismissed its own
> summary, echoed at the shell prompt, and **corrupted the next
> command typed** — so `dmesg | grep layout` came back empty and the
> audit looked like it had found nothing. The crash notice was
> dismissed by a dead app's unread ENTER; the loading screen was
> dropped by GRUB's. A key meant for something else is not an answer
> to a question that has not been asked.

---

## The numbers

| | v0.8.8.4 | v0.8.8.5 |
|---|---|---|
| suites | 48 | **50** |
| checks | 4635 | **4773** |
| injection harnesses | 30 | **32** |
| boot harnesses | 13 | **14** |
| `test_der` | — | **57**, 16 of 16 injections |
| `test_heap` | — | **62**, 13 of 13 injections |
| `test_console` | 259 | **278**, 28 of 28 injections |
| heap on a 2 GB machine | 256 KB | **64 MB** |
| PMM on a 4 GB machine | 2047 MB | **3071 MB** |

## What is NOT verified

- **Reading the 32 GB stick.** It enumerates and registers as a block
  device; `fat32`, `exfat` and `update` against it have not been run.
- **The grid fitting a 1920x1080 panel on real hardware.** Proven in
  the suite against a simulated panel; QEMU only ever hands this
  project 1280x800, which is the one size where the answer is "no
  change".
- **The I2C controllers still have no BAR**, so `i2c scan` has still
  never scanned anything, anywhere.
- **The DER writer has never encoded a real certificate** — there is
  no X.509 layer yet. 57 checks against hand-written byte strings is
  what it has.
- **Still unconfirmed from earlier betas:** the USB mouse, exFAT on a
  real stick, any TLS handshake since `rsa_public_encrypt()` moved
  onto the bignum.

## Still open

- **I2C-HID, now with a known cause.** Assign BAR0 for `00:15.0` and
  `00:15.1`, enable memory decode, then scan. The trackpad is behind
  that and nothing else.
- **Stage 3 piece two**: X.509 v3 construction and PKCS#1 v1.5
  signing, then the plist writer and PEM→DER base64 — the third
  base64 decoder in the tree, and the release where they should
  become one.
- **A real allocator.** 64 MB is capped because first-fit over a
  linked list does not scale; size classes should come before any
  more capacity.
- **64-bit**, scoped as its own release — see
  `havendos-xe-graphics-and-64bit.md` for what it buys and costs.
- **A GPU driver is not on this list**, and that document says why.
- **NTFS read-only**, **ext4**. Requested, not started.
