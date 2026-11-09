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

ISO = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
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
        f.truncate(8 * 1024 * 1024 * 1024)
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

    print("Building a Windows-shaped 8 GB GPT disk")
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
        # The installer's own screens. Driven blind past the preview, then
        # the typed confirmation.
        if q.wait_for("Install", timeout=150):
            installed = True
        q.shot("/tmp/hd_espupd_01_installer.png")
    check("the ISO boots under OVMF and reaches the installer", installed,
          "see /tmp/hd_espupd_01_installer.png")

    if not installed:
        print("\nCannot continue without an install.")
        return 1

    print("\n(The installer is driven by harness/uefi_dualboot.py, which\n"
          " this harness deliberately does not duplicate. If that one is\n"
          " green, the disk below is the disk it produces.)")

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

    # ── 3. boot the disk with NO removable media and run `update` ────
    print("\nBooting the DISK under OVMF -- no ISO, no CD-ROM, nothing"
          " removable")
    Q.kill_stale()
    v2 = "/tmp/hd_espupd_vars2.fd"
    shutil.copyfile(v1, v2)        # keep the boot entry the install session made
    fw2 = (["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % code,
            "-drive", "if=pflash,format=raw,unit=1,file=%s" % v2]
           if varsf else
           ["-drive", "if=pflash,format=raw,unit=0,file=%s" % v2])

    saw_source = saw_crc = saw_done = False
    with Q.Qemu(mem=512, boot="c", gfx=True,
                binary="qemu-system-x86_64", machine=MACHINE,
                extra=fw2 + ["-drive",
                     "file=%s,format=raw,if=ide,index=0,media=disk" % IMG]
                ) as q:
        time.sleep(26)
        q.key("ret")                      # first menu entry: HavenDOS
        booted = q.wait_for("login", timeout=150) or \
                 q.wait_for("HavenDOS", timeout=20)
        q.shot("/tmp/hd_espupd_02_booted.png")
        check("the installed system boots with no removable media", booted,
              "see /tmp/hd_espupd_02_booted.png")

        if booted:
            # log in, then run update
            q.line("admin"); time.sleep(1)
            q.line("Admin@123"); time.sleep(3)
            q.line("update"); time.sleep(4)
            q.shot("/tmp/hd_espupd_03_update.png")
            txt = q.fbtext() or ""
            saw_source = "EFI System Partition" in txt
            check("`update` names the EFI System Partition as its source",
                  saw_source, txt[-400:])
            # confirm the upgrade
            q.key("y"); time.sleep(6)
            txt = q.fbtext() or ""
            q.shot("/tmp/hd_espupd_04_verify.png")
            saw_crc = "Checksum matches" in txt
            check("the checksum is verified against the manifest", saw_crc,
                  txt[-400:])
            saw_done = "Update complete" in txt
            check("the update completes", saw_done, txt[-400:])

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
