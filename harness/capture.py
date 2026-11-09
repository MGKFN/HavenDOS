#!/usr/bin/env python3
"""
Drive HavenDOS through everything it does and photograph each screen.

Two machines are run: the install ISO, which only ever shows the installer,
and the disk that installer wrote, which boots the real system.  Each shot is
a QEMU screendump saved as PNG; the screen is also decoded back to characters
(fbtext) so the script can wait for a screen to arrive instead of sleeping and
hoping.

Two things bite anything that scripts this system and are handled here rather
than discovered again: long output pages and waits for a key, so a paging
command must be dismissed before the next one is typed; and the screensaver
arrives after two idle minutes, which a capture run spends easily.
"""
import sys, os, time, shutil
sys.path.insert(0, '/home/claude/havendos/harness')
from qemu import Qemu, kill_stale
import fbtext
from PIL import Image

SRC   = "/home/claude/havendos/HavenDOS-0.7.20-source"
ISO   = SRC + "/HavenDOS-0.8.0.iso"
OUT   = "/home/claude/havendos/screenshots"
BLANK = "/home/claude/havendos/blank.img"
DISK  = "/home/claude/havendos/installed.img"
STICK = "/home/claude/havendos/usbstick.img"

_n = [0]
_index = []


def shot(q, name, caption, settle=0.7):
    time.sleep(settle)
    _n[0] += 1
    ppm = "/tmp/hd_cap.ppm"
    if os.path.exists(ppm):
        os.unlink(ppm)
    q.cmd_qmp("screendump", filename=ppm)
    for _ in range(80):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            break
        time.sleep(0.05)
    time.sleep(0.1)
    fn = "%02d-%s.png" % (_n[0], name)
    Image.open(ppm).convert("RGB").save(os.path.join(OUT, fn))
    os.unlink(ppm)
    _index.append((fn, caption))
    print("  %-36s %s" % (fn, caption))
    return fn


def waitfor(q, needle, timeout=60, poll=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in q.screen():
            return True
        time.sleep(poll)
    print("    !! never saw %r" % needle)
    return False


def burst(q, frames=80, gap=0.18):
    """Grab frames as fast as QEMU will dump them, for screens that are on
    display too briefly to poll for.  Returns the frame paths, in order."""
    import tempfile
    d = tempfile.mkdtemp(prefix="hdburst")
    out = []
    for i in range(frames):
        f = os.path.join(d, "%03d.ppm" % i)
        try:
            q.cmd_qmp("screendump", filename=f)
        except Exception:
            break
        out.append(f)
        time.sleep(gap)
    time.sleep(0.5)
    return d, [f for f in out if os.path.exists(f) and os.path.getsize(f) > 1000]


def _decoded(cache, f):
    if f not in cache:
        try:
            cache[f] = "\n".join(fbtext.decode(f))
        except Exception:
            cache[f] = ""
    return cache[f]


def save_frame(path, name, caption):
    _n[0] += 1
    fn = "%02d-%s.png" % (_n[0], name)
    Image.open(path).convert("RGB").save(os.path.join(OUT, fn))
    _index.append((fn, caption))
    print("  %-36s %s" % (fn, caption))


def pick(frames, cache, needle, last=False):
    """The first (or last) frame whose decoded text holds `needle`."""
    hits = [f for f in frames if needle in _decoded(cache, f)]
    if not hits:
        print("    !! %r in none of %d frames" % (needle, len(frames)))
        return None
    return hits[-1] if last else hits[0]


def blank_disk(path, mb=64):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)


def dismiss(q):
    """Leave whatever the last command put on screen.

    Only the space bar is used to page: every other key is a command name at
    a shell prompt, and `q` in particular quits the shell to the desktop.  A
    run of spaces is a blank line there, which Enter discards, so the next
    command starts from a clean prompt whether or not a pager was waiting."""
    for _ in range(18):
        q.key("spc")
        time.sleep(0.09)
    q.key("esc")
    time.sleep(0.25)
    q.key("ret")          # discard the blank line the spaces left
    time.sleep(0.35)


def run(q, line, settle=1.6):
    """Type a command on a clean screen, and check it actually ran.

    A command whose output paged leaves a prompt waiting for a key, which eats
    whatever is typed next; the check catches that and tries once more rather
    than photographing the previous command's output under this one's name."""
    want = line.split()[0]
    for attempt in (0, 1):
        dismiss(q)
        q.line("clear"); time.sleep(0.7)
        q.line(line);    time.sleep(settle)
        s = q.screen()
        if want in s and "-- more --" not in s:
            return
        if attempt == 0:
            print("    .. retrying %r" % line)
    return


# ── the install media ────────────────────────────────────────────────
def run_installer():
    print("[1/3] the install ISO")
    blank_disk(BLANK)
    kill_stale()
    q = Qemu(ISO, disk=BLANK, usbstick=STICK).start()
    try:
        time.sleep(2.0)
        shot(q, "grub-menu", "The boot menu, themed and versioned from one header")
        q.key("ret")
        # The splash shows for about half a second and the boot log scrolls
        # past right behind it, which no decode-and-poll loop can keep up
        # with.  One burst covers both and the frames are chosen afterwards.
        d, frames = burst(q, frames=70, gap=0.2)
        cache = {}
        f = pick(frames, cache, "Beacon")
        if f:
            save_frame(f, "splash",
                       "The boot splash: name, version, channel and codename")
        f = pick(frames, cache, "NET: eth0", last=True) \
            or pick(frames, cache, "UHCI", last=True)
        if f:
            save_frame(f, "boot-log",
                       "Verbose boot - every driver reports, with real timestamps")
        shutil.rmtree(d, ignore_errors=True)
        waitfor(q, "Terms of Service", timeout=120)
        shot(q, "installer-terms", "Terms of service, gated on a read-progress counter")
        q.key("end"); time.sleep(0.8); q.key("a"); time.sleep(1.2)
        shot(q, "installer-privacy", "Privacy policy, step 2 of 7")
        q.key("end"); time.sleep(0.8); q.key("a"); time.sleep(1.4)
        shot(q, "installer-disk", "Disk selection, with sizes and a warning")
        q.key("ret"); time.sleep(1.4)
        shot(q, "installer-layout", "Exactly what will be written, before anything is")
        q.line("yes"); time.sleep(1.4)
        shot(q, "installer-progress", "Partitioning, formatting, writing GRUB and the kernel")
        waitfor(q, "Rebooting", timeout=120, poll=0.2)
        shot(q, "installer-done", "Installed, and restarting into the real system",
             settle=0.05)
    finally:
        q.stop()
    shutil.copy(BLANK, DISK)


def boot_disk():
    kill_stale()
    return Qemu(None, disk=DISK, usbstick=STICK, boot="c").start()


# ── first boot of the installed system ───────────────────────────────
def first_run(q):
    print("[2/3] first boot: the setup wizard")
    waitfor(q, "Terms of Service", timeout=150)
    q.key("end"); time.sleep(0.8); q.key("a"); time.sleep(1.2)
    q.key("end"); time.sleep(0.8); q.key("a"); time.sleep(1.4)
    shot(q, "setup-welcome", "First boot runs a setup wizard, once")
    q.key("ret"); time.sleep(1.3)
    shot(q, "setup-account", "Creating the first account")
    q.line("kyan"); time.sleep(1.0)
    q.key("ret");   time.sleep(0.9)
    shot(q, "setup-admin", "Administrator or standard user")
    q.key("ret");   time.sleep(1.4)
    shot(q, "setup-theme", "Theme picker, with a live preview of the desktop")
    q.key("ret");   time.sleep(1.6)
    shot(q, "setup-packages", "Optional packages, installed when setup finishes")
    q.key("ret");   time.sleep(2.4)
    shot(q, "setup-ready", "Setup finished")
    q.key("ret");   time.sleep(2.5)
    shot(q, "login-welcome", "The login manager: a new account, or a built-in one")
    q.key("down");  time.sleep(0.7)
    q.key("ret");   time.sleep(1.5)
    shot(q, "login-users", "Choosing a user, each with its own password")
    q.key("ret");   time.sleep(1.2)
    q.key("ret");   time.sleep(2.5)
    # Two idle minutes starts the screensaver, which a capture run reaches
    # easily; it gets a screen of its own later, on purpose.
    q.line("screensaver off"); time.sleep(1.0)


# (name, command, caption)
SHELL_SHOTS = [
    ("neofetch",  "neofetch",             "The system at a glance",),
    ("help",      "help",                 "The built-in command list",),
    ("sysinfo",   "sysinfo",              "What the kernel knows about this machine",),
    ("lspci",     "lspci -v",             "Every PCI device, decoded by class",),
    ("usb",       "usb -vv",              "USB host controllers, ports and devices",),
    ("dmesg",     "dmesg",                "The kernel log, replayed with timestamps",),
    ("disk",      "disk",                 "Disks, partitions and the filesystem",),
    ("ls",        "ls -l /",              "The filesystem root",),
    ("tree",      "tree /",               "The same, as a tree",),
    ("mem",       "mem",                  "Physical memory and the heap",),
    ("ps",        "ps",                   "Processes",),
    ("ifconfig",  "ifconfig",             "The network interface and its address",),
    ("netstat",   "netstat -s",           "Per-protocol network counters",),
    ("ping",      "ping 10.0.2.2",        "ICMP echo, over the real TCP/IP stack",),
    ("nslookup",  "nslookup example.com", "DNS resolution",),
    ("wget",      "wget example.com",     "Fetching a page over HTTP",),
    ("x86",       "x86",                  "The CPU, as the kernel detects it",),
    ("selftest",  "selftest",             "The built-in self test",),
    ("calc",      "calc 2 + 2 * 21",      "The expression calculator",),
    ("users",     "users",                "Accounts on this system",),
    ("df",        "df",                   "Free space",),
    ("env",       "env",                  "Shell environment and variables",),
    ("fortune",   "fortune",              "fortune, one of the optional packages",),
    ("cowsay",    "cowsay HavenDOS 0.8",  "cowsay",),
    ("banner",    "banner HAVEN",         "Large-letter banner text",),
    ("ascii-art", "ascii-art BEACON",     "The ASCII art renderer",),
    ("calendar",  "calendar",             "A month, with today marked",),
    ("man",       "man ls",               "The built-in manual",),
    ("bpkg",      "bpkg list",            "The package registry",),
    ("ver",       "ver",                  "Version, channel and codename",),
]


def shell_tour(q):
    print("[3/3] the shell")
    for name, cmd, caption in SHELL_SHOTS:
        run(q, cmd)
        shot(q, "cmd-" + name, caption)
    dismiss(q)


# full-screen programs: (name, command, caption, keys to leave)
FULLSCREEN = [
    ("dashboard",  "dashboard",      "The system dashboard",             ["q", "esc"]),
    ("edit",       "edit notes.txt", "The text editor",                  ["q", "esc"]),
    ("paint",      "paint",          "The ASCII drawing canvas",         ["q", "esc"]),
    ("snake",      "snake",          "Snake",                            ["q", "esc"]),
    ("quiz",       "quiz",           "The trivia quiz",                  ["esc", "q"]),
    ("hacker",     "hacker",         "Hacker mode",                      ["esc", "q"]),
    ("music",      "music",          "The PC-speaker music player",      ["q", "esc"]),
    ("matrix",     "matrix",         "The matrix screensaver",           ["esc", "q"]),
    ("stars",      "stars",          "The starfield screensaver",        ["esc", "q"]),
    ("pipes",      "pipes",          "The pipes screensaver",            ["esc", "q"]),
]


def fullscreen_tour(q):
    print("     full-screen programs")
    for name, cmd, caption, leave in FULLSCREEN:
        dismiss(q)
        q.line("clear"); time.sleep(0.6)
        q.line(cmd); time.sleep(2.2)
        shot(q, "app-" + name, caption)
        for k in leave:
            q.key(k)
            time.sleep(0.5)
        for _ in range(5):
            if "havendos:" in q.screen():
                break
            q.key("q"); time.sleep(0.3); q.key("esc"); time.sleep(0.3)
        time.sleep(0.4)


MENU_CATS = ["Apps", "System", "Fun", "Settings"]


# The desktop menu is two panels side by side: the categories in columns
# 0..21 and, when one is open, its items from column 22.  Both draw '>' beside
# whatever is selected, on the same screen row, so the marker has to be read by
# column - the first '>' on a line is the category one whether or not the
# category is the selected one.
MENU_W, SUB_W = 22, 26


def _marked(q, col, width, screen=None):
    """The item marked with '>' in the panel starting at `col`, or None.

    The last two columns of each panel are not the name: a category that has a
    submenu draws '>' there, and a submenu that scrolls draws '^' or 'v'.
    Reading them as part of the name is what made "HavenCode" unfindable the
    moment the list was long enough to scroll."""
    for line in (screen if screen is not None else q.screen()).split("\n"):
        if len(line) > col and line[col] == ">":
            t = line[col + 1:col + width - 2].strip()
            while t[-1:] in (">", "v", "^"):
                t = t[:-1].strip()
            return t
    return None


def _selected(q, names):
    cur = _marked(q, 0, MENU_W)
    return cur if cur in names else None


def _sub_selected(q):
    return _marked(q, MENU_W, SUB_W)


def menu_open(q, category, item, tries=3):
    """Open `item` under `category` from the desktop menu.

    The menu draws '>' beside whatever is selected, so the position is read
    off the screen and stepped to, rather than counted in keypresses from an
    assumed starting state - which is what quietly opened the wrong program
    the first time this ran."""
    for _ in range(tries):
        for _ in range(4):                       # back to a bare desktop
            scr = q.screen()
            if "ESC=Close" not in scr and _marked(q, 0, MENU_W, scr) is None:
                break
            q.key("esc"); time.sleep(0.6)
        q.key("esc"); time.sleep(1.1)            # open the menu
        cur = _marked(q, 0, MENU_W)
        if cur not in MENU_CATS:
            continue
        want = MENU_CATS.index(category)
        for _ in range(len(MENU_CATS)):
            if cur == category:
                break
            q.key("down" if MENU_CATS.index(cur) < want else "up")
            time.sleep(0.4)
            cur = _marked(q, 0, MENU_W) or cur
        if cur != category:
            continue
        q.key("ret"); time.sleep(1.1)            # into the submenu
        # A long submenu scrolls, so the walk is bounded by how many times the
        # selection stops moving rather than by how many names have been seen:
        # a repeated read mid-redraw is normal and is not the end of the list.
        last, stuck = None, 0
        for _ in range(20):
            here = _sub_selected(q)
            if here == item:
                q.key("ret"); time.sleep(2.4)
                return True
            stuck = stuck + 1 if here == last else 0
            if stuck >= 3:                       # bottom of the list
                break
            last = here
            q.key("down"); time.sleep(0.4)
    print("    !! could not open %s / %s" % (category, item))
    return False


def leave_app(q):
    for _ in range(6):
        if "ESC=Close" not in q.screen():
            return
        q.key("q"); time.sleep(0.4)
        q.key("esc"); time.sleep(0.5)


# ── the desktop ──────────────────────────────────────────────────────
DESKTOP_APPS = [
    ("Apps",   "Todo List",      "todo",         "The to-do list"),
    ("Apps",   "HavenCode",      "havencode",    "HavenCode, the H language environment"),
    ("Apps",   "Hex Viewer",     "hexviewer",    "The hex viewer"),
    ("System", "File Manager",   "file-manager", "The file manager"),
    ("System", "System Monitor", "sysmon",       "The system monitor"),
    ("System", "Network Info",   "netinfo",      "Network information"),
    ("System", "User Manager",   "usermgr",      "The user manager"),
    ("Fun",    "Tetris",         "tetris",       "Tetris"),
    ("Fun",    "Pong",           "pong",         "Pong"),
    ("Settings", "Settings",     "settings",     "Settings"),
]


def desktop_tour(q):
    print("     the desktop")
    dismiss(q)
    q.line("clear"); time.sleep(0.6)
    q.line("exit"); time.sleep(2.8)
    shot(q, "desktop", "The desktop: an icon grid, a status bar and a clock")
    q.key("esc"); time.sleep(1.4)
    shot(q, "desktop-menu", "The start menu, with its four categories")
    q.key("down"); time.sleep(0.5); q.key("ret"); time.sleep(1.3)
    shot(q, "desktop-system", "The System submenu")
    q.key("esc"); time.sleep(0.7); q.key("esc"); time.sleep(1.2)
    if menu_open(q, "Apps", "Calculator"):
        shot(q, "app-calculator", "The desktop calculator")
    leave_app(q)
    for cat, item, name, caption in DESKTOP_APPS:
        if menu_open(q, cat, item):
            shot(q, "app-" + name, caption)
        leave_app(q)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    shell_only = "--shell-only" in sys.argv
    if shell_only:
        # The installer and wizard screens are already captured; pick up the
        # numbering where they left off and photograph the system itself.
        import re as _re
        have = sorted(f for f in os.listdir(OUT) if f.endswith(".png"))
        keep = [f for f in have if int(f.split("-", 1)[0]) <= 17]
        for f in have:
            if f not in keep:
                os.unlink(os.path.join(OUT, f))
        _n[0] = 17
        q = boot_disk()
        try:
            waitfor(q, "Welcome to HavenDOS", timeout=150)
            q.key("down"); time.sleep(0.7)
            q.key("ret");  time.sleep(1.5)
            q.key("ret");  time.sleep(1.2)
            q.key("ret");  time.sleep(2.5)
            q.line("screensaver off"); time.sleep(1.0)
            shell_tour(q)
            fullscreen_tour(q)
            desktop_tour(q)
        finally:
            q.stop()
    else:
        if "--disk-only" not in sys.argv:
            run_installer()
        q = boot_disk()
        try:
            first_run(q)
            shell_tour(q)
            fullscreen_tour(q)
            desktop_tour(q)
        finally:
            q.stop()
    mode = "a" if shell_only else "w"
    with open(os.path.join(OUT, "index.txt"), mode) as f:
        if not shell_only:
            f.write('HavenDOS v0.8.0 Beta "Beacon" - screenshots\n')
            f.write("=" * 60 + "\n\n")
        for fn, cap in _index:
            f.write("%-36s %s\n" % (fn, cap))
    print("\n%d screens -> %s" % (_n[0], OUT))
