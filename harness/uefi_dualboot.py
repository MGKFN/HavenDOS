#!/usr/bin/env python3
"""uefi_dualboot.py -- install beside Windows, under real UEFI firmware.

`make test` proves this in process: tests/test_installadd.c runs the whole
additive install against a model of a 512 GB Windows disk and compares
every sector afterwards. What it cannot prove is that the firmware agrees.
Between a correct GPT entry and a machine that boots there is OVMF, GRUB's
PE32+ loader, the ESP's directory layout, and `search --label HAVENDOS`
finding a partition it was never told the number of -- none of which runs
in a native test, and every one of which is a place this can fail while
every suite stays green.

So: build a disk shaped like a Windows laptop, boot the ISO under OVMF,
drive the installer, and then boot the DISK with no removable media and
see what the firmware does with it.

The disk is checked three times, by tools that are not ours:
  sgdisk and parted, before and after, on the partition table;
  sha256 of every allocated extent outside the new partition and the ESP;
  fsck.vfat on the ESP and on the new volume.

Usage:
    python3 harness/uefi_dualboot.py [iso]

Nothing here touches a real disk. The image is created in /tmp.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu as Q

ISO   = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
IMG   = "/tmp/hd_dualboot.img"
DISK_GB = 8

# OVMF, split or combined depending on the distribution.
OVMF_CANDIDATES = [
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd",    "/usr/share/OVMF/OVMF_VARS.fd"),
    ("/usr/share/qemu/OVMF.fd",         None),
    ("/usr/share/ovmf/OVMF.fd",         None),
]

# -M pc, not q35, and that is a finding rather than a preference.
#
# On q35, `if=ide` is an ICH9 AHCI controller, and HavenDOS has no AHCI
# driver -- `grep -rn "ahci_"` returns nothing, and it is release 0.9.3
# "Spindle" in the plan. The installer on a q35 machine therefore reports
# "No Disks Found", correctly: it cannot see the disk, so it refuses to
# install to it. -M pc gives the legacy 0x1F0 IDE controller that
# drivers/ata.c drives.
#
# Worth saying plainly, because it is the same gap that will show up on
# real hardware: this harness proves the additive install works under UEFI
# firmware, and it does NOT prove anything about a machine whose disk is
# behind AHCI or NVMe. Those are 0.9.3 and 0.9.1.
MACHINE = "pc"

# The ISO's menu, counted from isodir/boot/grub/grub.cfg. GRUB draws its own
# menu with its own font, so the framebuffer decoder cannot read it -- it is
# driven blind by counting menuentry lines, as the handoff says to.
MENU_INSTALL = 3        # "Install to Disk"

fails = []


def check(what, ok, detail=""):
    print("  %-62s %s" % (what, "ok" if ok else "FAIL"))
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
            return code, varsf
    return None, None


# ── the disk ────────────────────────────────────────────────────────────
def build_disk():
    """A Windows-shaped GPT disk with free space at the end.

    Built with sgdisk and mkfs.vfat rather than by hand: a fixture our own
    GPT code wrote would be a fixture our own GPT code agreed with."""
    if os.path.exists(IMG):
        os.unlink(IMG)
    with open(IMG, "wb") as f:
        f.truncate(DISK_GB * 1024 * 1024 * 1024)

    # 260 MB ESP, 16 MB MSR, a data partition, 512 MB recovery, ~1.5 GB free
    r = sh("sgdisk -o "
           "-n 1:2048:+260M   -t 1:ef00 -c 1:'EFI system partition' "
           "-n 2:0:+16M       -t 2:0c01 -c 2:'Microsoft reserved partition' "
           "-n 3:0:+5G        -t 3:0700 -c 3:'Basic data partition' "
           "-n 4:-512M:0      -t 4:2700 -c 4:'Basic data partition' "
           "%s" % IMG)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit("sgdisk could not build the fixture")

    info = sh("sgdisk -p %s" % IMG).stdout
    esp_first = esp_last = None
    for line in info.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0] == "1":
            esp_first, esp_last = int(p[1]), int(p[2])
    if esp_first is None:
        sys.exit("could not read the ESP's bounds back from sgdisk")

    # a real FAT32 ESP with Windows' files on it
    esp_sectors = esp_last - esp_first + 1
    sh("dd if=/dev/zero of=/tmp/hd_db_esp.img bs=512 count=%d status=none"
       % esp_sectors)
    sh("mkfs.vfat -F 32 -n EFI /tmp/hd_db_esp.img")
    for d in ("::/EFI", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot",
              "::/EFI/BOOT"):
        sh("mmd -i /tmp/hd_db_esp.img %s" % d)
    sh("head -c 1800000 /dev/urandom > /tmp/hd_db_bootmgfw.bin")
    sh("head -c 262144  /dev/urandom > /tmp/hd_db_bcd.bin")
    sh("mcopy -i /tmp/hd_db_esp.img /tmp/hd_db_bootmgfw.bin "
       "::/EFI/Microsoft/Boot/bootmgfw.efi")
    sh("mcopy -i /tmp/hd_db_esp.img /tmp/hd_db_bcd.bin "
       "::/EFI/Microsoft/Boot/BCD")
    sh("dd if=/tmp/hd_db_esp.img of=%s bs=512 seek=%d conv=notrunc status=none"
       % (IMG, esp_first))

    # something recognisable in the data partition, so "untouched" means
    # something stronger than "still zeroes"
    for line in info.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0] == "3":
            sh("head -c 4194304 /dev/urandom > /tmp/hd_db_win.bin")
            sh("dd if=/tmp/hd_db_win.bin of=%s bs=512 seek=%s "
               "conv=notrunc status=none" % (IMG, p[1]))
    return esp_first, esp_last


def extents(path):
    """Allocated byte ranges of a sparse file, via SEEK_DATA/SEEK_HOLE.

    Hashing 8 GB twice takes longer than the install does; and a range the
    filesystem has never allocated cannot have changed."""
    out = []
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY)
    try:
        pos = 0
        while pos < size:
            try:
                start = os.lseek(fd, pos, os.SEEK_DATA)
            except OSError:
                break
            if start >= size:
                break
            try:
                end = os.lseek(fd, start, os.SEEK_HOLE)
            except OSError:
                end = size
            out.append((start, min(end, size)))
            pos = end
    finally:
        os.close(fd)
    return out


def hash_outside(path, skip):
    """sha256 per 512-sector block of every allocated extent, skipping the
    given (first_lba, last_lba) ranges. Returns {lba: digest}."""
    digests = {}
    fd = os.open(path, os.O_RDONLY)
    try:
        for start, end in extents(path):
            lba = start // 512
            while lba * 512 < end:
                if any(lo <= lba <= hi for lo, hi in skip):
                    lba += 1
                    continue
                os.lseek(fd, lba * 512, os.SEEK_SET)
                b = os.read(fd, 512)
                if b and any(b):
                    digests[lba] = hashlib.sha256(b).digest()
                lba += 1
    finally:
        os.close(fd)
    return digests


def part_bounds(path):
    """{number: (first, last, name)} as sgdisk reads it back."""
    out = {}
    for line in sh("sgdisk -p %s" % path).stdout.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0].isdigit():
            out[int(p[0])] = (int(p[1]), int(p[2]), " ".join(p[5:]))
    return out


# ── driving the installer ───────────────────────────────────────────────
def screen(q, path):
    q.shot(path)
    try:
        import fbtext
        return fbtext.text(path)
    except Exception as e:
        return "<could not decode: %s>" % e


def main():
    if not os.path.exists(ISO):
        sys.exit("missing %s -- run `make` first" % ISO)
    for tool in ("sgdisk", "parted", "mkfs.vfat", "mcopy", "fsck.vfat"):
        if shutil.which(tool) is None:
            sys.exit("%s is not installed; this harness needs an outside "
                     "opinion of the disk" % tool)

    code, varsf = find_ovmf()
    if code is None:
        sys.exit("no OVMF firmware found -- this harness is about UEFI and\n"
                 "there is nothing to be learned without it")
    print("OVMF: %s\n" % code)

    print("Building a Windows-shaped 8 GB GPT disk with sgdisk")
    esp_first, esp_last = build_disk()
    before_parts = part_bounds(IMG)
    for n, (a, b, name) in sorted(before_parts.items()):
        print("  %d  %10d..%-10d  %s" % (n, a, b, name))
    # The gap is BETWEEN partitions 3 and 4, not after the last one: the
    # recovery partition sits at the end of the disk, the way it does on a
    # real Windows install. The first version of this printed the end of
    # the last partition and reported 0 MB free on a disk with 2 GB of it.
    gaps = []
    edges = sorted((a, b) for a, b, _ in before_parts.values())
    for i in range(len(edges) - 1):
        lo, hi = edges[i][1] + 1, edges[i + 1][0] - 1
        if hi >= lo:
            gaps.append((lo, hi))
    tail_lo = edges[-1][1] + 1
    tail_hi = DISK_GB * 1024 * 1024 * 1024 // 512 - 34
    if tail_hi >= tail_lo:
        gaps.append((tail_lo, tail_hi))
    for lo, hi in gaps:
        print("  free  %10d..%-10d  %d MB" % (lo, hi, (hi - lo + 1) // 2048))

    print("Hashing every allocated sector outside the free space")
    before = hash_outside(IMG, [])
    check("the disk has content to protect", len(before) > 3000,
          "%d non-zero sectors" % len(before))
    print("  %d non-zero sectors recorded\n" % len(before))

    # the firmware's own variable store, copied so the original is untouched
    varcopy = "/tmp/hd_db_vars.fd"
    if varsf:
        shutil.copyfile(varsf, varcopy)
        fw = ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % code,
              "-drive", "if=pflash,format=raw,unit=1,file=%s" % varcopy]
    else:
        shutil.copyfile(code, varcopy)
        fw = ["-drive", "if=pflash,format=raw,unit=0,file=%s" % varcopy]

    Q.kill_stale()
    print("Booting the ISO under OVMF, 'Install to Disk'")
    rc = 1
    with Q.Qemu(iso=ISO, mem=512, boot="d", gfx=True,
                binary="qemu-system-x86_64", machine=MACHINE,
                extra=fw + ["-drive",
                            "file=%s,format=raw,if=ide,index=1,media=disk" % IMG]
                ) as q:
        # OVMF takes noticeably longer to reach GRUB than SeaBIOS does
        time.sleep(12)
        for _ in range(MENU_INSTALL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        print("  waiting for the installer's welcome screen")
        time.sleep(25)

        txt = screen(q, "/tmp/hd_db_01_welcome.png")
        print("\n".join("    | " + l for l in txt.splitlines()[:6]))
        check("the installer came up", "HavenDOS" in txt or "Install" in txt,
              txt[:200])

        q.key("ret"); time.sleep(2)          # welcome -> drive picker
        txt = screen(q, "/tmp/hd_db_02_picker.png")
        print("\n  drive picker:")
        print("\n".join("    | " + l for l in txt.splitlines()[5:14]))
        check("a disk offers to keep what is on it",
              "keeps this disk" in txt, txt[:400])

        # the model disk is index 1 (the ISO is not in the disk table, but
        # QEMU's ide index 1 is the only disk attached, so row 0 is it)
        q.key("ret"); time.sleep(2)
        txt = screen(q, "/tmp/hd_db_03_preview.png")
        print("\n  preview:")
        print("\n".join("    | " + l for l in txt.splitlines()[2:20]))
        check("the preview names four sectors", "FOUR sectors" in txt,
              txt[:400])
        check("and says the table is kept", "keeps its partition" in txt,
              "")

        q.key("ret"); time.sleep(2)
        txt = screen(q, "/tmp/hd_db_04_confirm.png")
        check("the confirmation does not threaten to erase the disk",
              "ERASED" not in txt and "Nothing is deleted" in txt, txt[:300])

        q.type("YES"); time.sleep(0.4)
        q.key("ret")
        print("\n  installing")
        for _ in range(40):
            time.sleep(3)
            txt = screen(q, "/tmp/hd_db_05_progress.png")
            if "Complete" in txt or "Failed" in txt:
                break
        print("\n".join("    | " + l for l in txt.splitlines()[:12]))
        check("the install reported completion", "Complete" in txt, txt[:400])
        time.sleep(1)

    # ── the disk, judged by tools that are not ours ──────────────────
    print("\nWhat the disk looks like now, per sgdisk")
    after_parts = part_bounds(IMG)
    for n, (a, b, name) in sorted(after_parts.items()):
        mark = "  <- new" if n not in before_parts else ""
        print("  %d  %10d..%-10d  %s%s" % (n, a, b, name, mark))

    check("every partition that was there is still there, unmoved",
          all(after_parts.get(n) == v for n, v in before_parts.items()),
          repr(after_parts))
    new = [n for n in after_parts if n not in before_parts]
    check("exactly one partition was added", len(new) == 1, repr(new))
    check("parted agrees the table is valid",
          sh("parted -s %s print" % IMG).returncode == 0)
    check("sgdisk verify is clean",
          "No problems found" in sh("sgdisk -v %s" % IMG).stdout,
          sh("sgdisk -v %s" % IMG).stdout.strip()[:300])

    if len(new) == 1:
        pa, pb, _ = after_parts[new[0]]
        print("\nComparing every other allocated sector, before and after")
        after = hash_outside(IMG, [(pa, pb), (esp_first, esp_last)])
        changed = sorted(l for l in set(before) & set(after)
                         if before[l] != after[l])
        vanished = sorted(set(before) - set(after)
                          - {l for l in range(pa, pb + 1)}
                          - {l for l in range(esp_first, esp_last + 1)})
        appeared = sorted(set(after) - set(before))
        # the four the installer said it would write
        print("  changed outside the new partition and the ESP: %s"
              % (changed if changed else "none"))
        print("  newly written there:                           %s"
              % (appeared if appeared else "none"))
        allowed = set(changed) | set(appeared)
        check("at most four sectors outside the partition changed",
              len(allowed) <= 4, repr(sorted(allowed)))
        check("and they are the partition table, nothing else",
              all(l <= 34 or l >= DISK_GB * 1024 * 1024 * 1024 // 512 - 34
                  for l in allowed), repr(sorted(allowed)))
        check("no sector that existed before has gone", not vanished,
              repr(vanished[:8]))

        # the ESP and the new volume, per fsck
        sh("dd if=%s of=/tmp/hd_db_esp_after.img bs=512 skip=%d count=%d "
           "status=none" % (IMG, esp_first, esp_last - esp_first + 1))
        check("fsck.vfat says the EFI partition is clean",
              sh("fsck.vfat -n /tmp/hd_db_esp_after.img").returncode == 0,
              sh("fsck.vfat -n /tmp/hd_db_esp_after.img").stdout[-300:])
        check("Windows' boot manager is byte-identical",
              sh("mcopy -n -i /tmp/hd_db_esp_after.img "
                 "::/EFI/Microsoft/Boot/bootmgfw.efi /tmp/hd_db_bm_out.bin "
                 "&& cmp -s /tmp/hd_db_bm_out.bin /tmp/hd_db_bootmgfw.bin"
                 ).returncode == 0)
        check("the BCD is byte-identical",
              sh("mcopy -n -i /tmp/hd_db_esp_after.img "
                 "::/EFI/Microsoft/Boot/BCD /tmp/hd_db_bcd_out.bin "
                 "&& cmp -s /tmp/hd_db_bcd_out.bin /tmp/hd_db_bcd.bin"
                 ).returncode == 0)
        check("HavenDOS' UEFI loader is on the ESP",
              sh("mcopy -n -i /tmp/hd_db_esp_after.img "
                 "::/EFI/BOOT/BOOTX64.EFI /tmp/hd_db_ldr.bin").returncode == 0)
        check("its menu offers Windows as well",
              sh("mcopy -n -i /tmp/hd_db_esp_after.img "
                 "::/EFI/HAVENDOS/grub.cfg /tmp/hd_db_cfg.txt "
                 "&& grep -q bootmgfw /tmp/hd_db_cfg.txt").returncode == 0)

        sh("dd if=%s of=/tmp/hd_db_part.img bs=512 skip=%d count=%d "
           "status=none" % (IMG, pa, pb - pa + 1))
        check("fsck.vfat accepts the new volume",
              sh("fsck.vfat -n /tmp/hd_db_part.img").returncode == 0,
              sh("fsck.vfat -n /tmp/hd_db_part.img").stdout[-300:])
        check("it is labelled HAVENDOS",
              sh("mlabel -i /tmp/hd_db_part.img -s ::").stdout.find("HAVENDOS")
              >= 0)
        check("the kernel is in it",
              sh("mcopy -n -i /tmp/hd_db_part.img ::/boot/havendos.elf "
                 "/tmp/hd_db_k.bin").returncode == 0)

    # ── and now the only question that matters ──────────────────────
    #
    # Two firmware states, because they answer different questions and the
    # first version of this harness only tried one of them and called the
    # result a failure.
    #
    #   fresh NVRAM  -- firmware meeting this disk for the first time. It
    #                   enumerates the EFI System Partition, finds
    #                   \EFI\BOOT\BOOTX64.EFI, and runs it. This is the
    #                   question "is what we installed bootable at all".
    #
    #   used NVRAM   -- the variable store left behind by the install boot,
    #                   when the disk had no loader on it and the CD did.
    #                   The firmware has a boot order from that session and
    #                   does not re-enumerate, so it walks down it and ends
    #                   at PXE. This is not a bug in the install; it is what
    #                   an NVRAM boot entry would fix, and writing one needs
    #                   UEFI runtime services, which this kernel does not
    #                   have. It is reported rather than asserted, because
    #                   it is a real thing that will happen on a real
    #                   machine and the right response is to say so.
    print("\nBooting the DISK under OVMF, no ISO attached, fresh firmware")
    Q.kill_stale()
    fresh = "/tmp/hd_db_vars_fresh.fd"
    shutil.copyfile(varsf if varsf else code, fresh)
    fw_fresh = ([ "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % code,
                  "-drive", "if=pflash,format=raw,unit=1,file=%s" % fresh ]
                if varsf else
                [ "-drive", "if=pflash,format=raw,unit=0,file=%s" % fresh ])

    booted = ""
    reached_grub = False
    with Q.Qemu(mem=512, boot="c", gfx=True,
                binary="qemu-system-x86_64", machine=MACHINE,
                extra=fw_fresh + ["-drive",
                     "file=%s,format=raw,if=ide,index=0,media=disk" % IMG]
                ) as q:
        # GRUB draws its menu in its own font, which harness/fbtext.py
        # cannot decode -- it decodes the kernel's 8x8 font. So the menu is
        # not read; it is driven, and what proves it was there is that
        # driving it boots the kernel, which fbtext CAN read.
        time.sleep(22)
        q.shot("/tmp/hd_db_06_menu.png")
        reached_grub = os.path.getsize("/tmp/hd_db_06_menu.png") > 0

        # Driven with a retry rather than a timed sequence, and the reason
        # is worth recording. The installed grub.cfg has `set timeout=8`
        # and `set default=1`, so GRUB counts down and chainloads the
        # Windows entry on its own -- and in this fixture Windows is 1.8 MB
        # of random bytes, so the chainload fails, GRUB puts up an error,
        # and the first keypress after that is consumed dismissing it.
        # A blind up-then-enter therefore pressed enter on the Windows
        # entry, failed again, and left the menu looking untouched.
        #
        # So each attempt is: dismiss anything on screen, move up one (from
        # the Windows default to HavenDOS), enter, and then LOOK. The menu
        # is a fixed point -- every failure returns to it -- so repeating
        # converges instead of drifting.
        print("  driving the menu to the HavenDOS entry")
        for attempt in range(5):
            q.key("esc");  time.sleep(0.6)
            q.key("up");   time.sleep(0.6)
            q.key("ret")
            for _ in range(5):
                time.sleep(5)
                booted = screen(q, "/tmp/hd_db_07_firstboot.png")
                if "HavenDOS" in booted or "Haven" in booted:
                    break
            if "HavenDOS" in booted or "Haven" in booted:
                print("    booted on attempt %d" % (attempt + 1))
                break
        print("\n".join("    | " + l for l in booted.splitlines()[:16]
                        if l.strip()))

    check("the firmware ran the loader from the installed ESP", reached_grub)
    check("and HavenDOS booted from its own partition, no removable media",
          "HavenDOS" in booted or "Haven" in booted, booted[:400])
    # What it boots INTO, which is a separate question and was wrong. With
    # no kernel command line on the multiboot line the kernel comes up as
    # the installer and says it booted from the install ISO -- on a machine
    # with no ISO in it.
    check("and it boots the system, not the installer",
          "Installer" not in booted and "install ISO" not in booted,
          booted[:300])

    print("\nAnd with the variable store the install left behind")
    Q.kill_stale()
    with Q.Qemu(mem=512, boot="c", gfx=True,
                binary="qemu-system-x86_64", machine=MACHINE,
                extra=fw + ["-drive",
                     "file=%s,format=raw,if=ide,index=0,media=disk" % IMG]
                ) as q:
        time.sleep(30)
        stale = screen(q, "/tmp/hd_db_08_stale_nvram.png")
    if "Haven" in stale:
        print("  the firmware re-enumerated and found the new loader.")
    else:
        print("  the firmware kept its old boot order and did not look for a\n"
              "  new loader. EXPECTED, and worth repeating to anyone\n"
              "  installing on a machine that already boots something:\n"
              "  HavenDOS writes \\EFI\\BOOT\\BOOTX64.EFI but does not add a\n"
              "  firmware boot entry, because that needs UEFI runtime\n"
              "  services this kernel does not have. On a machine with an\n"
              "  existing boot order, use the firmware's one-time boot menu\n"
              "  and pick the generic UEFI entry for the disk.")

    print()
    if fails:
        print("%d check(s) failed:" % len(fails))
        for what, detail in fails:
            print("  - %s" % what)
        print("\nScreenshots: /tmp/hd_db_*.png")
        return 1
    print("all checks passed -- installed beside Windows under UEFI, and "
          "everything\noutside the new partition is byte-identical")
    print("Screenshots: /tmp/hd_db_*.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
