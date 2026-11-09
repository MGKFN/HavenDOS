#!/usr/bin/env python3
"""check_iso_bootable.py -- the ISO must be able to boot both ways.

This check exists because a HavenDOS ISO that could not BIOS-boot at all
passed every other check in the build.

`grub-mkrescue` builds whatever boot images the GRUB platform packages
on the machine allow. With `grub-efi-amd64-bin` present and
`grub-pc-bin` ABSENT it emits a perfectly valid ISO carrying a UEFI El
Torito image and NO BIOS one -- 3.3 MB instead of 15 MB -- and says
nothing about it. A BIOS machine finds no boot image, falls through to
the next boot device, and lands in PXE.

Found by booting one: `q.text()` came back full of iPXE's banner
instead of the HavenDOS boot log, which is the only reason anybody
noticed. Every one of the eight build checks had passed on that ISO,
`check_iso_version.py` included -- it compares the kernel's version
with version.txt, and both were perfectly correct inside an ISO that
could not start.

v0.8.8.7 -- AND THE UEFI IMAGE HAS TO BE 64-BIT.

The check above passed an ISO that real firmware refuses. El Torito
platform id 0xEF means "UEFI" and says NOTHING about the architecture
of the loader inside: an efi.img carrying only BOOTIA32.EFI reports as
UEFI exactly like one carrying BOOTX64.EFI. Every x86_64 machine made
this decade -- Kyan's HP included, which has no CSM at all -- looks for
EFI/BOOT/BOOTX64.EFI and nothing else.

Booted under OVMF, the ia32-only ISO produced:

    BdsDxe: failed to load Boot0001 "UEFI QEMU DVD-ROM ..." : Not Found
    >>Start PXE over IPv4.

and this file said "ok, boots BIOS and UEFI". So the platform list is
necessary and not sufficient, and the fourth judge below opens the EFI
image and reads the PE machine field out of the loader itself.

Two judges, because our own code agreeing with itself is not evidence:

  1. The raw bytes. Sector 17 of an El Torito disc holds a Boot Record
     Volume Descriptor identifying "EL TORITO SPECIFICATION". No
     xorriso, no GRUB, just the file.
  2. xorriso, which already has to exist because grub-mkrescue uses it,
     reporting the platform of each boot image it finds.

Run:  python3 tools/check_iso_bootable.py havendos.iso
"""
import os
import re
import tempfile
import shutil
import subprocess
import sys

SECTOR = 2048
BRVD_LBA = 17                      # El Torito's fixed boot record sector
EL_TORITO_ID = b"EL TORITO SPECIFICATION"

WANT = ("BIOS", "UEFI")

HINT = {
    "BIOS": ("install grub-pc-bin (the i386-pc GRUB modules) and rebuild; "
             "without it grub-mkrescue silently omits the BIOS boot image"),
    "UEFI": ("install grub-efi-amd64-bin (the x86_64-efi GRUB modules) and "
             "rebuild; the HP boots UEFI and will not find this disc"),
}


def raw_has_el_torito(path):
    """Is there a Boot Record Volume Descriptor at all?"""
    with open(path, "rb") as f:
        f.seek(BRVD_LBA * SECTOR)
        d = f.read(SECTOR)
    if len(d) < 71:
        return False, "the image is too short to hold a boot record"
    # ISO 9660 volume descriptor: type byte, then "CD001", then version.
    if d[1:6] != b"CD001":
        return False, ("sector %d is not an ISO 9660 volume descriptor "
                       "(no CD001)" % BRVD_LBA)
    if d[0] != 0:
        return False, ("sector %d is volume descriptor type %d, not a boot "
                       "record (type 0)" % (BRVD_LBA, d[0]))
    if EL_TORITO_ID not in d[7:39]:
        return False, "the boot record does not identify El Torito"
    return True, "boot record present"


def xorriso_platforms(path):
    """Platforms xorriso reports a boot image for."""
    try:
        r = subprocess.run(["xorriso", "-indev", path,
                            "-report_el_torito", "plain"],
                           capture_output=True, text=True, timeout=120)
    except FileNotFoundError:
        return None, "xorriso is not installed"
    except subprocess.TimeoutExpired:
        return None, "xorriso did not finish"
    found = {}
    for line in (r.stdout + r.stderr).splitlines():
        if "El Torito boot img" not in line:
            continue
        # El Torito boot img :   1  BIOS  y   none  0x0000  0x00  4  2978
        m = re.search(r"boot img\s*:\s*(\d+)\s+(\S+)", line)
        if m:
            found.setdefault(m.group(2).upper(), line.strip())
    return found, None


def iso_grub_cfg(iso):
    """The grub.cfg packed INTO the ISO, as text, or (None, why).

    Out of the image and not off disk: the point is what the disc
    holds, and a build that packed the wrong thing is exactly the
    case worth catching.
    """
    # To a real file, not to stdout. `-extract ... -` returns 0 and
    # writes NOTHING, so the first version of this reported a perfectly
    # good ISO as having an empty menu -- a checker that fails a
    # correct build is worse than the gap it was closing, and it was
    # caught only by extracting the file by hand to see.
    tmp = tempfile.mkdtemp(prefix="hd_iso_")
    out = os.path.join(tmp, "grub.cfg")
    try:
        try:
            r = subprocess.run(
                ["xorriso", "-osirrox", "on", "-indev", iso,
                 "-extract", "/boot/grub/grub.cfg", out],
                capture_output=True, timeout=60)
        except FileNotFoundError:
            return None, "xorriso is not installed, so it could not be read"
        except subprocess.SubprocessError as e:
            return None, "xorriso failed: %s" % e
        if r.returncode != 0 or not os.path.exists(out):
            return None, "no /boot/grub/grub.cfg could be extracted from it"
        with open(out, "rb") as f:
            return f.read().decode("utf-8", "replace"), None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)



# ── judge 4: the architecture of the loader on the EFI image ─────────

PE_MACHINE = {0x8664: "x86_64", 0x014C: "i386", 0xAA64: "aarch64"}


def _pe_machine(data):
    """PE machine id, or None if this is not a PE image."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    off = int.from_bytes(data[0x3C:0x40], "little")
    if off + 6 > len(data) or data[off:off + 4] != b"PE\0\0":
        return None
    return int.from_bytes(data[off + 4:off + 6], "little")


def efi_image_path(iso):
    """The in-ISO path of the UEFI El Torito image, per xorriso."""
    try:
        r = subprocess.run(["xorriso", "-indev", iso,
                            "-report_el_torito", "plain"],
                           capture_output=True, text=True, timeout=120)
    except (FileNotFoundError, subprocess.SubprocessError):
        return None
    uefi_idx = None
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"boot img\s*:\s*(\d+)\s+(\S+)", line)
        if m and m.group(2).upper() == "UEFI":
            uefi_idx = m.group(1)
    if uefi_idx is None:
        return None
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"img path\s*:\s*(\d+)\s+(\S+)", line)
        if m and m.group(1) == uefi_idx:
            return m.group(2)
    return None


def uefi_loader_arch(iso):
    """(machine_id, how_we_know) for EFI/BOOT/BOOTX64.EFI, or (None, why).

    mtools when it is there, because that reads the real directory and
    the real file. A raw scan when it is not -- weaker, and labelled as
    weaker, because it proves an x86_64 PE is somewhere on the image
    rather than that it is the file the firmware will open.
    """
    path = efi_image_path(iso)
    if path is None:
        return None, "xorriso reported no UEFI boot image path"

    tmp = tempfile.mkdtemp(prefix="hd_efi_")
    try:
        img = os.path.join(tmp, "efi.img")
        try:
            r = subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                                "-extract", path, img],
                               capture_output=True, timeout=120)
        except (FileNotFoundError, subprocess.SubprocessError) as e:
            return None, "the EFI image could not be extracted: %s" % e
        if r.returncode != 0 or not os.path.exists(img):
            return None, "the EFI image %s could not be extracted" % path

        out = os.path.join(tmp, "BOOTX64.EFI")
        have_mtools = shutil.which("mcopy") is not None
        if have_mtools:
            # Does the directory entry exist at all? mcopy returning
            # non-zero does NOT mean mtools is missing -- the first
            # version of this said "mtools is not installed" on an ISO
            # whose only fault was having no BOOTX64.EFI, which sent
            # the reader after the wrong thing entirely.
            r = subprocess.run(["mcopy", "-n", "-i", img,
                                "::/EFI/BOOT/BOOTX64.EFI", out],
                               capture_output=True, timeout=60)
            if r.returncode != 0 or not os.path.exists(out):
                return None, ("the EFI image has no /EFI/BOOT/BOOTX64.EFI "
                              "(read with mtools)")
            with open(out, "rb") as f:
                head = f.read(4096)
            m = _pe_machine(head)
            if m is None:
                return None, ("/EFI/BOOT/BOOTX64.EFI on the EFI image is "
                              "not a PE executable")
            return m, "read out of /EFI/BOOT/BOOTX64.EFI with mtools"

        with open(img, "rb") as f:
            blob = f.read()
        if b"BOOTX64 EFI" not in blob:
            return None, ("the EFI image has no BOOTX64.EFI directory entry "
                          "(raw scan: mtools is not installed)")
        if b"PE\0\0\x64\x86" in blob:
            return 0x8664, ("raw scan of the EFI image -- mtools is not "
                            "installed, so this proves an x86_64 PE is on "
                            "the image, not that it is that file")
        return None, ("the EFI image holds a BOOTX64.EFI entry but no "
                      "x86_64 PE header (raw scan: mtools is not installed)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_iso_bootable.py <iso>")
    iso = sys.argv[1]
    if not os.path.exists(iso):
        print("check_iso_bootable: cannot read %s" % iso)
        return 1

    ok, why = raw_has_el_torito(iso)
    if not ok:
        print("check_iso_bootable: %s has no El Torito boot catalog\n" % iso)
        print("  %s" % why)
        print("\n  This disc cannot boot on any machine. A BIOS will fall")
        print("  through to the next boot device -- in QEMU that is PXE, and")
        print("  what comes up is iPXE, not HavenDOS.")
        return 1

    plats, err = xorriso_platforms(iso)
    if plats is None:
        # Not a pass: the raw check only proves a catalog exists, not that
        # it holds an image for each platform.
        print("check_iso_bootable: boot record present, but %s" % err)
        print("  so the per-platform images could not be verified.")
        return 1

    missing = [p for p in WANT if p not in plats]
    if missing:
        print("check_iso_bootable: %s boots %s only\n"
              % (iso, ", ".join(sorted(plats)) or "nothing"))
        for p in missing:
            print("  no %s boot image -- %s" % (p, HINT[p]))
        print("\n  Both are needed: the HP boots UEFI, and the BIOS image is")
        print("  what every other machine and `qemu-system-i386 -cdrom`")
        print("  uses. An ISO missing one of them still passes every other")
        print("  check in this build, version checks included.")
        return 1

    # ── AND THE UEFI LOADER HAS TO BE x86_64 ─────────────────────────
    mach, how = uefi_loader_arch(iso)
    if mach != 0x8664:
        print("check_iso_bootable: %s reports a UEFI boot image, but it is "
              "not 64-bit\n" % iso)
        if mach is None:
            print("  %s" % how)
        else:
            print("  \\EFI\\BOOT\\BOOTX64.EFI is %s (PE machine 0x%04X), "
                  "not x86_64" % (PE_MACHINE.get(mach, "unknown"), mach))
        print("\n  %s" % HINT["UEFI"])
        print("  A 64-bit machine with no CSM -- which is every laptop made")
        print("  this decade -- will not find a loader on this disc and will")
        print("  fall through to PXE. Verified under OVMF, not reasoned out.")
        return 1

    # ── AND IT HAS TO HAVE A MENU ────────────────────────────────────
    # v0.8.8.6. An ISO built from an EMPTY grub.cfg passed every check
    # here: the El Torito catalog was intact and both platform images
    # were present, so the disc "boots" in the only sense this file
    # was measuring. What it boots to is a GRUB rescue prompt.
    #
    # It happened by deleting isodir/boot/grub/grub.cfg on the
    # assumption that the build would regenerate it. It did not, the
    # file came back zero bytes, and the build said ok.
    #
    # Read out of the ISO rather than off disk, because the file on
    # disk is not necessarily the file that got packed.
    cfg, why = iso_grub_cfg(iso)
    if cfg is None:
        print("check_iso_bootable: %s has boot images but %s" % (iso, why))
        print("  A disc with no readable grub.cfg boots to a rescue")
        print("  prompt, which is not a failure any other check sees.")
        return 1
    n = cfg.count("menuentry")
    if n == 0:
        print("check_iso_bootable: %s has boot images and an EMPTY menu\n"
              % iso)
        print("  grub.cfg is %d bytes and holds no menuentry, so this"
              % len(cfg))
        print("  disc boots to a GRUB rescue prompt. Every other check")
        print("  in this build passes an ISO in that state, version")
        print("  checks included -- which is how it shipped past one.")
        return 1

    size_mb = os.path.getsize(iso) / (1024.0 * 1024.0)
    print("check_iso_bootable: ok, %s boots %s (UEFI loader x86_64), "
          "%d menu entries (%.1f MB)"
          % (iso, " and ".join(p for p in WANT if p in plats), n, size_mb))
    # A weaker verification has to say that it was weaker, or the line
    # above claims more than was actually established.
    if "raw scan" in how:
        print("  note: %s" % how)
    return 0


if __name__ == "__main__":
    sys.exit(main())
