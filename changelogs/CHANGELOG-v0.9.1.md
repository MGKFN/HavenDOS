# HavenDOS v0.9.1 Beta "Janus": reading the monitor

*11 November 2026. 62 test suites, 6428/6428 checks.*

## What the HP said on 0.9.0

Kyan ran `gfx`, `gfx vbt` and `gfx regs` on the HP with the monitor
plugged in. It was the first time HavenDOS had read this display engine.

- **It reads the right registers.** Pipe A's plane scans out GTT
  `00000000`, BAR2 is `40_00000000`, and the firmware framebuffer is
  `40_00000000`, so the built-in cross-check printed "= the framebuffer
  HavenDOS draws on". CDCLK decoded to 179.2 MHz (38.4 × 14 / 2 / 1.5),
  and CDCLK_CTL's own field agrees (0x164 → 179).
- **The panel path:** pipe A → transcoder A → DDI A, eDP, DP SST, 8 bpc,
  2 lanes, clocked by DPLL1. DPLL0 is off.
- **HDMI is wired directly to DDI B**, on GMBUS pin 2 and AUX B, with no
  LSPCON. The VBT came through RVDA (OpRegion 2.1, BDB 253). That is the
  reachable first target 0.9 was hoping for.
- **Pipes B and C are already powered**, so 0.9.2 needn't turn wells on.
- **The monitor** is an LG 24GN65R (from the label; its EDID will say the
  rest).

The 96 register values in that photo are now a fixture in
`tests/test_gfx.c`: the first test data in this series from real
silicon.

## Four things 0.9.0's summary said wrong

The values were all right. What the summary *said* about them wasn't:

1. **"asked for by the firmware: none".** 0.9.0 looked only at the BIOS
   requester register (45400 = 00005405, state bits only). Every request
   on the HP is in the **driver** register (45404 = 0000FC0F: PW_1, PW_2,
   PW_A, PW_B, PW_C). The firmware's own GOP driver uses that one. The
   report now lists requests per requester register.
   This matters for 0.9.2, which will be changing those requests.
2. **The stretch wasn't named.** The plane is 1024x768 and the transcoder
   is 1920x1080, because the pipe scaler stretches GRUB's mode to the
   panel. That is the "stretched" look HavenDOS has had on the HP since
   Helios. A new line under the pipe now says so.
3. **DDC was shown for the eDP panel** ("eDP panel, DDC 9"). An eDP
   child's DDC field is meaningless; it's shown only for HDMI/DVI now.
4. **The panel's refresh rate wasn't shown.** Transcoders now print the
   pixel clock and refresh, from the DP link M/N (two new registers per
   transcoder) times the port clock. PLL lines print each PLL's
   frequency (i915's `icl_ddi_combo_pll_get_freq`): DPLL1 on the HP is
   **269.7 MHz**, the eDP link at 2.7 Gbps.

The unclaimed-access self-check could not be used on this machine:
FPGA_DBG was `80000001` before and after, because the firmware had
already set it. Clearing it is a write, and it belongs to the first
release that touches the display pipeline.

## The release: `gfx edid`

```
gfx edid [pin]   the monitor's EDID over GMBUS: maker, name, limits,
                 every mode it offers (the preferred one first), the
                 raw bytes, and every GMBUS write with its read-back
```

The pin defaults to the one the VBT gives the HDMI connector, which is 2
on the HP.

### The first write in the display series, and its fence

GMBUS is the display engine's I2C controller: six registers,
`0xC5100`–`0xC5120`. This release writes those six and nothing else.

- **`gmbus_wr()` in `drivers/xegmbus.c` is the only place a write
  happens.** It refuses any offset outside the six and counts the
  refusal. The suite plants writes to plane A, the power wells, one
  below GMBUS0, one past GMBUS5 and an unaligned one, and all five are
  refused before reaching the model.
- **`tools/check_gfx_readonly.py` enforces it at build time.** In
  `drivers/xegmbus.c`, `GFX_WR` may appear once, inside `gmbus_wr()`,
  and that function must still hold the guard. `drivers/xedisp.c`,
  `shell/gfxcmd.c` and the new `drivers/edid.c` stay strictly
  read-only. Six planted violations were all refused.
- **GMBUS0 is read back after every write** and must hold the pin. If
  it doesn't, no command is issued (`ESTUCK`), because the transfer
  would run on whatever pin GMBUS0 does hold. GMBUS1 is a command
  register whose SW_RDY bit the hardware consumes, so its read-back is
  logged and not compared.
- **The sequence is i915's** (`do_gmbus_xfer`, index read). That covers
  the retry on a first NAK, for monitors waking from standby, and the
  three-write cleanup after a NAK. i915's clock-gating workaround is
  only for the SPT and CNP PCHs, not this one, so it isn't here; it
  would have been a write outside the six.
- **Every wait is a bounded poll**, with no IRQ.

### The parser

`drivers/edid.c` is pure: bytes in, a struct out. It reads:

- the PNP maker ID, product, serial, week and year;
- the name (0xFC), serial text (0xFF) and range-limits (0xFD)
  descriptors, including EDID 1.4's +255 range offsets;
- every detailed timing in block 0 and in CTA-861 extensions, with
  repeats dropped and capped at twelve.

Refresh is computed in 32 bits with no 64-bit division (the tree has no
`__udivdi3`).

**Judged against bytes from outside the project:** the 18-byte detailed
timings of the six built-in EDIDs Linux shipped in `drm_edid_load.c`
(v5.15), checked against the published VESA DMT and CEA-861 numbers.
The kernel's bytes and VESA disagree in two places: 1024x768's
horizontal sync (8/144 in the kernel, 24/136 in DMT) and 1680x1050's
hsync polarity. The suite asserts what the bytes say and records the
difference. The six headers were also run through `pyedid` (PyPI), an
independent decoder, and agreed. Ubuntu's archive was returning 503, so
`edid-decode` couldn't be installed.

## Tests

| what | result |
|---|---|
| `tests/test_edid.c` (new) | **99/99**, under ASAN and UBSAN: the parser, the GMBUS transfer against a strict model controller and monitor, the guard, and the screen |
| `tools/check_edid_suite.py` (new) | **24/24** injections caught across both files, harmless correctly missed |
| `tests/test_gfx.c` | **199/199** (was 174), now with the HP's 96 real registers |
| `tools/check_gfx_suite.py` | **38/38** (was 31), with seven new injections for the four fixes and the PLL decode |
| `harness/gfx_boot.py` | **10/10** in QEMU: `gfx edid` with no engine refuses and touches nothing |
| `make test` | **6428/6428** across 62 suites |

**The harnesses found holes again, after the suites had passed:**

- `check_edid_suite.py` reported 3 misses out of 24 after 95/95:
  - no test timing had a sync offset past 255; CEA 1080p24, with a
    638-pixel front porch, is in the suite now;
  - every test timing had equal h/v polarity; the 144 Hz test mode is
    +h −v now;
  - the "unannounced block" test used a block that is ignored either
    way; it is a valid CTA block now.
- `check_gfx_suite.py` found the Q-divider-without-mode case
  untested. Neither of the HP's PLLs has that combination, so the suite
  makes it.
- Writing the HP fixture caught a wrong test expectation. The check
  expected a KVMR request line, but `45408 = 40000000` is bit 30, the
  *state* bit of index 15, not a request. The report was right.

## Things worth knowing

- **0.9.0 never got a GitHub release.** Its workflow failed while
  installing build tools: the runner couldn't resolve Ubuntu's mirror.
  The re-run sat in the same step. Ubuntu's archive returned 503 to
  this build container too. The code is on `main`, the 0.9.0 bundle was
  handed over in chat, and 0.9.1 supersedes it.
- **EDID blocks 2 and up need a segment pointer** (I2C address 0x30),
  which isn't implemented. A 1080p monitor has two blocks at most.
- **If the VBT puts the HDMI connector behind an LSPCON**, its EDID comes
  over DP AUX, which this release doesn't do. The HP has no LSPCON.

## What has not been tested

`gfx edid` on real hardware. `docs/GFX-EDID-HARDWARE-TEST.md` is five
minutes: `gfx edid` with the monitor on, `gfx` for the four fixes, then
`gfx edid` with the monitor off.

## Next: 0.9.2

0.9.2 lights pipe B. With the panel's own configuration from `gfx` as a
model and the monitor's preferred mode from `gfx edid`, it does this
behind an explicit `gfx on` and never at boot:

- program DPLL0 (free) for the TMDS clock;
- route it to DDI B;
- enable DDI B's buffer;
- set transcoder B's timings and HDMI mode;
- turn on pipe B and plane B, scanning out a new framebuffer.

Every write is read back. Pipe A is never touched.
