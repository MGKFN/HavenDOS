#!/usr/bin/env python3
"""usb_files_0973.py -- 0.9.7.3: a USB stick is the folder /USB, and Files knows file types.

Kyan, 22 Nov: "maybe the ability to save something to USB and use some
general file name extensions ... just like windows or Mac OS Linux .txt
.md etc .zip". He chose "Everywhere" for where a stick can be written.

  1. `ls /USB` lists what is on the stick
  2. `echo ... > /USB/NEW.TXT` writes a file onto it; `cat` reads it back
  3. `mkdir`, `cp` and `rename` work inside /USB, folders included
  4. a long name with a lower-case extension (`longfilename-test.md`)
     is written and listed as typed
  5. `del` removes a file from the stick
  6. Files: the root has a USB folder whose type is "USB stick"; inside
     it, NOTE.TXT is "Text" and the long name is "Markdown"
  7. after QEMU exits, mtools -- not HavenDOS -- reads the stick image and
     finds every file with its contents, and the deleted one gone

Run:  python3 harness/usb_files_0973.py [path/to.iso]   (foreground)
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu as Q                                        # noqa: E402
from mouse_apps import boot_to_shell                    # noqa: E402
from usb_storage import make_stick                      # noqa: E402

ISO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "havendos.iso")
WORK = "/tmp/hd_usb0973"
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("  %s  %s" % ("ok  " if ok else "FAIL", name))
    if not ok and detail:
        print("          " + str(detail)[-1400:].replace("\n", "\n          "))
    return ok


def run(*a):
    return subprocess.run(list(a), check=True, capture_output=True, text=True).stdout


def cmd(q, line, wait=1.5):
    q.line("cls"); time.sleep(1.0)
    q.line(line); time.sleep(wait)
    return q.screen()


def main():
    if not os.path.exists(ISO):
        sys.exit("missing " + ISO)
    for t in ("mkfs.vfat", "mcopy", "mdir", "mtype", "dd"):
        if not shutil.which(t):
            sys.exit("needs " + t)
    print("0.9.7.3: the stick as /USB, written from HavenDOS; file types in Files")
    os.makedirs(WORK, exist_ok=True)
    stick = make_stick(os.path.join(WORK, "stick.img"), mb=64, fat=32)
    part = os.path.join(WORK, "part.img")
    Q.kill_stale()
    with Q.Qemu(iso=ISO, mem=256, usbstick=stick) as q:
        boot_to_shell(q)
        s = cmd(q, "ls /USB", 2.5)
        check("`ls /USB` lists the stick (ARGO.TXT and DOCS from the fixture)", "ARGO.TXT" in s and "DOCS" in s, s)

        cmd(q, "echo written by HavenDOS > /USB/NEW.TXT", 2.5)
        s = cmd(q, "cat /USB/NEW.TXT", 2.0)
        check("`echo > /USB/NEW.TXT`, then `cat` reads it back", "written by HavenDOS" in s.split("cat /USB/NEW.TXT")[-1], s)

        s = cmd(q, "mkdir /USB/SAVED", 2.0)
        check("`mkdir /USB/SAVED`", "Created" in s, s)
        s = cmd(q, "cp /USB/NEW.TXT /USB/SAVED/COPY.TXT", 2.5)
        check("`cp` into a folder on the stick", "Copied" in s, s)
        s = cmd(q, "rename /USB/SAVED/COPY.TXT /USB/SAVED/KEPT.TXT", 2.0)
        check("`rename` inside it", "->" in s and "Failed" not in s, s)
        s = cmd(q, "ls /USB/SAVED", 2.0)
        check("`ls /USB/SAVED` shows KEPT.TXT", "KEPT.TXT" in s and "COPY.TXT" not in s, s)

        cmd(q, "echo a long name > /USB/longfilename-test.md", 2.5)
        s = cmd(q, "ls /USB", 2.5)
        check("a long, lower-case name is written and listed as typed", "longfilename-test.md" in s, s)

        cmd(q, "echo gone soon > /USB/TEMP.TXT", 2.0)
        s = cmd(q, "del /USB/TEMP.TXT", 2.0)
        check("`del /USB/TEMP.TXT`", "Deleted" in s, s)

        # Files
        q.line("open files"); time.sleep(3.0)
        s = q.screen()
        check("Files: a USB folder at the root, typed \"USB stick\"", "USB stick" in s, s)
        # USB is listed last at the root: Page Down to the end, then Enter
        for _ in range(6):
            q.key("pgdn"); time.sleep(0.3)
        q.key("ret"); time.sleep(3.0)
        s = q.screen()
        row_note = next((l for l in s.splitlines() if "NEW.TXT" in l), "")
        row_md = next((l for l in s.splitlines() if "longfilename-test.md" in l), "")
        check("inside it, NEW.TXT is \"Text\"", "Text" in row_note, s)
        check("and longfilename-test.md is \"Markdown\"", "Markdown" in row_md, s)
        q.key("esc"); time.sleep(1.0)

    # what is really on the stick, read by something that is not HavenDOS
    run("dd", "if=" + stick, "of=" + part, "bs=512", "skip=2048")
    root = run("mdir", "-i", part, "-b", "::/")
    check("mtools: NEW.TXT, SAVED and the long name are on the stick",
          "NEW.TXT" in root.upper() and "SAVED" in root.upper() and "longfilename-test.md" in root, root)
    check("mtools: TEMP.TXT is not", "TEMP.TXT" not in root.upper(), root)
    t = run("mtype", "-i", part, "::/NEW.TXT")
    check("mtools: NEW.TXT holds what HavenDOS wrote", "written by HavenDOS" in t, t)
    t = run("mtype", "-i", part, "::/SAVED/KEPT.TXT")
    check("mtools: SAVED/KEPT.TXT is the copy", "written by HavenDOS" in t, t)
    t = run("mtype", "-i", part, "::/longfilename-test.md")
    check("mtools: the long-named file holds its text", "a long name" in t, t)
    fsck = subprocess.run(["fsck.vfat", "-n", part], capture_output=True, text=True)
    check("fsck.vfat finds the stick's filesystem clean", fsck.returncode == 0, fsck.stdout + fsck.stderr)

    print("\n%d/%d" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
