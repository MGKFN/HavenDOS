# -*- coding: utf-8 -*-
"""THS-IG-1001 -- Installing HavenDOS beside Windows. The content.

Written for someone who has never installed an operating system: every
step says where to click, what the screen should look like and what to do
when it does not. Same block vocabulary as THS-RM-1005 (docs/rm1005):

    ("p", html) ("h3", text) ("ul", [...]) ("ol", [...]) ("crt", text)
    ("note", html) ("caution", html) ("box", label, html)
    ("table", [heads], [[cells], ...], [widths]) ("spec", [(k, v)])
    ("fig", file, label, caption) ("raw", html)

[[Section title]] becomes "section N"; [[^Section title]] "Section N".

Every key, screen title and message quoted here is the text the 0.9.7.8
installer, boot menu and setup actually print -- shell/iso_installer.c,
shell/setup.c, fs/esp.c. If one of those changes, change this.
"""

K = '<span class="key">%s</span>'


def key(s):
    return K % s


C = '<span class="cmd">%s</span>'


def cmd(s):
    return C % s


META = dict(
    title="INSTALLING<br/>HAVENDOS<br/>BESIDE WINDOWS",
    subtitle=("A step-by-step guide for a first install,<br/>"
              "from an empty USB stick to the HavenDOS desktop,<br/>"
              "with Windows still there afterwards"),
    part="THS-IG-1001 REV A",
    date="24 NOV 2026",
    release="v0.9.7.8 Stable &ldquo;Mnemosyne&rdquo;",
    need=[
        ("TIME", "about 45 minutes"),
        ("PC", "a Windows 10 or 11 PC, 64-bit, from about 2012 on"),
        ("USB STICK", "1 GB or bigger &mdash; it will be erased"),
        ("FREE SPACE", "4 GB on the disk (64 MB at the very least)"),
        ("ALSO", "the charger plugged in, and the internet"),
    ],
)


SECTIONS = [

{"title": "Before you start", "body": [
 ("p", "This guide puts HavenDOS on your computer <b>next to</b> Windows. "
       "Nothing in Windows is deleted. When you switch the computer on "
       "afterwards, a menu lets you choose HavenDOS or Windows."),
 ("p", "You will do eight things, in this order:"),
 ("ol", ["Back up your files and find your BitLocker key ([[Step 1: Back up]]).",
         "Download HavenDOS and a tool called Rufus ([[Step 2: Download]]).",
         "Turn a USB stick into a HavenDOS installer ([[Step 3: Make the USB stick]]).",
         "Make some free room on the disk in Windows ([[Step 4: Make room on the disk]]).",
         "Switch off Secure Boot ([[Step 5: Switch off Secure Boot]]).",
         "Start the computer from the USB stick ([[Step 6: Start from the USB stick]]).",
         "Run the installer ([[Step 7: The HavenDOS menu]] and [[Step 8: The installer]]).",
         "Start HavenDOS for the first time ([[Step 9: The first start]])."]),
 ("caution", "HavenDOS is a hobby operating system, written from scratch. "
             "The installer is careful &mdash; it shows exactly what it will "
             "write before it writes anything, and it asks you to type YES "
             "&mdash; but any program that writes to a disk can go wrong. "
             "<b>Do not skip [[Step 1: Back up]].</b>"),
 ("note", "Words like <i>partition</i>, <i>unallocated</i> and <i>UEFI</i> "
          "are explained in [[Words you will meet]]. You do not need to know "
          "them in advance."),
]},

{"title": "Words you will meet", "body": [
 ("table", ["Word", "What it means here"], [
   ["Disk", "The drive inside the computer where Windows and your files are "
            "kept. Usually one per laptop."],
   ["Partition", "A section of the disk. Windows lives in one big partition "
                 "called C:. HavenDOS gets a partition of its own."],
   ["Unallocated", "Part of the disk that no partition uses &mdash; empty "
                   "room. HavenDOS installs into it."],
   ["ISO file", "One file that holds a whole installer, like a CD in a "
                "file. You download it and write it to a USB stick."],
   ["Boot", "Starting the computer. <i>Booting from the USB stick</i> means "
            "starting the computer from the stick instead of from Windows."],
   ["BIOS / UEFI", "The computer's own settings screen, before Windows "
                   "starts. Modern PCs call it UEFI; most people still say BIOS."],
   ["Boot menu", "A list, shown by the BIOS when you press a certain key at "
                 "start-up, of everything the computer can start from."],
   ["Secure Boot", "A BIOS setting that only lets Microsoft-approved "
                   "programs start. HavenDOS is not signed by Microsoft, so it "
                   "has to be off."],
   ["BitLocker", "Windows' disk lock. If it is on, Windows may ask for a "
                 "48-digit recovery key after a BIOS setting changes."],
 ], [26, 74]),
]},

{"title": "Step 1: Back up", "body": [
 ("p", "Copy everything you cannot lose &mdash; photos, documents, school "
       "or work files &mdash; to an external drive or to OneDrive or Google "
       "Drive. If something goes wrong in [[Step 4: Make room on the disk]] or "
       "[[Step 8: The installer]], this is what gets your files back."),
 ("h3", "Find your BitLocker recovery key"),
 ("p", "Many laptops have BitLocker switched on without anyone having done "
       "it. Switching off Secure Boot in [[Step 5: Switch off Secure Boot]] can make "
       "Windows ask for the recovery key the next time it starts. Without the "
       "key, Windows will not open."),
 ("ol", ["On a phone or another computer, go to <b>aka.ms/myrecoverykey</b> "
         "and sign in with the Microsoft account you use on the PC.",
         "If a key is listed for your PC, write it down on paper or take a "
         "photo of it. It is 48 digits long.",
         "If no key is listed, open the Start menu on the PC, type "
         "<b>manage bitlocker</b> and open it. If it says <i>BitLocker "
         "off</i>, you have nothing to do. If it says <i>on</i>, click "
         "<b>Back up your recovery key</b> and save it to a USB stick or "
         "print it &mdash; but not the stick you will use for HavenDOS."]),
 ("note", "Some Windows editions call it <i>Device encryption</i> and show it "
          "under Settings &rsaquo; Privacy &amp; security &rsaquo; Device encryption. "
          "The key is on the same web page."),
]},

{"title": "Step 2: Download", "body": [
 ("ol", ["Go to <b>github.com/MGKFN/HavenDOS/releases</b>. The release at the "
         "top, marked <b>Latest</b>, is the one to use.",
         "Under <b>Assets</b>, click the file that ends in <b>.iso</b> "
         "&mdash; for this release <b>HavenDOS-0.9.7.8.iso</b>, about 17 MB. "
         "It goes to your Downloads folder.",
         "Go to <b>rufus.ie</b> and download <b>Rufus</b>, the "
         "&ldquo;Standard&rdquo; or &ldquo;Portable&rdquo; version. It is a "
         "small free program that writes the ISO file to a USB stick. It "
         "does not need to be installed: you just open it."]),
 ("note", "Do not open the ISO file by double-clicking it. Windows would show "
          "it like a CD, which is not what you need. Rufus reads it."),
]},

{"title": "Step 3: Make the USB stick", "body": [
 ("caution", "<b>Everything on the USB stick is erased.</b> Copy anything on "
             "it somewhere else first. Unplug any other USB sticks and "
             "external drives, so you cannot pick the wrong one."),
 ("ol", ["Plug in the USB stick.",
         "Open <b>Rufus</b>. If Windows asks <i>Do you want to allow this app "
         "to make changes?</i>, click <b>Yes</b>. If it asks about updates, "
         "either answer is fine.",
         "Under <b>Device</b>, check that the name and size are your USB stick.",
         "Click <b>SELECT</b> and choose <b>HavenDOS-0.9.7.8.iso</b> from your "
         "Downloads folder.",
         "Leave every other setting as it is, and click <b>START</b>.",
         "Rufus asks how to write the image. Choose <b>Write in DD Image "
         "mode</b> and click <b>OK</b>.",
         "It warns that all data on the stick will be destroyed. Click "
         "<b>OK</b>.",
         "Wait until the bar is full and says <b>READY</b> &mdash; under a "
         "minute. Click <b>CLOSE</b>."]),
 ("p", "Windows may now say the stick needs to be formatted, or show it as "
       "an empty drive. <b>Click Cancel</b> and ignore it: the stick is fine, "
       "Windows just cannot read the format HavenDOS uses."),
 ("note", "On a Mac or Linux PC, <b>balenaEtcher</b> (etcher.balena.io) does "
          "the same job: Flash from file, select target, Flash."),
]},

{"title": "Step 4: Make room on the disk", "body": [
 ("p", "HavenDOS needs empty, <b>unallocated</b> space on the disk. Windows "
       "usually uses the whole disk, so you take some space away from C:. "
       "Your files stay where they are; C: just gets a little smaller."),
 ("ol", ["Right-click the <b>Start</b> button and click <b>Disk Management</b>.",
         "At the bottom you see your disk as a row of coloured blocks. Find "
         "the big one marked <b>(C:)</b>.",
         "Right-click <b>(C:)</b> and click <b>Shrink Volume&hellip;</b>. Wait "
         "while Windows works out how much it can give.",
         "In <b>Enter the amount of space to shrink in MB</b>, type "
         "<b>4096</b> &mdash; that is 4 GB.",
         "Click <b>Shrink</b>. After a few seconds a new block appears beside "
         "C:, with a <b>black</b> bar and the word <b>Unallocated</b>.",
         "<b>Leave it like that.</b> Do not right-click it and do not choose "
         "New Simple Volume. Close Disk Management."]),
 ("table", ["Space you shrink", "What HavenDOS does with it"], [
   ["64 MB or more", "Installs, formatted FAT32"],
   ["16 MB to 63 MB", "Installs, formatted FAT16 (a very small system)"],
   ["4096 MB (4 GB)", "Recommended: room for your files and music"],
   ["more than 32 GB", "Uses 32 GB and leaves the rest unallocated"],
 ], [38, 62]),
 ("note", "If Windows offers less than you asked for, any amount over 64 MB "
          "works. Windows cannot move some of its system files, and that is "
          "what limits it."),
 ("caution", "If you see a partition that is already empty but has a drive "
             "letter (for example a new <b>D:</b> you made earlier), the "
             "installer will <b>not</b> use it: it never touches a partition "
             "that has anything formatted on it. Right-click it, choose "
             "<b>Delete Volume</b>, and only if you are sure it is empty."),
]},

{"title": "Step 5: Switch off Secure Boot", "body": [
 ("p", "The easiest way into the BIOS is from Windows:"),
 ("ol", ["Open <b>Settings</b> &rsaquo; <b>System</b> &rsaquo; <b>Recovery</b>. "
         "(Windows 10: Settings &rsaquo; Update &amp; Security &rsaquo; Recovery.)",
         "Next to <b>Advanced startup</b>, click <b>Restart now</b>.",
         "On the blue screen, click <b>Troubleshoot</b> &rsaquo; <b>Advanced "
         "options</b> &rsaquo; <b>UEFI Firmware Settings</b> &rsaquo; "
         "<b>Restart</b>.",
         "The computer restarts into the BIOS. Use the arrow keys and "
         + key("Enter") + ". Look for <b>Secure Boot</b>, usually under "
         "<b>Boot</b>, <b>Security</b> or <b>Boot Options</b>, and set it to "
         "<b>Disabled</b>.",
         "Save and exit, usually with " + key("F10") + " and then "
         "<b>Yes</b>."]),
 ("table", ["Make", "Where Secure Boot usually is"], [
   ["HP", "Advanced &rsaquo; Boot Options (or Secure Boot Configuration). "
          "After saving, HP shows a 4-digit code: type it and press Enter."],
   ["Dell", "Boot Configuration (or Secure Boot) &rsaquo; Secure Boot Enable"],
   ["Lenovo", "Security &rsaquo; Secure Boot"],
   ["ASUS", "Boot &rsaquo; Secure Boot &rsaquo; OS Type: Other OS"],
   ["Acer", "Boot &rsaquo; Secure Boot. Acer may first ask you to set a "
            "supervisor password under Security."],
 ], [18, 82]),
 ("note", "If Windows starts by itself and asks for a <b>BitLocker recovery "
          "key</b>, type the 48 digits from [[Step 1: Back up]]. That happens "
          "once."),
]},

{"title": "Step 6: Start from the USB stick", "body": [
 ("p", "Plug in the USB stick. The quickest way is again from Windows:"),
 ("ol", ["Settings &rsaquo; System &rsaquo; Recovery &rsaquo; Advanced startup "
         "&rsaquo; <b>Restart now</b>.",
         "Click <b>Use a device</b>, then the USB stick. It may be called "
         "<b>USB Drive</b>, <b>UEFI: USB</b>, or by its brand, such as "
         "<b>SanDisk</b>."]),
 ("p", "If <b>Use a device</b> is not there, switch the computer off, switch "
       "it on, and tap the boot-menu key straight away, about twice a second, "
       "until a list appears. Pick the USB stick with the arrows and "
       + key("Enter") + "."),
 ("table", ["Make", "Boot-menu key"], [
   ["HP", key("Esc") + ", then " + key("F9")],
   ["Dell", key("F12")],
   ["Lenovo", key("F12") + " (some: the small Novo button by the power socket)"],
   ["ASUS", key("Esc") + " or " + key("F8")],
   ["Acer", key("F12")],
   ["MSI", key("F11")],
 ], [18, 82]),
]},

{"title": "Step 7: The HavenDOS menu", "body": [
 ("p", "A dark screen with <b>HavenDOS</b> in large letters and a list of "
       "choices appears. It waits for a minute before it starts the first "
       "choice by itself, so take your time."),
 ("fig", "grub-iso.png", "FIGURE 7-1",
  "The menu on the USB stick. The red bar is the choice that "
  + key("Enter") + " starts."),
 ("ol", ["Press " + key("&darr;") + " three times, to <b>HavenDOS v0.9.7.8 -- "
         "Install to Disk</b>.",
         "Press " + key("Enter") + "."]),
 ("note", "If every key you press later shows up <b>twice</b>, restart and "
          "choose <b>Install to Disk (firmware keyboard)</b> instead. It "
          "uses the computer's own keyboard handling, which works on any "
          "machine."),
]},

{"title": "Step 8: The installer", "body": [
 ("h3", "Welcome"),
 ("p", "It says <i>You have booted from the HavenDOS install ISO</i> and how "
       "many disks it found. Press " + key("Enter") + "."),
 ("h3", "Select Target Disk"),
 ("p", "One line per disk. Your laptop's disk is the big one &mdash; "
       "256 GB, 512 GB, 1 TB. The USB stick is listed too: it is the small "
       "one. Read the <b>Notes</b> column:"),
 ("table", ["Notes says", "Means"], [
   ["keeps this disk, uses <b>4095</b> MB",
    "The right one. HavenDOS goes in the free space from [[Step 4: Make room on the disk]], "
    "and this is how big it will be."],
   ["no free space - shrink first", "There is no unallocated space. Go back "
                                    "to [[Step 4: Make room on the disk]]."],
   ["no partition table - WIPES disk",
    "<b>Stop.</b> This disk is not your Windows disk, or the installer "
    "could not read it. Choosing it erases all of it."],
   ["running from this disk", "The USB stick itself. Not this one."],
 ], [44, 56]),
 ("fig", "picker.png", "FIGURE 10-1",
  "The disk picker in a test machine. Your disk will be bigger, and the "
  "number of MB is what HavenDOS will use."),
 ("p", "Pick the line that says <b>keeps this disk</b> with " + key("&uarr;")
       + " " + key("&darr;") + " and press " + key("Enter") + "."),
 ("h3", "What will be written"),
 ("p", "The installer lists, before it touches anything, the new partition "
       "it will make (its size, and <b>formatted FAT32</b>), and the four "
       "sectors of the partition table it updates to add it. Press "
       + key("Enter") + "."),
 ("h3", "The confirmation"),
 ("p", "It says <b>HavenDOS will be installed BESIDE what is already on "
       "disk</b>, <b>Nothing is deleted</b>, and, on a Windows laptop, that "
       "Windows was found and is expected to keep booting."),
 ("fig", "confirm.png", "FIGURE 10-2",
  "The last screen before anything is written. It waits for YES."),
 ("ol", ["Type " + key("Y") + key("E") + key("S") + " in capitals.",
         "Press " + key("Enter") + "."]),
 ("p", "A progress bar fills in about a minute. When it says "
       "<b>Installation Complete</b>, it counts down and restarts the "
       "computer. <b>Pull out the USB stick</b> while it counts."),
 ("caution", "If it says <b>Install Failed</b>, nothing that was there "
             "before has been changed. Take a photo of the screen &mdash; it "
             "says which step failed and why &mdash; and send it to Tech "
             "Haven Studios (see [[If something goes wrong]])."),
]},

{"title": "Step 9: The first start", "body": [
 ("h3", "If Windows starts instead"),
 ("p", "Most computers keep starting Windows until you tell them otherwise. "
       "Use the boot-menu key from [[Step 6: Start from the USB stick]], and "
       "this time pick <b>HAVENDOS</b> (some machines say <b>UEFI OS</b> or "
       "the name of the disk)."),
 ("p", "To get the HavenDOS menu every time, go into the BIOS as in "
       "[[Step 5: Switch off Secure Boot]], find <b>Boot Order</b> (or "
       "<b>UEFI Boot Order</b>), move <b>HAVENDOS</b> above <b>Windows Boot "
       "Manager</b>, and save with " + key("F10") + "."),
 ("h3", "The menu on your disk"),
 ("fig", "grub-disk.png", "FIGURE 11-1",
  "The menu HavenDOS installs, here with HavenDOS chosen. Left alone, it "
  "starts Windows after 8 seconds."),
 ("p", "It has three choices:"),
 ("table", ["Choice", "Does"], [
   ["HavenDOS v0.9.7.8 Stable", "Starts HavenDOS."],
   ["Windows Boot Manager", "Starts Windows. <b>Chosen by itself after "
                            "8 seconds</b>, so the computer still ends up "
                            "in Windows if you do nothing."],
   ["Firmware setup", "Opens the BIOS."],
 ], [40, 60]),
 ("h3", "Setting up HavenDOS"),
 ("p", "The first time, HavenDOS asks seven short questions:"),
 ("ol", ["<b>Terms of Service</b> &mdash; scroll to the end with "
         + key("PgDn") + " or " + key("End") + ", then accept.",
         "<b>Privacy Policy</b> &mdash; the same.",
         "<b>Welcome</b> &mdash; press " + key("Enter") + ".",
         "<b>Create Account</b> &mdash; a user name and a password you will "
         "remember.",
         "<b>Choose Theme</b> &mdash; <b>Studio</b> is already picked: the "
         "Tech Haven Studios colours. Any other is fine; Settings can change "
         "it later.",
         "<b>Packages</b> &mdash; extras; skip them for now.",
         "<b>Ready</b> &mdash; press " + key("Enter") + ", and the desktop "
         "appears."]),
 ("note", "HavenDOS also has built-in accounts. Change their passwords: in "
          "the Shell type " + cmd("passwd admin") + " (the default is "
          + cmd("Admin@123") + ")."),
]},

{"title": "Every day", "body": [
 ("ul", ["<b>To use HavenDOS:</b> switch on, and choose HavenDOS in the menu "
         "within 8 seconds.",
         "<b>To use Windows:</b> switch on and do nothing, or choose "
         "<b>Windows Boot Manager</b>.",
         "<b>To switch off HavenDOS:</b> Start menu &rsaquo; Power, or type "
         + cmd("shutdown") + " in the Shell.",
         "<b>To update HavenDOS:</b> type " + cmd("update") + " in the Shell, "
         "or see <b>update/README-UPDATE.md</b> in the release bundle."]),
 ("note", "Files you save in HavenDOS live on its own partition. Windows "
          "cannot see them, and HavenDOS cannot see Windows' C: drive. A USB "
          "stick formatted FAT32 or exFAT works in both."),
]},

{"title": "If something goes wrong", "body": [
 ("table", ["What you see", "What to do"], [
   ["<b>Secure Boot Violation</b>, or a red or blue security message, "
    "when starting from the stick", "Secure Boot is still on. Do "
    "[[Step 5: Switch off Secure Boot]] again."],
   ["The computer goes straight to Windows; the stick is ignored",
    "Use the boot-menu key, or <i>Use a device</i> ([[Step 6: Start from the USB stick]]). "
    "Try another USB port. Make the stick again in DD Image mode."],
   ["Every key shows up twice", "Restart; choose the <b>(firmware "
                                "keyboard)</b> choice in the menu."],
   ["The installer says <b>no free space - shrink first</b>",
    "[[Step 4: Make room on the disk]] was not done, or the space was given a "
    "drive letter. In Disk Management it must say <b>Unallocated</b>."],
   ["<b>HavenDOS already installed</b> next to the disk",
    "It is already there. Remove the stick and start from the disk "
    "([[Step 9: The first start]]), or see [[Installing again over an older HavenDOS]]."],
   ["Windows asks for a <b>BitLocker recovery key</b>",
    "Type the 48-digit key from [[Step 1: Back up]]. Once is enough."],
   ["A black screen after choosing HavenDOS", "Restart and choose <b>Safe "
                                               "Mode</b> from the USB stick's menu."],
 ], [44, 56]),
 ("p", "Anything else: take a photo of the screen and report it at "
       "<b>github.com/MGKFN/HavenDOS/issues</b>, or in the Tech Haven Studios "
       "Discord. Say which computer you have and which step you were on."),
]},

{"title": "Installing again over an older HavenDOS", "body": [
 ("p", "The installer will not put a second HavenDOS on a disk that already "
       "has one: two partitions both called HAVENDOS would leave the boot "
       "menu starting whichever it found first. It says <b>HavenDOS is "
       "already installed on disk</b> instead. To start fresh &mdash; for "
       "example to move from an old FAT16 install to FAT32:"),
 ("ol", ["Copy anything you want to keep out of HavenDOS onto a USB stick.",
         "Start the <b>old</b> HavenDOS, open the Shell, type "
         + cmd("uninstall") + ", then " + cmd("UNINSTALL") + ". Its "
         "partition becomes unallocated space again. (Or delete the HAVENDOS "
         "partition in Windows' Disk Management.)",
         "In Disk Management, check the space shows as <b>Unallocated</b> and "
         "is the size you want. Shrink C: as in [[Step 4: Make room on the disk]] "
         "if you want more.",
         "Carry on from [[Step 6: Start from the USB stick]]."]),
 ("note", "Only want the newest version, not a fresh start? Then do not "
          "reinstall: start HavenDOS and type " + cmd("update") + "."),
]},

{"title": "Removing HavenDOS", "body": [
 ("p", "HavenDOS can take itself off again and give the space back:"),
 ("ol", ["Start HavenDOS and open the <b>Shell</b>.",
         "Type " + cmd("uninstall") + " and press " + key("Enter") + ". It "
         "shows exactly what it will remove: its own partition and its boot "
         "files, nothing of Windows'.",
         "Type " + cmd("UNINSTALL") + " in capitals and press " + key("Enter") + ".",
         "Start Windows. In Disk Management the space is <b>Unallocated</b> "
         "again. Right-click <b>(C:)</b> &rsaquo; <b>Extend Volume&hellip;</b> "
         "&rsaquo; Next &rsaquo; Next &rsaquo; Finish to give it back to C:.",
         "If you like, switch Secure Boot back on in the BIOS."]),
]},

]
