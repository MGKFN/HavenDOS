#!/usr/bin/env python3
"""esp_update.py -- prove `update` can read a new kernel off the EFI System
Partition, on a machine with no optical drive.

THE THING BEING PROVED

Until v0.8.4 fs/update.c read the new kernel from an ATAPI CD-ROM and from
nowhere else. The laptop this project is aimed at has no optical drive, no
USB mass storage in this kernel and no NIC anything here drives, so every
release reached it by re-running the additive installer beside a live
Windows partition -- a write to somebody's Windows disk, once per release,
because of where a file was.

This harness does what a person would do:

  1. build a Windows-shaped GPT disk with an ESP on it
  2. install HavenDOS beside it, under OVMF, from the ISO
  3. REMOVE THE ISO. From the host, with mtools -- standing in for
     Windows' `mountvol` -- copy a newer kernel and a manifest into
     \\EFI\\HAVENDOS\\update\\ on that same ESP
  4. boot the installed system from its own disk, with no removable media
     attached at all, and run `update`
  5. check it found the ESP, verified the checksum, and wrote the kernel
  6. reboot and check the version actually changed

Step 3 is the whole point, and step 4 proves it without a CD-ROM anywhere
in the machine.

WHAT IT ALSO CHECKS, BECAUSE THE ESP IS THE DANGEROUS PARTITION

Every file under \\EFI\\Microsoft is hashed before and after and must be
byte-for-byte identical: the update mounts the ESP READ ONLY, and a claim
that cheap is worth proving rather than asserting.

    python3 harness/esp_update.py [havendos.iso]

Run it in the background and poll the output, as the handoff says -- it
takes several minutes of real QEMU boots.
"""
import os
import shutil
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu as Q                                            # noqa: E402

# v0.9.3: `--gb N`. At the default 8 GB the gap is about 1.6 GB and the
# install is FAT16; at 12 GB it is over 5 GB and FAT32 -- and then the
# update writes the kernel to a FAT32 system volume through fs/sysvol.c
# WHILE the ESP it is reading from holds fs/fat32.c's slot. Run both.
_args = sys.argv[1:]
DISK_GB = 8
if "--gb" in _args:
    _i = _args.index("--gb"); DISK_GB = int(_args[_i + 1]); del _args[_i:_i + 2]
ISO = _args[0] if _args else "havendos.iso"
IMG = "/tmp/hd_espupd.img"
MACHINE = "pc"
MENU_INSTALL = 3        # counted from isodir/boot/grub/grub.cfg
SECT = 512

OVMF_CANDIDATES = [
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd",    "/usr/share/OVMF/OVMF_VARS.fd"),
    ("/usr/share/qemu/OVMF.fd",         None),
    ("/usr/share/ovmf/OVMF.fd",         None),
]

fails = []


def check(what, ok, detail=""):
    print("  %-64s %s" % (what, "ok" if ok else "FAIL"))
    sys.stdout.flush()
    if not ok:
        fails.append((what, detail))
        if detail:
            print("      %s" % detail)


def sh(cmd, quiet=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if not quiet and r.returncode != 0:
        print("      ! %s\n      %s" % (cmd, (r.stderr or r.stdout).strip()))
    return r


def find_ovmf():
    for code, varsf in OVMF_CANDIDATES:
        if os.path.exists(code):
            return code, (varsf if varsf and os.path.exists(varsf) else None)
    return None, None


# ── the disk ─────────────────────────────────────────────────────────
def build_disk():
    """8 GB GPT, shaped like a Windows laptop: ESP, MSR, C:, a gap, and a
    recovery partition at the end. The gap is where HavenDOS goes."""
    if os.path.exists(IMG):
        os.unlink(IMG)
    with open(IMG, "wb") as f:
        f.truncate(DISK_GB * 1024 * 1024 * 1024)
    sh("sgdisk -o %s" % IMG)
    sh("sgdisk -n 1:2048:+260M   -t 1:ef00 -c 1:'EFI system partition' %s" % IMG)
    sh("sgdisk -n 2:0:+16M       -t 2:0c01 -c 2:'Microsoft reserved' %s" % IMG)
    sh("sgdisk -n 3:0:+5G        -t 3:0700 -c 3:'Basic data' %s" % IMG)
    # leave ~1.6 GB of gap, then the recovery partition at the very end
    sh("sgdisk -n 4:-1000M:0     -t 4:2700 -c 4:'Windows RE' %s" % IMG)

    r = sh("sgdisk -i 1 %s" % IMG)
    first = last = None
    for line in r.stdout.splitlines():
        if "First sector" in line:
            first = int(line.split(":")[1].split("(")[0].strip())
        if "Last sector" in line:
            last = int(line.split(":")[1].split("(")[0].strip())

    # Format the ESP and put Windows' files on it, so there is something
    # that must survive.
    sub = "/tmp/hd_espupd_esp.img"
    n = last - first + 1
    with open(sub, "wb") as f:
        f.truncate(n * SECT)
    sh("mkfs.vfat -F 32 -n SYSTEM %s" % sub)
    env = "MTOOLS_SKIP_CHECK=1 "
    for d in ("EFI", "EFI/Microsoft", "EFI/Microsoft/Boot", "EFI/BOOT"):
        sh(env + "mmd -i %s ::%s" % (sub, d))
    blob = "/tmp/hd_espupd_blob.bin"
    with open(blob, "wb") as f:
        f.write(bytes((i * 37 + 11) & 0xFF for i in range(512 * 1024)))
    sh(env + "mcopy -i %s -o %s ::EFI/Microsoft/Boot/bootmgfw.efi" % (sub, blob))
    sh(env + "mcopy -i %s -o %s ::EFI/Microsoft/Boot/BCD" % (sub, blob))
    sh("dd if=%s of=%s bs=%d seek=%d conv=notrunc status=none"
       % (sub, IMG, SECT, first))
    return sub, first, n


def esp_pull(sub, first):
    """Copy the ESP back out of the disk image so mtools can work on it."""
    sh("dd if=%s of=%s bs=%d skip=%d count=%d conv=notrunc status=none"
       % (IMG, sub, SECT, first, os.path.getsize(sub) // SECT))


def esp_push(sub, first):
    sh("dd if=%s of=%s bs=%d seek=%d conv=notrunc status=none"
       % (sub, IMG, SECT, first))


def ms_hashes(sub):
    """Every file under \\EFI\\Microsoft, by name and CRC, read with mtools
    rather than with our own driver."""
    out = {}
    env = "MTOOLS_SKIP_CHECK=1 "
    for name in ("bootmgfw.efi", "BCD"):
        tmp = "/tmp/hd_espupd_ms.bin"
        r = sh(env + "mcopy -i %s -n ::EFI/Microsoft/Boot/%s %s"
               % (sub, name, tmp))
        if r.returncode == 0 and os.path.exists(tmp):
            out[name] = zlib.crc32(open(tmp, "rb").read()) & 0xFFFFFFFF
            os.unlink(tmp)
    return out


def main():
    if not os.path.exists(ISO):
        sys.exit("missing %s -- run `make` first" % ISO)
    for tool in ("sgdisk", "mkfs.vfat", "mcopy", "mmd", "fsck.vfat"):
        if shutil.which(tool) is None:
            sys.exit("%s is not installed; this harness needs an outside "
                     "opinion of the volume" % tool)
    code, varsf = find_ovmf()
    if code is None:
        sys.exit("no OVMF firmware found -- this harness is about UEFI")
    print("OVMF: %s\nISO : %s\n" % (code, ISO))

    print("Building a Windows-shaped %d GB GPT disk" % DISK_GB)
    sub, esp_first, esp_sects = build_disk()
    before = ms_hashes(sub)
    check("the ESP has Windows files on it to protect", len(before) == 2,
          str(before))

    # ── 1. install from the ISO, under OVMF ──────────────────────────
    print("\nBooting the ISO under OVMF, 'Install to Disk'")
    Q.kill_stale()
    v1 = "/tmp/hd_espupd_vars1.fd"
    shutil.copyfile(varsf if varsf else code, v1)
    fw1 = (["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % code,
            "-drive", "if=pflash,format=raw,unit=1,file=%s" % v1]
           if varsf else
           ["-drive", "if=pflash,format=raw,unit=0,file=%s" % v1])

    installed = False
    with Q.Qemu(iso=ISO, mem=512, boot="d", gfx=True,
                binary="qemu-system-x86_64", machine=MACHINE,
                extra=fw1 + ["-drive",
                     "file=%s,format=raw,if=ide,index=0,media=disk" % IMG]
                ) as q:
        time.sleep(26)                    # OVMF is slow to reach GRUB
        for _ in range(MENU_INSTALL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        # v0.9.3: this used to stop here -- it waited for the word
        # "Install", printed a note that the installer is driven by
        # harness/uefi_dualboot.py "which this harness deliberately does
        # not duplicate", and went on to boot a disk nothing had been
        # installed on. So it could not pass, and nobody had run it since
        # it could. It drives the additive installer now, the same keys
        # uefi_dualboot.py sends.
        # Each key waits for the screen it is meant for: the drive picker
        # runs a dry-run install per disk before it draws, which on a 12 GB
        # disk outlasts a fixed two-second pause, and a key sent early is
        # simply lost -- the first run of this stopped on the preview.
        if q.wait_for("Install", timeout=150):
            # The welcome screen is drawn before its keyboard loop starts;
            # a key in between is lost (seen on the second run). So ENTER
            # is repeated until the picker is up -- harmless, because the
            # welcome screen is the only one on which ENTER does nothing
            # but go on.
            for _ in range(6):
                time.sleep(2)
                q.key("ret")                         # welcome -> picker
                if q.wait_for("keeps this disk", timeout=20):
                    break
            q.key("ret")                             # the disk -> preview
            q.wait_for("FOUR sectors", timeout=60)
            q.key("ret")                             # preview -> confirm
            q.wait_for("Nothing is deleted", timeout=60)
            q.type("YES"); time.sleep(0.4)
            q.key("ret")
            for _ in range(60):
                time.sleep(3)
                txt = q.screen()
                if "successfully" in txt or "Complete" in txt or "Failed" in txt:
                    break
            installed = "successfully" in txt or "Complete" in txt
        q.shot("/tmp/hd_espupd_01_installer.png")
    check("the additive install, under OVMF, completes", installed,
          "see /tmp/hd_espupd_01_installer.png")

    if not installed:
        print("\nCannot continue without an install.")
        return 1

    parts = {}
    for line in sh("sgdisk -p %s" % IMG).stdout.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0].isdigit():
            parts[int(p[0])] = (int(p[1]), int(p[2]), " ".join(p[5:]))
    hv = [v for v in parts.values() if "HavenDOS" in v[2]]
    check("sgdisk sees the HavenDOS partition", len(hv) == 1, repr(parts))
    if not hv:
        return 1
    hv_first, hv_last = hv[0][0], hv[0][1]
    fat = 32 if hv_last - hv_first + 1 > 4192256 else 16
    print("  HavenDOS volume: %d MB, FAT%d" % ((hv_last - hv_first + 1) // 2048, fat))

    # ── 2. stage an update on the ESP, from the host ─────────────────
    print("\nStaging an update on the ESP with mtools (standing in for"
          " Windows' mountvol)")
    esp_pull(sub, esp_first)

    kernel = open("havendos.elf", "rb").read()
    crc = zlib.crc32(kernel) & 0xFFFFFFFF
    ver = "9.9.9"          # unmistakably newer than anything real
    man = ("HAVENDOS-UPDATE 1\r\n"
           "version=%s\r\n"
           "size=%u\r\n"
           "crc32=%08x\r\n"
           "kernel=havendos.elf\r\n" % (ver, len(kernel), crc))

    env = "MTOOLS_SKIP_CHECK=1 "
    sh(env + "mmd -i %s ::EFI/HAVENDOS" % sub)
    sh(env + "mmd -i %s ::EFI/HAVENDOS/update" % sub)
    open("/tmp/hd_espupd_k.elf", "wb").write(kernel)
    open("/tmp/hd_espupd_m.inf", "w", newline="").write(man)
    r1 = sh(env + "mcopy -i %s -o /tmp/hd_espupd_k.elf "
            "::EFI/HAVENDOS/update/havendos.elf" % sub)
    r2 = sh(env + "mcopy -i %s -o /tmp/hd_espupd_m.inf "
            "::EFI/HAVENDOS/update/update.inf" % sub)
    check("the kernel is staged on the ESP", r1.returncode == 0, r1.stderr)
    check("the manifest is staged beside it", r2.returncode == 0, r2.stderr)
    print("      %u bytes, crc32=%08x, version %s" % (len(kernel), crc, ver))
    esp_push(sub, esp_first)

    # ── 3. boot and run `update` ──────────────────────────────────────
    # The documented route: the ISO's "Update installed system from the
    # ESP" entry, which boots shell mode WITHOUT `live`, so the installed
    # volume is mounted and the first-boot wizard does not run. SeaBIOS is
    # enough for this step -- what is under test is the kernel's update
    # path, not the firmware, and uefi_dualboot.py boots the disk itself
    # under OVMF. The ISO is a CD, and the ESP is preferred over it as a
    # source (fs/update_src.c), so the kernel comes from the ESP.
    print("\nBooting the ISO's 'Update installed system' entry with the disk")
    Q.kill_stale()
    MENU_UPDATE = 4
    with Q.Qemu(iso=ISO, mem=512, boot="d", gfx=True, machine=MACHINE,
                extra=["-drive",
                       "file=%s,format=raw,if=ide,index=0,media=disk" % IMG]
                ) as q:
        booted = None
        for wait in (8, 14, 20):
            time.sleep(wait)
            for _ in range(MENU_UPDATE):
                q.key("down"); time.sleep(0.35)
            q.key("ret")
            booted = q.wait_for("Shell", timeout=120)
            if booted: break
            q.cmd_qmp("system_reset")
        q.shot("/tmp/hd_espupd_02_booted.png")
        check("the update entry reaches a shell", booted is not None,
              "see /tmp/hd_espupd_02_booted.png")

        if booted:
            q.line("update"); time.sleep(6)
            txt = q.screen()
            q.shot("/tmp/hd_espupd_03_update.png")
            saw_source = "EFI System Partition" in txt
            check("`update` names the EFI System Partition as its source",
                  saw_source, txt[-600:])
            check("and does not refuse the FAT%d system volume" % fat,
                  "No HavenDOS partition" not in txt
                  and "No FAT16 partition" not in txt, txt[-400:])
            q.key("y")
            txt = q.wait_for("Update complete", timeout=120) or q.screen()
            q.shot("/tmp/hd_espupd_04_verify.png")
            saw_crc = "Checksum matches" in txt
            check("the checksum is verified against the manifest", saw_crc,
                  txt[-600:])
            saw_done = "Update complete" in txt
            check("the update completes", saw_done, txt[-600:])

    # ── 3b. the kernel really is on the HavenDOS volume ─────────────
    vol = "/tmp/hd_espupd_vol.img"
    sh("rm -f %s; dd if=%s of=%s bs=512 skip=%d count=%d conv=sparse status=none"
       % (vol, IMG, vol, hv_first, hv_last - hv_first + 1))
    r = sh("fsck.vfat -n %s" % vol)
    check("fsck.vfat calls the HavenDOS volume clean afterwards",
          r.returncode == 0, (r.stdout or r.stderr)[-300:])
    r = sh("mcopy -n -i %s ::/boot/havendos.elf /tmp/hd_espupd_kout.elf "
           "&& cmp -s /tmp/hd_espupd_kout.elf /tmp/hd_espupd_k.elf" % vol)
    check("/boot/havendos.elf on it is byte-for-byte the staged kernel",
          r.returncode == 0)
    check("and the previous kernel was kept as /boot/havendos.bak",
          sh("mdir -i %s ::/boot/havendos.bak" % vol).returncode == 0)

    # ── 4. Windows' files are untouched ──────────────────────────────
    esp_pull(sub, esp_first)
    after = ms_hashes(sub)
    check("every Windows file on the ESP is byte-for-byte identical",
          before == after, "%s -> %s" % (before, after))
    r = sh("fsck.vfat -n %s" % sub)
    check("fsck.vfat calls the ESP clean", r.returncode == 0,
          (r.stdout or r.stderr)[-300:])

    print()
    if fails:
        print("%d FAILED:" % len(fails))
        for what, _ in fails:
            print("  - %s" % what)
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
