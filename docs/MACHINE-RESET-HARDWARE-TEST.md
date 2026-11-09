# Testing `reboot` and `poweroff` on the HP

The reset ladder in `kernel/machine.c` is proved three ways, and the
third one needs the laptop. This is that third one.

What is already proved, and where:

| claim | proved by | where it ran |
|---|---|---|
| every stage is reached, in order | `tests/test_machine.c`, 46 checks | native, in process |
| the suite can actually fail | `tools/check_machine_suite.py`, 20 of 20 caught | native |
| a tenth copy fails the build | `tools/check_machine_reset.py` | `make` |
| `reboot` still reboots, and resets rather than shutting down | `harness/machine_reset.py` | QEMU, `pc` and `q35` |
| `0xCF9` is what carried that reset | both QEMU machine types implement it (`piix-reset-control`, `lpc-reset-control`) and it is tried first | QEMU |
| **`reboot` works on a machine with no 8042** | **nothing yet** | **the HP** |
| **`poweroff` reports honestly instead of hanging** | **nothing yet** | **the HP** |

The last two are the point of the whole change and neither can be
proved here. QEMU has a working 8042, so under QEMU the old code also
rebooted — which is exactly why this defect shipped nine times and was
only found by booting it on the laptop.

---

## 1. `reboot` — the one that should now work

Boot the ISO, Shell Mode, and type:

```
reboot
```

**Pass:** the machine restarts. Firmware splash, then GRUB.

**What it means.** Stage 1 is port `0xCF9`, the chipset reset control
register, and it is tried before anything else. The HP has no 8042, so
if the machine restarts, `0xCF9` is what restarted it — there is
nothing else it could have been. That is the above-4G-BAR moment for
this change: a register that has only ever executed against a
simulation.

**Fail, variant A — it still hangs under "Rebooting HavenDOS... Please
wait."** Then `0xCF9` did nothing, the 8042 pulse did nothing, and the
triple fault was caught or did not fault. Worth knowing which: boot the
keyboard diagnostic entry and watch whether the screen reaches the
failure message at all.

**Fail, variant B — it reaches "Reset failed: no method this machine
supports."** That is the ladder working and running out of methods,
which is a different and much better failure: every stage was tried and
the machine said so instead of lying. Next step would be ACPI reset
(FADT `RESET_REG`), which is the same table parsing `poweroff` needs.

**Fail, variant C — it powers off instead of restarting.** Should be
impossible; `machine_reset()` writes no power-off port and
`tests/test_machine.c` checks that. Report it, because it means the
suite is wrong about something.

---

## 2. `poweroff` — the one that should now tell the truth

```
poweroff
```

**Pass:** the screen reaches

```
Power off is not supported on this machine yet.
Hold the power button to switch off.
```

and the machine sits there, still on, having said so.

That *is* the fix. ACPI power-off is not implemented and is not
pretended to be: finding the RSDP, parsing the FADT for `PM1a_CNT_BLK`
and the DSDT for the `\_S5` sleep type is a release of its own. What
changed is that the machine no longer claims "It is now safe to power
off." while its fans are running — that line used to be printed
*before* anything was attempted, on both the shell and the desktop.

**Fail — it hangs with no message.** Then `machine_park()` was reached
without the message being drawn, which means something above it faulted.
`nm -n havendos.elf` against the EIP on the panic screen.

**Fail — it actually powers off.** Then one of QEMU's, Bochs' or
VirtualBox's PM1a addresses happens to be live on that chipset. Not a
bug, but say so, because it means real ACPI is closer than assumed and
0.9.x can be planned around it.

---

## 3. Every path that reaches it

Nine call sites collapsed into two functions, so there are nine ways to
reach this code and they are worth a pass each. The two `iso_installer`
ones only happen from the ISO.

| how | which |
|---|---|
| `reboot` at the shell | reset |
| Desktop → Power → 1 | reset |
| three failed logins at the login screen | reset |
| exiting the last session (`exit` out of Shell Mode) | reset |
| ESC on the installer welcome screen | reset |
| the installer's 5-second countdown after a successful install | reset |
| `poweroff` or `shutdown` at the shell | power off |
| Desktop → Power → 2 | power off |
| `S` on the installer's "Installed." screen | power off |

The installer countdown is the one that matters most: it is the last
thing a person sees after a successful install, and until now it hung
there, which reads as "the install broke" rather than "the install
finished".

---

## 4. If it hangs, what to look at first

The failure message is drawn at rows 16 and 17 by `vga_puts_at`. If the
machine hangs with the caller's own text on screen and nothing at row
16, execution never got past the ladder — so the suspect is a stage
that blocked rather than one that failed.

The candidate is `kbc_input_clear()`. It is bounded at `KBC_SPINS`
(100000 iterations, each a status read plus an `io_wait`, so on the
order of 0.2 s), and it is bounded precisely because the version it
replaced was not:

```asm
1: inb $0x64, %al
   testb $2, %al
   jnz 1b
```

On a controller whose status register never clears — an SMM emulation
under load, which is what this laptop has — that loop never exits, and
it never exits *before the reset write*. A hang there is
indistinguishable from a reset that did not work, which is the trap this
whole change is built around. `tests/test_machine.c` holds the input
buffer full for ever and asserts the wait returns anyway.

If a second is too long to wait through four stages, `KBC_SPINS` is the
knob.
