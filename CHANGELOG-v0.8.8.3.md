# HavenDOS v0.8.8.3 "Helios beta 4" — the keygen is seconds, and there is an I2C bus

Channel Beta, codename "Helios", **beta 4**. 2 November 2026.

| release | called | what it was |
|---|---|---|
| v0.8.8 | **Helios beta 1** | the console grid, and the timer tick |
| v0.8.8.1 | **Helios beta 2** | the mouse, exFAT, the bignum |
| v0.8.8.2 | **Helios beta 3** | the stretch, the stick, the primality bug |
| v0.8.8.3 | **Helios beta 4** | this one |

Two things, and the first of them is a number.

---

## 1. `rsa` — and RSA-2048 takes about two seconds

`net/rsa.c` shipped in beta 2 with Miller-Rabin, a stepping key
generator, 361 suite checks and a harness that catches all eighteen of
its planted defects — **and nothing in the shell called any of it.**
There was no way to type a command on the laptop and find out whether
a key generates there at all. Same shape as `test_rsa` sitting in the
tree without being in `make test`: the work existed and could not be
reached.

```
rsa                   what it can do
rsa keygen [bits]     generate a key (default 2048)
rsa show              the last key
rsa verify            check it again
rsa prime <n>         is n prime?
```

### The number

`HANDOFF.md` has said **"a full RSA-2048 keygen is 15 to 25 minutes"**
since the bignum was scoped. That came from measuring the old
byte-wise `bn_powmod` in `net/tls_crypto.c` at 0.073 s per exponent
byte. Montgomery replaced the inner loop in beta 2 and nobody has
measured anything since.

Measured in QEMU, ten runs:

| size | time | candidates |
|---|---|---|
| 512 | 0.44 s | 95 |
| 1024 | 0.83 s | 514 |
| **2048** | **1.0 – 4.8 s, median ~2 s** | 122 – 1031 |

**Seconds, not minutes.** The spread is the candidate search — how
many numbers the RNG has to try before two primes turn up — not the
arithmetic, which is why it varies by five times between runs.

So the stage-3 estimate everything downstream rested on was wrong by
about three orders of magnitude, in the good direction. Pair-request
generation is not a thing that needs a progress bar and a cup of tea;
it is a thing that happens while you look at it.

### It steps, which is what the tick was for

`rsa_keygen_step()` does one bounded piece of work and returns. The
command calls it, runs the bottom half, prints progress and checks for
ESC between steps — so a keygen looks like a machine working rather
than a machine hung, the DHCP retries keep going, and it can be
stopped.

That was the whole argument for doing Helios before stage 3, and this
is the first thing to actually use it.

### And it verifies the key, not the return code

`p * q == n`, `d * e == 1 mod lambda`, both primes re-tested with an
independent round count, and a round trip through **both** exponents —
`m^e then ^d`, and `m^d then ^e`, which is the direction PKCS#1
signing uses. Ten 2048-bit keys generated in a row, all ten pass every
property.

> One run in an early batch reported a verification failure. It was
> the measuring script grabbing the screen after the line had scrolled
> off, not the kernel — confirmed by re-running with a wait instead of
> a snapshot, ten for ten. Recorded because "it failed once" is worth
> chasing to an answer rather than leaving as a feeling.

## 2. An I2C bus, and a command that finds what is on it

The trackpad does not answer the PS/2 aux port. No keyboard appeared
on USB either. The October `lspci` on the HP found

```
00:15.0  8086:51E8  class 0C/80  Serial-IO I2C controller #0
00:15.1  8086:51E9  class 0C/80  Serial-IO I2C controller #1
```

which on a laptop is what the trackpad and the internal keyboard are
attached to. Both are Intel LPSS wrappers around a **Synopsys
DesignWare** I2C core — the same core on every Intel chipset since
Baytrail — so `drivers/i2c.c` is not HP-specific.

```
i2c          the controllers on this machine
i2c scan     every address that answers, on every bus
```

### The thing that is genuinely blocked, and the way round it

**An I2C device has no enumeration.** There is no descriptor to ask
for and no vendor ID to read: you must already know the seven-bit
address. On a PC that address lives in ACPI, in the device's `_CRS`,
and reaching it means an AML interpreter — RSDP, XSDT, DSDT, and a
bytecode evaluator. `kernel/machine.c` already records ACPI as "a
release of its own" for `\_S5`, and that is the *easy* half.

So this scans. A **zero-length read** at each address: put the address
on the wire, look at the acknowledge, stop. Nothing is written and no
register is named — a write to an unknown device can set a mode or
latch a configuration byte; a read cannot. `0x00`-`0x07` and
`0x78`-`0x7F` are skipped because the specification reserves them, and
`0x00` is the general-call address that every device on the bus
listens to.

One boot turns "the trackpad is probably at one of 0x05, 0x15, 0x2a,
0x2c or 0x4b" into a fact, and the HID layer can then be written
against an address instead of a list.

### What the driver does and does not do

Reads `IC_COMP_TYPE` before writing a single register, because class
0C/80 is "serial bus controller, *other*" and plenty of things report
it. Releases the LPSS reset first, because Intel holds the core in
reset out of a cold boot and until then every register reads back zero
— **indistinguishable from a BAR mapped at nothing, which is the bug
that cost v0.8.5 its USB.** Reads the FIFO depth off
`IC_COMP_PARAM_1` rather than assuming 32. Sets `IC_TAR` only while
the core is disabled, because the hardware ignores it otherwise and
the address you then talk to is whatever was there before.

The BAR is above 4 GB on this machine, like the xHCI, so it goes
through `pci_bar_mem64()` and `highfb_map_io()` with caching disabled.

Not here: interrupts (the touchpad signals through a GPIO and there is
no GPIO driver — the HID layer will **poll**, through `kernel_poll()`,
which only became possible in beta 1), 10-bit addressing, multi-master,
DMA.

### The suite, and the seam bug it found before any injection

`drivers/i2c.c` **has never seen an I2C controller.** It was written
from the register map for a machine that is not here. That is exactly
the position `tests/test_usbmux.c` was in when it passed 79 of 79
against a header the phone has never used.

So the simulated core in `tests/test_i2c.c` is written to be awkward
in the ways the hardware is awkward: it stays in reset and reads zero
until released, it *ignores* `IC_TAR` while enabled, it latches an
abort on a NAK, and it reports a 16-byte FIFO rather than 32.

**And the first version of the suite could not see a single write.**
It reached the driver through a `VGA_MEM_PTR`-style seam — a macro
returning a `volatile uint32_t *`. That works for the console, because
the console's memory *is* memory: writing to it is a write and nothing
else happens. An I2C controller is not memory; writing `IC_DATA_CMD`
puts a byte on a wire. Every write the driver made landed in the
scratch word the macro handed back, the reset was never released, and
31 of 41 checks failed in a way that looked like a broken driver.

The seam is two hooks now, `I2C_RD` and `I2C_WR`. **A pointer seam
cannot express a write to a device** — worth remembering the next time
one of these is added.

44 checks; `tools/check_i2c_suite.py` catches all 13 planted defects.

---

## The numbers

| | v0.8.8.2 | v0.8.8.3 |
|---|---|---|
| suites | 45 | **46** |
| checks | 4470 | **4514** |
| injection harnesses | 28 | **29** |
| `test_i2c` | — | **44** |
| I2C injections caught | — | **13 of 13** |

## What is NOT verified

- **The I2C driver has never seen an I2C controller.** QEMU has none,
  so `i2c` there correctly prints "No I2C controller found" and that
  is the whole of its runtime testing. The first real run is the HP.
- **`i2c scan` has never scanned anything.** Everything it does is
  checked against a simulated core which is this project's reading of
  the register map, and a driver written from the same reading agrees
  with it perfectly.
- **The keygen numbers are QEMU's**, on this container's CPU. The HP
  is a real Alder Lake and should be faster, not slower, but that is
  a prediction.
- **Still unconfirmed from earlier betas:** the USB mouse, exFAT on a
  real stick, the 8x16 font on a 1920x1080 panel, and any TLS
  handshake since `rsa_public_encrypt()` moved onto the bignum.

## Still open

- **The FAT32 stick.** Reported as *"it says the port didn't answer"*,
  which is **USB enumeration failing**, not a filesystem problem — so
  beta 3's exFAT work does not touch it. `usb -vv` already prints
  which of three causes it is (not powered / never trained / link up
  and no reply) and the boot log prints an error code beside it. That
  one line is still the blocker and has been for five releases.
- **I2C-HID itself.** This release is the bus. Reading the HID
  descriptor, the report descriptor, and polling input reports is the
  next one, and it needs the scan result first.
- **NTFS read-only**, **ext4**. Requested, not started.
- **Stage 3 from the certificates on**: DER, X.509 v3, PKCS#1 signing,
  a plist writer, PEM→DER base64.
