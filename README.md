# HavenDOS

HavenDOS is an operating system written from scratch in C and x86 assembly by Tech Haven Studios: its own 32-bit kernel, drivers, filesystems, TCP/IP and TLS stack, a shell and a text-mode desktop with the look of an old DOS machine, on modern UEFI hardware.

It boots on a 2022 HP laptop from its own partition beside Windows 11, drives that laptop's external HDMI monitor, reads USB sticks, and reaches the internet through an iPhone over USB — that laptop has no Ethernet port.

[**Download the latest release**](https://github.com/MGKFN/HavenDOS/releases/latest) · [All releases](https://github.com/MGKFN/HavenDOS/releases) · [Report a bug](https://github.com/MGKFN/HavenDOS/issues)

## Current version: 0.9.6.7 Beta "Daedalus"

0.9.6 made HavenDOS run several apps at once. Its maintenance updates came from testing on the HP laptop, on 16 and 17 November 2026.

**0.9.6.7. The boot no longer waits on the trackpad.**
- **The trackpad search held the HP's boot.** 0.9.6.6 gave the trackpad's controllers their memory window, and the search then ran for the first time, on waits of seconds each.
- **Now it runs after the USB keyboard,** each wait has a 50 ms clock, and a controller that times out once is left alone.
- **`noi2c` skips the whole step.**

[Changelog](changelogs/CHANGELOG-v0.9.6.7.md).

**0.9.6.6. What the HP showed in 0.9.6.5.**
- **The Shell's X closes it.**
- **Full screen fills the screen** for 80×25 apps, at bigger characters.
- **A mouse or stick unplugged and plugged back in works again:** keyboards and mice were never bound again after a USB rescan.
- **The trackpad's controller gets the memory window** the firmware never gave it.
- **The wheel scrolls** the Start menu and Files.
- **Your saved resolution is applied at boot.**

[Changelog](changelogs/CHANGELOG-v0.9.6.6.md).

**0.9.6.5. Apps are windows.** Every app has a title bar with minimize, full screen and close, and the taskbar stays on the bottom row with Start, a chip for each open app, and the clock.
- **Full screen:** F11 or Alt+Enter brings the bars back. Alt+Enter is there for laptops whose F11 is a media key.
- **The window's X** does what ESC does.
- **Settings works with the mouse**, however it was opened.

[Changelog](changelogs/CHANGELOG-v0.9.6.5.md).

**0.9.6.4. The monitor never gets stuck half set up.** A half-lit display pipe is cleared before the next try. When the monitor can't be scaled, it gets an unscaled picture and the log says why. Confirmed on the HP: "works now". [Changelog](changelogs/CHANGELOG-v0.9.6.4.md).

**0.9.6.3. The monitor fills, and the mouse works everywhere.**
- **The HDMI picture is scaled:** fitted at 4:3 by default, or stretched, set in Settings.
- **The monitor lights by itself** when plugged in, at boot or later.
- **Every app can be used with the mouse:** click to pick and open, clickable key hints, the scroll wheel or two-finger scrolling, and right-click for back.

[Changelog](changelogs/CHANGELOG-v0.9.6.3.md).

**0.9.6.2. The monitor's first picture.** Pipe B's display buffer was counted from the wrong slice, so every fetch came back empty and the monitor showed one colour edge to edge. Confirmed on the HP. [Changelog](changelogs/CHANGELOG-v0.9.6.2.md).

**0.9.6.1. The first fixes from the HP.**
- `resolution` works with apps open, and they are re-laid out when you switch back.
- The Welcome screen appears only when there is no account.
- BOOT Chat is out of the menus while its server is down.
- `gfx try` tests the external monitor.

[Changelog](changelogs/CHANGELOG-v0.9.6.1.md).

**0.9.6. Several apps at once.**
- **Tasks** switch when an app waits for a key or yields, and there are background jobs.
- **Alt+Tab** switches between open apps, and each keeps its state.
- **The bottom bar** has a chip per open app.
- **`migrate`** moves a FAT16 install to a new FAT32 partition on the same disk, from inside HavenDOS.

[Changelog](changelogs/CHANGELOG-v0.9.6.md).

## Coming in 0.9.7 (~26 November)

- **The new version numbers in the updater**, so every installed 0.9.x understands "HavenDOS 1, version 27H1 (OS Build 27100.1000)" before it arrives.
- **The editor:** select text with the mouse, and copy, cut and paste with one clipboard for every app.
- **Files:** a right-click menu (open, rename, copy, move, delete).
- **The taskbar:** pinned apps, and smaller desktop icons.
- **Retro PC.** An emulated PC inside HavenDOS that boots the studio's old builds, starting with **K-DOS Beta 1.1b**, the oldest one that survived:
  - an 80186 with the 386's 16-bit additions, a BIOS, a floppy and a pretend hard disk kept as a file, so it never touches a real disk;
  - your own `.IMG` floppy images, from a `RETRO` folder.

  It is finished on the [`emu86`](https://github.com/MGKFN/HavenDOS/tree/emu86) branch:
  - 86 tests;
  - it matches a real 8088 on 2.5 million recorded instructions;
  - K-DOS gives the same screens and disk writes as on QEMU's own PC.

  It joins `main` with 0.9.7, as exception E-1 in the roadmap.

## Earlier releases

| Version | What it did |
| --- | --- |
| [0.9.5.1](changelogs/CHANGELOG-v0.9.5.1.md) | The modern look in every app; 2048, Breakout, Tic-Tac-Toe, Memory, Notes, Stopwatch, Converter; `open` |
| [0.9.5](changelogs/CHANGELOG-v0.9.5.md) | The apps rebuilt: one command reference behind `help`, `man` and the Manual; a real Settings; one System app; twenty themes |
| [0.9.4.1](changelogs/CHANGELOG-v0.9.4.1.md) | Saving in your home directory on FAT32 installs, and fixes from the 1.0 screenshots |
| [0.9.4](changelogs/CHANGELOG-v0.9.4.md) | An installed HavenDOS can install HavenDOS, onto another disk |
| [0.9.3](changelogs/CHANGELOG-v0.9.3.md) | Installs over 2047 MB are FAT32, up to 32 GB |
| [0.9.2.2](changelogs/CHANGELOG-v0.9.2.2.md) | The monitor locks on to HavenDOS's signal; `gfx color` and `gfx b` to find out why it is black |
| [0.9.2.1](changelogs/CHANGELOG-v0.9.2.1.md) | `gfx on` past step 28; USB keyboards and mice on every interface |
| [0.9.2](changelogs/CHANGELOG-v0.9.2.md) | `gfx on` drives the HDMI port, every register write read back |
| [0.9.1](changelogs/CHANGELOG-v0.9.1.md) | `gfx edid` reads the external monitor over DDC |
| [0.9.0](changelogs/CHANGELOG-v0.9.0.md) | `gfx` reads the Iris Xe display engine; iPhone tethering confirmed on the HP |
| [0.8.9.9](changelogs/CHANGELOG-v0.8.9.9.md) | Pairing and tethering at once; the phone reaches the internet |
| [0.8.9.8](changelogs/CHANGELOG-v0.8.9.8.md) | DHCP works, for the first time on any network |
| [0.8.9.7](changelogs/CHANGELOG-v0.8.9.7.md) | The iPhone pairing transport fixed |
| [0.8.9.6](changelogs/CHANGELOG-v0.8.9.6.md) | Performance: a write-combining framebuffer; the pointer at every resolution |

**Helios** (0.8.8 to 0.8.9) was the iPhone release. The HP's only route to a network is an iPhone over USB, and iOS turns tethering on only for a computer it has *paired* with. Pairing took a usbmux channel, Apple property lists, RSA-2048 key generation, three X.509 certificates and TLS 1.2 with a client certificate, all written for this kernel.

Every release has its full changelog in [`changelogs/`](changelogs/).

## What's in it

- **Boot:** one hybrid ISO for BIOS and UEFI through GRUB. Framebuffer console at any resolution the firmware offers, with the character grid chosen by the user.
- **Install:** beside an existing Windows installation in free space, or on an empty disk with its own EFI System Partition. `uninstall` removes it again and never touches a boot loader it did not put there. `migrate` moves a FAT16 install to FAT32.
- **Update:** `update` installs a newer kernel from a USB stick, the EFI partition or CD, and checks a CRC before writing anything.
- **Tasks:** several apps open at once, Alt+Tab, background jobs. Apps are windows with minimize, full screen and close, under a taskbar.
- **Display:** the Iris Xe's HDMI port driven by HavenDOS itself: the monitor read over DDC, lit, scaled, and lit again when plugged back in.
- **Storage:** NVMe, AHCI, ATA/IDE and VirtIO disks; USB mass storage. FAT12, FAT16 and FAT32 read-write, exFAT read-only, GPT and MBR.
- **USB:** xHCI and UHCI, hubs, keyboards and mice, mass storage, and Apple's USB Ethernet interface.
- **Input:** PS/2 and USB keyboards and mice with the scroll wheel; an I2C-HID trackpad driver. The mouse works in every app.
- **Network:** RTL8139, e1000 and iPhone USB tethering; DHCP, DNS, TCP with retransmission, HTTP, TLS 1.2.
- **Cryptography, in-kernel:** big-number arithmetic with Montgomery reduction, RSA-2048 key generation, SHA-256, AES-128-CBC, X.509 v3 and DER, PKCS#1 v1.5.
- **Desktop:** a text-mode desktop with a start menu, file manager, editor, calculator, calendar, clock, Settings, a System app, games (Minefield, 2048, Breakout and more), a manual and a guide. User accounts and twenty themes.

## Hardware status

Tested on an HP laptop with an Intel Alder Lake CPU, NVMe SSD and xHCI USB, and in QEMU and UTM. HavenDOS also runs day to day on a friend's laptop.

| On the HP laptop | Status |
| --- | --- |
| UEFI boot, install beside Windows 11 on NVMe | Working |
| Built-in keyboard | Working |
| USB mouse | Working; plugged back in, works again since 0.9.6.6 |
| USB sticks (FAT16/FAT32; exFAT read-only) | Working |
| `update` from a USB stick | Working |
| iPhone USB tethering | **Working** since 0.8.9.9: DHCP, DNS, ping, HTTP |
| iPhone pairing | Stages 1 and 2 working; stage 3 pairs, and the TLS session after it is refused by the phone (not needed for tethering) |
| External HDMI monitor | **Working** since 0.9.6.2; on 0.9.6.5 it fell back to the unscaled picture, and why is being looked into |
| Several apps at once, windows, Alt+Tab | **Working** (0.9.6.5) |
| Built-in trackpad (I2C-HID) | Its controller now gets a memory window (0.9.6.6); the search for it held the boot until 0.9.6.7. To be tested |
| MediaTek Wi-Fi | No driver (planned for 28H2) |

AHCI has found a real SATA disk on a friend's older HP.

## Try it

Download `HavenDOS-<version>.iso` from the [latest release](https://github.com/MGKFN/HavenDOS/releases/latest). A virtual machine is the easiest place to start.

```sh
# BIOS
qemu-system-i386 -m 512 -cdrom HavenDOS-0.9.6.7.iso -boot d

# UEFI (needs OVMF firmware), with a disk to install to and a USB controller
qemu-img create -f raw disk.img 4G
qemu-system-x86_64 -m 1G -bios OVMF.fd -cdrom HavenDOS-0.9.6.7.iso \
  -drive file=disk.img,if=virtio,format=raw -device qemu-xhci -boot d
```

The GRUB loader is unsigned, so **Secure Boot must be off** on real hardware.

Default accounts after installation: `admin` / `Admin@123`, `bootos` / `BootOS2026`, `guest` with no password. Change them.

**Before installing on a real machine, back up your files** and read the installer's disk preview carefully. Installing beside Windows uses existing unallocated space and does not shrink Windows for you; the whole-disk option replaces the disk layout. The HavenDOS system partition is FAT16 up to 2047 MB and FAT32 above that, and the installer takes at most 32 GB of free space, leaving the rest free.

### Updating an installed system

Download `HavenDOS-<version>-bundle.zip` and read `update/README-UPDATE.md` inside it. On Windows, `Update-HavenDOS.cmd` puts the new kernel on a USB stick or the EFI partition; on Linux, `stage-havendos-update.sh` does the same. Then run `update` in HavenDOS.

## Build it

On Debian or Ubuntu:

```sh
sudo apt install gcc-multilib make python3 python3-pip grub-common grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools dosfstools exfatprogs exfat-fuse gdisk openssl zip
pip install hid-parser

make havendos_pass1.elf && make havendos.elf && make   # the ISO
sudo make test                                          # 72 suites, 7479 checks
```

`grub-pc-bin` and `grub-efi-amd64-bin` are both needed: without the first the ISO has no BIOS boot image. The build is two-pass — the kernel embeds a copy of itself for the installer — so build the two kernels before `make`. `make test` runs as root because the exFAT fixture is made with a loop device.

`HANDOFF.md` describes the state of the tree in detail and is the place to start reading the code.

## Repository layout

| Path | What |
| --- | --- |
| `kernel/`, `boot/` | Kernel entry, interrupts, memory, timer, tasks |
| `drivers/` | Display (including the Iris Xe), keyboard, mouse, PCI, storage controllers, I2C |
| `usb/` | Host controllers, USB core, HID, mass storage, hubs, usbmux, lockdown |
| `fs/` | FAT, exFAT, GPT, the block layer, installer, updater and `migrate` |
| `net/` | TCP/IP, DNS, HTTP, TLS, RSA, X.509, plists |
| `shell/` | The shell, the desktop, the window chrome and the apps |
| `tests/` | Native test suites — `make test` |
| `tools/` | Build scripts, the checks `make` runs, and the injection harnesses |
| `harness/` | Scripted QEMU testing |
| `rescue/` | The update staging scripts for Windows and Linux |
| `docs/` | Hardware test procedures; `docs/roadmap/` holds the roadmap PDFs, built from `docs/rm100x/` |
| `changelogs/` | One changelog per release |
| `manual/` | The operator's manual |
| `archive/` | Old source archives, kept for history |

## How releases are made

Every release is built by [GitHub Actions](.github/workflows/release.yml) from the source in this repository. When a commit on `main` changes the version in `include/version.h`, the workflow builds the ISO, runs the full test suite and publishes release `v<version>` with three files:

| File | For |
| --- | --- |
| `HavenDOS-<version>.iso` | Booting and installing |
| `HavenDOS-<version>-bundle.zip` | Updating an installed system, plus the hardware test docs |
| `HavenDOS-<version>-source.zip` | Building it yourself |

A release whose tests fail is not published. Work on other branches, like `emu86`, is never published until it is merged into `main` with a new version.

## Where it's going

The plan is [THS-RM-1005](docs/roadmap/HavenDOS-Roadmap-RM1005.pdf), every release from 0.9.6.5 to December 2029.

| Release | Target | What |
| --- | --- | --- |
| 0.9.7 | ~26 Nov 2026 | The new version numbers in the updater; Retro PC; the editor's clipboard; Files' right-click menu; taskbar pins |
| 0.9.8 | ~5 Dec 2026 | Real HTTPS (TLS 1.2 with X25519 and AES-GCM, certificates checked), then signed updates over the internet |
| 0.9.9 | ~12 Dec 2026 | Hardening; real audio through the speakers (Intel HD Audio); the freeze |
| **HavenDOS 1, version 27H1 "Brandaris"** | 1 Jan 2027 | OS Build 27100.1000, the first Stable release |
| 27H2 "Lange Jaap" | Dec 2027 | The web: a browser, a package store, email |
| 28H1 "Westhoofd" | Jun 2028 | Daily use on the HP: ACPI, audio, battery, sleep |
| 28H2 "Eierland" | Dec 2028 | Wi-Fi |
| 29H1 "Bornrif" | Jun 2029 | Overlapping windows and a file manager |
| 29H2 "Westkapelle" | Dec 2029 | Polish, and building apps on HavenDOS |

Dates are targets. The roadmap says what moves when one slips.

## Bugs and contributions

[Open an issue](https://github.com/MGKFN/HavenDOS/issues) with your version, the hardware or VM settings, BIOS or UEFI, and the steps that lead to the problem. A photo of the screen or of `dmesg` helps more than a description of it.

Hardware testing is the most useful contribution there is: every release so far has found something on a real machine that emulation never showed.

## License

[MIT](LICENSE) · Tech Haven Studios
