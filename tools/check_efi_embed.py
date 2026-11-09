#!/usr/bin/env python3
"""check_efi_embed.py -- is the UEFI loader actually inside the kernel?

This checker exists because of a bug it would have caught.

fs/esp.c includes include/grub_efi_embed.h only when HAVE_GRUB_EFI_EMBED is
defined, and falls back to a one-byte stub when it is not:

    static const uint8_t  grub_efi_img[] = { 0 };
    static const uint32_t grub_efi_img_size = 0;

The Makefile defined HAVE_GRUB_EMBED and HAVE_KERNEL_ELF but not
HAVE_GRUB_EFI_EMBED. So the tree compiled, linked, passed every suite,
built an ISO, and produced a kernel whose installer would write a
ZERO-BYTE \\EFI\\BOOT\\BOOTX64.EFI over whatever the machine was booting --
having first dutifully taken a backup, verified the empty file read back as
empty, and reported success.

That is the quietest possible version of the one failure this module was
written to prevent, and nothing in the build noticed, because every part of
it was behaving exactly as written.

fs/esp.c now refuses at runtime (ESP_E_LOADER) if the image is not a PE32+
x86_64 binary, which protects the disk. This protects the BUILD: a kernel
that cannot install under UEFI should not get as far as being an ISO that
claims it can.

Run:  python3 tools/check_efi_embed.py havendos.elf
"""
import sys

MIN = 64 * 1024          # must match ESP_LOADER_MIN in fs/esp.c


def find_pe32plus(data):
    """Every PE image embedded in the blob, as (offset, machine, name)."""
    out = []
    i = 0
    while True:
        i = data.find(b"MZ", i)
        if i < 0:
            return out
        if i + 0x40 <= len(data):
            off = int.from_bytes(data[i + 0x3C:i + 0x40], "little")
            if 0 < off < 4096 and i + off + 6 <= len(data) \
               and data[i + off:i + off + 4] == b"PE\0\0":
                mach = int.from_bytes(data[i + off + 4:i + off + 6], "little")
                out.append((i, mach, {0x8664: "x86_64",
                                      0x014C: "i386",
                                      0xAA64: "aarch64"}.get(mach, "?")))
        i += 2


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_efi_embed.py <kernel.elf>")
    path = sys.argv[1]
    try:
        data = open(path, "rb").read()
    except OSError as e:
        sys.exit("check_efi_embed: cannot read %s: %s" % (path, e))

    images = find_pe32plus(data)
    x64 = [im for im in images if im[1] == 0x8664]

    if not images:
        sys.stderr.write(
            "check_efi_embed: FAIL -- no PE image anywhere in %s.\n"
            "  fs/esp.c compiled against its zero-byte stub, so this kernel's\n"
            "  installer cannot write a UEFI loader. It will refuse with\n"
            "  ESP_E_LOADER at runtime rather than write an empty one, but it\n"
            "  should not have been built at all.\n"
            "  Fix: HAVE_GRUB_EFI_EMBED must be in CFLAGS and CFLAGS_PASS1, and\n"
            "  include/grub_efi_embed.h must be a prerequisite of the object\n"
            "  rules -- -MMD -MP cannot discover a header the preprocessor was\n"
            "  never told to include.\n" % path)
        return 1

    if not x64:
        sys.stderr.write(
            "check_efi_embed: FAIL -- %d PE image(s) in %s, none for x86_64:\n"
            % (len(images), path))
        for off, mach, name in images:
            sys.stderr.write("    offset 0x%06X  machine 0x%04X (%s)\n"
                             % (off, mach, name))
        sys.stderr.write(
            "  i386 is what include/grub_embed.h and grub_disk_embed.h are\n"
            "  built as (-O i386-pc). No 64-bit firmware will execute one.\n"
            "  The install embed must be built -O x86_64-efi.\n")
        return 1

    # The kernel carries two copies on purpose: its own, and the one inside
    # the pass-1 ELF embedded by kernel_elf.h, so that an INSTALLED HavenDOS
    # can install HavenDOS. One copy means pass 1 was built without the
    # define, and the installed system would be the one that cannot.
    where = ", ".join("0x%06X" % off for off, _, _ in x64)
    if len(x64) < 2:
        sys.stderr.write(
            "check_efi_embed: FAIL -- only one copy.\n"
            "  The final kernel should carry two: its own, and the one inside\n"
            "  the pass-1 ELF that include/kernel_elf.h embeds. With one, the\n"
            "  ISO can install a UEFI system but the system it installs\n"
            "  cannot -- a difference that shows up only on the second\n"
            "  install, on someone else's machine.\n"
            "  Fix: HAVE_GRUB_EFI_EMBED in CFLAGS_PASS1 as well as CFLAGS.\n"
            "  Found one, at %s.\n" % where)
        return 1

    print("check_efi_embed: ok, %d x86_64 PE32+ image(s) at %s"
          % (len(x64), where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
