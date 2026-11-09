#!/usr/bin/env python3
"""
Headless QEMU driver for HavenDOS.

Drives QEMU over QMP (JSON), not HMP: the HMP readline echo mangles anything
sent faster than a human types, which is where the first attempt at this
harness went wrong.

The screen is read by dumping the VGA text buffer at 0xB8000 with pmemsave
and decoding it, which gives the exact characters rather than an image to
squint at, and costs a fraction of a screendump.

Never uses pkill: the handoff records that `pkill -f qemu-system` kills the
script running it, because the pattern matches the script's own command line.
Processes are tracked and killed by PID.
"""
import os, socket, subprocess, sys, time, signal, glob, json

class Qemu:
    def __init__(self, iso=None, extra=None, mem=256, disk=None, usbstick=None,
                 audio=False, usb=True, sock="/tmp/hd_qmp.sock", boot=None,
                 binary="qemu-system-i386", machine=None, gfx=True):
        self.sockpath = sock
        self.gfx = gfx
        self._cols, self._rows = 80, 25
        self.proc = None
        self.sock = None
        self.f = None
        self.cmd = [binary, "-m", str(mem), "-display", "none",
                    "-qmp", f"unix:{sock},server,nowait"]
        if machine:
            self.cmd += ["-M", machine]
        if iso:
            self.cmd += ["-cdrom", iso]
        if disk:
            self.cmd += ["-drive", f"file={disk},format=raw,if=ide,index=0,media=disk"]
        self.cmd += ["-boot", boot or ("d" if iso else "c")]
        if usb:
            self.cmd += ["-usb"]
        if usbstick:
            self.cmd += ["-drive", f"if=none,id=stick,file={usbstick},format=raw",
                         "-device", "usb-storage,drive=stick,bus=usb-bus.0"]
        if audio:
            self.cmd += ["-audiodev", "none,id=snd0", "-device", "AC97,audiodev=snd0"]
        if extra:
            self.cmd += extra

    # ── lifecycle ────────────────────────────────────────────────────
    def start(self):
        if os.path.exists(self.sockpath):
            os.unlink(self.sockpath)
        self.proc = subprocess.Popen(self.cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(self.sockpath):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited at startup:\n"
                                   + self.proc.stderr.read().decode())
            time.sleep(0.1)
        else:
            raise RuntimeError("qmp socket never appeared")
        # The socket file can appear a moment before QEMU is listening on
        # it, and a just-killed instance can leave a stale one behind, so the
        # connect is retried rather than taken as fatal on the first refusal.
        last = None
        for _ in range(40):
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(self.sockpath)
                break
            except (ConnectionRefusedError, FileNotFoundError) as e:
                last = e
                try: self.sock.close()
                except Exception: pass
                if self.proc.poll() is not None:
                    raise RuntimeError("qemu exited:\n"
                                       + self.proc.stderr.read().decode())
                time.sleep(0.25)
        else:
            raise RuntimeError("could not reach the qemu monitor: %r" % (last,))
        self.sock.settimeout(20)
        self.f = self.sock.makefile("rwb")
        self._readmsg()                       # greeting
        self.cmd_qmp("qmp_capabilities")
        return self

    def stop(self):
        try:
            if self.f: self.f.close()
            if self.sock: self.sock.close()
        except Exception:
            pass
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            try: self.proc.wait(timeout=5)
            except Exception: pass
        if os.path.exists(self.sockpath):
            try: os.unlink(self.sockpath)
            except Exception: pass

    def __enter__(self): return self.start()
    def __exit__(self, *a): self.stop()

    # ── QMP ──────────────────────────────────────────────────────────
    def _readmsg(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            try:
                return json.loads(line.decode())
            except ValueError:
                continue

    def cmd_qmp(self, command, **args):
        msg = {"execute": command}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            m = self._readmsg()
            if "event" in m:           # async event, keep reading
                continue
            return m

    def hmp(self, line):
        r = self.cmd_qmp("human-monitor-command", **{"command-line": line})
        return r.get("return", "")

    # ── input ────────────────────────────────────────────────────────
    _MAP = {" ": ["spc"], "-": ["minus"], "_": ["shift", "minus"],
            ".": ["dot"], "/": ["slash"], ":": ["shift", "semicolon"],
            ";": ["semicolon"], ",": ["comma"], "=": ["equal"],
            "+": ["shift", "equal"], "\n": ["ret"], "\t": ["tab"],
            "*": ["shift", "8"], "?": ["shift", "slash"],
            "'": ["apostrophe"], '"': ["shift", "apostrophe"],
            # The shifted number row was missing, so a password containing
            # '@' silently lost that character and every login failed with
            # "Wrong password" -- admin's password is Admin@123.
            "!": ["shift", "1"], "@": ["shift", "2"], "#": ["shift", "3"],
            "$": ["shift", "4"], "%": ["shift", "5"], "^": ["shift", "6"],
            "&": ["shift", "7"], "(": ["shift", "9"], ")": ["shift", "0"],
            "<": ["shift", "comma"], ">": ["shift", "dot"],
            "|": ["shift", "backslash"], "\\": ["backslash"],
            "[": ["bracket_left"], "]": ["bracket_right"],
            "{": ["shift", "bracket_left"], "}": ["shift", "bracket_right"],
            "~": ["shift", "grave_accent"], "`": ["grave_accent"]}

    def key(self, *names, hold=None, settle=0.05):
        keys = [{"type": "qcode", "data": n} for n in names]
        self.cmd_qmp("send-key", keys=keys)
        if settle: time.sleep(settle)

    def type(self, s, settle=0.05):
        for ch in s:
            if ch in self._MAP:        self.key(*self._MAP[ch], settle=settle)
            elif ch.isupper():         self.key("shift", ch.lower(), settle=settle)
            else:                      self.key(ch, settle=settle)

    # ── mouse ────────────────────────────────────────────────────────
    # drivers/mouse.c maps one PS/2 unit to one text cell 1:1, starts at
    # (40,12) and clamps to 0..79 / 0..24.  So the pointer can be driven to
    # an exact cell by slamming it into the top-left corner first and then
    # moving by the coordinate itself.  (The clamp is hardcoded to 79/24,
    # which is its own bug at 90 columns or 60 rows -- not this harness's
    # problem, but it means mouse tests must stay inside 80x25.)
    def mouse_to(self, x, y, settle=0.3):
        """Park the pointer on an exact text cell.

        drivers/mouse.c accumulates raw PS/2 counts and divides by
        MOUSE_DIVISOR (4), so ONE CELL IS FOUR RAW UNITS -- passing the cell
        count straight through moves a quarter of the distance, which is
        what the first version of this did.  It also doubles any delta whose
        magnitude exceeds 6*divisor (24 raw), so each event carries exactly
        four units: one cell, below the acceleration threshold, every time.
        The driver clamps to the GRID, so a long run into the top-left
        corner gives a known origin to count from -- but the run has to
        be longer than the grid is wide.

        v0.8.8.7: the homing runs were a flat -100 and -40, written when
        the clamp really was 0..79 and 0..24. On a 160x50 console that
        is not far enough to reach the corner from the right-hand side,
        so every click afterwards landed at an offset nobody could see,
        and a correct taskbar hit box looked like a dead one. They are
        derived from the grid the readback is set to now, with the old
        values as a floor so 80x25 behaviour is unchanged."""
        DIV = 4                      # MOUSE_DIVISOR in drivers/mouse.c
        def walk(axis, cells):
            step = DIV if cells > 0 else -DIV
            for _ in range(abs(int(cells))):
                self.cmd_qmp("input-send-event",
                             events=[{"type": "rel",
                                      "data": {"axis": axis, "value": step}}])
                time.sleep(0.010)
        walk("x", -max(100, self._cols * 2))     # into the left edge
        walk("y", -max(40,  self._rows * 2))     # and the top edge
        time.sleep(0.15)
        walk("x", x)
        walk("y", y)
        time.sleep(settle)

    def click(self, x, y, button="left", settle=0.6):
        self.mouse_to(x, y)
        self.cmd_qmp("input-send-event",
                     events=[{"type": "btn",
                              "data": {"down": True, "button": button}}])
        time.sleep(0.08)
        self.cmd_qmp("input-send-event",
                     events=[{"type": "btn",
                              "data": {"down": False, "button": button}}])
        time.sleep(settle)

    def rclick(self, x, y, settle=0.6):
        self.click(x, y, button="right", settle=settle)

    def line(self, s, settle=0.05, pause=0.35):
        """Type a command and press return.  The setup wizard eats the first
        keystroke of a typed word (handoff, section 6), so the first character
        is sent on its own with a pause behind it."""
        if s:
            time.sleep(pause)
            self.type(s[0], settle)
            time.sleep(pause)
            self.type(s[1:], settle)
        time.sleep(0.2)
        self.key("ret")
        time.sleep(pause)

    # ── screen ───────────────────────────────────────────────────────
    def text(self, rows=25, cols=80):
        path = "/tmp/hd_vga_%d.bin" % os.getpid()
        if os.path.exists(path): os.unlink(path)
        self.cmd_qmp("pmemsave", val=0xB8000, size=rows * cols * 2, filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= rows * cols * 2:
                break
            time.sleep(0.05)
        else:
            return ["<no vga dump>"]
        data = open(path, "rb").read()
        out = []
        for r in range(rows):
            s = ""
            for c in range(cols):
                ch = data[(r * cols + c) * 2]
                s += chr(ch) if 32 <= ch < 127 else (" " if ch in (0, 255) else ".")
            out.append(s.rstrip())
        return out

    def fbtext(self, rows=25, cols=80):
        """The screen as characters, read back off a framebuffer screendump.

        In framebuffer mode there is no 0xB8000 to dump, so the pixels are
        decoded against the same font the kernel draws with.  This is exact,
        not OCR."""
        import fbtext as _fb
        ppm = "/tmp/hd_fb_%d.ppm" % os.getpid()
        if os.path.exists(ppm): os.unlink(ppm)
        self.cmd_qmp("screendump", filename=ppm)
        for _ in range(60):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000: break
            time.sleep(0.05)
        else:
            return ["<no screendump>"]
        time.sleep(0.05)
        try:
            out = _fb.decode(ppm, cols, rows)
        except Exception as e:
            out = ["<decode failed: %s>" % e]
        os.unlink(ppm)
        return out

    def screen(self, cols=None, rows=None):
        """Whichever readback this machine's console needs.

        v0.8.8: the grid is no longer always 80x25.  `cols`/`rows`
        override it for a console that has been switched, and
        set_grid() makes the override stick so a sequence of calls does
        not each have to repeat it.
        """
        c = cols if cols else self._cols
        r = rows if rows else self._rows
        if self.gfx:
            return "\n".join(self.fbtext(rows=r, cols=c))
        return "\n".join(self.text(rows=r, cols=c))

    def set_grid(self, cols=80, rows=25):
        """Tell the readback what grid the console is running."""
        self._cols, self._rows = cols, rows
        return self

    def shot(self, path):
        self.cmd_qmp("screendump", filename=path)
        return path

    def wait_for(self, needle, timeout=90, poll=1.0):
        t0 = time.time(); last = ""
        while time.time() - t0 < timeout:
            s = self.screen()
            if needle in s: return s
            last = s
            time.sleep(poll)
        sys.stderr.write("wait_for(%r) timed out. Screen:\n%s\n" % (needle, last))
        return None

    def wait_any(self, needles, timeout=90, poll=1.0):
        t0 = time.time(); last = ""
        while time.time() - t0 < timeout:
            s = self.screen()
            for n in needles:
                if n in s: return n, s
            last = s
            time.sleep(poll)
        return None, last


def kill_stale():
    """Kill leftover qemu by PID -- never by pattern (see handoff section 6)."""
    killed = []
    for p in glob.glob("/proc/[0-9]*/comm"):
        try: name = open(p).read().strip()
        except Exception: continue
        if name.startswith("qemu-system"):
            pid = int(p.split("/")[2])
            if pid == os.getpid(): continue
            try:
                os.kill(pid, signal.SIGKILL); killed.append(pid)
            except Exception: pass
    if killed: time.sleep(0.6)
    return killed


if __name__ == "__main__":
    kill_stale()
    iso = sys.argv[1] if len(sys.argv) > 1 else "havendos.iso"
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    with Qemu(iso) as q:
        time.sleep(wait)
        print(q.screen())
