# HavenDOS

HavenDOS is a hobby operating system written in C and x86 Assembly, developed by Tech Haven Studios. It has its own 32-bit kernel, drivers, filesystems and networking stack, with a text-based desktop and shell.

It started with a letter on a screen. It now boots on real UEFI hardware and runs from an NVMe SSD alongside Windows 11.

[Download HavenDOS](https://github.com/MGKFN/HavenDOS/releases) · [Report a bug](https://github.com/MGKFN/HavenDOS/issues)

## Current version

**0.8.3.1 Beta — Lodger**

On 18 October 2026, HavenDOS was installed to a Samsung NVMe SSD in an HP laptop. It boots from its own partition, the keyboard works, and Windows 11 still boots. Both the built-in touchpad and an external USB mouse are currently unusable on that laptop.

This release fixes NVMe access after installation, adds AHCI support, fixes installation on disks larger than 2 GB, and corrects the interrupt handlers and panic reporting.

The release includes the ISO, matching source archive and full changelog. Development is working towards 1.0.

## What's in it

- BIOS and UEFI boot through GRUB, with framebuffer output on UEFI systems.
- A text desktop, shell, file manager, editor, calculator, calendar and games.
- User accounts, themes, settings and hardware information tools.
- FAT16 storage, FAT32 support, and ATA/IDE, AHCI, NVMe and VirtIO block drivers.
- An installer that can use free space beside an existing Windows installation.
- A networking stack with RTL8139 and e1000 drivers.

## Try it

Download `HavenDOS-0.8.3.1.iso` from [Releases](https://github.com/MGKFN/HavenDOS/releases). A virtual machine is the easiest place to start. For a basic BIOS boot with QEMU:

```sh
qemu-system-i386 -m 128 -cdrom HavenDOS-0.8.3.1.iso -boot d
```

UEFI testing needs UEFI firmware such as OVMF. The supplied GRUB loader is unsigned, so Secure Boot must be off.

For installation, back up your files first and read the disk preview carefully. The option to install beside Windows uses existing unallocated space; it does not shrink Windows for you. The whole-disk option replaces the disk layout. HavenDOS partitions are currently limited to 2047 MB.

On the tested HP laptop, a firmware boot entry had to be added manually from Windows. Automatic boot-entry creation is not implemented yet.

## Hardware status

| On the tested HP laptop | Status |
| --- | --- |
| UEFI boot | Working |
| Install and run from NVMe | Working |
| Dual boot with Windows 11 | Working |
| Built-in keyboard | Working |
| Touchpad and external USB mouse | Not working |
| MediaTek network adapter | Unsupported |

AHCI has been tested with simulated controllers, but has not yet been verified on physical SATA hardware. Other computers may behave differently.

USB support is still limited: there is no xHCI or USB mass-storage driver. The current updater reads an ATAPI CD-ROM, so it cannot provide an upgrade path on the tested laptop. Reinstallation also has a known issue when an existing `BOOTX64.HDB` backup is present.

A panic occurred once while the laptop was idle. The interrupt-frame bug has since been fixed, but the cause of that particular panic is not confirmed.

## Source

Download `havendos-v0_8_3_1-source.zip` from the release assets and extract it. It contains the matching source tree, Makefile, tests and development notes. The older 0.7.18 archive in this repository is kept for history.

The build uses GCC with 32-bit support, GNU Binutils, Make, Python 3 and GRUB tools. See the included Makefile for the build targets and required image-building utilities. GitHub's automatic “Source code” downloads contain the repository snapshot; use the named source ZIP for the 0.8.3.1 code.

## What's next

The immediate priorities are updates through the EFI System Partition, smoother reinstalls, better display sizing, and xHCI/USB input and storage support.

## Bugs and contributions

[Open an issue](https://github.com/MGKFN/HavenDOS/issues) with your version, hardware or VM settings, boot mode and steps to reproduce the problem. A photo of the error screen or boot log helps.

Hardware testing and code contributions are welcome.

## License

[MIT](LICENSE) · Tech Haven Studios
