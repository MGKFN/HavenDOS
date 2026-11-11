# HavenDOS

HavenDOS is an operating system written from scratch in C and x86 assembly by Tech Haven Studios: its own 32-bit kernel, drivers, filesystems, TCP/IP and TLS stack, a shell and a text-mode desktop with the look of an old DOS machine, on modern UEFI hardware.

It boots on a 2022 HP laptop from its own partition beside Windows 11, reads USB sticks, and is part-way through teaching itself to pair with an iPhone so it can use the phone's connection — that laptop has no Ethernet port.

[**Download the latest release**](https://github.com/MGKFN/HavenDOS/releases/latest) · [All releases](https://github.com/MGKFN/HavenDOS/releases) · [Report a bug](https://github.com/MGKFN/HavenDOS/issues)

## Current version: 0.8.9.8 Beta "Helios"

**0.8.9.8** makes DHCP work, for the first time on any network. It had
never worked; in QEMU nobody could tell, because the fallback address is
QEMU's own. After 0.8.9.7 the iPhone shows the Personal Hotspot
indicator, which iOS only does for a host it trusts, so pairing appears
to work and the phone is tethering. This release lets HavenDOS ask that
network for an address.
[Changelog](changelogs/CHANGELOG-v0.8.9.8.md).


**0.8.9.7** fixes the iPhone pairing transport. Messages that were
exactly a multiple of 512 bytes were never terminated, so the phone
glued the pair request's two packets together. And the phone's own
explanation, a usbmux CONTROL message, was thrown away unread.
[Changelog](changelogs/CHANGELOG-v0.8.9.7.md).

**0.8.9.6 was a performance release.** The framebuffer is now
write-combining. It was uncached on real hardware, which made every
redraw crawl. The mouse moves at the same speed on every resolution,
the cursor reaches the whole screen on high-resolution grids, and the
trackpad driver powers its controller up and waits for the device after
resetting it. USB no longer tears the whole bus down every second when
a port flaps, and the kernel log keeps recording after the boot.
[Changelog](changelogs/CHANGELOG-v0.8.9.6.md).

### Helios

Helios is the iPhone release. The HP laptop HavenDOS is tested on has no Ethernet port and its Wi-Fi chip has no driver, so its route to a network is an iPhone over USB — and iOS only turns tethering on for a computer it has *paired* with. Pairing means a usbmux channel, Apple property lists, two 2048-bit RSA keys generated on the machine, three X.509 certificates, and TLS 1.2 with a client certificate, all written for this kernel.

Stages 1 and 2 of pairing are confirmed on the HP: lockdownd answers, and the phone hands over its public key. Stage 3 — the pair request itself — reached the phone for the first time in 0.8.9.4. 0.8.9.5 stopped the connection sitting idle through the 71-second keygen, and 0.8.9.7 ends full-size USB transfers the way the phone expects.

Every release has its full changelog in [`changelogs/`](changelogs/).

## What's in it

- **Boot:** one hybrid ISO for BIOS and UEFI through GRUB. Framebuffer console at any resolution the firmware offers, with the character grid chosen by the user.
- **Install:** beside an existing Windows installation in free space, or on an empty disk with its own EFI System Partition. `uninstall` removes it again and never touches a boot loader it did not put there.
- **Update:** `update` installs a newer kernel from a USB stick, the EFI partition or CD, and checks a CRC before writing anything.
- **Storage:** NVMe, AHCI, ATA/IDE and VirtIO disks; USB mass storage. FAT12, FAT16 and FAT32 read-write, exFAT read-only, GPT and MBR.
- **USB:** xHCI and UHCI, hubs, keyboards and mice, mass storage, and Apple's USB Ethernet interface.
- **Input:** PS/2 and USB keyboards and mice; an I2C-HID trackpad driver.
- **Network:** RTL8139, e1000 and iPhone USB tethering; DHCP (working since 0.8.9.8), DNS, TCP with retransmission, HTTP, TLS 1.2.
- **Cryptography, in-kernel:** big-number arithmetic with Montgomery reduction, RSA-2048 key generation, SHA-256, AES-128-CBC, X.509 v3 and DER, PKCS#1 v1.5.
- **Desktop:** a text-mode desktop with a start menu, file manager, editor, calculator, calendar, clock, chat client, games (Minefield among them) and settings. User accounts and themes.

## Hardware status

Tested on an HP laptop with an Intel Alder Lake CPU, NVMe SSD and xHCI USB, and in QEMU and UTM. HavenDOS also runs day to day on a friend's laptop.

| On the HP laptop | Status |
| --- | --- |
| UEFI boot, install beside Windows 11 on NVMe | Working |
| Built-in keyboard | Working |
| USB mouse | Working; slow and choppy before 0.8.9.6 |
| USB sticks (FAT16/FAT32; exFAT read-only) | Working |
| `update` from a USB stick | Working |
| iPhone pairing, stages 1 and 2 | Working |
| iPhone pairing, stage 3 | The phone shows the Personal Hotspot indicator after `pair`; networking over it is next |
| Built-in trackpad (I2C-HID) | Not working in 0.8.9.5; 0.8.9.6 fixes two likely causes, untested |
| MediaTek Wi-Fi | No driver |

AHCI has only ever run against simulated controllers — there is no SATA machine to test it on.

**Known issues:** the 0.8.9.6 performance and pointer fixes have not been confirmed on the HP yet. A USB transaction error can still make one rescan come back empty; 0.8.9.6 retries it once and holds off a port that keeps flapping.

## Try it

Download `HavenDOS-<version>.iso` from the [latest release](https://github.com/MGKFN/HavenDOS/releases/latest). A virtual machine is the easiest place to start.

```sh
# BIOS
qemu-system-i386 -m 512 -cdrom HavenDOS-0.8.9.8.iso -boot d

# UEFI (needs OVMF firmware), with a disk to install to and a USB controller
qemu-img create -f raw disk.img 4G
qemu-system-x86_64 -m 1G -bios OVMF.fd -cdrom HavenDOS-0.8.9.8.iso \
  -drive file=disk.img,if=virtio,format=raw -device qemu-xhci -boot d
```

The GRUB loader is unsigned, so **Secure Boot must be off** on real hardware.

Default accounts after installation: `admin` / `Admin@123`, `bootos` / `BootOS2026`, `guest` with no password. Change them.

**Before installing on a real machine, back up your files** and read the installer's disk preview carefully. Installing beside Windows uses existing unallocated space and does not shrink Windows for you; the whole-disk option replaces the disk layout. The HavenDOS system partition is FAT16, so it is limited to 2047 MB.

### Updating an installed system

Download `HavenDOS-<version>-bundle.zip` and read `update/README-UPDATE.md` inside it. On Windows, `Update-HavenDOS.cmd` puts the new kernel on a USB stick or the EFI partition; on Linux, `stage-havendos-update.sh` does the same. Then run `update` in HavenDOS.

## Build it

On Debian or Ubuntu:

```sh
sudo apt install gcc-multilib make python3 python3-pip grub-common grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools dosfstools exfatprogs exfat-fuse gdisk openssl zip
pip install hid-parser

make havendos_pass1.elf && make havendos.elf && make   # the ISO
sudo make test                                          # 60 suites
```

`grub-pc-bin` and `grub-efi-amd64-bin` are both needed: without the first the ISO has no BIOS boot image. The build is two-pass — the kernel embeds a copy of itself for the installer — so build the two kernels before `make`. `make test` runs as root because the exFAT fixture is made with a loop device.

`HANDOFF.md` describes the state of the tree in detail and is the place to start reading the code.

## Repository layout

| Path | What |
| --- | --- |
| `kernel/`, `boot/` | Kernel entry, interrupts, memory, timer |
| `drivers/` | Display, keyboard, mouse, PCI, storage controllers, I2C |
| `usb/` | Host controllers, USB core, HID, mass storage, hubs, usbmux, lockdown |
| `fs/` | FAT, exFAT, GPT, the block layer, installer and updater |
| `net/` | TCP/IP, DNS, HTTP, TLS, RSA, X.509, plists |
| `shell/` | The shell, the desktop and its apps |
| `tests/` | Native test suites — `make test` |
| `tools/` | Build scripts and the checks `make` runs |
| `harness/` | Scripted QEMU testing |
| `rescue/` | The update staging scripts for Windows and Linux |
| `docs/` | Hardware test procedures; `docs/roadmap/` holds the road-to-1.0 plans |
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

A release whose tests fail is not published.

## Where it's going

The plan to 1.0 is in [`docs/roadmap/`](docs/roadmap/). 1.0 is the hardware release: install, boot, update and run on real machines without a second operating system's help. After it, 1.1 brings modern TLS and 1.2 a web browser.

## Bugs and contributions

[Open an issue](https://github.com/MGKFN/HavenDOS/issues) with your version, the hardware or VM settings, BIOS or UEFI, and the steps that lead to the problem. A photo of the screen or of `dmesg` helps more than a description of it.

Hardware testing is the most useful contribution there is: every release so far has found something on a real machine that emulation never showed.

## License

[MIT](LICENSE) · Tech Haven Studios
