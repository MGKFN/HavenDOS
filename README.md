# HavenDOS
official HavenDOS Repo
# HavenDOS

<p align="center">
  <strong>A small 32-bit operating system built from scratch.</strong>
</p>

<p align="center">
  Written in C and x86 Assembly · Monolithic kernel · Experimental
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#building">Building</a> •
  <a href="#running">Running</a> •
  <a href="#roadmap">Roadmap</a> •
  <a href="#contributing">Contributing</a>
</p>

---

## About

**HavenDOS** is an experimental 32-bit operating system developed by **Tech Haven Studios**.

The project started as an attempt to understand how operating systems work at a low level, from booting a machine and interacting with hardware to building a kernel and implementing system functionality.

HavenDOS is written primarily in **C and x86 Assembly** and is designed to run on x86-compatible systems and virtual machines.

The project is still under active development and should be considered **experimental software**.

> **Warning:** HavenDOS is not intended to replace your main operating system. Some versions may contain bugs that can cause data loss or prevent a system from booting. Testing in a virtual machine is strongly recommended.

---

## Features

HavenDOS currently includes or has included the following components:

- 32-bit x86 kernel
- Monolithic kernel architecture
- C and x86 Assembly
- BIOS boot support
- Text-mode interface
- 80×25 text display
- Keyboard input
- User management
- Basic text editor
- CMOS real-time clock support
- Live system time
- In-memory system snapshots
- Basic storage/filesystem experimentation
- Network stack experimentation
- Hardware driver development
- Virtual machine support

Features can change between versions as the operating system is actively developed.

---

## Architecture

HavenDOS currently follows a **monolithic kernel** design.

The project is primarily written in:

| Component | Technology |
|---|---|
| Kernel | C |
| Low-level hardware code | x86 Assembly |
| Architecture | x86 / 32-bit |
| Boot process | BIOS |
| Kernel type | Monolithic |
| Interface | Text-based |

The goal is to keep the system relatively small while allowing direct experimentation with hardware and low-level operating-system concepts.

---

## Project Structure

The repository structure may change as HavenDOS develops, but the project generally contains components such as:

```text
HavenDOS/
├── boot/          # Bootloader and boot-related code
├── kernel/        # Kernel source
├── drivers/       # Hardware drivers
├── fs/            # Filesystem/storage code
├── net/           # Networking components
├── include/       # Header files
├── tools/         # Build and development tools
├── scripts/       # Build scripts
├── docs/          # Documentation
└── README.md
```

Check the repository itself for the current structure.

---

## Building

### Requirements

A typical development environment requires:

- GCC cross-compiler or suitable C compiler
- NASM
- GNU Binutils
- Make
- GRUB or another supported bootloader
- QEMU or another x86 virtual machine

A Linux environment is recommended for building HavenDOS.

### Clone the repository

```bash
git clone https://github.com/YOUR_USERNAME/HavenDOS.git
cd HavenDOS
```

### Build

```bash
make
```

The exact build commands may change between releases. Check the repository's build scripts and documentation for the current instructions.

---

## Running

The safest way to test HavenDOS is inside a virtual machine.

For example, with QEMU:

```bash
qemu-system-i386 -cdrom havenDOS.iso
```

The exact image name and launch parameters depend on the current build system.

### Recommended

Use a virtual machine when experimenting with HavenDOS.

**Do not boot experimental builds directly on a machine containing important data.**

---

## Current Status

HavenDOS is an **experimental operating system**.

The project has gone through multiple development stages, including work on:

- Bootloader development
- Kernel development
- Memory management
- Hardware access
- Keyboard input
- Display handling
- Storage
- Filesystems
- User management
- Networking
- Drivers
- System utilities

Some components are incomplete, unstable, or temporarily disabled.

A feature appearing in the repository does not necessarily mean that it is production-ready.

---

## Roadmap

The roadmap is intentionally flexible because HavenDOS is a learning and experimentation project.

### Kernel

- [ ] Improve memory management
- [ ] Improve kernel stability
- [ ] Hardware abstraction
- [ ] Better error handling
- [ ] Kernel debugging facilities

### Hardware

- [ ] Improve storage drivers
- [ ] Improve network drivers
- [ ] Additional hardware support
- [ ] Better device management

### Filesystems

- [ ] Improve filesystem support
- [ ] Reliable disk installation
- [ ] File permissions
- [ ] Better filesystem utilities

### Networking

- [ ] Improve TCP/IP implementation
- [ ] Improve network drivers
- [ ] DNS improvements
- [ ] More reliable network applications

### Userland

- [ ] More system utilities
- [ ] Improved text editor
- [ ] Shell improvements
- [ ] More user management functionality

### Boot

- [ ] Improve bootloader
- [ ] Better BIOS compatibility
- [ ] Investigate UEFI support
- [ ] Faster boot process

---

## Versioning

HavenDOS versions follow the project's development versioning scheme.

Development releases may contain experimental or unfinished features.

For stable or recommended builds, see the repository's **Releases** section.

---

## Contributing

Contributions, bug reports and ideas are welcome.

Before submitting a pull request:

1. Make sure the code builds successfully.
2. Test your changes in a virtual machine where possible.
3. Clearly describe what you changed.
4. Include reproduction steps when fixing a bug.
5. Avoid introducing unnecessary dependencies.

For larger changes, opening an issue first is recommended so the approach can be discussed before implementation.

---

## Bug Reports

If you find a bug, open a GitHub Issue and include:

- HavenDOS version
- Hardware or virtual machine used
- Steps to reproduce the problem
- Expected behaviour
- Actual behaviour
- Relevant logs or screenshots
- Build configuration, if applicable

Do not include sensitive information in bug reports.

---

## Development Philosophy

HavenDOS is primarily a project for learning, experimentation and understanding how computers work at a low level.

The project focuses on learning by building rather than relying entirely on existing operating-system abstractions.

That means HavenDOS may sometimes contain experimental implementations, incomplete features and intentionally low-level code.

---

## Related Projects

HavenDOS is part of the projects developed under **Tech Haven Studios**.

Other projects may share ideas, components or development history with HavenDOS.

---

## License

HavenDOS is released under the **MIT License**.

See [`LICENSE`](LICENSE) for the complete license text.

Copyright © 2026 Tech Haven Studios

---

## Disclaimer

HavenDOS is provided for educational and experimental purposes.

It is not guaranteed to be stable, secure, compatible with specific hardware, or suitable for production use.

Use experimental builds at your own risk.

---

## Credits

Developed by **Tech Haven Studios**.

Built with C, x86 Assembly and a lot of experimentation.
