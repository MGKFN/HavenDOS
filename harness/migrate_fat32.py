#!/usr/bin/env python3
"""migrate_fat32.py -- v0.9.6: a FAT16 install moves itself to FAT32.

Under OVMF, on a Windows-shaped GPT disk, the whole of what Kyan asked for:

  [1] an OLDER ISO (0.9.7.7) installs HavenDOS beside "Windows" into a
      1.2 GB gap: FAT16, the way installs under 2 GB were made until 0.9.7.8
      made everything from 64 MB FAT32. Then this release's kernel is put
      on it, as `update` would: the system that runs `migrate` is the
      release under test, on the kind of volume real installs still have. An 8 GB partition the
      owner made and never formatted sits after it, and 3 GB of free space.
  [2] the FAT16 volume is given a life: dotfiles under FAT16's own names
      (_ACCOUNT, _HAVENDO ...), a home folder three levels deep, a hundred
      small files, a 3 MB one, and a 70 MB one bigger than the copy buffer.
  [3] it boots, and `migrate` lists where it can go -- and what it will
      not touch and why (the NTFS partition, Microsoft reserved, recovery).
      `migrate 1` takes the empty 8 GB partition, after ERASE is typed.
  [4] the disk, by sgdisk, fsck.fat and mtools: the partition is HavenDOS's
      now, FAT32, clean; every file is there byte for byte, the dotfiles
      under their REAL names; the big one is not, and the screen said so;
      the ESP menu names both installs by serial; the NTFS bytes and
      Windows' boot manager are unchanged.
  [5] the real menu, entry 0, desktop mode: the NEW system boots, and goes
      to the login screen -- not the setup wizard, which is what a missing
      .havendos_config would mean.
  [6] the new system, by its serial, in shell mode: `df` says FAT32,
      `migrate` says the move is done, `migrate finish` + REMOVE.
  [7] the disk again: the FAT16 partition is gone from the table and its
      boot sector is zero, the menu has one HavenDOS entry, sgdisk -v clean.
  [8] the real menu once more: still boots, still to login.

Run:  python3 harness/migrate_fat32.py [iso] [--gap]
      --gap moves into the free space instead of the empty partition.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu as Q                                              # noqa: E402

args = sys.argv[1:]
USE_GAP = "--gap" in args
args = [a for a in args if a != "--gap"]
ISO = args[0] if args else os.path.join(os.path.dirname(HERE), "havendos.iso")
# v0.9.7.8: the release that makes the FAT16 system. Any ISO before
# 0.9.7.8; the last one is kept beside the tree by the release flow.
OLD_ISO = os.environ.get("MIGRATE_OLD_ISO",
                         os.path.join(os.path.dirname(HERE), "HavenDOS-0.9.7.7.iso"))
NEW_KERNEL = os.path.join(os.path.dirname(HERE), "havendos_pass1.elf")
IMG = "/tmp/hd_mig.img"
GB = 16
MENU_INSTALL = 3
MACHINE = "pc"
OVMF = [("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd")]
HAVEN_GUID = "45564148-444E-534F-2D50-4152542D3031"

fails, total = [], [0]


def ck(what, ok, detail=""):
    total[0] += 1
    print("    %s %s" % ("ok  " if ok else "FAIL", what))
    if not ok:
        fails.append(what)
        if detail:
            print("         " + str(detail)[:600].replace("\n", "\n         "))


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def firmware():
    for code, varsf in OVMF:
        if os.path.exists(code):
            fresh = "/tmp/hd_mig_vars.fd"
            shutil.copyfile(varsf, fresh)
            return ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % code,
                    "-drive", "if=pflash,format=raw,unit=1,file=%s" % fresh]
    sys.exit("no OVMF")


def vm(iso=None):
    Q.kill_stale()
    extra = firmware() + ["-drive", "file=%s,format=raw,if=ide,index=%d,media=disk"
                          % (IMG, 1 if iso else 0)]
    return Q.Qemu(iso=iso, mem=512, boot="d" if iso else "c", gfx=True,
                  binary="qemu-system-x86_64", machine=MACHINE, extra=extra)


def screen(q, name):
    path = "/tmp/hd_mig_%s.png" % name
    q.shot(path)
    time.sleep(0.3)
    try:
        import fbtext
        return fbtext.text(path)
    except Exception as e:
        return "<decode: %s>" % e


def wait_any(q, needles, timeout, name):
    t0, s = time.time(), ""
    while time.time() - t0 < timeout:
        s = screen(q, name)
        for n in needles:
            if n in s:
                return n, s
        time.sleep(2)
    return None, s


def parts():
    out = {}
    for line in sh("sgdisk -p %s" % IMG).stdout.splitlines():
        p = line.split()
        if len(p) >= 6 and p[0].isdigit():
            out[int(p[0])] = (int(p[1]), int(p[2]), p[5])
    return out


def ptype(n):
    m = re.search(r"Partition GUID code: (\S+)", sh("sgdisk -i %d %s" % (n, IMG)).stdout)
    return m.group(1) if m else ""


def at(lba):
    return "%s@@%d" % (IMG, lba * 512)


def bpb(lba):
    with open(IMG, "rb") as f:
        f.seek(lba * 512)
        return f.read(512)


def fsck_part(first, last):
    """fsck.fat has no @@offset; the volume is cut out, sparsely."""
    sh("rm -f /tmp/hd_mig_vol.img; dd if=%s of=/tmp/hd_mig_vol.img bs=512 skip=%d count=%d "
       "conv=sparse status=none" % (IMG, first, last - first + 1))
    return sh("fsck.fat -n /tmp/hd_mig_vol.img")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


# ── [1] the disk and the FAT16 install ───────────────────────────────────
def build():
    if os.path.exists(IMG):
        os.unlink(IMG)
    with open(IMG, "wb") as f:
        f.truncate(GB << 30)
    # ESP, MSR, "C:" 3 GB, [1.2 GB gap], an 8 GB never-formatted basic data
    # partition, [~3 GB gap], recovery at the end
    r = sh("sgdisk -o "
           "-n 1:2048:+260M -t 1:ef00 -c 1:'EFI system partition' "
           "-n 2:0:+16M -t 2:0c01 -c 2:'Microsoft reserved partition' "
           "-n 3:0:+3G -t 3:0700 -c 3:'Basic data partition' "
           "-n 4:9437184:+8G -t 4:0700 -c 4:'Basic data partition' "
           "-n 5:-512M:0 -t 5:2700 -c 5:'Basic data partition' %s" % IMG)
    if r.returncode:
        sys.exit("sgdisk: " + r.stderr)
    p = parts()
    e0, e1, _ = p[1]
    sh("dd if=/dev/zero of=/tmp/hd_mig_esp.img bs=512 count=%d status=none" % (e1 - e0 + 1))
    sh("mkfs.vfat -F 32 -n EFI /tmp/hd_mig_esp.img")
    for d in ("::/EFI", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot", "::/EFI/BOOT"):
        sh("mmd -i /tmp/hd_mig_esp.img %s" % d)
    sh("head -c 1800000 /dev/urandom > /tmp/hd_mig_bootmgfw.bin")
    sh("mcopy -i /tmp/hd_mig_esp.img /tmp/hd_mig_bootmgfw.bin ::/EFI/Microsoft/Boot/bootmgfw.efi")
    sh("dd if=/tmp/hd_mig_esp.img of=%s bs=512 seek=%d conv=notrunc status=none" % (IMG, e0))
    # "C:" is NTFS: a boot sector with the signature, and data behind it
    sh("head -c 4194304 /dev/urandom > /tmp/hd_mig_ntfs.bin")
    with open("/tmp/hd_mig_ntfs.bin", "r+b") as f:
        f.seek(3); f.write(b"NTFS    ")
        f.seek(510); f.write(b"\x55\xaa")
    sh("dd if=/tmp/hd_mig_ntfs.bin of=%s bs=512 seek=%d conv=notrunc status=none" % (IMG, p[3][0]))
    # recovery is NTFS too
    sh("dd if=/tmp/hd_mig_ntfs.bin of=%s bs=512 seek=%d count=64 conv=notrunc status=none"
       % (IMG, p[5][0]))
    return p


def install(before):
    iso_for_install = OLD_ISO
    print("\n  [1] %s installs HavenDOS into the 1.2 GB gap (FAT16)" % os.path.basename(OLD_ISO))
    with vm(iso_for_install) as q:
        time.sleep(12)
        for _ in range(MENU_INSTALL):
            q.key("down"); time.sleep(0.35)
        q.key("ret")
        hit, s = wait_any(q, ["Install"], 60, "01_welcome")
        ck("the installer came up", hit is not None, s[:300])

        # Each screen is waited for by what it says, not by a sleep: the
        # second run of this harness lost a keypress between the picker
        # and the preview and read every screen one step late.
        def step_to(needle, name, tries=6):
            for _ in range(tries):
                q.key("ret")
                h, t = wait_any(q, [needle], 8, name)
                if h:
                    return t
            return t
        time.sleep(2)
        step_to("keeps this disk", "02a_picker")
        s = step_to("What will be written", "02_preview")
        ck("the preview says FAT16", "formatted FAT16" in s, s[:700])
        s = step_to("Nothing is deleted", "02b_confirm")
        q.type("YES"); time.sleep(0.4); q.key("ret")
        hit, s = wait_any(q, ["Complete", "Failed"], 180, "03_progress")
        ck("the install completed", hit == "Complete", s[:400])
    now = parts()
    new = [n for n in now if n not in before]
    ck("one partition was added", len(new) == 1, repr(now))
    return now[new[0]] if new else None


# ── [2] a FAT16 volume with a life in it ────────────────────────────────
SEED = {
    "_HAVENDO": b"setup_done=1\nautologin=0\nverbose_boot=0\n",
    "_ACCOUNT": b"kyan:5f4dcc3b5aa765d61d8327deb882cf99:1\nguest::0\n",
    "_THEME": b"ocean\n",
    "_CALC_HI": b"2+2=4\n",
    "HOSTS.TXT": b"10.0.2.2 host\n",
    "ETC/ALIASES.TXT": b"ll=ls -l\n",
    "ETC/TODO.TXT": b"[ ] move to FAT32\n",
    "HOME/KYAN/NOTES.TXT": b"these notes must survive the move\n",
}
REAL = {"_HAVENDO": ".havendos_config", "_ACCOUNT": ".accounts",
        "_THEME": ".theme", "_CALC_HI": ".calc_history"}


def seed(lba):
    print("\n  [2] filling the FAT16 volume the way a used system is filled")
    img = at(lba)
    for d in ("HOME", "HOME/KYAN", "HOME/KYAN/DOCS", "HOME/KYAN/DOCS/DEEP",
              "HOME/KYAN/MANY", "HOME/KYAN/MUSIC", "ETC"):
        sh("mmd -D o -i %s ::/%s" % (img, d))
    for name, data in SEED.items():
        open("/tmp/hd_mig_seed", "wb").write(data)
        r = sh("mcopy -D o -i %s /tmp/hd_mig_seed ::/%s" % (img, name))
        if r.returncode:
            print("      mcopy %s: %s" % (name, r.stderr.strip()))
    sh("head -c 2048 /dev/urandom > /tmp/hd_mig_pair.bin")
    sh("mcopy -D o -i %s /tmp/hd_mig_pair.bin ::/_PAIRREC" % img)
    sh("head -c 3145728 /dev/urandom > /tmp/hd_mig_a.bin")
    sh("mcopy -D o -i %s /tmp/hd_mig_a.bin ::/HOME/KYAN/DOCS/DEEP/A.BIN" % img)
    for i in range(100):
        open("/tmp/hd_mig_f", "w").write("file %d\n" % i * (i + 1))
        sh("mcopy -D o -i %s /tmp/hd_mig_f ::/HOME/KYAN/MANY/F%03d.TXT" % (img, i))
    sh("head -c 73400320 /dev/urandom > /tmp/hd_mig_big.bin")
    sh("mcopy -D o -i %s /tmp/hd_mig_big.bin ::/HOME/KYAN/MUSIC/BIG.BIN" % img)
    ck("mtools wrote all of it", sh("mdir -i %s ::/HOME/KYAN/MANY/F099.TXT" % img).returncode == 0
       and sh("mdir -i %s ::/_PAIRREC" % img).returncode == 0)
    r = fsck_part(lba, parts_of(lba))
    ck("fsck.fat: the FAT16 volume is clean before the move", r.returncode == 0,
       (r.stdout + r.stderr)[-300:])


def parts_of(first):
    for a, b, _ in parts().values():
        if a == first:
            return b
    return first


def write_esp_cfg(text):
    p = parts()
    e0 = p[1][0]
    open("/tmp/hd_mig_cfg.txt", "w").write(text)
    return sh("mcopy -o -i %s /tmp/hd_mig_cfg.txt ::/EFI/HAVENDOS/grub.cfg" % at(e0)).returncode == 0


def read_esp_cfg():
    p = parts()
    sh("rm -f /tmp/hd_mig_cfg_out.txt")
    sh("mcopy -n -i %s ::/EFI/HAVENDOS/grub.cfg /tmp/hd_mig_cfg_out.txt" % at(p[1][0]))
    try:
        return open("/tmp/hd_mig_cfg_out.txt").read()
    except OSError:
        return ""


def shell_boot(q_needles=("Shell",), name="shell"):
    q = vm()
    q.start()
    hit, s = wait_any(q, list(q_needles), 120, name)
    return q, hit, s


def run(q, cmd, needles, timeout, name):
    q.line(cmd)
    return wait_any(q, needles, timeout, name)


def real_menu_boot(name):
    """The menu as `migrate` wrote it: Windows is the default and counts
    down from eight, so HOME is pressed until GRUB is up -- it stops the
    countdown and selects entry 0, the new install -- then ENTER."""
    q = vm()
    q.start()
    for _ in range(70):
        time.sleep(0.4)
        q.key("home")
    q.key("ret")
    hit, s = wait_any(q, ["Enter=Select", "How would you like", "Password:",
                          "Terms of Service", "Installer", "install ISO"], 150, name)
    return q, hit, s


def main():
    for t in ("sgdisk", "mtools", "fsck.fat", "mkfs.vfat"):
        if t != "mtools" and not shutil.which(t):
            sys.exit("needs " + t)
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    if not os.path.exists(OLD_ISO):
        sys.exit("missing %s -- the FAT16 system is made by a release before "
                 "0.9.7.8 (set MIGRATE_OLD_ISO)" % OLD_ISO)
    print("migrate: FAT16 -> FAT32 on one disk, under OVMF%s" % (" (into free space)" if USE_GAP else ""))
    before = build()
    for n, (a, b, nm) in sorted(before.items()):
        print("    %d  %10d..%-10d %s" % (n, a, b, nm))
    ntfs_before = sha("/tmp/hd_mig_ntfs.bin")

    old = install(before)
    if not old:
        return 1
    old_first, old_last, _ = old
    # this release's kernel onto the old FAT16 volume, as `update` puts it
    r = sh("mcopy -o -i %s %s ::/BOOT/HAVENDOS.ELF" % (at(old_first), NEW_KERNEL))
    ck("the FAT16 system now runs this release's kernel", r.returncode == 0,
       r.stderr[-200:])
    seed(old_first)
    old_serial = int.from_bytes(bpb(old_first)[39:43], "little")

    # the harness's own menu for the FAT16 system: one entry, shell mode
    ok = write_esp_cfg("set timeout=0\nset default=0\n"
                       "menuentry \"HavenDOS FAT16 (harness, shell)\" {\n"
                       "    search --no-floppy --set=root --label HAVENDOS\n"
                       "    multiboot /boot/havendos.elf mode=shell gfx=text\n"
                       "    boot\n}\n")
    ck("the harness's shell entry is on the ESP", ok)

    # ── [3] migrate ──────────────────────────────────────────────────
    print("\n  [3] the FAT16 system runs `migrate`")
    q, hit, s = shell_boot(name="10_fat16_shell")
    try:
        ck("the FAT16 install booted to its shell", hit == "Shell", s[:400])
        time.sleep(1)
        q.line("cls")
        hit, s = run(q, "migrate", ["Then:", "Cannot", "nowhere"], 30, "11_migrate_list")
        print("\n".join("      | " + l for l in s.splitlines() if l.strip()))
        ck("it lists places to go", hit == "Then:", s[:800])
        ck("the empty 8 GB partition, as never used",
           re.search(r"partition 4\s+8192 MB\s+empty -- never used", s) is not None, s)
        ck("and the free space", "free space" in s, s)
        ck("it says what it left alone: the NTFS partition",
           re.search(r"partition 3\s+\d+ MB\s+holds NTFS", s) is not None, s)
        ck("and recovery, by type", "Windows recovery" in s, s)
        lines = s.splitlines()
        pick = None
        for l in lines:
            m = re.match(r"\s*(\d)\s+(free space|partition 4)", l)
            if m and ((USE_GAP and "free" in l) or (not USE_GAP and "partition 4" in l)):
                pick = m.group(1)
        ck("the target has a number", pick is not None, s)
        q.line("cls")
        hit, s = run(q, "migrate %s" % (pick or "1"), ["to go ahead"], 30, "12_migrate_plan")
        ck("it explains and asks for a typed word", hit is not None, s[:800])
        word = "MOVE" if USE_GAP else "ERASE"
        ck("the word is %s" % word, ("Type %s" % word) in s, s[:800])
        q.line(word)
        hit, s = wait_any(q, ["migrate finish", "FAILED"], 900, "13_migrate_done")
        print("\n".join("      | " + l for l in s.splitlines() if l.strip()))
        ck("the move completed", hit == "migrate finish" and "FAILED" not in s, s)
        ck("and it said the 70 MB file was not copied",
           "NOT copied" in s and "big.bin" in s.lower(), s)
    finally:
        q.stop()

    # ── [4] the disk ─────────────────────────────────────────────────
    print("\n  [4] the disk, by sgdisk, fsck.fat and mtools")
    after = parts()
    for n, (a, b, nm) in sorted(after.items()):
        print("    %d  %10d..%-10d %s  %s" % (n, a, b, nm, ptype(n)))
    newp = [n for n, v in after.items() if ptype(n) == HAVEN_GUID and v[0] != old_first]
    ck("there is exactly one new HavenDOS partition", len(newp) == 1, repr(after))
    if not newp:
        return 1
    nf, nl, _ = after[newp[0]]
    if USE_GAP:
        ck("it is in the free space after partition 4",
           nf > before[4][1] and nl < before[5][0], (nf, nl))
    else:
        ck("it is partition 4, same sectors, retyped",
           newp[0] == 4 and (nf, nl) == before[4][:2], (newp, nf, nl))
    for n in (1, 2, 3, 5):
        ck("partition %d unmoved" % n, after.get(n, (0, 0))[:2] == before[n][:2])
    ck("the old FAT16 partition is still there", any(v[0] == old_first for v in after.values()))
    v = sh("sgdisk -v %s" % IMG).stdout
    ck("sgdisk -v: no problems", "No problems found" in v, v[-300:])
    b = bpb(nf)
    ck("the new volume is FAT32", b[82:87] == b"FAT32", b[82:90])
    ck("labelled HAVENDOS", b[71:82] == b"HAVENDOS   ", b[71:82])
    new_serial = int.from_bytes(b[67:71], "little")
    ck("its serial differs from the old one's", new_serial != old_serial)
    r = fsck_part(nf, nl)
    ck("fsck.fat: the new volume is clean", r.returncode == 0, (r.stdout + r.stderr)[-400:])

    def get(lba, path, out):
        sh("rm -f %s" % out)
        return sh("mcopy -n -i %s ::%s %s" % (at(lba), path, out)).returncode == 0

    for fat16, real in REAL.items():
        ok = get(nf, "/" + real, "/tmp/hd_mig_x") and open("/tmp/hd_mig_x", "rb").read() == SEED[fat16]
        ck("%s arrived as %s, byte for byte" % (fat16, real), ok)
        ck("and not ALSO as %s" % fat16.lower(),
           sh("mdir -i %s ::/%s" % (at(nf), fat16)).returncode != 0)
    ck(".pairrec arrived, byte for byte",
       get(nf, "/.pairrec", "/tmp/hd_mig_x") and sha("/tmp/hd_mig_x") == sha("/tmp/hd_mig_pair.bin"))
    ck(".shell_history arrived (the shell had written to it)",
       sh("mdir -i %s ::/.shell_history" % at(nf)).returncode == 0)
    for name in ("HOSTS.TXT", "ETC/ALIASES.TXT", "ETC/TODO.TXT", "HOME/KYAN/NOTES.TXT"):
        ck("/%s, byte for byte" % name,
           get(nf, "/" + name, "/tmp/hd_mig_x") and open("/tmp/hd_mig_x", "rb").read() == SEED[name])
    ck("the 3 MB file three folders down, byte for byte",
       get(nf, "/HOME/KYAN/DOCS/DEEP/A.BIN", "/tmp/hd_mig_x") and sha("/tmp/hd_mig_x") == sha("/tmp/hd_mig_a.bin"))
    many = sh("mdir -b -i %s ::/HOME/KYAN/MANY" % at(nf)).stdout.split()
    ck("all hundred small files", len(many) == 100, len(many))
    ck("the last of them, byte for byte",
       get(nf, "/HOME/KYAN/MANY/F099.TXT", "/tmp/hd_mig_x")
       and open("/tmp/hd_mig_x").read() == "file 99\n" * 100)
    ck("the 70 MB file was NOT copied (bigger than the buffer)",
       sh("mdir -i %s ::/HOME/KYAN/MUSIC/BIG.BIN" % at(nf)).returncode != 0)
    ck("the kernel is the one the FAT16 system runs",
       get(nf, "/boot/havendos.elf", "/tmp/hd_mig_k1") and get(old_first, "/boot/havendos.elf", "/tmp/hd_mig_k2")
       and sha("/tmp/hd_mig_k1") == sha("/tmp/hd_mig_k2"))
    ser = "%04x-%04x" % (new_serial >> 16, new_serial & 0xFFFF)
    oser = "%04x-%04x" % (old_serial >> 16, old_serial & 0xFFFF)
    ck("the partition's own menu names its serial",
       get(nf, "/boot/grub/grub.cfg", "/tmp/hd_mig_x") and ("--fs-uuid " + ser) in open("/tmp/hd_mig_x").read())
    cfg = read_esp_cfg()
    print("      ESP grub.cfg:\n" + "\n".join("      | " + l for l in cfg.splitlines()))
    ck("the ESP menu: the new install first, by serial, with vol=",
       cfg.find("--fs-uuid " + ser) > 0 and ("vol=" + ser) in cfg
       and cfg.find(ser) < cfg.find("bootmgfw"), cfg[:300])
    ck("the old one after Windows, by its serial", ("--fs-uuid " + oser) in cfg and cfg.find(oser) > cfg.find("bootmgfw"))
    ck("Windows is still the default", "set default=1" in cfg)
    e0, e1, _ = after[1]
    sh("dd if=%s of=/tmp/hd_mig_esp_after.img bs=512 skip=%d count=%d status=none" % (IMG, e0, e1 - e0 + 1))
    ck("fsck.fat: the ESP is clean", sh("fsck.fat -n /tmp/hd_mig_esp_after.img").returncode == 0)
    ck("Windows' boot manager is byte-identical",
       sh("mcopy -n -i /tmp/hd_mig_esp_after.img ::/EFI/Microsoft/Boot/bootmgfw.efi /tmp/hd_mig_x").returncode == 0
       and sha("/tmp/hd_mig_x") == sha("/tmp/hd_mig_bootmgfw.bin"))
    sh("dd if=%s of=/tmp/hd_mig_x bs=512 skip=%d count=8192 status=none" % (IMG, before[3][0]))
    ck("the NTFS partition's first 4 MB are byte-identical", sha("/tmp/hd_mig_x") == ntfs_before)

    # ── [5] the real menu ────────────────────────────────────────────
    print("\n  [5] the menu as written, entry 0, desktop mode")
    q, hit, s = real_menu_boot("20_new_desktop")
    q.stop()
    ck("the NEW system booted", hit is not None and hit not in ("Installer", "install ISO"), s[:300])
    ck("to the login screen, not the setup wizard (.havendos_config came over)",
       hit in ("Enter=Select", "How would you like", "Password:"), s[:400])

    # ── [6] finish, from the new system ──────────────────────────────
    print("\n  [6] the new system, shell mode, by its serial: `migrate finish`")
    saved = cfg
    shell_entry = re.search(r'menuentry "HavenDOS[^"]*" \{.*?\n\}\n', cfg, re.S).group(0)
    shell_entry = shell_entry.replace("mode=desktop", "mode=shell").replace(
        'menuentry "HavenDOS', 'menuentry "harness: new HavenDOS shell', 1)
    write_esp_cfg("set timeout=0\nset default=0\n" + shell_entry)
    q, hit, s = shell_boot(name="30_new_shell")
    try:
        ck("the new system's shell", hit == "Shell", s[:300])
        time.sleep(1)
        q.line("cls")
        hit, s = run(q, "df", ["FAT32", "FAT16", "ramfs"], 20, "31_df")
        ck("df: the system volume is FAT32", hit == "FAT32", s[:500])
        q.line("cls")
        hit, s = run(q, "migrate", ["migrate finish", "Cannot", "already"], 30, "32_migrate_status")
        ck("migrate: the move is done, here is how to finish", hit == "migrate finish", s[:600])
        q.line("cls")
        hit, s = run(q, "migrate finish", ["to go ahead", "Cannot"], 30, "33_finish_plan")
        ck("finish explains and asks", hit == "to go ahead" and "Type REMOVE" in s, s[:600])
        q.line("REMOVE")
        hit, s = wait_any(q, ["is gone", "FAILED"], 120, "34_finish_done")
        print("\n".join("      | " + l for l in s.splitlines() if l.strip()))
        ck("finish completed", hit == "is gone", s[:600])
    finally:
        q.stop()

    # ── [7] the disk again ───────────────────────────────────────────
    print("\n  [7] the disk after `migrate finish`")
    fin = parts()
    for n, (a, b2, nm) in sorted(fin.items()):
        print("    %d  %10d..%-10d %s  %s" % (n, a, b2, nm, ptype(n)))
    ck("the FAT16 partition is gone from the table", all(v[0] != old_first for v in fin.values()))
    ck("its boot sector is zero (nothing can mount it)", not any(bpb(old_first)))
    ck("the FAT32 one is still there, unmoved", any(v[:2] == (nf, nl) for v in fin.values()))
    for n in (1, 2, 3, 5):
        ck("partition %d unmoved" % n, fin.get(n, (0, 0))[:2] == before[n][:2])
    v = sh("sgdisk -v %s" % IMG).stdout
    ck("sgdisk -v: no problems", "No problems found" in v, v[-300:])
    cfg = read_esp_cfg()
    ck("the menu has one HavenDOS entry, the new one",
       cfg.count("/boot/havendos.elf") == 1 and ser in cfg and oser not in cfg, cfg)
    ck("and the harness's entry is gone -- finish wrote the menu", "harness" not in cfg)
    ck("Windows is still in it, and the default", "bootmgfw" in cfg and "set default=1" in cfg)
    r = fsck_part(nf, nl)
    ck("fsck.fat: the FAT32 volume is still clean, after booting it twice", r.returncode == 0, (r.stdout + r.stderr)[-300:])

    # ── [8] boots once more ──────────────────────────────────────────
    print("\n  [8] the menu as `finish` left it")
    q, hit, s = real_menu_boot("40_after_finish")
    q.stop()
    ck("it boots the FAT32 system to the login screen",
       hit in ("Enter=Select", "How would you like", "Password:"), s[:400])

    print()
    if fails:
        print("%d of %d checks failed:" % (len(fails), total[0]))
        for f in fails:
            print("  - " + f)
        print("screenshots: /tmp/hd_mig_*.png")
        return 1
    print("all %d checks passed -- FAT16 moved to FAT32 on one disk, booted, finished"
          % total[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
