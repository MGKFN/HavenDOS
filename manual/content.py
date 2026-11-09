# -*- coding: utf-8 -*-
"""The text of the HavenDOS operator's manual.  Prose and structure only."""

SHOTS_DIR = "/home/claude/havendos/screenshots"

META = dict(
    ver="0.8.0", channel="Beta", codename="Beacon",
    part="THS-HD-0800-OM-A", date="October 2026",
    crt=("+--------------------------+\n"
         "| 32-bit x86  .  real mode |\n"
         "| to protected mode in one |\n"
         "| jump, and everything     |\n"
         "| after it written here.   |\n"
         "+--------------------------+"),
)

SECTIONS = [

# ═══════════════════════════════════════════════════════════════════ 1
dict(title="About HavenDOS", body=[
 ("p", "HavenDOS is a 32-bit operating system for x86 machines, written from "
       "nothing by TechHaven Studios. It is not a Linux distribution, not a "
       "DOS clone and not built on anyone else&rsquo;s kernel. The bootloader "
       "hands control to code in this source tree and everything after that "
       "&mdash; the memory manager, the drivers, the filesystems, the TCP/IP "
       "stack, the shell, the desktop &mdash; is in it."),
 ("p", "It looks like 1988 on purpose and runs like it too: the whole system "
       "is a character grid, 80 columns by 25 rows, and every program in it "
       "draws with characters. What changed in this release is that the grid "
       "is no longer made of VGA text memory. It is drawn pixel by pixel into "
       "a framebuffer at 1280&times;800, which is why the letters are sharp "
       "on a modern screen and why the system boots on UEFI machines that "
       "have no text mode at all."),
 ("fig", ("cmd-neofetch.png", "The system describing itself.")),
 ("h3", "What this manual covers"),
 ("p", "This is the operator&rsquo;s manual for HavenDOS 0.8 Beta, codename "
       "&ldquo;Beacon&rdquo;. It covers installing the system, what happens "
       "when it boots, every screen the setup wizard shows, the desktop, the "
       "shell and its commands, the hardware it drives, and &mdash; in "
       "section 14 &mdash; what it cannot do yet. A beta manual that only "
       "listed the good parts would waste your afternoon."),
 ("h3", "The shape of the system"),
 ("table", ["Area", "Lines", "What is in it"], [
   ["kernel",  "1,950",  "boot, GDT/IDT, the PIT, physical memory, the heap, the kernel log, config"],
   ["drivers", "4,520",  "VGA and the framebuffer console, keyboard, mouse, PCI, UHCI (USB), ATA, ATAPI, VirtIO, RTL8139, e1000, RTC, speaker"],
   ["fs",      "3,470",  "FAT12, FAT16, ISO 9660, a RAM filesystem, the VFS over them, the installer, the updater"],
   ["net",     "4,150",  "Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, HTTP, TLS"],
   ["shell",   "17,120", "the shell and its commands, the desktop, the apps, the games, setup, packages"],
   ["tests",   "3,470",  "437 checks that run against the real driver code on the build machine"],
 ]),
 ("note", "Line counts are approximate and are there to show the balance "
          "between the parts, not to be precise."),
]),

# ═══════════════════════════════════════════════════════════════════ 2
dict(title="What you need", body=[
 ("h3", "A machine"),
 ("p", "HavenDOS runs on a 32-bit or 64-bit x86 machine, real or virtual. It "
       "wants very little:"),
 ("table", ["Requirement", "Minimum", "Notes"], [
   ["CPU",     "any 32-bit x86",   "it runs in protected mode and uses no instruction newer than a 486"],
   ["Memory",  "16 MB",            "256 MB is what the test machines are given"],
   ["Disk",    "15 MB",            "the installer refuses anything smaller"],
   ["Display", "any",              "BIOS VGA text or a GRUB framebuffer; UEFI needs the framebuffer, which it gets"],
   ["Firmware","BIOS or UEFI",     "the ISO carries a boot image for both"],
 ]),
 ("h3", "A disk controller it can talk to"),
 ("p", "This is the part that decides whether an install will work, so it is "
       "worth reading before you burn anything. HavenDOS drives:"),
 ("ul", ["<b>Legacy IDE / ATA</b> at the standard ports 0x1F0 and 0x170. "
         "This is what a PIIX3 chipset gives you &mdash; in QEMU, the "
         "<code>i440fx</code> machine type; in UTM, an emulated PC.",
         "<b>VirtIO block</b>, which is what UTM SE and most paravirtual "
         "setups offer.",
         "<b>ATAPI CD-ROM</b> on the same legacy ports, which is how the "
         "installer and the updater read the ISO."]),
 ("caution", "HavenDOS does <b>not</b> drive AHCI or NVMe. On a Q35 or ICH9 "
             "machine the CD-ROM is behind an AHCI controller, so the system "
             "will boot and install but <code>update</code> will not find the "
             "ISO. Use an i440FX machine type if you want in-place updates."),
 ("h3", "Known-good configurations"),
 ("table", ["Host", "Settings"], [
   ["QEMU",   "<code>-M pc</code> (i440FX), 256 MB, IDE disk, IDE CD-ROM, <code>-usb</code>"],
   ["UTM SE", "x86_64, emulated PC, 512 MB, VirtIO or IDE disk, CD/DVD on IDE"],
   ["Real PC","BIOS or UEFI boot, SATA set to <b>Legacy IDE</b> in firmware setup"],
 ]),
]),

# ═══════════════════════════════════════════════════════════════════ 3
dict(title="Installing", body=[
 ("p", "Write <code>HavenDOS-0.8.0.iso</code> to a USB stick or attach it as "
       "a CD-ROM, and boot from it. The ISO is hybrid: the same image boots "
       "on a BIOS machine and on a UEFI machine without any switch."),
 ("fig", ("grub-menu.png",
          "The boot menu. Four entries, all from the same kernel; the version "
          "in the footer is generated at build time rather than painted into "
          "the background image.")),
 ("h3", "The installer"),
 ("p", "The install ISO boots straight into the installer. There is no live "
       "desktop on the install media &mdash; the ISO exists to put the system "
       "on a disk. It runs in seven steps."),
 ("figrow", [("installer-terms.png",
              "Step 1. The terms, which you must scroll to the end of before "
              "the accept key does anything."),
             ("installer-privacy.png", "Step 2. The privacy policy.")]),
 ("figrow", [("installer-disk.png",
              "Disk selection, with the size of each candidate. Read the "
              "sizes: the stick you booted from is a disk too."),
             ("installer-layout.png",
              "Exactly what will be written, shown before anything is.")]),
 ("caution", "The selected disk is wiped completely. The installer asks you "
             "to type <code>YES</code> rather than press a key, because a key "
             "is too easy to press by accident."),
 ("figrow", [("installer-progress.png",
              "Partitioning, formatting FAT16, writing GRUB to the MBR and "
              "the kernel into /boot."),
             ("installer-done.png", "Done, and restarting.")]),
 ("h3", "The live shell"),
 ("p", "The boot menu's <b>Shell Mode</b> entry gives a shell with nothing "
       "installed. It is the quickest way to find out whether HavenDOS can "
       "see a machine before committing a disk to it: <code>lspci -v</code>, "
       "<code>usb -vv</code>, <code>dmesg</code> and <code>ifconfig</code> all "
       "work there. <code>install</code> from that shell runs the same "
       "installer with a disk list you choose from."),
 ("fig", ("live-shell.png",
          "The ISO's live shell. The boot log is still on screen above the "
          "prompt, the way a Linux console leaves it.")),
 ("figrow", [("installer-disks.png",
              "The installer's disk list: bus and size for each candidate."),
             ("installer-confirm.png",
              "What will be written, and the confirmation it asks for.")]),
 ("h3", "What ends up on the disk"),
 ("crt", """
  Sector 0          MBR + GRUB boot code
  Sectors 1-2047    GRUB core image
  Sector 2048+      FAT16 partition, type 0x0E
                      /boot/grub/grub.cfg
                      /boot/havendos.elf
                      /havendos.txt        version marker
"""),
 ("p", "The partition is an ordinary FAT16 volume. Any other operating system "
       "can read it, which matters when something goes wrong and you want to "
       "look at the kernel file yourself."),
]),

# ═══════════════════════════════════════════════════════════════════ 4
dict(title="Starting up", body=[
 ("p", "Three things happen between power and a prompt, and all three are "
       "visible on purpose."),
 ("h3", "The boot menu"),
 ("p", "GRUB offers the desktop, shell mode, safe mode (no mouse) and a "
       "reinstall entry. It times out into the desktop after ten seconds."),
 ("h3", "The splash"),
 ("p", "A short splash with the name, version, channel and codename. It "
       "holds for about half a second and any key cuts it short. It makes no "
       "claim about your hardware, which is a change: earlier releases showed "
       "a fake power-on self test announcing a &ldquo;TechHaven BIOS&rdquo; "
       "that does not exist, after the real firmware had already done the "
       "real POST."),
 ("fig", ("splash.png", "The splash. Name, version, channel, codename; nothing "
          "it cannot back up.")),
 ("h3", "The boot log"),
 ("p", "Then each subsystem reports as it initialises, with a real timestamp "
       "in milliseconds taken from the programmable interval timer. Every "
       "line is written from inside the code that knows the number, so the "
       "log describes the machine in front of you rather than a machine "
       "someone imagined while writing string literals."),
 ("fig", ("boot-log.png",
          "The verbose boot log. Each line is printed by the subsystem it "
          "describes, at the moment it happens.")),
 ("crt", """
[   0.700]  ok  GDT: 3 descriptors at 0x229000, flat 32-bit segments
[   0.704]  ok  Heap: 255 KB static pool
[   0.644]  ok  IDE: primary 0x01F0, secondary 0x0170
[   0.649]  ok  UHCI hc0 at 0xC040, IRQ 11, 2 ports
[   0.763]  ok  USB hc0 port 0: 46F4:0001 full-speed, address 1
[   0.767] warn ATA: nothing on 0x1F0/0x170 (no legacy IDE here)
[   3.184]  ok  NET: eth0 10.0.2.15
"""),
 ("p", "Echo is on from the very first line, so a kernel that dies during "
       "initialisation has already said how far it got. Add <code>quiet</code> "
       "to the kernel command line to silence it, and run <code>dmesg</code> "
       "afterwards to replay it."),
 ("fig", ("cmd-dmesg.png", "dmesg replays the same log, a page at a time.")),
 ("note", "The log is the first place to look when hardware is not found. "
          "A line like <code>ATA: nothing on 0x1F0/0x170</code> tells you "
          "immediately that the machine has no legacy IDE, which is why a "
          "later <code>update</code> would fail."),
 ("h3", "Boot parameters"),
 ("table", ["Parameter", "Effect"], [
   ["mode=desktop", "start the desktop (the default)"],
   ["mode=shell",   "start the shell directly"],
   ["mode=safe",    "shell with the mouse disabled"],
   ["mode=lowres",  "force the smaller text grid"],
   ["install=1",    "run the installer"],
   ["quiet",        "suppress the boot log (dmesg still has it)"],
   ["mouse=on|off", "override mouse detection"],
   ["gfx=text|mode13|vesa640|vesa800", "the graphics mode apps use"],
 ]),
]),

# ═══════════════════════════════════════════════════════════════════ 5
dict(title="First run", body=[
 ("p", "The first time the installed system boots it runs a setup wizard. It "
       "runs once."),
 ("figrow", [("setup-welcome.png", "What setup is about to do."),
             ("setup-account.png", "Your account. Three characters or more; "
              "a blank password is allowed and means no password.")]),
 ("figrow", [("setup-theme.png",
              "Twenty themes, with a live preview of the desktop in each."),
             ("setup-packages.png",
              "Optional packages. Four are selected by default.")]),
 ("fig", ("setup-ready.png", "Setup finished.")),
 ("h3", "Signing in"),
 ("p", "After setup the login manager appears. It offers a new account or one "
       "of the three built-in ones &mdash; <code>admin</code>, "
       "<code>bootos</code> and <code>guest</code> &mdash; which exist so a "
       "system you have locked yourself out of is still usable."),
 ("figrow", [("login-welcome.png", "New account, or a built-in one."),
             ("login-users.png", "Choosing a user.")]),
 ("caution", "The built-in accounts have no password until you give them one. "
             "On any machine other people can reach, set one with "
             "<code>passwd admin</code> as your first action."),
]),

# ═══════════════════════════════════════════════════════════════════ 6
dict(title="The desktop", body=[
 ("p", "The desktop is an icon grid with a status bar along the bottom and a "
       "clock. Arrow keys move the selection, Enter opens, Tab goes straight "
       "to the shell and Escape opens the menu. A mouse works if one is "
       "attached."),
 ("fig", ("desktop.png",
          "The desktop. Memory and uptime on the left of the status bar, the "
          "user and notifications on the right.")),
 ("figrow", [("desktop-menu.png",
              "The menu: Apps, System, Fun and Settings, plus Lock."),
             ("desktop-system.png", "A submenu flies out to the right.")]),
 ("h3", "What is on it"),
 ("table", ["Category", "Programs"], [
   ["Apps",     "Shell, Text Editor, Calculator, Todo List, HavenCode, Hex Viewer, File Manager"],
   ["System",   "System Info, Dashboard, System Monitor, Clock, Disk Info, Network Info, User Manager, Log Viewer"],
   ["Fun",      "Snake, Tetris, Pong, Matrix, Pipes"],
   ["Settings", "Screensaver, Settings, Power / Reboot"],
 ]),
 ("figrow", [("app-calculator.png", "The calculator."),
             ("app-usermgr.png", "The user manager.")]),
 ("figrow", [("app-netinfo.png", "Network information."),
             ("app-settings.png", "Settings.")]),
 ("note", "A program launched from the desktop clears the screen, runs, and "
          "clears it again on the way out. In releases before 0.8 the desktop "
          "stayed visible underneath, showing through wherever the program "
          "did not draw."),
]),

# ═══════════════════════════════════════════════════════════════════ 7
dict(title="The shell", body=[
 ("p", "The shell is where most of the system is. It has tab completion, "
       "command history on the arrow keys, reverse search on Ctrl-R, line "
       "editing with Home and End, environment variables, aliases, "
       "redirection, pipes and scripts."),
 ("fig", ("cmd-help.png", "help lists every command, grouped by what it does.")),
 ("h3", "Pipes and redirection"),
 ("p", "A full stop is the pipe character, because the vertical bar is "
       "awkward on several of the keyboard layouts HavenDOS is used with."),
 ("crt", """
kyan@havendos:~# ls . sort > listing.txt
kyan@havendos:~# cat listing.txt . wc
kyan@havendos:~# echo $USER
kyan@havendos:~# set GREETING hello
kyan@havendos:~# echo $GREETING
"""),
 ("h3", "Scripts"),
 ("p", "<code>run script.bsh</code> executes a file of shell commands. "
       "<code>if</code>, <code>else</code>, <code>endif</code>, "
       "<code>while</code>, <code>endwhile</code> and <code>repeat</code> are "
       "available, which is enough for the kind of small job a script is "
       "actually used for."),
 ("h3", "Getting help"),
 ("note", "If a command is not on the help screen, try it anyway and try "
          "<code>man</code> on it. Tab completion knows every command the "
          "shell can run, which is the authoritative list."),
 ("p", "<code>help</code> is the index; <code>man &lt;command&gt;</code> is "
       "the detail; <code>guide</code> is a built-in tour for someone who has "
       "just arrived."),
 ("fig", ("cmd-man.png", "A manual page.")),
]),

# ═══════════════════════════════════════════════════════════════════ 8
dict(title="Command reference", body=[
 ("h3", "Files and filesystem"),
 ("table", ["Command", "What it does"], [
   ["ls, dir",   "list files; cyan is a directory, green a file, red locked"],
   ["tree",      "the filesystem as a tree"],
   ["cat",       "show a file"],
   ["head, tail","the first or last lines of a file"],
   ["write",     "write text to a file"],
   ["touch",     "create an empty file"],
   ["mkdir",     "create a directory"],
   ["rm, del",   "delete a file"],
   ["mv, rename","rename or move"],
   ["cp",        "copy"],
   ["find",      "find files, with <code>*</code> globbing"],
   ["grep",      "search inside a file"],
   ["wc",        "count lines, words and characters"],
   ["sort, uniq","sort lines; sort and deduplicate"],
   ["diff",      "compare two files"],
   ["stat",      "size, type and lock state"],
   ["hex",       "hexdump, or <code>hex -s</code> for strings"],
   ["zip, unzip","archive and extract"],
   ["edit",      "the text editor"],
 ]),
 ("fig", ("cmd-ls.png", "A directory listing.")),
 ("h3", "Shell and scripting"),
 ("table", ["Command", "What it does"], [
   ["echo",     "print text, expanding <code>$VAR</code>"],
   ["set, unset, export, env", "environment variables"],
   ["alias, unalias", "command aliases"],
   ["history",  "what you have typed"],
   ["ps1",      "show or set the prompt format"],
   ["run",      "run a .BSH script"],
   ["which",    "does this command exist"],
   ["calc",     "an expression calculator"],
   ["repeat, seq", "run something N times; a number sequence"],
   ["clear, cls", "clear the screen"],
 ]),
 ("h3", "System"),
 ("table", ["Command", "What it does"], [
   ["sysinfo",   "everything the kernel knows about the machine"],
   ["neofetch",  "the same, short, with the logo"],
   ["mem",       "physical memory and the heap"],
   ["uptime",    "how long the machine has been up"],
   ["disk",      "disks, partitions and the filesystem"],
   ["df",        "free space"],
   ["dmesg",     "replay the kernel log"],
   ["ps",        "the process list"],
   ["x86",       "the CPU as detected"],
   ["lspci",     "every PCI device, decoded; <code>-v</code> for detail"],
   ["usb",       "USB controllers and devices; <code>-v</code>, <code>-vv</code>"],
   ["dashboard, htop", "a live monitor"],
   ["selftest",  "the built-in self test"],
   ["ver, version", "version, channel and codename"],
   ["date, time",   "the real-time clock"],
   ["reboot, shutdown", "restart or power off"],
 ]),
 ("figrow", [("cmd-lspci.png", "The PCI bus, decoded by class."),
             ("cmd-usb.png", "USB controllers, ports and what is attached.")]),
 ("h3", "Users and security"),
 ("table", ["Command", "What it does"], [
   ["whoami",  "who you are"],
   ["users",   "every account"],
   ["passwd",  "change a password"],
   ["adduser, deluser", "add or remove an account (admin only)"],
   ["chmod",   "lock a file (admin only)"],
   ["chown",   "change a file&rsquo;s owner (admin only)"],
   ["log",     "the audit log (admin only)"],
   ["lock",    "lock the screen"],
 ]),
 ("h3", "Network"),
 ("table", ["Command", "What it does"], [
   ["ifconfig", "show or set the interface configuration"],
   ["dhcp",     "ask for an address"],
   ["ping",     "ICMP echo"],
   ["netstat",  "connections; <code>-s</code> for per-protocol counters"],
   ["nslookup, dns, host", "resolve a name"],
   ["wget, curl", "fetch a URL over HTTP"],
   ["udpsend, tcpsend, tcptest", "send datagrams, or open a connection by hand"],
   ["tls, tlsinfo", "the TLS layer and what it negotiated"],
   ["chat",     "the chat client"],
 ]),
 ("h3", "Fun and extras"),
 ("table", ["Command", "What it does"], [
   ["calendar",  "this month, with today marked"],
   ["todo",      "the to-do list"],
   ["bpkg",      "list and install the optional packages"],
   ["h, h-lang", "HavenCode, the TechHaven H language"],
   ["snake, tetris, pong", "games"],
   ["matrix, pipes, stars", "screensavers you can run on demand"],
   ["fortune",  "a quote"],
   ["banner, ascii-art", "large letters"],
   ["cowsay",   "an important system utility"],
   ["quiz, hacker", "a trivia quiz; a fake intrusion"],
   ["music, play, beep", "the PC speaker"],
   ["paint",    "an ASCII canvas"],
 ]),
]),

# ═══════════════════════════════════════════════════════════════════ 9
dict(title="Storage and files", body=[
 ("p", "HavenDOS has a VFS with three filesystems under it."),
 ("table", ["Filesystem", "Used for", "Access"], [
   ["FAT16",    "the installed system partition", "read and write"],
   ["FAT12",    "floppy images",                  "read and write"],
   ["ISO 9660", "the install and update media",   "read"],
   ["RAM",      "scratch files before an install","read and write"],
 ]),
 ("p", "The installed partition is plain FAT16 with type 0x0E, which means "
       "any other system can mount it. Files carry an owner and a lock flag; "
       "<code>chmod</code> and <code>chown</code> set them and the listing "
       "shows a locked file in red."),
 ("fig", ("cmd-disk.png", "What the disk layer sees.")),
 ("h3", "The editor"),
 ("p", "<code>edit</code> is a full-screen editor with the usual movement "
       "keys, insert and overwrite, and page up and down."),
 ("fig", ("app-edit.png", "The editor.")),
]),

# ═══════════════════════════════════════════════════════════════════ 10
dict(title="Networking", body=[
 ("p", "The network stack is written from the frame up: an Ethernet driver, "
       "then ARP, IPv4, ICMP, UDP and TCP on top of it, then DHCP, DNS, HTTP "
       "and TLS on top of those. Two NICs are driven &mdash; the Realtek "
       "RTL8139 and the Intel e1000 &mdash; which between them cover almost "
       "every virtual machine."),
 ("figrow", [("cmd-ifconfig.png", "The interface and its address."),
             ("cmd-netstat.png", "Per-protocol counters, which are the "
              "quickest way to see where packets are being lost.")]),
 ("p", "An address is obtained by DHCP at boot. <code>ping</code> goes out "
       "over the real stack, not an emulation of one, and "
       "<code>nslookup</code> resolves through a real DNS query."),
 ("figrow", [("cmd-ping.png", "ICMP echo."),
             ("cmd-nslookup.png", "A DNS lookup.")]),
 ("fig", ("cmd-wget.png", "Fetching a page over HTTP.")),
 ("note", "TLS is present and negotiates, but treat it as experimental. "
          "<code>tlsinfo</code> reports what was agreed."),
]),

# ═══════════════════════════════════════════════════════════════════ 11
dict(title="Hardware", body=[
 ("h3", "The PCI bus"),
 ("p", "The bus is walked at boot and every device recorded with its class, "
       "vendor and device ID. Multifunction devices are enumerated properly "
       "&mdash; which sounds like housekeeping and is not. Before 0.7.20 the "
       "scan looked at function 0 only, and on a PIIX3 chipset the USB "
       "controller is function 2 of device 1. It was not undetected; it was "
       "undetectable."),
 ("h3", "USB"),
 ("p", "HavenDOS drives UHCI, which is USB 1.1. It resets a port, assigns an "
       "address and reads a device&rsquo;s descriptor, and it has working "
       "control and bulk transfer primitives. Every controller on the machine "
       "is driven, not just the first: a PIIX3 has one, an ICH has three to "
       "five, and a device in any of the others used to be invisible with no "
       "error anywhere."),
 ("p", "There is one piece of this worth understanding, because it is why USB "
       "appears to do nothing on a modern chipset. A USB 2.0 controller is an "
       "EHCI with UHCI companions sharing the same physical sockets, and "
       "which one gets a device is decided by speed: a 2.0 stick attaches to "
       "the EHCI at 480 Mbit and the companion sees an empty port. HavenDOS "
       "asks the EHCI for the ports back &mdash; the specification provides "
       "for exactly this, and it is the state the hardware resets into "
       "&mdash; and the device re-attaches at full speed to a controller this "
       "kernel can drive."),
 ("fig", ("cmd-usb.png", "usb -vv: controllers, ports, and what is on them.")),
 ("caution", "A USB stick is <b>seen</b>, not <b>read</b>. Mass storage needs "
             "Bulk-Only Transport and a small SCSI layer on top of the "
             "transfer primitives that now exist. That is the next piece of "
             "work, not a feature of this release."),
 ("h3", "Everything else"),
 ("table", ["Device", "Driver"], [
   ["Display",   "VGA text, and a framebuffer console at whatever mode GRUB provides"],
   ["Keyboard",  "PS/2, with the full scancode set"],
   ["Mouse",     "PS/2, used by the desktop"],
   ["Disk",      "ATA/IDE (PIO LBA28) and VirtIO block"],
   ["CD-ROM",    "ATAPI on the legacy ports"],
   ["Network",   "RTL8139 and Intel e1000"],
   ["Clock",     "the CMOS real-time clock"],
   ["Timer",     "the 8253 PIT at 100 Hz, which is also where boot timestamps come from"],
   ["Sound",     "the PC speaker"],
 ]),
 ("fig", ("cmd-x86.png", "The CPU, as the kernel detects it.")),
]),

# ═══════════════════════════════════════════════════════════════════ 12
dict(title="The display", body=[
 ("p", "Every character HavenDOS draws goes into a shadow buffer, and exactly "
       "one piece of code turns a cell in that buffer into something visible. "
       "That is why this release could change what a cell looks like without "
       "touching a single program."),
 ("p", "On a BIOS machine with no framebuffer, a cell is written to VGA text "
       "memory at 0xB8000 and the card draws it. When GRUB provides a "
       "framebuffer &mdash; which it now does, because the kernel asks for "
       "one &mdash; a cell is drawn glyph by glyph into pixels at "
       "1280&times;800. The 8&times;8 font is scaled twice across and four "
       "times down, which gives exactly 80&times;25 and is how an 8&times;8 "
       "font looked on a 400-line display in the first place."),
 ("p", "This is also why HavenDOS boots on UEFI. A UEFI machine has no VGA "
       "text memory and no INT 10h; without the framebuffer console it comes "
       "up to a black screen, and with it, to a usable shell."),
 ("h3", "Changing the grid"),
 ("table", ["Command", "Result"], [
   ["res 80 25", "80&times;25, the default, with tall glyphs"],
   ["res 80 50", "80&times;50, uniform scaling and square pixels"],
   ["res 90 30", "90&times;30 (text mode only)"],
   ["res 90 60", "90&times;60 (text mode only)"],
 ]),
 ("h3", "Themes"),
 ("p", "Twenty themes ship with the system, from Midnight Cyan through "
       "Amber, Terminal Green and Monochrome to Volcano. <code>theme</code> "
       "lists them and <code>theme &lt;n&gt;</code> switches, immediately."),
]),

# ═══════════════════════════════════════════════════════════════════ 13
dict(title="Apps, games and packages", body=[
 ("figrow", [("app-dashboard.png", "The live system dashboard."),
             ("app-sysmon.png", "The system monitor.")]),
 ("figrow", [("app-file-manager.png", "The file manager."),
             ("app-havencode.png", "HavenCode, the H language environment.")]),
 ("figrow", [("app-tetris.png", "Tetris."),
             ("app-pong.png", "Pong.")]),
 ("figrow", [("app-snake.png", "Snake."),
             ("app-paint.png", "The ASCII canvas.")]),
 ("h3", "Packages"),
 ("p", "Ten optional packages can be installed at setup or afterwards with "
       "<code>bpkg</code>. They are part of the kernel image rather than "
       "separate binaries &mdash; the package system records which are "
       "enabled, not which are present."),
 ("table", ["Package", "What you get"], [
   ["Fortune",       "a quote generator"],
   ["ASCII Art",     "a large-letter banner renderer"],
   ["Paint",         "an ASCII canvas you can save to a file"],
   ["Music Player",  "melodies on the PC speaker"],
   ["Calendar",      "a month with today marked"],
   ["Quiz",          "a trivia quiz with scoring"],
   ["Hacker Mode",   "a retro intrusion animation"],
   ["SysInfo+",      "a deeper system information panel"],
   ["Themes Extra",  "ten more colour themes"],
   ["H Language",    "TechHaven H / H++ / H#, the HavenCode toolchain"],
 ]),
 ("fig", ("cmd-bpkg.png", "The package registry.")),
 ("figrow", [("cmd-cowsay.png", "An important system utility."),
             ("cmd-banner.png", "Large letters.")]),
 ("h3", "The screensaver"),
 ("figrow", [("app-matrix.png", "Matrix."), ("app-pipes.png", "Pipes.")]),
 ("p", "After two idle minutes a screensaver starts. "
       "<code>screensaver &lt;seconds&gt;</code> changes the delay and "
       "<code>screensaver off</code> disables it."),
]),

# ═══════════════════════════════════════════════════════════════════ 14
dict(title="Updating, and what to do when it fails", body=[
 ("p", "<code>update</code> reads a newer ISO from the CD-ROM drive, compares "
       "its version with the running one and replaces the kernel on disk. "
       "<code>rollback</code> puts the previous kernel back. The version "
       "comparison is a real one &mdash; it will not install an older kernel "
       "over a newer one by accident."),
 ("caution", "<code>update</code> needs the CD-ROM to be on the legacy IDE "
             "ports. On a Q35 or ICH9 machine it is behind AHCI, which this "
             "kernel does not drive, and the update will fail to find the "
             "ISO. The boot log says so, at boot, in the "
             "<code>ATA: nothing on 0x1F0/0x170</code> line."),
 ("caution", "<code>install</code> on a system that is already running "
             "HavenDOS will tell you it cannot proceed, and will not touch "
             "the disk. The kernel an installed system runs carries no image "
             "of itself to write &mdash; see section 15. Install from the ISO, "
             "or from its live shell."),
 ("h3", "Working around it"),
 ("ul", ["Switch the virtual machine to an i440FX machine type and attach the "
         "CD-ROM as IDE. This is the configuration HavenDOS is tested on.",
         "Or boot the new ISO and install over the top. The installer "
         "rewrites the kernel and bootloader; it does not preserve files on "
         "the partition, so copy anything you care about off first."]),
 ("h3", "Checking what you are running"),
 ("fig", ("cmd-ver.png", "Version, channel and codename.")),
 ("p", "The version lives in exactly one header and everything else is "
       "generated from it &mdash; the kernel, the boot menu titles, the "
       "version marker on the installed partition, the splash and the theme "
       "label in the GRUB footer. The build refuses to finish if any of them "
       "disagree."),
]),

# ═══════════════════════════════════════════════════════════════════ 15
dict(title="What this beta cannot do", body=[
 ("p", "This section exists because a beta that only lists its features will "
       "waste your time. These are the real limits."),
 ("h3", "A USB stick is seen, not read"),
 ("p", "Enumeration works, transfers work, and there is no mass-storage class "
       "driver on top of them yet. You cannot copy a file off a stick."),
 ("h3", "Bootable is not the same as usable on modern hardware"),
 ("p", "The ISO will boot a current UEFI laptop. On that laptop the keyboard "
       "is USB behind an xHCI controller, which this kernel does not drive, "
       "and the disk is AHCI or NVMe, which it also does not drive. It will "
       "come up and you will not be able to type into it. A virtual machine "
       "configured as described in section 2 is the supported way to run "
       "HavenDOS today."),
 ("h3", "An installed system cannot install HavenDOS"),
 ("p", "The build is two-pass: the first pass produces a kernel with no "
       "embedded image, the second embeds that kernel and is what ships on "
       "the ISO. So the kernel the installer writes to a disk &mdash; and "
       "therefore the one an installed system runs &mdash; has nothing to "
       "install. The installer checks before it starts and refuses, rather "
       "than formatting a disk and failing at the last step, which is what it "
       "used to do. Install from the ISO."),
 ("h3", "Only one ATA disk is seen"),
 ("p", "The driver probes the primary master, so a second IDE disk does not "
       "appear in the installer's list."),
 ("h3", "update needs legacy IDE"),
 ("p", "As in section 14. The diagnosis is in the boot log; the workaround is "
       "a machine type change or a reinstall."),
 ("h3", "Programs still lay out for 80&times;25"),
 ("p", "The console can do 80&times;50 and the framebuffer could do more, but "
       "every app draws for an 80&times;25 grid. They are cleared and redrawn "
       "so nothing bleeds through, but they do not yet use a larger one."),
 ("h3", "The ISO is 16 MB"),
 ("p", "Earlier releases were five or six. The difference is the UEFI boot "
       "image, which is carried alongside the BIOS one so that a single file "
       "boots both kinds of machine."),
 ("h3", "No sound beyond the speaker"),
 ("p", "There is a PC speaker driver and nothing else. AC&rsquo;97 is "
       "planned, and is what the USB and storage work is ultimately heading "
       "towards."),
]),

# ═══════════════════════════════════════════════════════════════════ 16
dict(title="Colophon", body=[
 ("raw", '<div class="colophon">'),
 ("p", "HavenDOS is written by TechHaven Studios. It began as an experiment "
       "in whether a usable system could be written from the first "
       "instruction, on a phone, in an emulator. The installer that once only "
       "worked against a VirtIO disk now installs from an IDE CD-ROM onto an "
       "IDE disk, boots on BIOS and UEFI alike, and enumerates USB devices on "
       "chipsets that did not exist when the project started."),
 ("raw", '<div class="kv"><div class="k">Release</div>'
         '<div>0.8.0 Beta, codename &ldquo;Beacon&rdquo;</div></div>'
         '<div class="kv"><div class="k">Supersedes</div>'
         '<div>0.7.21, 0.7.22 and 0.7.23, which were one body of work '
         'under three numbers</div></div>'
         '<div class="kv"><div class="k">Checks passing</div>'
         '<div>437, up from 303 at 0.7.20</div></div>'
         '<div class="kv"><div class="k">Image</div>'
         '<div>HavenDOS-0.8.0.iso, hybrid BIOS + UEFI, about 16 MB</div></div>'
         '<div class="kv"><div class="k">Part number</div>'
         '<div>THS-HD-0800-OM-A</div></div>'),
 ("p", "Set in Courier Prime, with IBM Plex Mono for headings, VT323 for "
       "screen listings and Press Start 2P on the cover. Screens were "
       "photographed from a running system, not reconstructed."),
 ("raw", '</div>'),
]),
]
