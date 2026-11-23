#!/usr/bin/env python3
"""test_retropc.py -- Retro PC as a program of its own.  v0.9.7.5.

Runs retropc/retropc.c through retropc/host_null.c -- the same program
the Windows and Linux windows run, with a script typing and the screen
printed as text at the end -- and checks what an old PC shows and what
it leaves on disk.

    python3 tests/test_retropc.py <retropc-headless> [<asan build>]

Each run copies the program into a directory of its own, because the
RETRO folder is the one beside the program: a run must not see another
run's images or hard disks.

The ASAN build runs the K-DOS cases again (seconds each). The 386 cases
run on the -O2 build only: a BOOT OS boot is about 1.4 billion
instructions, which is 20 s optimised and minutes under the sanitiser.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCH = os.path.join(ROOT, "archive", "retro")
KDOS = os.path.join(ARCH, "kdos-beta-1.1b.img")
HD_BYTES = 1 * 16 * 63 * 512

total = fails = 0


def ck(what, ok, detail=""):
    global total, fails
    total += 1
    if ok:
        if os.environ.get("V"):
            print("  ok    " + what)
    else:
        fails += 1
        print("  FAIL  " + what)
        if detail:
            for ln in str(detail).rstrip().splitlines()[-30:]:
                print("        | " + ln)


def slurp(p):
    return open(p, "rb").read() if os.path.exists(p) else b""


def home(binary):
    d = tempfile.mkdtemp(prefix="rptest_")
    shutil.copy(binary, os.path.join(d, "retropc"))
    return d


def run(d, *args, keys="", after=1500, timeout=300):
    cmd = [os.path.join(d, "retropc")]
    if keys:
        cmd += ["--keys", keys]
    cmd += ["--after", str(after)] + list(args)
    p = subprocess.run(cmd, cwd=d, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def kdos_cases(binary, tag):
    # 1. a built-in floppy: K-DOS, its login, a command
    d = home(binary)
    rc, out, err = run(d, "--boot", "KDOS11B", keys=r"admin\n1234\nhelp\n")
    ck(f"{tag}: K-DOS boots, admin/1234 logs in, help lists its commands",
       rc == 0 and "K-DOS-Beta-1.1b Active Shell." in out and "Commands: time, calc, user" in out, out + err)
    ck(f"{tag}: nothing written, so no hard disk file is made",
       not os.path.exists(os.path.join(d, "RETRO", "KDOS11B.HDD")), os.listdir(d))

    # 2. its hard disk: `save` writes sector 10 of drive 80h, and that
    # is RETRO/KDOS11B.HDD beside the program, 504 KB, as on HavenDOS
    rc, out, err = run(d, "--boot", "KDOS11B", keys=r"admin\n1234\nuser\n1\nabc\nsave\n")
    hdp = os.path.join(d, "RETRO", "KDOS11B.HDD")
    data = slurp(hdp)
    ck(f"{tag}: save says [OK] and writes RETRO/KDOS11B.HDD, 504 KB",
       "save [OK]" in out and len(data) == HD_BYTES, out + err)
    # head 1 sector 10 is LBA 72, and K-DOS writes its input buffer
    # there -- the word "save" -- which is what QEMU's disk showed too
    # (tests/test_emu86.c checks the same thing inside the emulator)
    ck(f"{tag}: what it wrote is 'save' at LBA 72 (head 1, sector 10), and nothing else",
       data[72 * 512:72 * 512 + 4] == b"save" and not any(data[:72 * 512]) and not any(data[73 * 512:]),
       "nonzero sectors: " + str(sorted({i // 512 for i, b in enumerate(data) if b})))
    ck(f"{tag}: the log names the file it saved", "saved " in err and "KDOS11B.HDD" in err, err)

    # 3. a boot that writes nothing leaves the saved disk as it was:
    # read in, not written back, not truncated
    before = data
    rc, out, err = run(d, "--boot", "KDOS11B", keys=r"admin\n1234\nload\n")
    after_ = slurp(hdp)
    ck(f"{tag}: a later boot that writes nothing leaves the saved disk exactly as it was",
       rc == 0 and "Active Shell" in out and before and after_ == before and "saved" not in err, out + err)
    shutil.rmtree(d)

    # 4. a file on the command line, its hard disk beside it
    d = home(binary)
    img = os.path.join(d, "MINE.IMG")
    shutil.copy(KDOS, img)
    rc, out, err = run(d, img, keys=r"admin\n1234\nuser\n1\nxyz\nsave\n")
    ck(f"{tag}: retropc <file> boots it, and its disk is MINE.HDD beside it",
       "Active Shell" in out and len(slurp(os.path.join(d, "MINE.HDD"))) == HD_BYTES, out + err)
    ck(f"{tag}: the image itself is never written", open(img, "rb").read() == open(KDOS, "rb").read())

    # 5. F4 leaves; started on a file, that ends the program -- no
    # screen is printed, because the window was not closed
    rc, out, err = run(d, img, keys=r"admin\n{F4}", after=60000, timeout=60)
    ck(f"{tag}: F4 leaves, and started on a file that quits", rc == 0 and out.strip() == "", out + err)

    # 6. files that will not boot say so on the screen
    bad = os.path.join(d, "BAD.IMG")
    open(bad, "wb").write(b"\x00" * 1024)
    rc, out, err = run(d, bad, after=500)
    ck(f"{tag}: an image with no boot signature: 'will not boot', with the reason",
       "This image will not boot." in out, out + err)
    big = os.path.join(d, "BIG.IMG")
    open(big, "wb").write(b"\x00" * 3000000)
    rc, out, err = run(d, big, after=500)
    ck(f"{tag}: bigger than 2.88 MB: said, in those words", "bigger than a 2.88 MB floppy" in out, out + err)
    rc, out, err = run(d, os.path.join(d, "NOPE.IMG"), after=500)
    ck(f"{tag}: a file that is not there: 'could not read the file'", "could not read the file" in out, out + err)
    shutil.rmtree(d)

    # 7. the list: the built-ins, then RETRO beside the program
    d = home(binary)
    os.mkdir(os.path.join(d, "RETRO"))
    shutil.copy(KDOS, os.path.join(d, "RETRO", "OLDONE.IMG"))
    open(os.path.join(d, "RETRO", "NOTES.TXT"), "w").write("not an image")
    rc, out, err = run(d, after=500)
    ck(f"{tag}: the list shows the four built in, then RETRO's .IMG and not its .TXT",
       all(s in out for s in ("K-DOS Beta 1.1b", "BOOT OS 0.3.1", "BOOT OS 0.5.0", "BOOT OS 0.5.2",
                              "OLDONE.IMG", "RETRO folder")) and "NOTES.TXT" not in out, out)
    rc, out, err = run(d, keys=r"{DOWN}{DOWN}{DOWN}{DOWN}\nadmin\n1234\nhelp\n")
    ck(f"{tag}: Enter on it boots it from the folder", "Commands: time, calc" in out, out + err)
    ck(f"{tag}: and F4 from a PC started in the list goes back to the list",
       "Up/Down choose" in run(d, keys=r"\n{W500}{F4}", after=800)[1])
    rc, out, err = run(d, keys="q", after=60000, timeout=60)
    ck(f"{tag}: Q in the list quits", rc == 0 and out.strip() == "", out + err)
    shutil.rmtree(d)

    # 8. arguments
    d = home(binary)
    rc, out, err = run(d, "--boot", "NOSUCH", after=100)
    ck(f"{tag}: --boot with an unknown id is refused, and the ids are listed",
       rc == 2 and "no built-in image NOSUCH" in err and "KDOS11B" in err, err)
    rc, out, err = run(d, "--mem", "1", after=100)
    ck(f"{tag}: --mem outside 4..256 is refused", rc == 2, err)
    shutil.rmtree(d)


def boot386_cases(binary):
    d = home(binary)
    # the GRUB menu, as its CD had it, with the text desktop chosen
    rc, out, err = run(d, "--boot", "BOOTOS31", after=200)
    ck("386: BOOT OS 0.3.1's own boot menu, with what each entry is",
       "Its boot menu, as GRUB showed it:" in out and "[Text Desktop]" in out and "graphics" in out, out)
    # then the whole way: boot, log in, the shell, a command
    rc, out, err = run(d, "--boot", "BOOTOS31",
                       keys=r"{W500}\n{W25000}bootos\n1234\n{W3000}\t{W2000}help\n", after=3000)
    ck("386: BOOT OS 0.3.1 boots, bootos/1234 logs in, Tab opens its shell, help answers",
       "bootos@bootos> help" in out and "BOOT OS  -  Command Reference" in out, out + err)

    # an ISO off the disk: the 0.5.2 kernel and its grub.cfg, put on a
    # CD the way its own ISO was, read through ISO 9660 like any other
    iso_dir = os.path.join(d, "cd")
    os.makedirs(os.path.join(iso_dir, "boot", "grub"))
    shutil.copy(os.path.join(ARCH, "bootos-0.5.2.elf"), os.path.join(iso_dir, "boot", "bootos.elf"))
    shutil.copy(os.path.join(ARCH, "bootos-0.5.2.cfg"), os.path.join(iso_dir, "boot", "grub", "grub.cfg"))
    os.mkdir(os.path.join(d, "RETRO"))
    iso = os.path.join(d, "RETRO", "BOOT52.ISO")
    subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-R", "-o", iso, iso_dir], check=True,
                   capture_output=True)
    rc, out, err = run(d, keys=r"{DOWN}{DOWN}{DOWN}{DOWN}\n", after=500)
    ck("386: a .ISO in RETRO is listed and Enter shows the menu off the CD",
       "BOOT OS v0.5.2" in out and "Its boot menu" in out, out + err)
    rc, out, err = run(d, iso, keys=r"{W500}\n{W25000}", after=500)
    ck("386: and its kernel boots to its login screen", "Password:" in out and "v0.5.2" in out, out + err)
    shutil.rmtree(d)


def font_case():
    # The font: the box lines and blocks the kernel's 8x16 font leaves
    # blank are made at start-up. Checked by shape, from C.
    src = os.path.join(tempfile.mkdtemp(prefix="rpfont_"), "f.c")
    open(src, "w").write(r'''
#include <stdio.h>
void rp_font_build(unsigned char *f);
static unsigned char f[4096];
static int px(int c, int x, int y){ return (f[c * 16 + y] >> (7 - x)) & 1; }
int main(void){
    int blank = 0;
    rp_font_build(f);
    for(int c = 0xB0; c <= 0xDF; c++){ int any = 0; for(int y = 0; y < 16; y++) any |= f[c * 16 + y]; if(!any) blank++; }
    printf("blank %d\n", blank);
    /* C4: one row across, nothing else */
    printf("C4 %d\n", f[0xC4*16+7] == 0xFF && f[0xC4*16+6] == 0 && f[0xC4*16+8] == 0);
    /* B3: two pixels down the middle, top to bottom */
    { int ok = 1; for(int y = 0; y < 16; y++) ok &= f[0xB3*16+y] == 0x18; printf("B3 %d\n", ok); }
    /* C9 (top-left double corner): outer lines meet at (1,5), inner at (5,9);
       nothing above row 5 or left of column 1 */
    { int ok = px(0xC9,1,5) && px(0xC9,7,5) && px(0xC9,1,15) && px(0xC9,5,9) && px(0xC9,7,9) && px(0xC9,5,15)
             && !px(0xC9,3,3) && !px(0xC9,0,8) && !px(0xC9,3,7) && !px(0xC9,6,7);
      for(int y = 0; y < 5; y++) ok &= f[0xC9*16+y] == 0;
      printf("C9 %d\n", ok); }
    /* CE: the four inner corners, the middle empty */
    printf("CE %d\n", !px(0xCE,3,7) && !px(0xCE,4,7) && px(0xCE,1,0) && px(0xCE,6,15) && px(0xCE,0,5) && px(0xCE,7,9));
    /* ASCII is the kernel's: 'A' unchanged */
    { int any = 0; for(int y = 0; y < 16; y++) any |= f['A'*16+y]; printf("A %d\n", any != 0); }
    return 0;
}
''')
    exe = src[:-2]
    subprocess.run(["gcc", "-O1", "-iquote", os.path.join(ROOT, "include"), "-o", exe, src,
                    os.path.join(ROOT, "retropc", "font.c")], check=True)
    r = dict(ln.split() for ln in subprocess.run([exe], capture_output=True, text=True).stdout.split("\n") if ln)
    ck("font: every glyph from B0 to DF is drawn (the kernel's font leaves them blank)", r.get("blank") == "0", r)
    ck("font: C4 is one row across", r.get("C4") == "1", r)
    ck("font: B3 is two pixels down the middle", r.get("B3") == "1", r)
    ck("font: C9's double lines meet as two nested corners", r.get("C9") == "1", r)
    ck("font: CE has four inner corners and an empty middle", r.get("CE") == "1", r)
    ck("font: ASCII is the kernel's 8x16 font", r.get("A") == "1", r)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: test_retropc.py <retropc-headless> [<asan build>]")
    print("test_retropc -- Retro PC as a program: K-DOS, BOOT OS, files, the font")
    font_case()
    kdos_cases(sys.argv[1], "O2")
    if len(sys.argv) > 2:
        kdos_cases(sys.argv[2], "asan")
    boot386_cases(sys.argv[1])
    print(f"\n{total - fails}/{total} passed")
    sys.exit(1 if fails else 0)


main()
