/* ================================================================
   shell/guide.c  --  the user guide.  Rewritten for v0.9.5.

   The guide this replaces was written for 0.6. It told people their
   files lived in RAMFS "and are lost on reboot (by design)", listed
   the built-in accounts with their passwords, sent them to `sysinfo`
   and `dashboard`, called `h` an alias for help, and knew nothing of
   installing, updating, USB sticks, the network, Settings or anything
   else from the two years since. Kyan's photo of page 1 is why this
   file exists.

   Ten pages, read through the same scrolling viewer as `man`, laid
   out for whatever console it is shown on: Left/Right turn pages,
   Up/Down scroll, ESC closes. `guide <n>` opens on page n.

   Everything here was checked against the code it describes. Command
   names are written in the text, so when one is renamed this file
   must change too; tools/check_help.py fails the build if an example
   line ("> command ...") starts with a command the shell does not have.
   ================================================================ */
#include "types.h"

extern void man_pages_view(const char *const *titles, const char *const *texts, int n, int start);
extern int  atoi(const char*);

static const char *const titles[] = {
    "Guide 1 -- Welcome",
    "Guide 2 -- The desktop",
    "Guide 3 -- The shell",
    "Guide 4 -- Files and disks",
    "Guide 5 -- Scripts",
    "Guide 6 -- Settings and the look",
    "Guide 7 -- Accounts",
    "Guide 8 -- The network",
    "Guide 9 -- Installing and updating",
    "Guide 10 -- When something goes wrong",
};

static const char *const pages[] = {
/* 1 */
"# Welcome to HavenDOS\n"
"HavenDOS is a 32-bit operating system written from scratch in C and "
"assembly by TechHaven Studios. GRUB starts it; after that every driver, "
"the filesystem, the network stack, the desktop and the shell are its own.\n"
"\n"
"# Using this guide\n"
"- Left and Right turn the page, Up and Down scroll, ESC closes.\n"
"- guide <n> opens on page n, so guide 9 goes straight to installing.\n"
"\n"
"# The three places you will look things up\n"
"> help\tthe topics, and the commands in each\n"
"> help <command>\thow to use one command\n"
"> man <command>\tits full page; man on its own browses them all\n"
"\n"
"# Live or installed\n"
"Booted from the ISO or a USB stick, HavenDOS is a live session: it runs "
"from memory, leaves the machine's own disks alone and keeps nothing when "
"you switch off. Installed on a disk (page 9), your files, settings and "
"accounts are saved on its system volume.\n",

/* 2 */
"# The desktop\n"
"- The Windows key, or ESC, opens the start menu. Its rows are Favorites, "
"Apps, System, Hardware, Games, Media, Settings and All Apps.\n"
"- In the start menu, D puts the highlighted app on the desktop and F makes "
"it a favourite. Settings > Desktop does the same with a checklist.\n"
"- Tab opens the shell. exit in the shell comes back.\n"
"- The mouse and touchpads work: click an icon to open it, right-click for "
"its menu (open, desktop icon, favourite, Settings).\n"
"\n"
"# Several apps at once\n"
"Opening an app no longer closes the one before. Hold Alt and press Tab "
"to see what is open; keep pressing Tab to move, and let go of Alt to "
"switch. A quick Alt+Tab flips between the last two.\n"
"- Open apps are shown on the bottom bar: click one to bring it back.\n"
"- An app behind waits where you left it. The Stopwatch and the Music "
"Player keep going: a timer still rings and a song plays on.\n"
"- Opening an app that is already open brings it forward.\n"
"- In the switcher, X closes the highlighted app.\n"
"\n"
"# Apps worth knowing\n"
"> System\tstatus: memory, storage, network, devices\n"
"> Settings\ttheme, accounts, network, desktop icons, more\n"
"> File Manager\tbrowse and open files\n"
"> Text Editor\tCtrl+S saves, Ctrl+Q quits\n"
"> HavenCode\tthe editor for the H language (page 5)\n"
"> Manual\tevery command, browsable\n"
"> Notes / Stopwatch\twith the Converter; the Stopwatch runs behind\n"
"> 2048 / Breakout\tnew games, with Tic-Tac-Toe and Memory\n"
"\n"
"Apps written for an 80x25 screen open in a centred box on a bigger one; "
"System, Settings and the shell use the whole screen.\n",

/* 3 */
"# The shell\n"
"Type a command and press Enter. Tab completes command and file names; "
"Up and Down bring back earlier commands (history lists them).\n"
"\n"
"# Pipes, files and variables\n"
"> ls . grep TXT\ta dot between commands is a pipe\n"
"> dmesg . tail 20\tthe last twenty lines of the kernel log\n"
"> echo hello > note.txt\t> saves the output to a file\n"
"> echo again >> note.txt\t>> adds to the end\n"
"> set NAME Kyan\tthen $NAME means Kyan\n"
"> echo $OS $VER\tvariables set at startup\n"
"\n"
"# Jobs: a command that runs behind\n"
"> ping 8.8.8.8 &\t& at the end runs it as a job\n"
"> ps\tlists open apps and jobs\n"
"> fg 3\tbrings job or app 3 to the front\n"
"> kill 3\tcloses it\n"
"A job has its own screen; Alt+Tab shows it. The alerts say when it ends.\n"
"\n"
"# Shortcuts\n"
"> alias up=cd ..\tup now goes up a folder; saved\n"
"> unalias up\tand gone again\n"
"Built in: ll is ls, q is exit, ? is help and g is guide.\n"
"\n"
"# Making it yours\n"
"> ps1 \\w> \ta shorter prompt; ps1 reset undoes it\n"
"> color 10\tgreen text; color reset undoes it\n"
"> resolution fit\ttext sized for this screen\n"
"All three are saved, and all three are in Settings too.\n",

/* 4 */
"# Your files\n"
"Each account has a home folder, HOME/<name>, and the shell starts there. "
"cd on its own goes back to it.\n"
"> ls\twhat is here (folders cyan, files green)\n"
"> cd docs\tinto a folder; cd .. goes up\n"
"> cat notes.txt\tshow a file\n"
"> edit notes.txt\topen it in the editor\n"
"> cp a.txt b.txt\tcopy; mv renames or moves; rm deletes\n"
"> find *.txt\tsearch this folder and everything below it\n"
"> tree\tthe whole layout\n"
"\n"
"# Disks\n"
"> disk\tevery disk and what is on it\n"
"> df\thow full the system volume is\n"
"> system storage\tthe same, drawn\n"
"Outside the installer, disks that are not HavenDOS's are only ever "
"read: an NVMe disk with Windows on it is listed, never written.\n"
"\n"
"# USB sticks\n"
"Plug one in, then:\n"
"> fat32\tthe FAT32 volumes, numbered from 0\n"
"> fat32 ls 0\tlist volume 0\n"
"> exfat\texFAT volumes -- most big sticks\n"
"> fat16 cp notes.txt 0:NOTES.TXT\tcopy a file onto a FAT16 stick\n"
"FAT32 and exFAT are read-only here; fat16 cp is the one way to write "
"a stick.\n",

/* 5 */
"# Shell scripts\n"
"A .bsh file is a list of shell commands, one per line, run with run (or "
"source, or a dot). Lines starting with # are comments. Scripts understand "
"variables, if/else/endif, while/endwhile, and input <name> to read a line "
"from the keyboard. Comparisons: == != < > <= >=.\n"
"\n"
"# An example\n"
"Make count.bsh with edit count.bsh and type:\n"
"> set i 1\tset does arithmetic: $i+1\n"
"> while $i <= 3\n"
">   echo line $i\n"
">   set i $i+1\n"
"> endwhile\n"
"Then run count.bsh prints three lines.\n"
"\n"
"# Handy around scripts\n"
"> repeat 3 beep\trun a command several times\n"
"> watch 2 mem\tre-run one every two seconds\n"
"> timer selftest\thow long a command takes\n"
"> notify success Done\ta notification when something finishes\n"
"\n"
"# The H language\n"
"H is HavenDOS's own programming language. Install it with bpkg install "
"h-lang; then h opens its prompt and h <file.h> runs a program. HavenCode on "
"the desktop is its editor.\n",

/* 6 */
"# Settings\n"
"Open Settings from the start menu or type settings. Every change is saved "
"the moment you make it.\n"
"- Appearance: the theme, with a preview of the desktop; the wallpaper; the "
"memory and uptime strip along the bottom of the desktop.\n"
"- Display: how many characters the console holds.\n"
"- Desktop: which apps have icons, which are favourites.\n"
"- Shell: the prompt and the shell's text colour.\n"
"- Accounts: passwords, roles, new accounts, auto-login (page 7).\n"
"- Network: DHCP, an address by hand, the DNS server.\n"
"- Startup: the boot log, the screensaver, the hostname.\n"
"settings accounts, settings display and so on open straight on a section.\n"
"\n"
"# Themes\n"
"Twelve themes come with every install, each with its own desktop colour "
"and wallpaper; the themes-extra package adds eight more.\n"
"> theme\tevery theme, with a strip showing it\n"
"> theme ocean\tswitch by name, or by number\n",

/* 7 */
"# Accounts\n"
"> users\tevery account: role, password, home, last login\n"
"> users kyan\tone account, with its recent logins\n"
"> whoami\twho you are\n"
"> passwd\tchange your password\n"
"> lock\tlock the screen until your password is typed\n"
"\n"
"# For administrators\n"
"> adduser sam pw123\ta new account with its home folder\n"
"> deluser sam\tremove it (its files stay)\n"
"> log\tevery login and failed login, with times\n"
"Settings > Accounts does all of this from a list, and can make an account "
"an administrator or turn auto-login on.\n"
"\n"
"# The built-in accounts\n"
"Every install also has admin, bootos and guest, hidden from the login "
"screen. admin and bootos start with passwords that are written in "
"HavenDOS's source code, so anyone could type them: users marks them "
"DEFAULT in yellow. Give them passwords of your own with passwd admin and "
"passwd bootos, or delete bootos and guest.\n",

/* 8 */
"# Getting online\n"
"HavenDOS asks the network for an address at boot. If that did not work:\n"
"> ifconfig\tthe address, and where it came from\n"
"> ifconfig dhcp\task again\n"
"> ping 1.1.1.1\tis the internet there?\n"
"> nslookup example.com\tdo names resolve?\n"
"A wired network card or a USB Ethernet adapter works. Wi-Fi does not yet.\n"
"\n"
"# Using it\n"
"> wget http://example.com/ page.htm\tdownload to a file\n"
"> curl http://example.com/\tprint a page\n"
"> chat kyan@10.0.2.2\tBOOT Chat\n"
"HTTPS works with servers that still offer TLS 1.2 with RSA key exchange; "
"tls says why a handshake failed when one does not.\n"
"\n"
"# An iPhone as the network\n"
"Turn on Personal Hotspot and plug the phone in by USB. usb eth shows the "
"link; pair asks the phone to trust this computer -- tap Trust on the "
"phone, then run pair again.\n",

/* 9 */
"# Installing\n"
"From the live session, type install or open Install HavenDOS.\n"
"- Beside Windows, HavenDOS adds its own partition and a boot entry and "
"touches nothing else.\n"
"- On a disk you choose to give it, it can use the whole disk.\n"
"- Up to 2 GB the system volume is FAT16; bigger ones are FAT32, up to "
"32 GB.\n"
"- Nothing is written until you type the confirmation word.\n"
"An installed HavenDOS can also install itself onto another disk: the "
"same install command, from the desktop or the shell.\n"
"\n"
"# Updating\n"
"Every release has a zip with the new kernel and its update.inf. The "
"scripts in it copy them to a USB stick or the EFI partition from Windows "
"or Linux. Then, in HavenDOS:\n"
"> update\tcheck the new kernel and install it\n"
"> rollback\tgo back to the one before\n"
"The old kernel is kept as /boot/havendos.bak until the next update.\n"
"\n"
"# Removing it\n"
"uninstall shows exactly what it will undo -- the boot loader it puts back, "
"the partition entries it removes -- and does it only after you type the "
"confirmation word.\n",

/* 10 */
"# Finding out what happened\n"
"> dmesg\teverything the kernel logged since boot\n"
"> dmesg . grep usb\tjust one driver's lines\n"
"> system\tmemory, storage, network and devices, live\n"
"> selftest\tthe built-in checks\n"
"> lspci\tthe hardware, as the PCI bus reports it\n"
"\n"
"# If an app crashes\n"
"HavenDOS catches the fault, says which app it was and returns to the "
"desktop; the rest of the system carries on.\n"
"\n"
"# If the screen is wrong\n"
"> resolution reset\tback to 80x25, even typed blind\n"
"A console size that does not display reverts by itself after ten seconds.\n"
"\n"
"# Telling us\n"
"A photo of the screen and the version (ver) is the best bug report. "
"Releases, source and issues: github.com/MGKFN/HavenDOS.\n",
};

#define NPAGES ((int)(sizeof pages / sizeof pages[0]))

void cmd_guide_page(const char *arg){
    int start = (arg && arg[0]) ? atoi(arg) - 1 : 0;
    man_pages_view(titles, pages, NPAGES, start);
}

void cmd_guide(void){ cmd_guide_page(""); }
