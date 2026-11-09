# -*- coding: utf-8 -*-
"""THS-RM-1003 — the content.

Plain structures only; docs/rm1003/build.py turns these into the HTML the
house stylesheet expects.  Same block vocabulary as manual/content.py and
docs/rm1002/content.py so the three documents cannot drift in appearance:

    ("p", html)              a paragraph
    ("h3", text)             a subheading
    ("ul", [html, ...])      a bulleted list
    ("ol", [html, ...])      a numbered list
    ("crt", text)            a screen listing, rendered as a CRT
    ("note", html)           a boxed NOTE
    ("caution", html)        a boxed CAUTION
    ("box", label, html)     a boxed callout with a custom label
    ("table", [heads], [[cells], ...])
    ("spec", [(k, v), ...])  a key/value spec block
    ("raw", html)

Every decision in this document was taken on 19 October 2026, question by
question.  Where a decision contradicts RM-1002 the reason is written down
beside it, because a plan that quietly changes its mind is a plan nobody can
audit.
"""

META = dict(
    ver="1003", channel="", codename="",
    title="HAVENDOS<br/>ROAD TO 1.0",
    subtitle=("The release plan from v0.8.3.1 to 1.0, and the web at 1.1<br/>"
              "Fifteen releases, decided question by question"),
    part="THS-RM-1003 REV A",
    date="19 OCT 2026",
    author="Kyan Hoekstra / TechHaven Studios",
    baseline="v0.8.3.1 &ldquo;Lodger&rdquo;, 1657 checks across 26 suites",
    supersedes="THS-RM-1002 in full",
    status="PROPOSAL &mdash; decided 19 OCT 2026, not yet started",
    cover_bars=[
        ("BLOCK LAYER",  16, 20, "ATA, VirtIO, AHCI, NVMe r/w"),
        ("CONTROLLERS",   6, 20, "UHCI only, no xHCI"),
        ("NETWORK",       5, 20, "RTL8139 and 82540 only"),
        ("TRANSPORT",     9, 20, "TLS 1.2, one RSA suite"),
        ("DISPLAY",       7, 20, "1280x800, 80x25 grid"),
        ("APPLICATIONS",  4, 20, "45 apps, every one at 80x25"),
    ],
)

SECTIONS = [

# ══════════════════════════════════════════════════════════════════ 1
dict(title="What changed since THS-RM-1002", body=[
 ("p", "RM-1002 planned ten releases from v0.8.2 to 1.0 and was approved on "
       "12 October 2026. Two of them have shipped, four things have been "
       "learned since, and one of those four moves a whole arc of the plan "
       "out of 1.0. This document replaces RM-1002 rather than amending it."),

 ("h3", "The numbers came apart from the map"),
 ("p", "RM-1002&rsquo;s <b>0.9.0 &ldquo;Ledger&rdquo;</b> &mdash; the "
       "additive GPT and ESP installer &mdash; shipped as <b>v0.8.3 "
       "&ldquo;Lodger&rdquo;</b>. Its <b>0.9.1 &ldquo;Anchor&rdquo;</b> "
       "&mdash; the NVMe write path &mdash; shipped as <b>v0.8.3.1</b>, "
       "along with AHCI, the MMIO test seam and six bugs found in one "
       "afternoon on real hardware. Its <b>0.9.3 &ldquo;Spindle&rdquo;</b> "
       "is substantially done too: AHCI exists, and the ATA driver no longer "
       "stops probing at the first drive it finds."),
 ("p", "So three of ten are delivered and the version numbers no longer "
       "point at the document. Every release below is renumbered from "
       "0.8.4 upward, in dependency order."),

 ("h3", "The test laptop has no Ethernet"),
 ("box", "FINDING 1",
       "The HP&rsquo;s only network adapter is a MediaTek "
       "<span class=\"cmd\">14C3:7902</span>, and it is <b>Wi-Fi</b>. There "
       "is no RJ45 socket on the machine. Wi-Fi needs signed firmware blobs "
       "per chipset and a full 802.11 MAC, which RM-1002 ruled out of reach "
       "for a solo bare-metal kernel and this document does not reopen. "
       "<b>The only way that laptop reaches a network is a USB Ethernet "
       "adapter</b> &mdash; which means xHCI, which means the USB core."),
 ("p", "This is the single finding that most changes the shape of the plan. "
       "USB is no longer one feature among several: on the one machine this "
       "project is actually aimed at, USB gates <b>input</b>, <b>removable "
       "storage</b> and <b>the network</b>. It becomes the spine, and four "
       "consecutive releases hang off it."),

 ("h3", "There is no SATA machine in the house"),
 ("box", "FINDING 2",
       "The ThinkPad X140e used through v0.7.11 belonged to a friend and is "
       "no longer available. The HP has <b>no SATA controller at all</b>. "
       "<span class=\"cmd\">drivers/ahci.c</span> therefore cannot be proved "
       "on real silicon by anyone working on this tree, and will ship at 1.0 "
       "having run only against the simulated controller in "
       "<span class=\"cmd\">tests/test_ahci.c</span> and QEMU&rsquo;s q35."),
 ("p", "The decision taken is to <b>ship it and say so in writing</b>, in "
       "the release notes and in the 1.0 claims, rather than quietly imply "
       "hardware coverage that does not exist. "
       "[[^Release gates]] carries the qualification."),

 ("h3", "The browser is gated on cryptography, not HTML"),
 ("box", "FINDING 3",
       "<span class=\"cmd\">net/tls.c</span> offers exactly one cipher "
       "suite: <span class=\"cmd\">TLS_RSA_WITH_AES_128_CBC_SHA256</span> "
       "(0x003C). RSA key exchange was removed in TLS 1.3 and most public "
       "servers stopped accepting it years ago. A browser built on what the "
       "tree has today would reach a shrinking handful of sites; an "
       "HTTP-only browser in 2026 reaches almost nothing at all."),
 ("p", "The work the browser actually needs is X25519, AES-GCM, HKDF, the "
       "TLS 1.3 handshake, signature verification and a trust store &mdash; "
       "none of which exists. RM-1002 called that 1.1 and was right. "
       "The browser follows it."),

 ("h3", "There is no upgrade path to the test machine"),
 ("p", "<span class=\"cmd\">fs/update.c</span> reads the new image from an "
       "<b>ATAPI CD-ROM</b>. The HP has no optical drive, no USB mass "
       "storage support in HavenDOS and no supported NIC, so every one of "
       "the fourteen releases below would otherwise reach it by full "
       "reinstall &mdash; which means running the additive installer beside "
       "a live Windows partition, fourteen more times. That is why "
       "[[0.8.4 &ldquo;Hermes&rdquo; &mdash; the update path]] comes first and "
       "nothing else does."),
]),

# ══════════════════════════════════════════════════════════════════ 2
dict(title="Ground truth after 0.8.3.1", body=[
 ("p", "What the tree actually contains on 19 October 2026, read rather "
       "than remembered. Everything in [[The release map]] is scoped against "
       "this table and nothing else."),
 ("table", ["Layer", "Has", "Has not"], [
   ["Block",
    "ATA/IDE with LBA48, ATAPI, AHCI, NVMe read and write, VirtIO",
    "USB mass storage. SD, eMMC, SAS, RAID, Intel VMD"],
   ["Filesystem",
    "FAT12, FAT16 read/write/format, FAT32 read, ISO9660, GPT read/write, "
    "ESP writer, ramfs, VFS",
    "A FAT32 <i>formatter</i> &mdash; so every partition stops at 2047 MB. "
    "No NTFS, no ext"],
   ["USB",
    "UHCI: control and bulk transfers, port reset, enumeration, 930 lines, "
    "110 checks",
    "xHCI, EHCI. No HID class, no mass-storage class, no USB networking"],
   ["Network",
    "RTL8139, Intel 82540 (e1000), ARP, IPv4, ICMP, UDP, TCP with six "
    "sockets and retransmission, DHCP, DNS with cache, HTTP",
    "Every NIC made after about 2008. RTL8169/8168, e1000e, I219"],
   ["Transport security",
    "TLS 1.2 with one RSA/CBC suite, MAC verification, bounds-checked "
    "certificate parsing",
    "ECDHE, AES-GCM, TLS 1.3, signature verification, a trust anchor store"],
   ["Display",
    "VGA text modes, a 1280x800 framebuffer console, VBE, mode 13h, an 8x8 "
    "font with a CP437 patch table",
    "Any grid other than the compiled one. No panel auto-detection"],
   ["Input", "PS/2 keyboard and mouse",
    "USB HID. I2C-HID. The HP has no PS/2 port of either kind"],
   ["Audio", "PC speaker",
    "AC97, HD Audio. The music player is parked behind all of it"],
   ["Kernel",
    "PMM, heap, PIT, RTC, IDT with working frame pointers since 0.8.3.1, "
    "entropy pool, klog",
    "A scheduler, processes, memory protection, UEFI runtime services"],
 ]),
 ("note",
  "The IDT fix in 0.8.3.1 matters more than its changelog entry suggests. "
  "Neither interrupt stub had ever passed the frame pointer to its handler, "
  "so <span class=\"cmd\">irq_handler</span> was selecting a function "
  "pointer by an index read out of low memory about a hundred times a "
  "second. Nothing depended on interrupts, because HavenDOS polls, so "
  "nothing reported it. Audio is the first thing in this plan that "
  "genuinely needs an IRQ &mdash; "
  "<span class=\"cmd\">include/uhci.h</span> says so in its own header "
  "comment &mdash; and it now has one that works."),
]),

# ══════════════════════════════════════════════════════════════════ 3
dict(title="What 1.0 means, and what 1.1 means", body=[
 ("p", "RM-1002&rsquo;s sentence stands unchanged. What changed is where the "
       "web sits relative to it."),
 ("box", "1.0",
       "HavenDOS installs itself onto a computer made in the last twenty "
       "years &mdash; whatever the disk is attached to &mdash; boots from it "
       "under BIOS or UEFI, fills the screen it is given, reaches the "
       "network, takes input, and does so without damaging whatever was on "
       "the machine beforehand."),
 ("p", "Six testable claims: <b>installs, boots, fills the screen, networks, "
       "takes input, harms nothing.</b> [[^Release gates]] turns each into "
       "something observable on hardware."),
 ("box", "1.1",
       "HavenDOS browses the web. Modern TLS first &mdash; X25519, AES-GCM, "
       "TLS 1.3, signature verification, a trust store &mdash; then a text "
       "browser on top of it."),
 ("p", "Splitting them this way was a decision, not an accident. 1.0 is a "
       "hardware release and it can be finished; the web arc is a second "
       "body of work of comparable size, and binding the two together means "
       "neither ships. The browser is not cut &mdash; it is "
       "[[1.1 &ldquo;Aegis&rdquo; and 1.2 &ldquo;Janus&rdquo; &mdash; the web]], "
       "written out in the same detail as everything before it."),
 ("p", "&ldquo;Every possible drive&rdquo; is scoped honestly as IDE/PATA, "
       "SATA through AHCI, NVMe, VirtIO and USB mass storage &mdash; with "
       "the AHCI qualification from [[^What changed since THS-RM-1002]] "
       "attached. SD, eMMC, SAS, hardware RAID and Intel VMD are out, and "
       "[[Deliberately not in 1.0]] says why."),
]),

# ══════════════════════════════════════════════════════════════════ 4
dict(title="How the releases are written", body=[
 ("p", "Each release below has the same five parts, so they can be compared "
       "and picked up in whatever order the dependencies allow."),
 ("table", ["Part", "What it answers"], [
   ["GOAL", "What a person can do after it that they could not before."],
   ["WORK", "Numbered items. Each is a sitting of work or close to it."],
   ["TEST", "How it is proved. A suite that cannot fail proves nothing."],
   ["DONE", "The acceptance check. Something observable, not &ldquo;it compiles&rdquo;."],
   ["RISK", "What could go wrong and what to do about it."],
 ]),
 ("h3", "The patch-release rule"),
 ("box", "DECIDED",
       "Every feature release is followed onto hardware, and whatever the "
       "hardware finds ships as a <b>patch release</b> &mdash; 0.8.5 then "
       "0.8.5.1, exactly as 0.8.3 was followed by 0.8.3.1. Fixes are not "
       "folded silently into the next feature."),
 ("p", "The reason is on the record: 0.8.3.1 fixed six bugs found in a "
       "single afternoon on metal, and <b>five of them had survived 1636 "
       "checks and eight injection harnesses</b>. If those six had been "
       "folded into the next feature release, the next regression would have "
       "had two causes to bisect between instead of one. A patch release is "
       "also permission to ship a feature before it is perfect, which is "
       "what keeps the fourteen below from becoming four."),
 ("p", "A patch release carries no new feature and no version-number "
       "ambiguity: <span class=\"cmd\">HAVENDOS_VER_NUM</span> is the single "
       "source of truth, <span class=\"cmd\">tools/check_iso_version.py</span> "
       "refuses a build whose ISO and kernel disagree, and that check stays "
       "green through a patch exactly as through a feature."),
]),

# ══════════════════════════════════════════════════════════════════ 5
dict(title="The release map", body=[
 ("table", ["Release", "Name", "Unlocks"], [
   ["0.8.3.1", "Lodger", "Additive UEFI install, AHCI, NVMe write &mdash; done"],
   ["0.8.4", "Hermes", "Update from the ESP &mdash; new versions reach the laptop"],
   ["0.8.5", "Atlas", "xHCI, the USB core, USB HID &mdash; keyboard and pointer"],
   ["0.8.6", "Argo", "USB mass storage &mdash; install from and to a stick"],
   ["0.8.7", "Iris", "USB Ethernet, RTL8169/8168, e1000e/I219"],
   ["0.8.8", "Helios", "Any grid, auto-detected, plus the viewport layer"],
   ["0.8.9", "Daedalus", "App port wave 1 &mdash; the screens nobody can skip"],
   ["0.9.0", "Hephaestus", "App port wave 2 &mdash; the remaining ~35 apps"],
   ["0.9.1", "Gaia", "FAT32 formatter &mdash; the end of the 2047 MB cap"],
   ["0.9.2", "Prometheus", "UEFI boot entry, and a system that can install itself"],
   ["0.9.3", "Orpheus", "AC97 and HD Audio &mdash; the music player unparked"],
   ["0.9.4", "Midas", "I2C-HID trackpad, if the measurement says it is needed"],
   ["0.9.5&ndash;0.9.9", "&mdash;", "Fuzzing, soak, hardware matrix, manual, freeze, RC"],
   ["1.0", "Olympus", "The six gates, on hardware"],
   ["1.1", "Aegis", "Modern TLS &mdash; X25519, AES-GCM, TLS 1.3, a trust store"],
   ["1.2", "Janus", "The browser"],
 ]),
 ("p", "Fourteen releases to 1.0 and two beyond it, each shipping a bootable "
       "ISO, a source archive and a changelog, as every milestone in this "
       "project has. Plus a patch release after any of them that hardware "
       "disagrees with."),
 ("crt",
  "  0.8.4 HERMES ...... every later release reaches the HP\n"
  "    |\n"
  "  0.8.5 ATLAS --> 0.8.6 ARGO --> 0.8.7 IRIS\n"
  "    |              USB storage    USB Ethernet + NICs\n"
  "    +-- 0.9.4 MIDAS ... only if the trackpad is I2C\n"
  "\n"
  "  0.8.8 HELIOS --> 0.8.9 DAEDALUS --> 0.9.0 HEPHAESTUS\n"
  "    any grid         wave 1            wave 2\n"
  "\n"
  "  0.9.1 GAIA, 0.9.2 PROMETHEUS, 0.9.3 ORPHEUS -- float\n"
  "\n"
  "  1.0 OLYMPUS --> 1.1 AEGIS --> 1.2 JANUS\n"),
 ("raw", "<div class='figcap'><span class='lab'>FIGURE 5-1</span>&nbsp;&nbsp;"
         "What blocks what. The USB chain is strictly ordered. The screen "
         "chain is strictly ordered. Gaia, Prometheus and Orpheus can move "
         "anywhere after 0.8.6. Nothing in the web arc can start before the "
         "crypto in 1.1.</div>"),
 ("note",
  "The order puts the HP first without making other people&rsquo;s machines "
  "an afterthought: it <b>updates</b> at 0.8.4, <b>takes input</b> at 0.8.5, "
  "<b>boots from a stick</b> at 0.8.6, <b>reaches a network</b> at 0.8.7 and "
  "<b>looks right</b> at 0.8.8. Breadth for desktops arrives inside 0.8.7 "
  "with the two Realtek and Intel gigabit families, and at 0.9.1 with "
  "partitions larger than 2 GB."),
]),

# ══════════════════════════════════════════════════════════════════ 6
dict(title="0.8.4 &ldquo;Hermes&rdquo; &mdash; the update path", body=[
 ("spec", [
   ("SIZE", "Small"),
   ("DEPENDS", "Nothing. The FAT32 reader and the ESP writer both exist"),
   ("UNLOCKS", "Every release after this one, on the only real machine"),
   ("RISK", "Low. It reads from a partition it already knows how to read"),
 ]),
 ("p", "GOAL. A new HavenDOS version is installed by copying one file onto "
       "the EFI System Partition from Windows and running "
       "<span class=\"cmd\">update</span> &mdash; no optical drive, no "
       "reinstall, and no second pass of the additive installer beside a "
       "live Windows partition."),
 ("p", "WORK."),
 ("ol", [
   "An <b>ESP source</b> for <span class=\"cmd\">fs/update.c</span>. The "
   "existing code is four steps bound to ATAPI: detect the drive, mount "
   "ISO9660, read <span class=\"cmd\">/version.txt</span>, stream "
   "<span class=\"cmd\">/boot/havendos.elf</span>. Split that into a source "
   "interface with two implementations &mdash; the existing ATAPI one, and a "
   "new one reading <span class=\"cmd\">\\EFI\\HAVENDOS\\update\\</span> off "
   "the FAT32 ESP. <b>Do not add a second block layer.</b> The ESP is found "
   "the way the installer finds it, by partition type GUID.",
   "A <b>manifest</b> beside the kernel: version, byte length, and a "
   "checksum of the image. <span class=\"cmd\">update</span> verifies the "
   "checksum before it overwrites anything, which the ATAPI path never did "
   "&mdash; it trusted the ISO.",
   "<b>Source auto-selection.</b> Prefer the ESP when it carries a newer "
   "manifest; fall back to ATAPI; say which one it chose on screen. The bus "
   "label lesson from 0.8.3.1 applies &mdash; one function decides, every "
   "screen asks it.",
   "The <b><span class=\"cmd\">ESP_E_BACKUP</span> fix.</b> "
   "<span class=\"cmd\">esp_install()</span> stops when "
   "<span class=\"cmd\">BOOTX64.HDB</span> already exists rather than "
   "deciding which of two backups matters. If the current "
   "<span class=\"cmd\">BOOTX64.EFI</span> is already our own loader, there "
   "is nothing worth backing up: skip the backup and carry on.",
   "A short <span class=\"cmd\">rescue/</span> note and a PowerShell helper "
   "that drops a new image into the ESP from Windows with "
   "<span class=\"cmd\">mountvol</span>, so the whole route is one "
   "documented command rather than folklore.",
 ]),
 ("p", "TEST."),
 ("ul", [
   "A new suite over the manifest parser and the version comparison, with "
   "an injection harness. <span class=\"cmd\">ver_cmp()</span> has shipped a "
   "bug before &mdash; <span class=\"cmd\">fs/update.c</span> once carried a "
   "hard-coded <span class=\"cmd\">\"0.5.9.14\"</span> and compared every "
   "ISO against it, so an <i>older</i> image applied as an upgrade.",
   "A model ESP built by the test, with the image present, absent, "
   "truncated, and checksum-wrong. Each must produce its own message.",
   "In QEMU under OVMF: install, write a newer image to the ESP from the "
   "host with <span class=\"cmd\">mtools</span>, boot, "
   "<span class=\"cmd\">update</span>, reboot, confirm the version.",
   "Byte-identity on the ESP before and after: the new file is added, and "
   "no existing file is touched except the kernel the update replaces.",
 ]),
 ("box", "DONE",
   "On the HP: copy one file into the ESP from Windows, boot HavenDOS, run "
   "<span class=\"cmd\">update</span>, and the machine comes back up on the "
   "new version with Windows still booting. A second install into the same "
   "gap completes without hand-restoring "
   "<span class=\"cmd\">BOOTX64.EFI</span>."),
 ("caution",
  "<span class=\"cmd\">update</span> overwrites a kernel on a disk that has "
  "Windows on it. The checksum is verified <b>before</b> the first byte is "
  "written, not after, and the old kernel is kept beside the new one until "
  "the new one has booted once."),
]),

# ══════════════════════════════════════════════════════════════════ 7
dict(title="0.8.5 &ldquo;Atlas&rdquo; &mdash; xHCI, the USB core and HID", body=[
 ("spec", [
   ("SIZE", "Large &mdash; the largest release before 1.0"),
   ("DEPENDS", "The MMIO seam, which 0.8.3.1 delivered"),
   ("UNLOCKS", "Input on a machine with no PS/2 port. And 0.8.6, and 0.8.7"),
   ("RISK", "xHCI bring-up is intricate and the HP is the only test target"),
 ]),
 ("p", "GOAL. HavenDOS enumerates devices on a USB 3 controller and takes "
       "keyboard and pointer input from them. On the HP &mdash; which has "
       "<span class=\"cmd\">8086:51ED</span> and no UHCI whatsoever &mdash; "
       "that is the difference between a usable machine and one that cannot "
       "be typed at."),
 ("h3", "The USB core comes first, and it is the real work"),
 ("box", "ARCHITECTURE",
   "<span class=\"cmd\">drivers/uhci.c</span> is controller-shaped all the "
   "way up: the device struct is <span class=\"cmd\">uhci_dev_t</span>, the "
   "transfers are <span class=\"cmd\">uhci_control()</span> and "
   "<span class=\"cmd\">uhci_bulk()</span>, and the enumeration walk lives "
   "inside the driver. Adding xHCI beside it would produce two device "
   "models, two enumeration walks, and two places that choose a bus &mdash; "
   "<b>which is exactly the mistake that cost 0.8.3.1 three of its six "
   "bugs.</b>"),
 ("p", "So this release introduces <span class=\"cmd\">usb/core.c</span>: "
       "one device table, one descriptor walk, one place that assigns "
       "addresses and chooses configurations, and a host-controller "
       "interface with <b>two</b> back-ends behind it from the day it is "
       "born. UHCI is refactored onto it in the same release, where its 110 "
       "existing checks catch any mistake, and only then is xHCI written. A "
       "registry born with one user is a wrapper; born with two it is an "
       "interface."),
 ("p", "WORK."),
 ("ol", [
   "<b><span class=\"cmd\">usb/core.c</span> and "
   "<span class=\"cmd\">include/usb.h</span>.</b> "
   "<span class=\"cmd\">usb_dev_t</span>, the standard descriptor walk, "
   "address assignment, configuration and interface selection, endpoint "
   "records, and <span class=\"cmd\">usb_control()</span> / "
   "<span class=\"cmd\">usb_bulk()</span> / "
   "<span class=\"cmd\">usb_interrupt()</span> dispatching to a back-end.",
   "<b>UHCI moved behind it</b>, with its existing suite green and its "
   "injection harness re-run. No behaviour change is permitted in this step; "
   "it is a refactor and it is proved as one.",
   "<b>xHCI bring-up:</b> BAR0 and the capability registers, the operational "
   "registers, CRCR and the command ring, DCBAA, the device context array, "
   "the event ring and its segment table, port registers, and a reset that "
   "waits on CNR rather than on a delay.",
   "<b>xHCI transfers:</b> Enable Slot, Address Device, Configure Endpoint, "
   "control transfers over the default pipe, bulk and interrupt endpoints, "
   "transfer rings with cycle-bit handling, and the event ring polled. "
   "<b>Polled, as everything else is</b> &mdash; the house rule that no I/O "
   "depends on an interrupt stands.",
   "<b>USB HID:</b> boot-protocol keyboard and boot-protocol mouse. Set "
   "Protocol to boot, poll the interrupt IN endpoint, and feed the existing "
   "keyboard and mouse queues so nothing above the driver changes. Boot "
   "protocol is a fixed 8-byte and 3-or-4-byte report; the full report "
   "descriptor parser is not in this release and is not needed for it.",
   "<b>A device view.</b> <span class=\"cmd\">usb -vv</span> already exists; "
   "it now lists every device on every controller with its class, speed, "
   "endpoints and chosen configuration. This is also the instrument that "
   "answers [[Open questions]] about the trackpad.",
 ]),
 ("p", "TEST."),
 ("ul", [
   "A <b>simulated xHCI controller</b> that executes commands, in the shape "
   "of <span class=\"cmd\">tests/test_ahci.c</span> and "
   "<span class=\"cmd\">tests/test_nvmerw.c</span> &mdash; rings that "
   "advance, cycle bits that flip, events that are posted. A mock returning "
   "success proves nothing and the tree has said so since 0.8.2.",
   "An injection harness on the core <i>and</i> on xHCI. Every harness "
   "written so far has found holes; assume this one does too.",
   "The UHCI refactor proved by its own unchanged suite, run before and "
   "after.",
   "A descriptor-walk suite with real captured descriptors: a hub, a "
   "composite device, a device with more endpoints than the table holds, and "
   "a device that STALLs the configuration request.",
 ]),
 ("box", "DONE",
   "A USB keyboard types at the HavenDOS shell prompt on the HP, with no "
   "PS/2 port in the machine, and <span class=\"cmd\">usb -vv</span> lists "
   "the devices actually plugged in rather than an invented list &mdash; the "
   "<span class=\"cmd\">check_boot_log.py</span> rule about invented output "
   "applies to this screen."),
 ("caution",
  "xHCI is the one release here with no fallback. If it does not come up on "
  "the HP, 0.8.6 and 0.8.7 have nothing to stand on, and the laptop has no "
  "input, no removable storage and no network. Budget a patch release for it "
  "from the start rather than discovering the need."),
]),

# ══════════════════════════════════════════════════════════════════ 8
dict(title="0.8.6 &ldquo;Argo&rdquo; &mdash; USB mass storage", body=[
 ("spec", [
   ("SIZE", "Medium"),
   ("DEPENDS", "0.8.5 &mdash; bulk transfers need endpoints, which need the walk"),
   ("UNLOCKS", "Install from a stick, install to a stick, update from a stick"),
   ("RISK", "A new bus in the block layer, which has bitten this tree twice"),
 ]),
 ("p", "GOAL. A USB stick is a disk. It can be read, written, installed "
       "from, installed to, and updated from. On a laptop with no optical "
       "drive this is the first removable medium HavenDOS has ever had."),
 ("p", "WORK."),
 ("ol", [
   "<b>Bulk-Only Transport.</b> Command Block Wrapper out, data in or out, "
   "Command Status Wrapper in; tag matching, residue handling, and the "
   "Get Max LUN request.",
   "<b>SCSI over BOT:</b> INQUIRY, TEST UNIT READY, READ CAPACITY (10 and "
   "16), READ (10), WRITE (10), REQUEST SENSE. Plus the retry a real stick "
   "needs on <span class=\"cmd\">UNIT ATTENTION</span> after enumeration, "
   "which is the single most common reason a first read fails.",
   "<b>Stall recovery:</b> Clear Feature HALT on the offending endpoint, "
   "then Bulk-Only Mass Storage Reset if that is not enough. Without it one "
   "bad command ends the session.",
   "<b>Registration as a block device</b>, through whatever "
   "<span class=\"cmd\">fs/fat16.c</span>, "
   "<span class=\"cmd\">fs/fat32.c</span> and the installer use to choose a "
   "bus &mdash; <b>all of them, in one commit.</b>",
   "<b>Removable-media semantics:</b> media present, media changed, and an "
   "installer that refuses a stick it is currently booted from.",
 ]),
 ("box", "THE RULE THIS RELEASE EXISTS TO HONOUR",
   "0.8.3.1&rsquo;s own post-mortem: <i>&ldquo;the installer has one block "
   "layer and the filesystem has another, and only one of them got "
   "taught.&rdquo;</i> Three of its six bugs were that mistake. <b>When you "
   "add a bus, grep for every place that chooses one</b> &mdash; and this "
   "release adds a bus. Expect at least four call sites and prove each of "
   "them with a test that reaches it."),
 ("p", "TEST."),
 ("ul", [
   "A simulated BOT device that executes SCSI commands, including the "
   "failure paths: short transfers, phase errors, stalls, "
   "<span class=\"cmd\">UNIT ATTENTION</span>, and a device that reports a "
   "capacity it cannot deliver.",
   "An injection harness over BOT and over the SCSI layer.",
   "A bus-selection suite in the shape of "
   "<span class=\"cmd\">tests/test_fat16nvme.c</span>: the model disk "
   "reachable <b>only</b> over USB, every other bus reporting nothing, and "
   "the filesystem required to find it anyway.",
   "In QEMU, with <span class=\"cmd\">-device usb-storage</span>: mount, "
   "read, write, <span class=\"cmd\">fsck.vfat</span> clean afterwards, and "
   "an install to the stick that boots.",
 ]),
 ("box", "DONE",
   "A FAT16 stick plugged into the HP is listed by "
   "<span class=\"cmd\">disk</span>, mounted, read and written; and an "
   "<span class=\"cmd\">update</span> image placed on that stick from "
   "Windows upgrades the machine."),
]),

# ══════════════════════════════════════════════════════════════════ 9
dict(title="0.8.7 &ldquo;Iris&rdquo; &mdash; three network adapters", body=[
 ("spec", [
   ("SIZE", "Large &mdash; three drivers, one of them over USB"),
   ("DEPENDS", "0.8.5 for the USB adapter; nothing for the two PCI ones"),
   ("UNLOCKS", "A network on the HP at all, and on most real desktops"),
   ("RISK", "Three chip families, none of which can be fully tested here"),
 ]),
 ("p", "GOAL. HavenDOS gets a DHCP address on hardware people actually own. "
       "Today it drives an RTL8139 and an Intel 82540 &mdash; both of them "
       "QEMU parts, neither of them in a machine made this decade."),
 ("h3", "Why three, and why these three"),
 ("table", ["Adapter", "Reaches", "Why it is in"], [
   ["USB Ethernet",
    "The HP, and every thin laptop with no RJ45",
    "The only route to a network on the one machine this plan is aimed at"],
   ["RTL8169 / 8168",
    "Most consumer desktops and older laptops",
    "The commonest gigabit NIC there is. RTL8139 is its ancestor, not it"],
   ["Intel e1000e / I219",
    "The other half of real desktops, 2008 onward",
    "The existing e1000 driver covers the 82540 only &mdash; a QEMU part"],
 ]),
 ("p", "<b>virtio-net was considered and left out.</b> It is cheap to add "
       "and helps testing, but it helps no physical machine, and this "
       "release is specifically about physical machines."),
 ("p", "WORK."),
 ("ol", [
   "<b>A NIC interface</b> in <span class=\"cmd\">net/</span>: open, "
   "close, MAC address, send a frame, poll for a frame, link status. Three "
   "drivers land at once, so the interface is born with three users and the "
   "lesson from 0.8.5 is applied rather than relearned. "
   "<span class=\"cmd\">net/ethernet.c</span> stops naming a chip.",
   "<b>USB Ethernet:</b> CDC-ECM first, because it is the standard and the "
   "simplest; then ASIX AX88179 and Realtek RTL8153, which is what most "
   "cheap dongles actually are. Bulk IN for receive, bulk OUT for transmit, "
   "the interrupt endpoint for link status.",
   "<b>RTL8169/8168:</b> descriptor rings in main memory, the "
   "<span class=\"cmd\">OWN</span> bit, transmit and receive poll "
   "registers, and the per-revision quirks table &mdash; this family has "
   "many revisions and they differ.",
   "<b>Intel e1000e / I219:</b> the same descriptor-ring shape as the "
   "existing e1000, with the newer register layout, PHY access over MDIC, "
   "and the device-ID table that separates them.",
   "<b>Driver selection:</b> one probe that walks PCI and USB, picks the "
   "first adapter it can drive, and reports which one on screen and in "
   "<span class=\"cmd\">ifconfig</span>. One function decides, every screen "
   "asks it.",
   "<b>The networking backlog</b>, which has been open since v0.7.15 and "
   "costs little once the stack is being touched: DHCP lease renewal, HTTP "
   "POST, chunked transfer, keep-alive, <span class=\"cmd\">wget</span> "
   "progress and resume, and <span class=\"cmd\">traceroute</span>. Also "
   "<span class=\"cmd\">dns_resolve()</span>, which still returns 0 both for "
   "a failed lookup and for the address 0.0.0.0.",
 ]),
 ("p", "TEST."),
 ("ul", [
   "A simulated descriptor-ring NIC that moves frames, with an injection "
   "harness, shared by the two PCI drivers.",
   "In QEMU, each PCI driver against its emulated part where one exists; "
   "the full stack exercised by <span class=\"cmd\">ping</span>, "
   "<span class=\"cmd\">nslookup</span>, <span class=\"cmd\">wget</span> and "
   "BOOT Chat.",
   "A loopback test for the USB Ethernet path that does not need the "
   "hardware: BOT-style framing over the simulated xHCI.",
   "On the HP with a real dongle: <span class=\"cmd\">ifconfig</span> shows "
   "a DHCP lease from the actual router.",
 ]),
 ("box", "DONE",
   "The HP, with a USB Ethernet adapter plugged in, gets a DHCP address and "
   "BOOT Chat connects &mdash; which is the 1.0 <b>networks</b> gate, met on "
   "the machine that could not meet it any other way."),
 ("note",
  "Buy the dongle before starting this release and check what chip it "
  "contains. A &pound;8 adapter is usually RTL8153 or AX88179; CDC-ECM-only "
  "parts are rarer than the specification would suggest. The driver written "
  "first should be the one in the adapter on the desk."),
]),

# ══════════════════════════════════════════════════════════════════ 10
dict(title="0.8.8 &ldquo;Helios&rdquo; &mdash; any grid, chosen at boot", body=[
 ("spec", [
   ("SIZE", "Medium"),
   ("DEPENDS", "Nothing. Can be done in parallel with the USB chain"),
   ("UNLOCKS", "0.8.9 and 0.9.0, and the <b>fills the screen</b> gate"),
   ("RISK", "A console that will not sync leaves no way to read a prompt"),
 ]),
 ("p", "GOAL. HavenDOS asks the firmware what the panel is, picks the "
       "largest grid that fits it, and renders the console at that size. On "
       "a 1080p laptop that is roughly 240x67 cells at 8x16, not 80x25 "
       "centred in the middle of the screen."),
 ("p", "WORK."),
 ("ol", [
   "<b>Mode discovery.</b> Take what GRUB hands over with "
   "<span class=\"cmd\">gfxpayload=keep</span>, and enumerate VBE modes as a "
   "fallback. Choose by panel size, not by a compiled constant.",
   "<b>A console sized at runtime.</b> "
   "<span class=\"cmd\">vga_rows()</span> and "
   "<span class=\"cmd\">vga_cols()</span> already exist and the desktop "
   "chrome already derives from them since v0.7.18. The shadow buffer, "
   "<span class=\"cmd\">fbc_painted[]</span>, the scroll stride, the cursor "
   "wrap and the tab stops all become dynamic. <b>v0.7.12 found five sites "
   "pinned to the compile-time 80-column constant</b>; assume there are more "
   "and grep for every one.",
   "<b>The viewport layer</b> &mdash; a push and pop that centres a "
   "sub-grid inside the real one and offsets every positioned draw. One flag "
   "per entry in the app registry says whether an app has been ported.",
   "<b>A confirm-or-revert timer</b> on any user-chosen mode, as v0.7.10 "
   "established. And remember <span class=\"cmd\">get_ticks()</span> counts "
   "10 ms units &mdash; that has been got wrong twice, once on this exact "
   "timer, where a &ldquo;10 second&rdquo; revert was really 100 seconds.",
   "<b>An 8x16 font option.</b> At 1920x1080 an 8x8 cell gives 240x135, "
   "which is unreadable. The font work must not repeat the v0.7.9 trap "
   "where uploading an 8x8 font to VGA plane 2 destroyed the BIOS 8x16 one.",
 ]),
 ("box", "WHY THE VIEWPORT IS NOT OPTIONAL",
   "Without it this release is a <b>regression</b>. A 240x67 console with 45 "
   "apps still drawing at 80x25 puts every one of them in the top-left "
   "corner of a very large screen, which is worse than the centred box they "
   "sit in today. The viewport is what makes 0.8.9 and 0.9.0 safe to do app "
   "by app instead of as one 550-site commit."),
 ("p", "TEST."),
 ("ul", [
   "A geometry suite over mode selection: 1920x1080, 1366x768, 1280x800, "
   "1024x768, 800x600, and a firmware that reports something absurd.",
   "A viewport suite: a draw at (0,0) inside an 80x25 viewport on a 240x67 "
   "grid lands at the centred offset, and a clipped draw clips.",
   "<span class=\"cmd\">harness/fbtext.py</span> reads the screen back by "
   "decoding pixels against the kernel&rsquo;s own font &mdash; it must keep "
   "working at every new grid size.",
   "<span class=\"cmd\">tools/check_screen_width.py</span> currently refuses "
   "a string literal past column 80. It stays: 80 columns remains the "
   "minimum supported width, and a wider screen must not become an excuse "
   "for text that breaks on a narrow one.",
 ]),
 ("box", "DONE",
   "The HP boots to a console that fills its panel, at a grid nobody told it "
   "to use, and every un-ported app still looks exactly as it does today."),
]),

# ══════════════════════════════════════════════════════════════════ 11
dict(title="0.8.9 &ldquo;Daedalus&rdquo; &mdash; app port, wave one", body=[
 ("spec", [
   ("SIZE", "Medium"),
   ("DEPENDS", "0.8.8 &mdash; there is no larger grid to port to without it"),
   ("UNLOCKS", "The screens a new user cannot avoid seeing"),
   ("RISK", "Low, and bounded: an app that fights back keeps its viewport"),
 ]),
 ("p", "GOAL. The screens nobody can skip use the whole display: the "
       "desktop, the shell, the installer, the first-boot setup wizard, the "
       "file manager, <span class=\"cmd\">help</span> and "
       "<span class=\"cmd\">man</span>."),
 ("p", "WORK. Each app in turn: replace absolute 80x25 coordinates with "
       "layout derived from <span class=\"cmd\">vga_cols()</span> and "
       "<span class=\"cmd\">vga_rows()</span>, clear its ported flag in the "
       "registry, and check it at 80x25, 120x40 and 240x67. The pagers in "
       "<span class=\"cmd\">help</span> and <span class=\"cmd\">man</span> "
       "need their page height derived rather than assumed &mdash; they are "
       "also the thing that breaks scripted testing, so fixing their "
       "geometry is worth more than it looks."),
 ("p", "TEST. A screenshot of each ported app at three grid sizes, read back "
       "with <span class=\"cmd\">harness/fbtext.py</span> and checked for "
       "the frame reaching the edges. The desktop menu&rsquo;s two-panel "
       "marker layout must be read by column, as the test notes already "
       "say."),
 ("box", "DONE",
   "A first boot on the HP &mdash; installer, setup wizard, desktop, shell "
   "&mdash; never shows an 80x25 box in the middle of a 1080p screen."),
]),

# ══════════════════════════════════════════════════════════════════ 12
dict(title="0.9.0 &ldquo;Hephaestus&rdquo; &mdash; app port, wave two", body=[
 ("spec", [
   ("SIZE", "Medium, and long rather than hard"),
   ("DEPENDS", "0.8.9"),
   ("UNLOCKS", "The <b>fills the screen</b> gate, completely"),
   ("RISK", "Tedium. Thirty-five apps is thirty-five chances to be careless"),
 ]),
 ("p", "GOAL. The remaining ~35 apps fill the grid: the clock, the "
       "dashboard, the monitor, the games, the screensavers, the chat "
       "client, <span class=\"cmd\">neofetch</span>, the package manager, "
       "the BOOT language editor and the rest."),
 ("p", "WORK. The same recipe as 0.8.9, app by app, in order of how often "
       "anyone opens them. An app that genuinely wants a fixed aspect &mdash; "
       "Tetris, mode 13h things &mdash; <b>keeps its viewport on purpose</b> "
       "and the registry records that it is finished rather than pending."),
 ("box", "THE CONTRACT THAT BREAKS FIRST",
   "Every one of these apps is a draw-poll-sleep loop, and the framebuffer "
   "console has exactly two commit points: "
   "<span class=\"cmd\">keyboard_waitchar()</span> flushes before it blocks, "
   "and <span class=\"cmd\">sleep_ms()</span> calls "
   "<span class=\"cmd\">vga_present()</span>. An app that polls without "
   "sleeping never commits a frame. Touching 35 draw loops is the most "
   "likely way to break that contract, and it was broken once already."),
 ("p", "TEST. The same three-grid screenshot check, plus a flicker check: "
       "<span class=\"cmd\">put_cell()</span> must skip a cell the screen "
       "already shows, in both branches. The asymmetry between them was the "
       "whole of the flicker in 0.8 Beta."),
 ("box", "DONE",
   "Every entry in the app registry is marked ported or deliberately fixed, "
   "and nothing in the desktop opens into a corner."),
]),

# ══════════════════════════════════════════════════════════════════ 13
dict(title="0.9.1 &ldquo;Gaia&rdquo; &mdash; the end of the 2 GB cap", body=[
 ("spec", [
   ("SIZE", "Medium"),
   ("DEPENDS", "Nothing. The FAT32 reader has been proved since 0.8.2"),
   ("UNLOCKS", "A HavenDOS partition the size of the disk it is on"),
   ("RISK", "It writes filesystem metadata. Byte-identity or nothing"),
 ]),
 ("p", "GOAL. A HavenDOS partition can be any size. Today "
       "<span class=\"cmd\">fat16_geom()</span> stops at 65524 clusters of "
       "64 sectors &mdash; <b>2047 MB</b> &mdash; so a 512 GB SSD gets a 2 GB "
       "HavenDOS and 510 GB of nothing, which is also what caused the E15 "
       "failure that 0.8.3.1 fixed."),
 ("p", "WORK."),
 ("ol", [
   "<b><span class=\"cmd\">fs/fat32fmt.c</span></b>, in the shape of "
   "<span class=\"cmd\">fs/fat16fmt.c</span>: BPB and the FAT32 extensions, "
   "FSInfo, the backup boot sector at sector 6, two FATs, a root directory "
   "that is an ordinary cluster chain, and cluster sizing by volume size as "
   "Microsoft&rsquo;s table specifies.",
   "<b>The installer offers FAT32</b> above a threshold and FAT16 below it, "
   "with the geometry computed and validated <b>before the first sector is "
   "written</b> &mdash; the rule 0.8.3.1 established after a failed install "
   "left a disk unbootable at step 4 of 9.",
   "<b>The FAT32 write path</b>, which the reader does not have: directory "
   "entry creation, cluster allocation through FSInfo, and long filenames.",
   "<b>GRUB and the boot chain</b> on a FAT32 root &mdash; the "
   "<span class=\"cmd\">search --label HAVENDOS</span> needs the root "
   "directory entry with attribute 0x08, not the BPB copy. That exact "
   "mistake has already been made once on FAT16.",
   "<b>The FAT16 dotfile bug</b>, carried since before 0.8 and now in two "
   "formatters&rsquo; worth of code: "
   "<span class=\"cmd\">name_to_83()</span> turns "
   "<span class=\"cmd\">.desktop</span> into a blank basename with extension "
   "<span class=\"cmd\">DES</span>. Nine files are affected. The FAT32 "
   "driver&rsquo;s 8.3 check is the shape the fix should take.",
 ]),
 ("p", "TEST. <span class=\"cmd\">fsck.vfat</span> judges every image, as it "
       "has since 0.8.2 &mdash; our code agreeing with itself is not "
       "evidence. A size sweep in the shape of "
       "<span class=\"cmd\">tests/test_wholedisk_size.c</span>, from the "
       "FAT32 minimum to 2 TB, asserting a valid geometry, a partition that "
       "fits, megabyte alignment, and an MBR or GPT entry describing the same "
       "extent the filesystem occupies. An injection harness on the "
       "formatter; the FAT16 one found 6 holes in 21."),
 ("box", "DONE",
   "An install to a 512 GB disk offers a partition larger than 2047 MB, "
   "formats it, boots from it, and <span class=\"cmd\">fsck.vfat</span> "
   "reports it clean."),
]),

# ══════════════════════════════════════════════════════════════════ 14
dict(title="0.9.2 &ldquo;Prometheus&rdquo; &mdash; a boot entry, and self-hosting", body=[
 ("spec", [
   ("SIZE", "Medium"),
   ("DEPENDS", "Nothing, though it is more useful after 0.8.6"),
   ("UNLOCKS", "Installing without Windows, and installing from HavenDOS"),
   ("RISK", "UEFI runtime services from a 32-bit kernel is the hard part"),
 ]),
 ("p", "GOAL. Two things that both read as &ldquo;the installer finished but "
       "you are not done yet&rdquo;."),
 ("h3", "A real firmware boot entry"),
 ("p", "HavenDOS writes the removable-media fallback "
       "<span class=\"cmd\">\\EFI\\BOOT\\BOOTX64.EFI</span>, and firmware "
       "with an existing boot order does not look for it. On the HP the "
       "entry had to be added <b>from Windows</b> with "
       "<span class=\"cmd\">bcdedit /copy {bootmgr}</span> pointed at "
       "<span class=\"cmd\">\\EFI\\HAVENDOS\\grubx64.efi</span>. A machine "
       "that needs another operating system to become bootable is not "
       "installed. The fix is a real NVRAM entry, which needs UEFI runtime "
       "services &mdash; <span class=\"cmd\">SetVariable</span> on "
       "<span class=\"cmd\">Boot####</span> and "
       "<span class=\"cmd\">BootOrder</span> &mdash; reached from a 32-bit "
       "kernel, which is the genuinely awkward part and should be "
       "investigated before the release is scheduled."),
 ("h3", "A system that can install itself"),
 ("p", "The two-pass build means pass 1 produces a kernel with no embedded "
       "image and pass 2 embeds <i>that</i> one, so the kernel the installer "
       "writes to a disk carries no image of its own. <b>An installed "
       "HavenDOS cannot install HavenDOS.</b> Before 0.8 this was discovered "
       "at step 7 of 9, after the disk had been repartitioned and formatted; "
       "<span class=\"cmd\">havendos_install_image_available()</span> is a "
       "precondition now, so it refuses safely &mdash; but it still refuses. "
       "The fix is a streaming install that reads the image from the running "
       "system&rsquo;s own boot partition or from removable media rather "
       "than from a copy compiled into the kernel."),
 ("box", "DONE",
   "A fresh install on the HP appears in the firmware boot menu without "
   "touching Windows; and an installed HavenDOS can install HavenDOS onto a "
   "second disk."),
]),

# ══════════════════════════════════════════════════════════════════ 15
dict(title="0.9.3 &ldquo;Orpheus&rdquo; &mdash; audio, and the player unparked", body=[
 ("spec", [
   ("SIZE", "Large"),
   ("DEPENDS", "0.8.6 &mdash; the player reads its files off a stick"),
   ("UNLOCKS", "The music player project, parked since 11 October 2026"),
   ("RISK", "The first thing in this kernel that genuinely needs an IRQ"),
 ]),
 ("p", "GOAL. HavenDOS plays audio. The MP3-from-USB player that was parked "
       "behind the road to 1.0 becomes buildable, because the storage half "
       "of it arrived at 0.8.6."),
 ("box", "A DECISION THE TREE ASKED TO BE MADE OUT LOUD",
   "<span class=\"cmd\">include/uhci.h</span> says it in its own header: "
   "<i>&ldquo;Audio will be the first thing in this kernel that genuinely "
   "needs an IRQ, and that is a decision to make out loud when it arrives, "
   "not to smuggle in here.&rdquo;</i> It has arrived. A codec consuming a "
   "ring buffer in real time cannot be driven by a polling loop that also "
   "has to draw a screen. <b>Audio takes an interrupt; nothing else does.</b> "
   "The IDT stubs were only fixed in 0.8.3.1, and UTM SE&rsquo;s TCG does "
   "not deliver IRQs reliably at all &mdash; so the driver must degrade to "
   "polled playback with a larger buffer when the interrupt does not "
   "arrive, and say which mode it is in."),
 ("p", "WORK."),
 ("ol", [
   "<b>AC97</b> &mdash; the simpler of the two, and what QEMU emulates: "
   "the mixer, bus-master descriptor lists, PCM out, and the interrupt on "
   "buffer completion.",
   "<b>HD Audio (Intel HDA)</b> &mdash; what every real machine since about "
   "2005 actually has, the HP included: CORB and RIRB, the codec walk, "
   "widget discovery, stream descriptors and BDL. Larger than AC97 and the "
   "reason this release is marked Large.",
   "<b>A mixer and an audio device interface</b>, so the two chips look the "
   "same from above and whatever comes third does too.",
   "<b>WAV playback</b>, then <b>MP3 decode</b> &mdash; fixed-point, because "
   "there is no FPU use anywhere in this kernel and introducing one here "
   "would be a surprise.",
   "<b>The player UI</b>, written for the grid the machine has rather than "
   "for 80x25, since 0.8.8 has landed by now.",
 ]),
 ("p", "TEST. A simulated codec that consumes a buffer and raises completion, "
       "with an injection harness. Decoder correctness against known-good "
       "PCM produced by <span class=\"cmd\">ffmpeg</span> on the build host "
       "&mdash; our code agreeing with itself is not evidence. Playback in "
       "QEMU with <span class=\"cmd\">-device AC97</span>; remember the "
       "<span class=\"cmd\">audiodev</span> must be declared before the "
       "device or QEMU refuses."),
 ("box", "DONE",
   "A WAV and an MP3 from a USB stick both play on the HP through its HD "
   "Audio codec, without the UI stuttering."),
]),

# ══════════════════════════════════════════════════════════════════ 16
dict(title="0.9.4 &ldquo;Midas&rdquo; &mdash; the trackpad, if it is I2C", body=[
 ("spec", [
   ("SIZE", "Medium, and conditional"),
   ("DEPENDS", "The measurement in [[Open questions]]"),
   ("UNLOCKS", "A laptop that works without anything plugged into it"),
   ("RISK", "The release may turn out to be unnecessary. That is a good risk"),
 ]),
 ("p", "GOAL. The HP&rsquo;s built-in trackpad works. Whether this release "
       "exists at all is decided by one boot of "
       "<span class=\"cmd\">usb -vv</span> after 0.8.5, and that boot should "
       "happen as early as possible."),
 ("table", ["What the measurement shows", "What happens"], [
   ["The trackpad enumerates as USB HID",
    "0.8.5 already fixed it. This release is cancelled and the slot is "
    "returned to the schedule"],
   ["It is I2C-HID behind Intel Serial-IO",
    "This release is written: the LPSS I2C controller, then HID-over-I2C"],
 ]),
 ("p", "WORK, if needed. The Intel LPSS I2C controller is a Synopsys "
       "DesignWare part behind a PCI function with an MMIO BAR &mdash; which "
       "the MMIO seam from 0.8.3.1 already makes testable. On top of it, the "
       "HID-over-I2C protocol: read the HID descriptor from the register "
       "the ACPI tables name, then input reports. <b>The awkward part is "
       "ACPI</b>: the descriptor address and the interrupt normally come "
       "from the DSDT, and this kernel has no ACPI parser. A minimal table "
       "walk to find one device is in scope; a general AML interpreter is "
       "not."),
 ("box", "DECIDED",
   "If the trackpad is I2C-HID, <b>the driver gets written</b>. RM-1002 "
   "listed it as out of 1.0 and that is reversed here: the difference "
   "between a laptop that works and one that needs a dongle permanently "
   "plugged into it is the difference between an operating system and a "
   "demonstration."),
 ("box", "DONE",
   "The pointer moves on the HP with nothing plugged into any port."),
]),

# ══════════════════════════════════════════════════════════════════ 17
dict(title="0.9.5 to 0.9.9 &mdash; stabilisation", body=[
 ("p", "Five releases with no new capability in them. This is the part of "
       "every plan that gets compressed when the end is in sight, and the "
       "part that decides whether 1.0 is a version number or a release."),
 ("table", ["Release", "What it is"], [
   ["0.9.5", "<b>Zero-warning build.</b> "
             "<span class=\"cmd\">desktop.c</span> alone had 17 warnings at "
             "3400 lines. Then the static-analysis pass"],
   ["0.9.6", "<b>Fuzzing.</b> FAT16 and FAT32 structures, the TCP/IP "
             "receive path, TLS records, and the shell parser. Each gets a "
             "corpus and a crash budget of zero"],
   ["0.9.7", "<b>Soak and performance.</b> Days of uptime, not minutes "
             "&mdash; which is also the only realistic way to catch the "
             "idle panic. Memory growth measured, not assumed"],
   ["0.9.8", "<b>The hardware matrix.</b> Every machine available, every "
             "bus, every install path, written down as a table that ships "
             "with the release"],
   ["0.9.9", "<b>Feature freeze and the release candidate.</b> The manual "
             "and its screenshots regenerated from a running system; "
             "documentation gaps closed; the RC built from the source "
             "archive in an empty directory"],
 ]),
 ("h3", "The carried bugs, all of which must close here or earlier"),
 ("ul", [
   "<b>The idle panic.</b> The laptop panicked once sitting at a shell "
   "prompt untouched. The broken IRQ frame pointer is the best candidate "
   "and is fixed; it has not been reproduced. The panic screen is truthful "
   "now, so if it recurs, take the EIP and run "
   "<span class=\"cmd\">nm -n havendos.elf</span>. <b>0.9.7&rsquo;s soak is "
   "the test that would find it.</b>",
   "<b><span class=\"cmd\">tests/test_fat16path.c</span> still skips</b> "
   "when <span class=\"cmd\">BOOTDISK</span> is unset, so "
   "<span class=\"cmd\">make test</span> never exercises it. Give it a "
   "generated fixture or delete it &mdash; a suite that reports nothing is "
   "worse than no suite. It also writes to the image it is pointed at, and "
   "once replaced a 631 KB kernel with a 2 KB test file.",
   "<b>The FAT16 dotfile bug</b>, if 0.9.1 did not already take it.",
   "<b>The security and login audit.</b> Credentials look plaintext in the "
   "tree. Whatever the answer is, 1.0 should state it rather than leave a "
   "reader to find out.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 18
dict(title="1.0 &ldquo;Olympus&rdquo;", body=[
 ("ul", [
   "The six gates in [[Release gates]], each on hardware rather than in "
   "QEMU, with the AHCI qualification stated in the release notes.",
   "The standing ones: full suite green, the five build checks passing, the "
   "help checker consistent, and the build reproducible from the source "
   "archive in an empty directory.",
   "Every carried bug in [[0.9.5 to 0.9.9 &mdash; stabilisation]] closed, or "
   "written down as a known limitation with its reason &mdash; which is how "
   "this tree has always handled the ones it could not close.",
   "The manual and its screenshots regenerated from a running system.",
   "A changelog that says what 1.0 cannot do as plainly as what it can.",
 ]),
 ("box", "WHAT 1.0 IS NOT",
   "1.0 is not the end of HavenDOS and it is not the browser. It is the "
   "point at which an operating system written by one person installs "
   "itself onto a modern laptop beside Windows, fills the screen, takes "
   "input from USB, reaches a network, and leaves everything else on the "
   "disk byte-identical. That is the thing worth shipping, and the web is "
   "the next thing after it."),
]),

# ══════════════════════════════════════════════════════════════════ 19
dict(title="1.1 &ldquo;Aegis&rdquo; and 1.2 &ldquo;Janus&rdquo; &mdash; the web", body=[
 ("p", "Written out here in full, because a thing deferred without a plan is "
       "a thing abandoned."),
 ("h3", "1.1 &ldquo;Aegis&rdquo; &mdash; modern TLS"),
 ("spec", [
   ("SIZE", "Large"),
   ("DEPENDS", "1.0, and a working network from 0.8.7"),
   ("UNLOCKS", "HTTPS that real servers accept &mdash; wget, curl, chat"),
   ("RISK", "Cryptographic code is correct or it is worthless"),
 ]),
 ("p", "The tree offers one cipher suite, "
       "<span class=\"cmd\">TLS_RSA_WITH_AES_128_CBC_SHA256</span>. What is "
       "missing, in the order it has to be built:"),
 ("ol", [
   "<b>X25519</b> &mdash; field arithmetic modulo 2<sup>255</sup>-19 and a "
   "Montgomery ladder. Compact, and the single most valuable piece: it is "
   "what makes ECDHE possible, and no modern server will negotiate without "
   "it or an equivalent.",
   "<b>AES-GCM</b> &mdash; counter mode over the existing AES, plus GHASH, "
   "which is a carry-less multiply in GF(2<sup>128</sup>). Constant-time, "
   "table-free.",
   "<b>HKDF and the TLS 1.3 key schedule</b> over the existing SHA-256.",
   "<b>The TLS 1.3 handshake</b> &mdash; which is <i>simpler</i> than 1.2, "
   "not harder: one round trip, everything after ServerHello encrypted, no "
   "renegotiation, no compression, no CBC padding oracle to get wrong. The "
   "three padding bugs v0.7.5 fixed cannot recur because the construction is "
   "gone.",
   "<b>Signature verification</b> &mdash; RSA-PSS and ECDSA on P-256, which "
   "is the part that actually makes a certificate mean something. The parser "
   "is already bounds-checked; what is missing is the verify.",
   "<b>A trust store</b> &mdash; a small pinned set of root CAs compiled "
   "in, with an explicit, visible way to say &ldquo;connect anyway&rdquo; "
   "that is never the default. This has been open since v0.7.7.",
 ]),
 ("note",
  "Test against real servers from day one, not against our own code. The "
  "existing TLS was broken against real servers in three separate ways "
  "&mdash; a record-splitting bug, a padding-semantics bug and a "
  "NewSessionTicket abort &mdash; and each only showed up against a server "
  "nobody here wrote."),
 ("h3", "1.2 &ldquo;Janus&rdquo; &mdash; the browser"),
 ("spec", [
   ("SIZE", "Large"),
   ("DEPENDS", "1.1. There is no point rendering pages it cannot fetch"),
   ("UNLOCKS", "The thing that was asked for"),
   ("RISK", "Scope. A browser is infinite unless its edges are drawn first"),
 ]),
 ("p", "<b>What it is:</b> a text browser on the HavenDOS console grid. "
       "HTTP/1.1 and HTTPS, HTML parsed into a document tree, block and "
       "inline layout to the grid width, links numbered and followed by "
       "keyboard, history, bookmarks, and forms with GET and POST. Redirects, "
       "chunked encoding, gzip, and character-set handling that reuses the "
       "CP437 work from v0.7.15."),
 ("p", "<b>What it is not, and this is written down before a line is "
       "typed:</b> no JavaScript, no CSS layout beyond "
       "<span class=\"cmd\">display</span> and a handful of inline hints, no "
       "images in the first release, no video, no fonts. The reference point "
       "is Lynx, not Chrome &mdash; and Lynx is genuinely useful, which is "
       "the argument for aiming there deliberately rather than arriving "
       "there disappointed."),
 ("p", "<b>Concurrency:</b> none. The decision taken is that the fetch loop "
       "calls the same present-and-poll functions the rest of the system "
       "uses, so the screen stays live and ESC cancels. HavenDOS polls "
       "everything; a browser does not get to be the exception, and the "
       "scheduler that would make it one is "
       "[[After 1.2 &mdash; the horizon]]."),
]),

# ══════════════════════════════════════════════════════════════════ 20
dict(title="Cross-cutting work", body=[
 ("h3", "One layer per concern, taught once"),
 ("box", "THE RULE",
   "<b>When you add a bus, grep for every place that chooses one.</b> Three "
   "of the six bugs found on metal in 0.8.3.1 were one mistake: the "
   "installer has one block layer and the filesystem has another, and only "
   "one of them got taught. This plan adds <b>a USB bus</b> (0.8.6), "
   "<b>three network adapters</b> (0.8.7) and <b>a second filesystem "
   "format</b> (0.9.1). Each is the same trap."),
 ("p", "The countermeasure is structural rather than diligent: 0.8.5 builds "
       "the USB core with two back-ends from birth, and 0.8.7 builds the NIC "
       "interface with three drivers from birth. An abstraction with one "
       "implementation is a wrapper and proves nothing; with two, the "
       "compiler enforces it."),
 ("h3", "The seams that make testing possible"),
 ("ul", [
   "<b><span class=\"cmd\">include/io.h</span></b> &mdash; port I/O behind "
   "six functions, which is how PCI enumeration and UHCI are tested without "
   "booting anything.",
   "<b><span class=\"cmd\">include/mmio.h</span></b> &mdash; delivered in "
   "0.8.3.1 and adopted by AHCI. <b>xHCI, HD Audio and the Intel I2C "
   "controller are all MMIO devices</b>, so all three are testable from the "
   "day they are written. This is the single most valuable thing 0.8.3.1 "
   "left behind.",
   "<b>The viewport layer</b> &mdash; 0.8.8 delivers it, and it is what "
   "makes 0.8.9 and 0.9.0 incremental rather than one 550-site commit.",
 ]),
 ("h3", "The build checks stay, and grow"),
 ("table", ["Check", "What it refuses"], [
   ["check_efi_embed.py", "A kernel with no UEFI loader in it, or only one copy"],
   ["check_screen_width.py", "A string literal written past column 80"],
   ["check_boot_log.py", "Invented boot output"],
   ["check_help.py", "A command that dispatches but is not documented"],
   ["check_iso_version.py", "An ISO whose kernel and version.txt disagree"],
 ]),
 ("p", "Two more are worth adding as the plan proceeds: one that refuses a "
       "new block-device bus not registered everywhere a bus is chosen, and "
       "one that refuses an app registry entry with no ported flag."),
]),

# ══════════════════════════════════════════════════════════════════ 21
dict(title="Sequencing", body=[
 ("p", "Four hard prerequisites. Everything else can move."),
 ("table", ["This", "cannot land before", "because"], [
   ["Everything", "0.8.4 Hermes",
    "otherwise every release reaches the HP by reinstalling beside Windows"],
   ["0.8.6 USB storage", "0.8.5 the USB core and xHCI",
    "bulk transfers need endpoints, which need a configuration walk"],
   ["0.8.7 USB Ethernet", "0.8.5 the USB core and xHCI",
    "same reason; the two PCI NICs in that release have no such dependency"],
   ["0.8.9 and 0.9.0 app port", "0.8.8 the console and viewport",
    "there is no larger grid to port to, and no way to not regress"],
 ]),
 ("box", "SEQUENCING RULE, CARRIED FROM RM-1001 AND RM-1002",
   "A write path does not land before the thing that protects it. NVMe write "
   "waited for the additive installer; USB mass-storage <i>write</i> waits "
   "for the same installer refusals to know about USB; the FAT32 formatter "
   "validates geometry before the first sector rather than at step 4 of 9. "
   "The refusals are the second line of defence, not the first."),
 ("p", "Three releases float: <b>0.9.1 Gaia</b>, <b>0.9.2 Prometheus</b> and "
       "<b>0.9.3 Orpheus</b> depend on nothing but 0.8.6, and any of them can "
       "move earlier if the USB chain stalls. <b>0.8.8 Helios</b> depends on "
       "nothing at all and is the obvious thing to do while waiting on a "
       "dongle to arrive or a controller to come up."),
]),

# ══════════════════════════════════════════════════════════════════ 22
dict(title="Release gates", body=[
 ("p", "The six claims in [[What 1.0 means, and what 1.1 means]], each "
       "turned into something observable. 1.0 ships when all six pass on "
       "hardware, not in QEMU."),
 ("table", ["Claim", "Gate", "Lands at"], [
   ["Installs",
    "Installs onto a GPT disk with another OS on it, from the installer UI, "
    "with no hand-editing", "0.8.3 &mdash; done"],
   ["Boots",
    "The machine boots the installed system under UEFI with no removable "
    "media attached <b>and no boot entry added from another OS</b>",
    "0.9.2 Prometheus"],
   ["Fills the screen",
    "The desktop and every app occupy the whole panel on a 1080p machine, "
    "at a grid chosen without being told", "0.9.0 Hephaestus"],
   ["Networks",
    "<span class=\"cmd\">ifconfig</span> shows a DHCP address and BOOT Chat "
    "connects", "0.8.7 Iris"],
   ["Takes input",
    "Keyboard and a pointing device both work on a machine with no PS/2 "
    "port", "0.8.5, or 0.9.4 if the trackpad is I2C"],
   ["Harms nothing",
    "Every sector outside the installed partition is byte-identical before "
    "and after, proved by checksum, and the other operating system still "
    "boots", "0.8.3 &mdash; done, and re-proved each release"],
 ]),
 ("box", "THE QUALIFICATION, STATED RATHER THAN IMPLIED",
   "<b>AHCI will ship at 1.0 having never run on real silicon.</b> There is "
   "no SATA machine available to this project. It is proved against the "
   "simulated controller in <span class=\"cmd\">tests/test_ahci.c</span>, a "
   "32-check suite whose injection harness found 12 holes, and against "
   "QEMU&rsquo;s q35 &mdash; and <span class=\"cmd\">q35 + if=ide</span> is "
   "AHCI, not IDE, which is how it is exercised at all. The release notes "
   "say this in the same words. A reader with a SATA desktop is the obvious "
   "person to ask, and the Discord is the obvious place to ask them."),
]),

# ══════════════════════════════════════════════════════════════════ 23
dict(title="Deliberately not in 1.0", body=[
 ("table", ["Left out", "Why"], [
   ["Modern TLS and the browser",
    "Not cut &mdash; moved. 1.1 and 1.2, written out in full. Binding them "
    "to 1.0 means neither ships"],
   ["Wi-Fi",
    "Signed firmware blobs per chipset and a full 802.11 MAC. The HP&rsquo;s "
    "MediaTek 14C3:7902 is exactly this problem, and a USB dongle is the "
    "honest answer at 1.0"],
   ["Intel VMD / RST",
    "A firmware setting solves it in seconds; a driver would take weeks and "
    "help only machines whose owner will not change that setting"],
   ["SD, eMMC, SAS, hardware RAID",
    "Each is a controller family of its own. &ldquo;Every drive&rdquo; is "
    "scoped to IDE, AHCI, NVMe, VirtIO and USB"],
   ["NTFS",
    "Reading a Windows partition is not needed in order to live beside one"],
   ["Secure Boot",
    "Requires a signed loader. Users can turn it off, and 1.0 does not need "
    "to solve it"],
   ["Multitasking",
    "Orthogonal to running on modern hardware, and large. The browser was "
    "the strongest argument for it and the polling decision removes that"],
   ["64-bit",
    "Invalidates every piece of 32-bit assembly and every driver&rsquo;s "
    "pointer assumptions, for no capability 1.0 claims"],
 ]),
 ("p", "All four of the last entries are named in "
       "[[After 1.2 &mdash; the horizon]] rather than left unmentioned, "
       "because an item with no plan is an item quietly dropped."),
]),

# ══════════════════════════════════════════════════════════════════ 24
dict(title="After 1.2 &mdash; the horizon", body=[
 ("p", "Not scheduled, and named on purpose. Each of these is larger than "
       "any single release in this document."),
 ("table", ["Next", "What it is, honestly"], [
   ["Multitasking and processes",
    "A preemptive scheduler, memory protection, real isolation. The thing "
    "that turns HavenDOS from an OS-shaped program into an operating "
    "system. Probably the first of these four to attempt, because "
    "everything else is easier on top of it"],
   ["64-bit, long mode",
    "More than 4 GB of RAM and a modern toolchain. Invalidates the "
    "bootstrap, the paging setup, every piece of assembly and every "
    "driver&rsquo;s assumption that a pointer fits a uint32_t &mdash; which "
    "is also what <span class=\"cmd\">-no-pie</span> on the driver suites "
    "exists to preserve today"],
   ["Wi-Fi",
    "One specific chipset with a firmware blob and an 802.11 MAC &mdash; "
    "probably <i>not</i> the HP&rsquo;s MT7902, which is new and poorly "
    "documented. Pick the chipset by what is documented, not by what is in "
    "the laptop"],
   ["Port shell, filesystem and network into HavenOS",
    "The long-stated plan: HavenDOS&rsquo;s userland becomes HavenOS&rsquo;s, "
    "and the graphical system becomes the main project. Everything built in "
    "this document is built once and used twice, which is the argument for "
    "keeping the layers clean while building them"],
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 25
dict(title="Open questions", body=[
 ("p", "Three, and each is one boot on the HP. Each changes the plan, so "
       "they are worth answering before the release they affect rather than "
       "during it. The fourth question in RM-1002 &mdash; whether the "
       "NVMe is hidden behind RST &mdash; is answered: it is visible, and "
       "the drive is in the boot log."),
 ("ol", [
   "<b>What bus is the trackpad on?</b> One run of "
   "<span class=\"cmd\">usb -vv</span> after 0.8.5. USB means 0.8.5 already "
   "fixed it and [[0.9.4 &ldquo;Midas&rdquo; &mdash; the trackpad, if it is "
   "I2C]] is cancelled. I2C-HID behind Intel Serial-IO means that release is "
   "written.",
   "<b>What does the firmware hand over with "
   "<span class=\"cmd\">gfxpayload=keep</span>?</b> Decides whether "
   "0.8.8&rsquo;s auto-detect sees 1920x1080 or something the panel scales. "
   "Answerable today, before 0.8.8 starts.",
   "<b>What chip is in the USB Ethernet adapter?</b> Not a measurement on "
   "the laptop but a decision about what to buy. It decides which of the "
   "three USB NIC drivers in 0.8.7 is written first, and the right answer is "
   "the one in the adapter already on the desk.",
 ]),
 ("note",
  "Questions 1 and 2 can both be answered in a single boot, and question 2 "
  "needs no new code at all. Doing them in the week 0.8.4 is being built "
  "costs nothing and removes two unknowns from the two largest releases."),
]),

# ══════════════════════════════════════════════════════════════════ 26
dict(title="How all of it is verified", body=[
 ("p", "None of this is new. It is what the tree already enforces, applied "
       "to fourteen more releases."),
 ("ul", [
   "<b><span class=\"cmd\">make test</span> green</b>, growing per release. "
   "1657 checks across 26 suites today.",
   "<b>Every new driver gets an injection harness.</b> Real bugs put into "
   "the code, each of which the suite must notice. Every harness written so "
   "far has found holes &mdash; FAT32 missed 10 of 26, ESP 8 of 16, the "
   "formatter 6 of 21, the additive installer 9 of 21, AHCI 12 of 32. "
   "<b>A suite that passes on its first run is the least trustworthy result "
   "available.</b>",
   "<b>A driver is tested against a controller that executes its "
   "commands</b>, not a mock that returns success. "
   "<span class=\"cmd\">tests/test_ahci.c</span> and "
   "<span class=\"cmd\">tests/test_nvmerw.c</span> are the template.",
   "<b>Our code agreeing with itself is not evidence.</b> "
   "<span class=\"cmd\">fsck.vfat</span>, mtools, sgdisk, parted, "
   "<span class=\"cmd\">ffmpeg</span> and a real web server judge the "
   "results.",
   "<b>Byte-identity, not &ldquo;it looks right.&rdquo;</b> Anything that "
   "writes to a disk is checked sector by sector, before and after.",
   "<b>A fixture a suite needs is generated by the build</b>, or the suite "
   "is decoration. One suite sat in the output with no result for three "
   "releases because its fixture never existed.",
   "<b>No invented output.</b> The boot-log checker enforces it in the boot "
   "path, and any new status screen is held to the same rule &mdash; which "
   "now includes <span class=\"cmd\">usb -vv</span>, "
   "<span class=\"cmd\">ifconfig</span> and the audio device list.",
   "<b>And, since 0.8.3.1: simulation answers only the questions you "
   "thought to ask.</b> Six bugs in one afternoon on metal says the "
   "remaining ones are in the places no harness models &mdash; bring-up "
   "state left by firmware, devices that exist but are the wrong kind, and "
   "paths nothing depends on until they fail. Every feature release in this "
   "document is followed onto hardware, and "
   "[[How the releases are written]] says what happens next.",
 ]),
 ("box", "THE SHORTEST VERSION OF THIS DOCUMENT",
   "Make new versions reachable (0.8.4). Make USB work, because it is "
   "input, storage and network on the only real machine (0.8.5&ndash;0.8.7). "
   "Make the screen fill (0.8.8&ndash;0.9.0). Finish the edges "
   "(0.9.1&ndash;0.9.4). Prove it (0.9.5&ndash;0.9.9). Ship it (1.0). "
   "Then build the web (1.1&ndash;1.2)."),
]),

]
