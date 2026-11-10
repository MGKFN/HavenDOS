# HavenDOS v0.8.9.6 Beta "Helios" — performance and the pointer

*10 November 2026. 59 test suites, 6027/6027 checks.*

Kyan, on the HP: *the trackpad does not work, an external mouse works
but is very slow, sluggish and choppy, everything is, and at high
resolution the cursor would not go further than the middle of the
screen.* This release is about those, and about the three things the
last few releases kept listing as still open: a USB bus that collapses,
a kernel log that stops recording, and a desktop that stutters.

Six causes, all found in the code. **None of them has run on the HP
yet.** QEMU boots the release on both firmware paths, but it cannot
show the speed difference, because its framebuffer is ordinary host
memory and its I2C bus does not exist.

---

## 1. Why everything was slow: the framebuffer was uncached

`kernel/highfb.c` mapped the framebuffer **write-through** (PWT). The
comment above it said:

> *Write-combining would be better still, but that needs MTRRs or PAT
> and this is a diagnostic path.*

On the HP the framebuffer is above 4 GB, in the range the firmware's
MTRRs mark **uncacheable**. When the MTRR says UC and the page says WT,
the CPU uses UC. So every 32-bit pixel store was its own uncached bus
transaction, and each one had to finish before the next could start.

Rough numbers:
- A 16x32 cell is 512 stores.
- A full 80x25 repaint is about a million.
- The shell repaints **every cell** each time it scrolls a line.

That is "everything is choppy". QEMU could never show it: its
framebuffer is host memory and the memory type changes nothing.

**Now:** the PAT (Page Attribute Table) is programmed so that entry 4
is **write-combining**, and the framebuffer pages select it. MTRR-UC
combined with PAT-WC gives WC, whatever the firmware says. The CPU now
gathers pixel stores into 64-byte bursts and does not wait for each one.

- **Entry 4 is the one reprogrammed** (PAT bit set, PCD and PWT clear).
  Nothing in this kernel has ever set the PAT bit, so no existing page
  changes type. Entries 0–3, which `highfb_map_io()` relies on for
  uncached device registers, keep their reset values. Linux puts WC in
  entry 1 instead; doing that here would have silently turned every PWT
  page in the kernel into a WC one.
- **The write is read back.** A hypervisor that ignores it must not
  leave the kernel setting PAT bits that now mean write-back.
- **Framebuffers below 4 GB too.** They need no window, but with paging
  off their memory type is the MTRR's, which for a graphics aperture is
  usually UC as well. `highfb_wc_low()` turns paging on with the same
  identity map the high path builds and marks the framebuffer's pages
  WC. This is new for every machine with a low framebuffer, QEMU
  included, so `harness/wc_boot.py` boots it both ways (BIOS on i386,
  UEFI on x86_64 with OVMF) and checks the shell still runs on the new
  page tables. 8/8.
- **The boot log says which it got:** `Framebuffer memory:
  write-combining (PAT)`, or a warning that drawing will be slow.
- **`nowc`** on the kernel command line turns it off. `nohighfb` still
  means paging stays off, so it skips this too.

### Found on the way: the PAE path dropped the PAT bit

`highfb_pae_pde()` masked its flags with `0x0FFF`. In a 2 MB entry
bit 12 *is* PAT, so a write-combining request on a CPU without PSE-36
would have become write-back without a word. The mask is `0x1FFF` now.

### Found on the way: a 4K framebuffer drew into the USB controller

Every window took **one 16 MB slot**, whatever its size. The page loop
then mapped as many pages as the size needed. A framebuffer over
16 MB ran on into the next slot, and the next slot is where xHCI's
registers get mapped:

| Resolution | Framebuffer size | Fits in one slot? |
|---|---|---|
| 1920x1080 | 8 MB | yes |
| 2560x1600 | 16.4 MB | no |
| 3840x2160 | 33 MB | no |

On the two that do not fit, xHCI's pages overwrote the bottom of the
framebuffer window. Pixels drawn at the bottom of the screen were
writes to the USB controller's registers. A window now takes as many
slots as it needs. Up to 16 MB nothing moves: `test_highfb` checks that
xHCI still lands at `E1120000`, exactly where the HP has always
printed it.

## 2. The cursor stopped halfway down the screen

`shell/ui.c`, in the function that draws the desktop pointer:

```c
if(my>23) my=23;
```

That is row 23 of an 80x25 screen. On a high-resolution grid of 50 or
67 rows it is halfway down the glass, so the pointer stopped there
however far the mouse moved. It is now `vga_rows() - 2`: everything
above the taskbar.

`mouse.c` was fixed for this exact mistake in v0.8.5.6. The copy in the
drawing code was missed. A grep for the same pattern turned up no
others.

## 3. The mouse was slow at high resolution, and too fast vertically

The pointer moved **4 counts per cell**:

| Grid | Counts to cross the screen |
|---|---|
| 80x25 | 320 |
| 240x67 | 960 |

So on a high-resolution grid the same hand movement crossed a third as
much of the screen. Nothing was wrong with the mouse; the speed was tied
to the wrong unit.

The two axes also disagreed. A cell is twice as tall as it is wide, so
4 counts per cell moved the pointer twice as many **pixels** down as
across. On 80x25 at 1280x800 that was 4 px per count horizontally and
8 px per count vertically.

**Now:**
- **320 counts cross the width on every grid.** At 80 columns that is
  exactly the old speed, so the stock desktop feels the same left to
  right. Only the grids that were slow got faster.
- **The vertical span** is whatever keeps one count the same number of
  pixels on both axes. `vga_cell_px()` is new, so the mouse can ask a
  cell's size.
- **Acceleration** works on the raw counts in each report, with the
  magnitude taken before comparing. That is the v0.7.17 rule, carried
  over now that `adaptive_delta()` is gone.
- **Pushing against an edge leaves no debt.** Movement into an edge
  used to pile up in the accumulator, and the first movement back out
  had to pay it off before the pointer moved: up to a cell of dead
  travel at every edge. A clamped axis now clears its remainder.

## 4. The mouse was read once per pass, and the pass was slow

The desktop's idle was `sleep_ms(20)`, and a USB or I2C pointer is only
read when somebody asks where it is. So it was read once per pass, and
with the uncached framebuffer a pass was far longer than 20 ms. A
boot-protocol report carries at most 127 counts per axis, so a fast
movement that built up over one slow pass was clipped.

`ui_idle()` waits the same 20 ms in 4 ms slices and reads the pointer
in each one. It returns early the moment the pointer moves, a button
changes or a key arrives, so the desktop redraws now rather than at the
end of the wait.

## 5. The trackpad: powered down, and not given time after its reset

The driver was written in 0.8.8.11 and had never run on the HP. Two
things in it would have stopped it there, and both are fixed.

**The controller may have been in D3.** Firmware leaves a Serial-IO
controller it does not use powered down. In that state every register
reads `FFFFFFFF`, and `bring_up()` then said "not a DesignWare core".

`pci_set_d0()` (new, in `drivers/pci.c`) walks the capability list,
writes PMCSR to D0, waits the 10 ms the specification requires, and
**puts the BARs and command register back**. A D3hot→D0 transition may
reset the function, and a reset clears them. `test_pci` simulates
exactly that reset; without the restore, two checks fail.

**The device was read the moment it was reset.** `i2c_hid_init` sent
SET_POWER and RESET and read the report descriptor in the same breath.
A device still busy with its reset refuses that read, `ihid_bring_up()`
returned `IHID_E_IO`, and the trackpad was never attached.
`ihid_power_and_reset()` now does it in order:

1. power on
2. wait 10 ms
3. reset
4. wait 50 ms
5. poll for the two-byte reset response, for up to a second

The report-descriptor read then gets three tries. The mock device in
`test_i2c_hid` now refuses transfers for a while after a reset, as real
ones do. Every check before this one used a device that was ready the
instant it was told to reset, and one new check confirms that the old
order fails against it.

**Two smaller fixes in the same bring-up:**
- **The LPSS reset is now asserted before it is released**, the order
  Linux's `intel-lpss` uses.
- **The wait after it is a real wait.** It was `for(t = 0; t < 10000u;
  t++) { }`, which `-O2` deletes outright, so there had never been any
  delay at all.

**`i2chid` now shows each step on screen:** the reset (answered in how
many ms, or not at all), the report descriptor, and the pointer. That
way one photograph says where it stopped. With no controller it now
points at `dmesg`, which says whether the BAR was missing, the
controller was powered down, or it was not a DesignWare core.

## 6. USB: a storm is not a series of plug events

Every port change rescans the **whole** bus. Every device is released
and enumerated again, mouse and keyboard included, and that means
hundreds of milliseconds of port resets during which nothing on USB
answers. That is right for one plug event and ruinous for a port that
flaps: the bus is torn down every second, the pointer freezes in time
with it, and the log fills a dozen lines at a time. Choppiness, a full
log and eventually `0 devices after hot-plug` can all be that one thing.

- **The first change is still acted on at once**, so plugging in a
  stick does not wait.
- **A storm is held off.** A change that arrives when three rescans
  already ran in the last five seconds is held off for 10 s, then 20,
  40, 80 while it continues. The log says so once, and `usb rescan`
  still works whenever a person asks. A minute of quiet resets it.
  Changes seen during a hold are not lost: the first pass after it
  rescans once.
- **A walk that finds nothing on an occupied bus gets one retry**, a
  second later. A device caught mid-reset by the walk answers nothing.
  The retry happens once, because a bus that is genuinely broken should
  be reported, not hammered.

## 7. The kernel log stopped recording after the boot

160 lines, keeping the start and dropping everything after it. On the
HP the boot alone writes over a hundred, so a few minutes of USB events
filled the rest. From then on **nothing new was kept**: `dmesg | grep
lockdown` after a failed pair found nothing, because the lines that
would have explained it were the dropped ones.

Now the first 192 lines are pinned (the boot), and everything after
them goes round a 320-line ring, newest replacing oldest. `dmesg` marks
the gap: `(... N older lines rotated out after the boot ...)`. That is
40 KB of `.bss`; the heap has been 125 times bigger than that since
0.8.8.5.

---

## Testing

| Suite | Before | Now | What is new |
|---|---|---|---|
| test_highfb | 125 | 150 | write-combining, PAT slots, the PAE mask, 4K windows, low framebuffers |
| test_kbdinit | 34 | 42 | grid-independent speed, both axes, mirror symmetry, edges |
| test_kpoll | 146 | 156 | the storm hold, the zero-device retry, two plugs that are not a storm |
| test_i2c_hid | 71 | 84 | busy-after-reset, the old order failing, a device that never answers |
| test_pci | 26 | 34 | D3 to D0 with the reset that clears the BARs |
| test_i2c | 44 | 49 | a controller left in D3 |
| test_klog | 22 | 31 | the ring, four laps, the gap |

**Injection harnesses:**
- New: 3 in `check_hotplug_suite`, 1 in `check_i2c_suite`, 4 in
  `check_pointer_suite`. All caught.
- One old injection, *"the LPSS reset is never released"*, went MISSED
  the moment the driver started asserting the reset first. The
  simulated controller took **any** write to the reset register as a
  release, so a driver that only ever asserted passed. It reads the
  value now.
- Five injections had gone stale and were re-anchored. Three of them
  predate this release: the two `mouse_is_ready()` ones since 0.8.8.11,
  and the BAR one since 0.8.8.4.

`harness/wc_boot.py` (new) boots the ISO on BIOS and on UEFI with paging
on and checks `dmesg` reports write-combining: 8/8.

## Still open

- **None of this is confirmed on the HP.** The order to try it in:
  1. Does the desktop feel different? `dmesg` should say
     `write-combining (PAT)`.
  2. Does the cursor reach the bottom of a high-resolution grid?
  3. What does `i2chid` say?
- **iPhone pairing stage 3** waits for the cable. 0.8.9.5's fix (hang
  up during the keygen) is in this release unchanged.
- The desktop still repaints the whole frame once a second for the
  clock. With write-combining that should be cheap; if it still
  stutters, that is next.

## Files

| File | What changed |
|---|---|
| `kernel/highfb.c`, `include/highfb.h` | PAT write-combining, `highfb_wc_low()`, window slots by size, the PAE mask |
| `kernel/main.c` | `nowc`; WC for low framebuffers; the boot-log line |
| `drivers/mouse.c` | speed as a share of the screen, symmetric axes, edge remainders |
| `drivers/vga.c`, `include/vga.h` | `vga_cell_px()` |
| `shell/ui.c`, `include/theme.h` | the row-23 clamp; `ui_idle()` |
| `shell/desktop.c` | idles through `ui_idle()` |
| `drivers/pci.c`, `include/pci.h` | `pci_set_d0()` |
| `drivers/i2c.c` | D0 before use; reset asserted then released; a real delay |
| `drivers/i2c_hid.c`, `include/i2c_hid.h` | `ihid_power_and_reset()`, retries, the step results |
| `shell/i2chidcmd.c` | each step on screen |
| `usb/hotplug.c`, `include/usb.h` | storm hold-off, one zero-device retry |
| `kernel/klog.c`, `shell/shell.c` | pinned boot plus ring; `dmesg` shows the gap |
| `harness/wc_boot.py` | new |
