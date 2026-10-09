# HavenDOS

**An independent operating system built from scratch by Tech Haven Studios.**

C and x86 Assembly · 32-bit monolithic kernel · BIOS and UEFI boot

[Downloads](https://github.com/MGKFN/HavenDOS/releases) · [Report a bug](https://github.com/MGKFN/HavenDOS/issues) · [Features](#features) · [Running](#running) · [Roadmap](#roadmap)

## About

HavenDOS is a small operating system with its own kernel, hardware drivers, filesystem code, networking stack and desktop utilities. It began as a low-level learning project and has grown into a bootable environment for compatible x86 computers and virtual machines.

**0.8.1 is designated the project's first official stable release.** Its major milestone is booting on real UEFI hardware with framebuffer rendering, extending HavenDOS beyond machines that provide legacy BIOS or CSM support.

On **9 October 2026**, the developer demonstrated HavenDOS booting and displaying its setup interface on an HP laptop with UEFI firmware and no CSM. Installation on that laptop's NVMe SSD remains unsupported.

“Stable” identifies the project's release channel. HavenDOS remains a hobby operating system with limited hardware support and experimental components; it is not a general-purpose replacement for Windows or Linux. Some interface text in the supplied 0.8.1 image still says “Beta”.

## Getting started

Download `HavenDOS-0.8.1.iso` and the accompanying checksum from the [Releases page](https://github.com/MGKFN/HavenDOS/releases). Start with a virtual machine and choose **Desktop** from the boot menu.

For installation tests, use an empty virtual disk. The installer formats its selected target; keep important disks out of the test environment.

## Features

### Boot and display

- BIOS and x86-64 UEFI boot paths packaged in one ISO, using GRUB to load the 32-bit kernel.
- Framebuffer rendering for the character-based interface on UEFI hardware.
- A logical 80×25 interface with desktop, shell and safe-mode boot options.
- Preferred boot-menu display mode of 1280×800, with fallback modes. The visible desktop may occupy only part of a larger display.
- Boot-menu diagnostics for firmware information, kernel handoff and framebuffer setup.

### Desktop and utilities

- User management, keyboard input and desktop utilities.
- Text editor, calculator, calendar and to-do list.
- CMOS real-time clock and live system time.
- Settings, hardware information and shell commands.

### Storage and updates

- FAT16 disk storage, with FAT12 and RAM filesystem components.
- ATA/IDE and VirtIO block support on compatible configurations.
- Disk installer for supported storage devices.
- ISO-based update and rollback facilities on supported disk installations.

### Networking and hardware

- Networking components including ARP, ICMP, UDP, TCP, DHCP and DNS.
- RTL8139 and e1000 network support on compatible configurations.
- PCI device enumeration and hardware reporting.

Availability depends on the selected boot mode, hardware and configuration. Networking and device support continue to receive development and testing.

## Architecture

| Component | Implementation |
|---|---|
| Kernel | 32-bit x86, monolithic |
| Languages | C and x86 Assembly |
| ISO bootloader | GRUB / Multiboot |
| Firmware boot paths | BIOS and x86-64 UEFI |
| Interface | Character-based desktop and shell; framebuffer rendering on UEFI |
| Persistent filesystem | FAT16 on supported disks |
| Storage interfaces | ATA/IDE and VirtIO block |

UEFI boot support does not make the kernel 64-bit or provide drivers for every device supported by the firmware.

## Running

### Quick BIOS boot with QEMU

With QEMU installed, run this from the directory containing the ISO:

```bash
qemu-system-i386 -m 128 -cdrom HavenDOS-0.8.1.iso -boot d
```

This example boots the ISO without attaching an installation disk. It is a suggested launch configuration, not an independently verified test of this release.

The boot menu includes **Desktop**, **Shell Mode**, **Safe Mode (no mouse)** and **Install to Disk**, followed by diagnostic entries.

### UEFI and real hardware

Use an x86-64 UEFI virtual machine or compatible computer. UEFI virtual machines require suitable firmware, such as OVMF; ordinary QEMU BIOS boot does not test the UEFI path.

UEFI boot and display have been demonstrated on the developer's HP laptop. This confirms that boot path on that machine, rather than general laptop compatibility. Secure Boot compatibility has not been established.

## Known limitations

- **NVMe storage is unsupported.** The tested HP laptop boots HavenDOS but cannot install to its internal NVMe SSD. Recognizing a controller in a hardware list does not mean it has a working storage driver.
- **The display may remain at the top-left.** The interface does not yet scale to fill every panel or selected framebuffer mode.
- **UEFI boot does not guarantee a bootable UEFI disk installation.** The demonstrated UEFI result is booting the supplied image; installation depends on the disk driver and installer boot layout.
- **Storage support is configuration-dependent.** Do not assume modern SATA/AHCI, Intel VMD/RST or USB storage works merely because firmware can boot the ISO.
- **USB support is incomplete.** Bootloader USB access and HavenDOS runtime USB drivers are separate capabilities.
- **Networking remains under development.** TCP reliability and compatibility vary. BootChat availability also depends on a compatible running server; a bundled client does not guarantee a public service is online.
- **Some 0.8.1 interface labels still say “Beta”.** The supplied release image retains those labels.

Back up data before any installation or update. A successful boot is not evidence that disk writes are safe on another machine.

## Source and building

```bash
git clone https://github.com/MGKFN/HavenDOS.git
cd HavenDOS
```

The repository currently contains `LICENSE`, this README and the archived source package `havendos-v0_7_18-source.zip`. That archive is an earlier version, not the matching source tree for the 0.8.1 ISO.

Extract a source archive and follow its included build instructions. There is currently no top-level build tree in this repository, so running `make` immediately after cloning is not a documented way to reproduce 0.8.1.

Typical source-build tools include a suitable x86 C toolchain, NASM, GNU Binutils, Make and GRUB image-building tools. Exact dependencies and commands belong to the relevant source version.

## Roadmap

- [x] Boot on real UEFI hardware and render the setup interface through a framebuffer.
- [ ] Improve display scaling, centering and mode handling.
- [ ] Add native NVMe storage support.
- [ ] Expand storage and USB compatibility.
- [ ] Validate UEFI disk installation and boot layouts.
- [ ] Improve networking reliability and driver coverage.
- [ ] Publish matching source and reproducible build instructions for current releases.
- [ ] Expand hardware testing and improve installer/update reliability.

These are development goals, not promised delivery dates.

## Contributing and bug reports

Bug reports, hardware test results and focused contributions are welcome through [GitHub Issues](https://github.com/MGKFN/HavenDOS/issues).

Include the version, computer or VM configuration, BIOS/UEFI boot mode, steps to reproduce, expected result and actual result. Attach relevant logs or screenshots and describe the disk/controller configuration for storage problems.

For code contributions, identify the source version, describe the change and test it in a virtual machine where practical. Discuss substantial architecture changes before implementation.

## License

The repository is licensed under the [MIT License](LICENSE). See that file for the authoritative copyright notice and terms.

Developed by **Tech Haven Studios**.
