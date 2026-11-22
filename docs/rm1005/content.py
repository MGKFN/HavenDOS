# -*- coding: utf-8 -*-
"""THS-RM-1005 — the content.

The HavenDOS roadmap, from 0.9.6.5 to 29H2, laid out release by release:
what is in each, the work it takes, how it is accepted, what it depends
on. Same block vocabulary as RM-1003 and RM-1004 (see docs/rm1004):

    ("p", html) ("h3", text) ("ul", [...]) ("ol", [...]) ("crt", text)
    ("note", html) ("caution", html) ("box", label, html)
    ("table", [heads], [[cells], ...], [widths]) ("spec", [(k, v)]) ("raw", html)

[[Section title]] becomes "section N"; [[^Section title]] "Section N".

RM-1004 stays the numbering standard (its sections 1-11). Everything it
said about WHEN and WHAT is carried here, with the decisions Kyan took
on 16 November 2026 folded in.
"""

C = '<span class="cmd">%s</span>'


def cmd(s):
    return C % s


META = dict(
    ver="1005", channel="", codename="",
    title="HAVENDOS<br/>THE ROADMAP<br/>0.9.6.5 &rarr; 29H2",
    subtitle=("Every release from today to December 2029,<br/>"
              "laid out: what is in it, the work,<br/>"
              "how it is accepted, what it waits on"),
    part="THS-RM-1005 REV E",
    date="22 NOV 2026",
    author="Kyan Hoekstra / TechHaven Studios",
    baseline="v0.9.7 Beta &ldquo;Mnemosyne&rdquo;, 7654 checks across 75 suites",
    supersedes="THS-RM-1004 sections 12&ndash;22 (its numbering standard stands)",
    status="DECIDED TO 20 NOV 2026 &mdash; eight questions open (section 21)",
    cover_bars=[
        ("MONITOR",      18, 20, "lit, scaled, automatic"),
        ("MOUSE",        15, 20, "every app; HP to confirm"),
        ("VERSIONING",    3, 20, "0.9.7"),
        ("HTTPS",         4, 20, "0.9.8"),
        ("ONLINE UPDATE", 2, 20, "0.9.8, signed"),
    ],
)


# ── the release timeline, drawn from the dates rather than typed ─────
_RELEASES = [
    # name, dev from, testing from, stable (all as (year, month))
    ("27H1 BRANDARIS",   (2026, 11), (2026, 12), (2027, 1)),
    ("27H2 LANGE JAAP",  (2027, 1),  (2027, 9),  (2027, 12)),
    ("28H1 WESTHOOFD",   (2027, 7),  (2028, 3),  (2028, 6)),
    ("28H2 EIERLAND",    (2028, 1),  (2028, 9),  (2028, 12)),
    ("29H1 BORNRIF",     (2028, 7),  (2029, 3),  (2029, 6)),
    ("29H2 WESTKAPELLE", (2029, 1),  (2029, 9),  (2029, 12)),
]


def _idx(ym):
    return (ym[0] - 2026) * 12 + (ym[1] - 11)


def _timeline():
    lab, span = 18, _idx((2029, 12)) + 1
    axis = [" "] * span
    marks = [" "] * span
    for y in (2027, 2028, 2029):
        for m, ch in ((1, "J"), (7, "J")):
            marks[_idx((y, m))] = ch
        i = _idx((y, 1))
        axis[i:i + 4] = list(str(y))
    lines = [" " * lab + "".join(axis).rstrip(),
             " " * lab + "".join(marks).rstrip(),
             " " * lab + "+" + "-" * (span - 1)]
    for name, dev, test, stable in _RELEASES:
        row = [" "] * span
        for i in range(_idx(dev), _idx(test)):
            row[i] = "-"
        for i in range(_idx(test), _idx(stable)):
            row[i] = "="
        row[_idx(stable)] = "*"
        lines.append(name.ljust(lab) + "".join(row).rstrip())
    lines.append("")
    lines.append(" " * lab + "-  development   =  Beta to Release Preview   *  Stable")
    return "\n".join(lines)


# ── the 0.9.x road, drawn as a week grid from the same dates ──────────
_ROAD = [
    # label, from (month, day), to (month, day)  -- all 2026/27
    ("0.9.6.x  HP",       (11, 16), (11, 21)),
    ("0.9.7    VERS+UI",  (11, 18), (11, 20)),
    ("0.9.8    HTTPS",    (11, 27), (12, 5)),
    ("0.9.9    HARD+HDA", (12, 6),  (12, 12)),
    ("27100    CH30",     (12, 13), (12, 15)),
    ("27100    CH40",     (12, 16), (12, 31)),
]


def _day(md):
    m, d = md
    if m == 11:
        return d - 16
    if m == 12:
        return 14 + d
    return 45 + d                       # January


def _road():
    lab, span = 19, _day((1, 1)) + 1
    head = [" "] * span
    for md, s in (((11, 16), "16N"), ((11, 23), "23N"), ((12, 1), "1D"),
                  ((12, 12), "12D"), ((12, 16), "16D"), ((1, 1), "1J")):
        i = _day(md)
        head[i:i + len(s)] = list(s)
    out = [" " * lab + "".join(head).rstrip(), " " * lab + "+" + "-" * (span - 1)]
    for name, a, b in _ROAD:
        row = [" "] * span
        for i in range(_day(a), _day(b) + 1):
            row[i] = "#"
        out.append(name.ljust(lab) + "".join(row).rstrip())
    row = [" "] * span
    row[_day((1, 1))] = "*"
    out.append("27100    CH50".ljust(lab) + "".join(row).rstrip())
    out.append("")
    out.append(" " * lab + "one character is one day; * Stable, 1 January 2027")
    return "\n".join(out)


SECTIONS = [

# ══════════════════════════════════════════════════════════════════ 1
dict(title="What this document is", body=[
 ("p", "The official HavenDOS roadmap: every release from today&rsquo;s "
       "0.9.6.5 to 29H2 Westkapelle in December 2029, each with its content, "
       "the work it takes in the tree, how it is accepted, and what it waits "
       "on."),
 ("p", "It carries forward the plan of THS-RM-1004 (15 November 2026) and "
       "folds in what was decided and shipped on 16 November: five "
       "maintenance updates in a day, two of them by Kyan&rsquo;s choice "
       "carrying features, and update-over-the-internet moved from 27H2 into "
       "0.9.8."),
 ("spec", [
   ("STILL IN FORCE", "THS-RM-1004 sections 1&ndash;11: build numbers, release "
                      "IDs, channels, update levels, the display string, the "
                      "updater manifest"),
   ("REPLACED HERE", "THS-RM-1004 sections 12&ndash;22: the road, the "
                     "half-years, the audit, risks, open questions"),
   ("EDITABLE COPY", "the Claude doc &ldquo;HavenDOS Versioning Specification "
                     "and Roadmap to 29H2&rdquo;, and the project note "
                     "&ldquo;havendos-versioning-and-roadmap-decisions&rdquo;"),
   ("WHEN THEY DIFFER", "the one changed last is right; the other is brought "
                        "into line"),
 ]),
 ("note", "Dates are targets, not promises. [[^Risks]] says what moves "
          "when one slips, and [[Decisions still open]] lists what "
          "has not been decided."),
]),

# ══════════════════════════════════════════════════════════════════ 2
dict(title="Where HavenDOS stands", body=[
 ("p", "17 November 2026. The tree is at 0.9.6.5; every release below is on "
       "GitHub, published by the Release workflow, with a bundle zip."),
 ("table", ["Release", "Date", "What it did", "On the HP"], [
   ["0.9.6", "15 Nov", "Cooperative tasks, Alt+Tab, bar chips, "
    "jobs; " + cmd("migrate") + " from FAT16 to FAT32", "not yet run"],
   ["0.9.6.1", "16 Nov", cmd("resolution") + " with apps open; Welcome only "
    "when no account; BOOT Chat out of the menus; " + cmd("gfx try"),
    "photos sent"],
   ["0.9.6.2", "16 Nov", "Pipe B&rsquo;s buffer counted from slice 3: the "
    "HDMI monitor&rsquo;s first picture", "<b>confirmed</b>"],
   ["0.9.6.3", "16 Nov", "The monitor scaled to fit and lit by itself; the "
    "mouse in every app; the scroll wheel", "auto-on stuck"],
   ["0.9.6.4", "16 Nov", "A half-set-up pipe B cleared; an unscaled "
    "fallback that says why", "<b>&ldquo;works now&rdquo;</b>"],
   ["0.9.6.5", "16 Nov", "Apps are windows: title bar, minimize, full "
    "screen, X; the taskbar always; the mouse in Settings however opened",
    "<b>&ldquo;cool&rdquo;</b>; fixes in 0.9.6.6"],
   ["0.9.6.6", "17 Nov", "The Shell&rsquo;s X; full screen fills; mice and "
    "sticks plugged back in; the trackpad&rsquo;s memory window; the wheel "
    "in lists; the resolution at boot", "to test"],
 ], [12, 10, 58, 20]),
 ("h3", "What is proved, and how"),
 ("ul", [
   "<b>7401 checks across 71 suites,</b> every one able to fail: each "
   "suite has a floor, and the injection harnesses plant defects and "
   "require them caught (" + cmd("check_xemode_suite") + " 47/47, "
   + cmd("check_uimouse_suite") + " 14/14, " + cmd("check_window_suite")
   + " 9/9 among them).",
   "<b>QEMU boots:</b> " + cmd("window_apps") + " 11/11 and "
   + cmd("mouse_apps") + " 14/14 with a USB mouse; " + cmd("appguard_boot")
   + ", " + cmd("resolution_tasks") + ", " + cmd("hid_boot") + ", "
   + cmd("gfx_boot") + ".",
   "<b>The HP is the judge</b> of everything QEMU does not have: the Iris "
   "Xe display engine, the I2C trackpad, the iPhone tether, the disk "
   + cmd("migrate") + " writes.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 3
dict(title="What changed since RM-1004", body=[
 ("table", ["Date", "Decision", "Where it lands"], [
   ["16 Nov", "The monitor&rsquo;s picture is fitted keeping 4:3 by "
    "default, with Settings to stretch it", "0.9.6.3, shipped"],
   ["16 Nov", "A monitor plugged in lights by itself, at boot and later; "
    "a Settings switch turns it off", "0.9.6.3, shipped"],
   ["16 Nov", "The mouse in every app: click to pick and open, clickable "
    "key hints, the wheel, right-click for back", "0.9.6.3, shipped"],
   ["16 Nov", "Both of those ship in a maintenance update, bending the "
    "fixes-only rule once", "0.9.6.3"],
   ["16 Nov", "Apps are windows with minimize, full screen and X; the "
    "taskbar always visible; X behaves as ESC", "0.9.6.5, shipped"],
   ["16 Nov", "That ships now too: the rule bent a second time",
    "0.9.6.5"],
   ["16 Nov", "Update over the internet moves from 27H2 into 0.9.8, signed, "
    "keeping the ESP, USB and CD updates", "[[0.9.8: updating "
    "over the internet]]"],
   ["17 Nov", "Retro PC: the old builds on an emulated PC inside "
    "HavenDOS, starting with K-DOS Beta 1.1b. Built now, as exception E-1",
    "0.9.7, branch " + cmd("emu86")],
   ["17 Nov", "Full screen fills: an 80&times;25 app is drawn at the "
    "biggest cell the screen takes", "0.9.6.6, as a fix"],
   ["17 Nov", "Mouse selection and copy/paste in the editor (E-2), a "
    "right-click menu in Files (E-3), taskbar pins and smaller icons (E-4)",
    "0.9.7"],
   ["17 Nov", "Real audio through the speakers: Intel HD Audio (E-5), "
    "moved from 28H1", "0.9.9"],
   ["22 Nov", "Releases go out tested in QEMU, marked &ldquo;Not tested on "
    "a real machine&rdquo; until the HP has run them; no 0.9.6.8&ndash;.10",
    "0.9.7.1 onward"],
   ["22 Nov", "Retro PC gets a 32-bit 386 that boots the BOOT OS ISOs, "
    "as exception E-6", "0.9.7.2"],
 ], [11, 63, 26]),
 ("caution", "The fixes-only rule was bent twice in one day. From 0.9.7 it "
             "holds again: a feature asked for during a 0.9.x line goes into "
             "the next line, or is decided here, in writing, as an exception."),
 ("box", "EXCEPTION E-1 &mdash; RETRO PC IN 0.9.7, DECIDED 17 NOV 2026",
  "<p><b>What.</b> An app, " + cmd("retro") + ", that boots Kyan&rsquo;s "
  "old builds on an emulated PC: an 80186 with the 386&rsquo;s 16-bit "
  "additions, a BIOS, a floppy and a 504&nbsp;KB pretend hard disk kept as "
  "a file in " + cmd("RETRO") + ". The first image is K-DOS Beta 1.1b, the "
  "oldest build that survived.</p>"
  "<p><b>Why it is an exception.</b> The road to HavenDOS 1 lists every "
  "release&rsquo;s content, and Retro PC was on none of them. Kyan chose "
  "to build it in 0.9.x rather than leave it for 27H2.</p>"
  "<p><b>Its bounds.</b> It is built on the branch " + cmd("emu86") + " and "
  "lands with 0.9.7, never in a 0.9.6.N, so fixes from the HP still go out "
  "first. Outside its own files it touches only the app list, the shell "
  "command, the window&rsquo;s X and one USB keyboard mapping (F1&ndash;F4, "
  "now the same as PS/2). The emulated PC has no path to a real disk. More "
  "old builds may be added to " + cmd("archive/retro") + " in any later "
  "release, as content; new emulator features (graphics modes, sound, a "
  "DOS) are not covered and go to a half-year release.</p>"
  "<p><b>What it costs.</b> 0.9.7 moves from about 22 to about 24 "
  "November. 0.9.8 keeps its 3 December target and starts on 25 November, "
  "two days shorter; [[^Risks]] carries the consequence. (REV C moves "
  "these again, for E-2 to E-5.)</p>"),
 ("box", "EXCEPTIONS E-2 TO E-5 &mdash; DECIDED 17 NOV 2026",
  "<p><b>What.</b> Asked for with Kyan&rsquo;s 0.9.6.5 test and chosen, all "
  "four, to come before HavenDOS 1, with 1 January kept: &ldquo;we just "
  "need to test quicker&rdquo;.</p>"
  "<p><b>E-2, the editor:</b> select text with the mouse, Ctrl+C, Ctrl+X, "
  "Ctrl+V, one clipboard shared by every app. Not on any half-year "
  "before; 0.9.7.<br/>"
  "<b>E-3, Files:</b> right-click a file for open, rename, copy, move, "
  "delete. From 29H1; 0.9.7.<br/>"
  "<b>E-4, the taskbar:</b> pin apps to it; smaller desktop icons. From "
  "29H1; 0.9.7.<br/>"
  "<b>E-5, real audio:</b> an Intel HD Audio driver for the HP&rsquo;s "
  + cmd("8086:51C8") + ", so music plays through the speakers instead of "
  "the PC speaker&rsquo;s beeps. From 28H1; 0.9.9, beside the hardening, "
  "because it is the largest of the four.</p>"
  "<p><b>Its bounds.</b> Each is the feature named and nothing grown out "
  "of it: no overlapping windows (29H1), no file manager rewrite, no "
  "audio recording or volume keys (28H1).</p>"
  "<p><b>What it costs.</b> 0.9.7 to about 26 November, 0.9.8 to about 5 "
  "December, 0.9.9 and the freeze to about 12 December. CH30 on 13 "
  "December, CH40 from 16 December; <b>Stable stays 1 January.</b> The "
  "Release Preview fortnight is what absorbs it. [[^Risks]] lists HD Audio "
  "and the shorter Preview.</p>"),
 ("box", "EXCEPTION E-6 &mdash; A 386 FOR RETRO PC, DECIDED 22 NOV 2026",
  "<p><b>What.</b> Kyan&rsquo;s BOOT OS ISOs (0.3.1, 0.5.0, 0.5.2) are "
  "32-bit Multiboot kernels started by GRUB, which Retro PC&rsquo;s 8086 "
  "cannot run. 0.9.7.2 adds a second emulated PC: a 386 in flat protected "
  "mode with the 8259s, the 8254, the 8042 and VGA text, and a loader that "
  "does GRUB&rsquo;s job &mdash; the ISO&rsquo;s own " + cmd("grub.cfg") +
  " as the menu, the kernel loaded as GRUB loads it. The three kernels are "
  "built in; any such ISO boots from " + cmd("RETRO") + " on a stick.</p>"
  "<p><b>Its bounds.</b> 32-bit text mode. Graphics modes (mode 13h, VESA) "
  "and the 64-bit K-DOS kernels are not covered and are decided again "
  "later; Kyan chose &ldquo;later, after BOOT OS works&rdquo; for 64-bit.</p>"
  "<p><b>What it costs.</b> About a day, taken now at Kyan&rsquo;s choice "
  "(&ldquo;Now, as 0.9.7.2&rdquo;). 0.9.8 may slip a few days past 5 "
  "December; <b>Stable stays 1 January.</b></p>"),
]),

# ══════════════════════════════════════════════════════════════════ 4
dict(title="How a release is made", body=[
 ("ol", [
   "<b>The work, with tests that can fail.</b> A new behaviour gets a suite "
   "or checks in one; a suite whose count drops under its floor stops "
   + cmd("make test") + ". Risky code gets an injection harness.",
   "<b>" + cmd("include/version.h") + " is bumped,</b> and "
   + cmd("changelogs/CHANGELOG-v&lt;ver&gt;.md") + " written. "
   + cmd("HANDOFF.md") + " gets a START HERE section.",
   "<b>" + cmd("make") + " and " + cmd("make test") + " green,</b> then the "
   "QEMU harnesses that touch what changed.",
   "<b>" + cmd("tools/make_release_bundle.py") + "</b> builds the bundle: "
   "ISO (BIOS and UEFI), kernel and manifest, update stagers for Windows "
   "and Linux, source, handoff, changelog. It refuses a mismatched CRC, "
   "version or missing changelog.",
   "<b>Pushed to MGKFN/HavenDOS.</b> The Release workflow publishes when "
   + cmd("HAVENDOS_VER_NUM") + " changes and the changelog exists.",
   "<b>Kyan updates the HP</b> and photographs what it shows. Failures are "
   "diagnosed from the photos, never by asking for a downgrade, and the "
   "next release makes the failure explain itself on screen.",
 ]),
 ("spec", [
   ("0.9.x LINES", "0.9.6, 0.9.7, 0.9.8, 0.9.9; each gets .1 to .10, only as "
                   "many as needed, fixes only"),
   ("A FEATURE", "starts the next line, never a .N &mdash; except as section "
                 "[[What changed since RM-1004]] records"),
   ("FROM 0.9.7", "every release a GitHub pre-release on channel CH20"),
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 5
dict(title="The plan at a glance", body=[
 ("table", ["Release", "Target", "In one line"], [
   ["0.9.6.x", "to 21 Nov", "What the HP shows; nothing new"],
   ["0.9.7", "<b>20 Nov, released</b>", "The new versioning in the updater; Retro PC "
    "(E-1); the editor&rsquo;s clipboard (E-2); Files&rsquo; right-click "
    "menu (E-3); taskbar pins and smaller icons (E-4)"],
   ["0.9.8", "~5 Dec", "Real HTTPS, then updating over the internet"],
   ["0.9.9", "~12 Dec, freeze", "Hardening; real audio (E-5); the ship gate"],
   ["27100.1000", "13 Dec CH30<br/>16 Dec CH40<br/>1 Jan CH50",
    "HavenDOS 1, version 27H1 Brandaris"],
   ["27H2 Lange Jaap", "31 Dec 2027", "The web"],
   ["28H1 Westhoofd", "30 Jun 2028", "Daily use on the HP"],
   ["28H2 Eierland", "31 Dec 2028", "Wi-Fi"],
   ["29H1 Bornrif", "30 Jun 2029", "Desktop and files"],
   ["29H2 Westkapelle", "31 Dec 2029", "Polish and groundwork"],
 ], [24, 22, 54]),
 ("h3", "To HavenDOS 1, by day"),
 ("crt", _road()),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 5-1</span> '
         '16 November 2026 to 1 January 2027. Drawn by '
         '<span class="cmd">content.py</span> from the same dates as the '
         'table.</div>'),
]),

# ══════════════════════════════════════════════════════════════════ 6
dict(title="0.9.6.x: finishing on the HP", body=[
 ("p", "The line closes when the HP has shown each of these, or a .N has "
       "fixed what it showed. Nothing new goes in."),
 ("table", ["Check on the HP", "Since", "Doc in the bundle"], [
   ["Apps as windows: minimize, full screen, X, the chips, Start", "0.9.6.5",
    cmd("MOUSE-AND-MONITOR-HARDWARE-TEST")],
   ["Alt+Enter out of full screen (F11 may be a media key)", "0.9.6.5", "same"],
   ["Settings with the mouse, from the Start menu and the right-click "
    "menu", "0.9.6.5", "same"],
   ["The monitor scaled, not the unscaled fallback (" + cmd("dmesg")
    + " says)", "0.9.6.4", "same"],
   ["Unplug and replug: dark, then lit by itself", "0.9.6.3", "same"],
   ["Two-finger scrolling on the trackpad; a USB mouse&rsquo;s wheel",
    "0.9.6.3", "same"],
   ["Alt+Tab with the USB keyboard; the bar chips; music behind",
    "0.9.6", cmd("TASKS-HARDWARE-TEST")],
   [cmd("migrate") + "&rsquo;s list, then the move to FAT32 and "
    + cmd("migrate finish"), "0.9.6", cmd("MIGRATE-HARDWARE-TEST")],
   [cmd("resolution") + " changed on the HP&rsquo;s own panel", "0.9.6.1",
    cmd("GFX-ON-HARDWARE-TEST")],
 ], [56, 12, 32]),
 ("note", "Closes F-008 of the audit: &ldquo;0.9.6 not yet on the HP&rdquo;."),
]),

# ══════════════════════════════════════════════════════════════════ 7
dict(title="0.9.7: the new versioning in the updater", body=[
 ("spec", [("TARGET", "Released 20 November 2026 as &ldquo;Mnemosyne&rdquo;, with exceptions E-1 to E-4, six days early"), ("CHANNEL", "CH20 Beta, the "
           "first pre-release"), ("STANDARD", "THS-RM-1004 sections 1&ndash;11")]),
 ("p", "The updater learns the numbers HavenDOS 1 will carry, so that by "
       "the switch every installed 0.9.x already understands them. Nothing a "
       "person sees changes yet except " + cmd("ver") + " and the channel."),
 ("h3", "The work"),
 ("ul", [
   cmd("include/version.h") + ": product, release, codename, build, revision; "
   "the channel macro becomes a fallback only.",
   cmd("fs/update_src.c") + ", " + cmd("fs/update.c") + ": the keys "
   + cmd("product") + ", " + cmd("release") + ", " + cmd("codename") + ", "
   + cmd("build") + ", " + cmd("revision") + ", " + cmd("level") + ", "
   + cmd("channel") + "; a manifest for a lower channel than the "
   "subscription is refused, by name.",
   cmd("update channel") + " and a Settings row: the subscription, "
   "default Stable, kept in " + cmd("/BOOT/CHANNEL.INF") + " beside the "
   "kernel.",
   "One function draws the version: boot splash, " + cmd("ver") + ", System, "
   "Settings &mdash; &ldquo;HavenDOS 1, version 27H1 (OS Build "
   "27100.1000)&rdquo;, the channel shown only off Stable.",
   "The tools: " + cmd("make_iso_manifest.py") + ", " + cmd("make_version.py")
   + ", " + cmd("make_release_bundle.py") + " and the workflow learn the "
   "keys, the ISO name " + cmd("HavenDOS-1-27H1.iso") + ", the tag "
   + cmd("27100.1000") + " and the pre-release flag.",
   "<b>Retro PC, exception E-1:</b> " + cmd("emu/cpu86.c") + ", "
   + cmd("emu/pcbios.c") + ", " + cmd("shell/retro.c") + " and the images "
   "in " + cmd("archive/retro") + ", merged from the branch " + cmd("emu86")
   + ". Built on 17 November; what remains is the merge and the HP.",
   "<b>The editor, exception E-2:</b> drag to select, Shift+arrows, "
   "Ctrl+C/X/V and Ctrl+A, one clipboard every app can read; the editor "
   "reads the mouse every pass, not only after a key.",
   "<b>Files, exception E-3:</b> a right-click menu on a file or folder: "
   "open, rename, copy, move, delete (with a confirmation), new folder.",
   "<b>The taskbar, exception E-4:</b> pinned apps beside Start, kept in "
   "the user&rsquo;s settings; a smaller desktop icon size in Settings.",
 ]),
 ("h3", "Accepted when"),
 ("ul", [
   cmd("test_vercmp") + " proves 0.9.9.10 &lt; 27100.1000 &lt; 27100.1001 "
   "&lt; 27101.1000, and the 0999/1000 boundary of local builds.",
   cmd("test_update") + " proves every key, every refusal and the "
   "subscription; an injection suite catches each refusal removed.",
   "An installed 0.9.6.5 offered a 0.9.7 manifest takes it; 0.9.7 offered a "
   "manifest for a channel it is not subscribed to says so and stops.",
   cmd("test_emu86") + " (86 checks) passes; the CPU matches all 2,522,158 "
   "cases of the SingleStepTests 8088 suite it can be held to; "
   + cmd("check_emu86_suite") + " catches its 28 planted defects and misses "
   "the harmless one; " + cmd("retro_boot") + " boots K-DOS in QEMU, logs in "
   "and runs commands. K-DOS&rsquo;s screens and disk writes match K-DOS on "
   "QEMU&rsquo;s own PC.",
   "On the HP, Retro PC boots K-DOS Beta 1.1b, logs in as admin, and F4 "
   "and the window&rsquo;s X both come back to HavenDOS.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 8
dict(title="0.9.8: real HTTPS", body=[
 ("spec", [("TARGET", "~5 December 2026, with [[0.9.8: updating "
           "over the internet]]"), ("CLOSES", "audit F-007")]),
 ("p", "Today " + cmd("net/tls.c") + " offers one cipher suite, "
       "TLS_RSA_WITH_AES_128_CBC_SHA256, and checks no certificate. Modern "
       "servers, GitHub&rsquo;s among them, want forward-secret key "
       "exchange. 0.9.8 replaces the handshake and adds the checks."),
 ("h3", "The work"),
 ("ol", [
   "<b>TLS 1.2 with ECDHE:</b> X25519 key exchange; AES-128-GCM; SHA-256 "
   "and HMAC; the ServerKeyExchange signature checked.",
   "<b>Signatures:</b> RSA PKCS#1 v1.5 and PSS on the existing bignum code; "
   "ECDSA P-256.",
   "<b>The chain:</b> every certificate&rsquo;s signature checked up to one "
   "of about ten bundled roots, chosen at the start of the release from the "
   "sites HavenDOS actually visits.",
   "<b>Hostname and expiry:</b> subjectAltName matched, including one-level "
   "wildcards; validity against the clock.",
   "<b>Every refusal named on screen:</b> which certificate, which check, "
   "and for expiry what the clock reads &mdash; a wrong RTC fails every "
   "site.",
   "<b>" + cmd("wget") + " and " + cmd("http_get()") + " follow "
   "redirects</b> across hosts, as GitHub&rsquo;s release downloads need.",
 ]),
 ("h3", "Accepted when"),
 ("ul", [
   "Known-answer tests for X25519, GCM and P-256 from the RFCs and NIST "
   "vectors; a recorded handshake replayed byte for byte.",
   "An injection suite catches each check removed: chain, hostname, expiry, "
   "signature.",
   "On the HP, over the iPhone tether: " + cmd("wget") + " of a file from "
   "GitHub succeeds; a self-signed and an expired test certificate are "
   "refused by name.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 9
dict(title="0.9.8: updating over the internet", body=[
 ("spec", [("DECIDED", "16 November 2026, moved from 27H2"),
           ("WAITS ON", "[[0.9.8: real HTTPS]]"),
           ("KEEPS", "the ESP, USB stick and CD/ISO updates, unchanged")]),
 ("p", "With a working HTTPS client the HP can update itself over the "
       "iPhone tether: no stick, no second computer."),
 ("h3", "How it works"),
 ("ol", [
   cmd("update online") + " fetches " + cmd("update.inf") + " and "
   + cmd("update.inf.sig") + " for the subscribed channel from the "
   "GitHub release.",
   "<b>The signature is checked first.</b> RSA-2048, PKCS#1 v1.5 over "
   "SHA-256, against a public key compiled into the kernel. No signature, "
   "or a wrong one: refused, and nothing else is fetched.",
   "The manifest is compared exactly as a local update&rsquo;s is: newer, "
   "the right channel, the right product.",
   cmd("havendos.elf") + " is downloaded, its length and CRC-32 checked "
   "against the signed manifest, then installed by the same code as every "
   "other source &mdash; old kernel kept for the boot menu.",
 ]),
 ("box", "WHY A SIGNATURE, AND NOT ONLY HTTPS",
  "HTTPS proves the file came from GitHub. The signature proves it came "
  "from the release workflow &mdash; a release is a kernel that runs with "
  "every privilege, so the key, not the transport, is what a person "
  "trusts. A signed manifest stays safe even if a root certificate is "
  "wrong."),
 ("h3", "The work"),
 ("ul", [
   "The release workflow signs " + cmd("update.inf") + " with a private "
   "key held as a GitHub Actions secret, and uploads the signature beside "
   "it.",
   "The public key goes into " + cmd("include/") + " and the kernel; "
   + cmd("make_release_bundle.py") + " refuses a bundle whose signature "
   "does not verify.",
   cmd("update online") + ", and an &ldquo;Online&rdquo; source in the "
   "updater&rsquo;s list beside ESP, USB and CD.",
 ]),
 ("h3", "Accepted when"),
 ("ul", [
   "Tests: a good signature accepted; a byte changed in the manifest, a "
   "wrong key and a missing signature each refused by name.",
   "On the HP: 0.9.8 updates itself to a test release over the tether, and "
   "boots it.",
 ]),
 ("caution", "Who generates the signing key, and where its backup lives, is "
             "open ([[Decisions still open]]). A lost key means a new "
             "one shipped by stick; a leaked one means anyone can sign a "
             "kernel."),
]),

# ══════════════════════════════════════════════════════════════════ 10
dict(title="0.9.9: hardening and the freeze", body=[
 ("spec", [("TARGET", "~12 December 2026; the freeze"),
           ("AFTER IT", "only release-blocking fixes")]),
 ("h3", "One item per audit finding"),
 ("ul", [
   "<b>Memory a task allocated is freed</b> on exit, kill and crash; each "
   "allocation records its owner (F-004).",
   "<b>One UDP receive slot per task</b> (F-003).",
   "<b>" + cmd("wget") + " and " + cmd("http_get()") + " yield between "
   "reads,</b> so a download is a real background job (F-001).",
   "<b>A stall watchdog</b> names any task that runs too long without "
   "yielding.",
   "<b>A spawn-and-kill stress test</b> to heap exhaustion, all eight slots "
   "full (F-006).",
   "<b>" + cmd("docs/SECURITY.md") + "</b>: how passwords are stored, what "
   "HTTPS and the updater check, where randomness comes from, that apps "
   "are not isolated (F-002).",
 ]),
 ("h3", "Real audio, exception E-5"),
 ("ul", [
   "<b>An Intel HD Audio driver</b> for the HP&rsquo;s " + cmd("8086:51C8")
   + ": the controller&rsquo;s CORB and RIRB, the codec found and its "
   "speaker path set up, one output stream from a buffer.",
   "<b>The music player plays through it,</b> the PC speaker staying as the "
   "fallback when no codec answers.",
   "<b>Accepted when</b> a suite drives the controller and codec against a "
   "model, an injection harness catches its defects, and the HP plays a "
   "file through its speakers.",
 ]),
 ("box", "THE SHIP GATE &mdash; ALL FOUR, ON ONE BUILD",
  "1. The six gates of RM-1003 &mdash; installs, boots, fills the screen, "
  "networks, takes input, harms nothing &mdash; recorded on the HP with "
  "photos.<br/>2. Every app opened, used and closed on the HP.<br/>"
  "3. No known bug that loses data, hangs or crashes; every known limit "
  "written down.<br/>4. A second tester runs the Release Preview on their "
  "own machine."),
]),

# ══════════════════════════════════════════════════════════════════ 11
dict(title="HavenDOS 1: from 0.9.9 to Stable", body=[
 ("ol", [
   "<b>0.9.9.N is frozen,</b> about 12 December.",
   "<b>" + cmd("version.h") + " changes once:</b> product 1, release 27H1, "
   "codename Brandaris, build 27100, revision 1000. Nothing else in the "
   "source changes.",
   "<b>27100.1000 is published as CH30 Public Beta</b> on 13 December. An "
   "installed 0.9.9 sees 27100 &gt; 0 and offers it &mdash; over the "
   "internet, since 0.9.8.",
   "<b>From 16 December it is CH40 Release Preview:</b> the same kernel; "
   "only the manifest, the channel file and the release title change.",
   "<b>On 1 January 2027 it is CH50 Stable,</b> marked latest. A fix found "
   "in Release Preview becomes 27100.1001 and restarts the 14 days: the "
   "date moves rather than a bug ships.",
 ]),
 ("spec", [("ISO", cmd("HavenDOS-1-27H1.iso")), ("TAG", cmd("27100.1000")),
           ("SHOWN AS", "HavenDOS 1, version 27H1 (OS Build 27100.1000)")]),
]),

# ══════════════════════════════════════════════════════════════════ 12
dict(title="Six releases, one every half-year", body=[
 ("p", "Brandaris on 1 January 2027, then one release every six months to "
       "Westkapelle in December 2029. Each one&rsquo;s development overlaps "
       "the previous one&rsquo;s testing."),
 ("crt", _timeline()),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 12-1</span> '
         'One character is one month, from November 2026.</div>'),
 ("spec", [
   ("H1 RELEASE", "development Jul&ndash;Feb, Beta Mar (feature freeze), "
                  "Public Beta Apr, Release Preview May&ndash;Jun, Stable by 30 Jun"),
   ("H2 RELEASE", "development Jan&ndash;Aug, Beta Sep (feature freeze), "
                  "Public Beta Oct, Release Preview Nov&ndash;Dec, Stable by 31 Dec"),
   ("BETWEEN", "the Stable release gets Level 1 updates, and Level 2 until "
               "the next release&rsquo;s first public build exists"),
   ("DELIVERY", "every one of them over the internet, from 0.9.8 on"),
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 13
dict(title="27H2 Lange Jaap: the web", body=[
 ("spec", [("STABLE BY", "31 December 2027"), ("BUILDS", "from 27500"),
           ("RULE", "ships when all of it is done; nothing moves to 28H1"),
           ("WAITS ON", "0.9.8&rsquo;s HTTPS")]),
 ("ol", [
   "<b>Background downloads:</b> a Downloads app; " + cmd("wget") + " as a "
   "job with progress on the taskbar.",
   "<b>Browser, stage 1 &mdash; text:</b> headings, links, lists, tables, "
   "forms and colours, in the HavenDOS look; a window like any other app.",
   "<b>Haven News:</b> what is new, release notes, and this build against "
   "the newest in your channel.",
   "<b>IRC:</b> Libera.Chat over TLS, with a #havendos channel.",
   "<b>LAN chat:</b> HavenDOS machines on one network find each other and "
   "chat directly, with no server.",
   "<b>Package store:</b> " + cmd("bpkg") + " installs from a signed list "
   "on GitHub, with the updater&rsquo;s key and code.",
   "<b>Browser, stage 2 &mdash; images:</b> PNG, JPEG and GIF.",
   "<b>Email:</b> IMAP and SMTP over TLS.",
   "<b>Browser, stage 3 &mdash; CSS:</b> layout, fonts and colours from "
   "stylesheets.",
 ]),
 ("note", "Update over the internet was this release&rsquo;s first item; "
          "it is 0.9.8&rsquo;s now. IRC, Haven News and LAN chat together "
          "replace BOOT Chat, whose server is shut down until further "
          "notice."),
]),

# ══════════════════════════════════════════════════════════════════ 14
dict(title="28H1 Westhoofd: daily use on the HP", body=[
 ("spec", [("STABLE BY", "30 June 2028"), ("BUILDS", "from 28100")]),
 ("ol", [
   "<b>An ACPI interpreter (AML).</b> Battery, brightness, lid and sleep all "
   "live in the firmware&rsquo;s ACPI methods, so it comes first &mdash; "
   "and it is the largest single item.",
   "<b>Real sound:</b> Intel HD Audio and the HP&rsquo;s codec; the Music "
   "Player plays MP3s through the speakers and the jack.",
   "<b>Battery and charging</b> on the taskbar, with a low warning.",
   "<b>Brightness and volume keys.</b>",
   "<b>Sleep on lid close, and waking.</b> Every driver must restart after "
   "resume &mdash; the display engine&rsquo;s pipe B included.",
   "<b>A Wi-Fi research build, CH00 only:</b> read the MediaTek chip and "
   "load its firmware, so 28H2 starts from facts.",
 ]),
 ("caution", "Sleep and resume is the riskiest item in this plan. Whether "
             "28H1 waits for it is open."),
]),

# ══════════════════════════════════════════════════════════════════ 15
dict(title="28H2 Eierland: Wi-Fi", body=[
 ("spec", [("STABLE BY", "31 December 2028"), ("BUILDS", "from 28500"),
           ("CHIP", "MediaTek " + cmd("14C3:7902") + ", the HP&rsquo;s only "
                    "network card")]),
 ("ol", [
   "<b>The chip</b> up over PCIe, its firmware loaded.",
   "<b>Scanning:</b> the networks around, with signal strength.",
   "<b>Open networks,</b> and DHCP over Wi-Fi.",
   "<b>WPA2-Personal:</b> the four-way handshake, PBKDF2, AES-CCMP.",
   "<b>WPA3:</b> SAE, on 0.9.8&rsquo;s elliptic-curve code.",
   "<b>School and work (eduroam):</b> WPA2-Enterprise, EAP over 0.9.8&rsquo;s "
   "TLS.",
   "<b>Wi-Fi in Settings:</b> saved networks, joining by itself; the "
   "taskbar shows the signal.",
 ]),
 ("note", "The firmware blob ships as it is, with MediaTek&rsquo;s licence "
          "beside it. The only reference for the chip is Linux&rsquo;s mt76 "
          "driver."),
]),

# ══════════════════════════════════════════════════════════════════ 16
dict(title="29H1 Bornrif: desktop and files", body=[
 ("spec", [("STABLE BY", "30 June 2029"), ("BUILDS", "from 29100"),
           ("STARTS FROM", "0.9.6.5&rsquo;s windows: a title bar per app, "
                           "the taskbar, the pointer on the whole screen")]),
 ("ol", [
   "<b>Overlapping windows:</b> several visible at once, moved by their "
   "title bars, resized, stacked. Every task has had its own console since "
   "0.9.6 and its own box since 0.9.6.5; this draws more than one.",
   "<b>A file manager:</b> disks, sticks and folders; copy, move, rename, "
   "delete; open in the right app.",
   "<b>Open and Save dialogs,</b> one, shared by every app.",
   "<b>Wallpapers and themes,</b> on 27H2&rsquo;s image decoders.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 17
dict(title="29H2 Westkapelle: polish and groundwork", body=[
 ("spec", [("STABLE BY", "31 December 2029"), ("BUILDS", "from 29500")]),
 ("ol", [
   "<b>Polish and speed:</b> bugs, performance, accessibility, a Dutch "
   "translation, documentation.",
   "<b>Building apps on HavenDOS:</b> a loadable app format first &mdash; "
   "today every app is compiled into the kernel &mdash; then a small C "
   "compiler and an editor for it.",
   "<b>Groundwork for HavenDOS 2,</b> on the 32-bit core: preemptive task "
   "switching and memory owned per task.",
 ]),
 ("caution", "Three large items. The compiler is the one most likely to "
             "spill past December 2029."),
]),

# ══════════════════════════════════════════════════════════════════ 18
dict(title="After 29H2", body=[
 ("ul", [
   "<b>HavenDOS 2</b> stays part of this project and keeps its 32-bit "
   "architecture. It is not planned yet; work starts when it is needed or "
   "convenient, and 29H2&rsquo;s groundwork is its beginning.",
   "<b>The HavenOS port is dropped.</b>",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 19
dict(title="The audit, by release", body=[
 ("p", "THS-HD-096-AUD read 0.9.6. Where each finding is closed:"),
 ("table", ["Finding", "What", "Closed in"], [
   ["F-001", "A blocking task starves the rest", "0.9.9"],
   ["F-002", "No memory isolation", "stated in 0.9.9; groundwork 29H2"],
   ["F-003", "One UDP receive slot", "0.9.9"],
   ["F-004", cmd("kill") + " leaks memory", "0.9.9"],
   ["F-005", "Overflow caught after the fact", "stated; guard pages need paging"],
   ["F-006", "Eight tasks at most", "stays; the full-slots path tested in 0.9.9"],
   ["F-007", "HTTPS checks no certificate", "0.9.8"],
   ["F-008", "0.9.6 not yet on the HP", "partly: the monitor confirmed in "
    "0.9.6.2 and 0.9.6.4; the rest in [[0.9.6.x: finishing on the HP]]"],
   ["F-009", "Suite not reproduced", "the auditor&rsquo;s missing "
    + cmd("mkfs.vfat") + "; nothing to fix"],
   ["F-010", "Evidence per release", "the bundle carries it; 0.9.9 adds the "
    "hardware record"],
 ], [14, 40, 46]),
]),

# ══════════════════════════════════════════════════════════════════ 20
dict(title="Risks", body=[
 ("table", ["Risk", "Where", "If it bites"], [
   ["HD Audio is a new driver on undocumented-to-us silicon, in the last "
    "week before the freeze", "0.9.9",
    "it follows HavenDOS 1 as 27101 (open question 8); the PC speaker stays"],
   ["Five features in 0.9.7 and 0.9.9 leave the Release Preview two weeks "
    "and HP testing the critical path", "0.9.7&ndash;CH40",
    "1 January moves, or an exception waits for 27101; Kyan: &ldquo;we "
    "just need to test quicker&rdquo;"],
   ["0.9.8 carries HTTPS and online updates in nine days", "0.9.8",
    "online updates move to 0.9.9 or 27101 (open question 2); HTTPS does "
    "not move"],
   ["The clock is wrong", "0.9.8", "every certificate fails; the refusal "
    "says what the clock reads"],
   ["The signing key is lost or leaks", "0.9.8 on", "lost: a new key by "
    "stick; leaked: anyone can sign a kernel"],
   ["The HP shows something new in 0.9.6.5&rsquo;s windows", "0.9.6.x",
    "more .N releases; 0.9.7 slides by the days they take"],
   ["The AML interpreter is browser-sized", "28H1",
    "battery, brightness, lid and sleep all wait"],
   ["Sleep and resume", "28H1", "could take the half-year alone"],
   ["The MediaTek chip is undocumented", "28H2",
    "28H1&rsquo;s research build exists to learn this early"],
   ["Three large items in one half", "29H2", "the compiler spills"],
 ], [38, 12, 50]),
]),

# ══════════════════════════════════════════════════════════════════ 21
dict(title="Decisions still open", body=[
 ("ol", [
   "<b>If HTTPS slips past 5 December,</b> does 1 January move, or does "
   "HavenDOS 1 ship and HTTPS follow as 27101? HTTPS is a must-have, so the "
   "recommendation is that the date moves.",
   "<b>If online updating does not fit in 0.9.8,</b> does it go into 0.9.9, "
   "or ship after Stable as 27101? Recommended: 0.9.9, before the freeze, "
   "so HavenDOS 1 can receive its own fixes over the internet.",
   "<b>Background downloads early:</b> do they ship to Stable as 27102 in "
   "February or March, before 27H2&rsquo;s Beta?",
   "<b>ISO names off Stable:</b> should a Beta ISO say so, as "
   + cmd("HavenDOS-1-27H2-Beta.iso") + "?",
   "<b>28H1 without sleep:</b> if sleep is not working by May 2028, does "
   "28H1 ship without it, or wait?",
   "<b>The roots and the key:</b> the ten roots, decided at the start of "
   "0.9.8; who generates the update-signing key, and where its backup is "
   "kept.",
   "<b>The second tester:</b> who, on what machine, by 16 December.",
   "<b>If HD Audio (E-5) is not ready by the freeze,</b> does HavenDOS 1 "
   "ship with the PC speaker and HD Audio follow as 27101, or does the "
   "freeze wait? Recommended: 27101, so 1 January holds.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 22
dict(title="Document control", body=[
 ("table", ["Document", "Date", "What it was"], [
   ["THS-RM-1002", "Oct 2026", "The road to 1.0, first plan"],
   ["THS-RM-1003", "Nov 2026", "The road to 1.0 with the six gates; "
    "1.1 Aegis and 1.2 Janus"],
   ["THS-RM-1004 REV A", "15 Nov 2026", "Versioning standard and roadmap "
    "to 29H2; its numbering sections stand"],
   ["THS-RM-1005 REV A", "17 Nov 2026", "This roadmap"],
   ["THS-RM-1005 REV B", "17 Nov 2026", "Exception E-1: Retro PC "
    "in 0.9.7; 0.9.7 to ~24 Nov"],
   ["THS-RM-1005 REV C", "17 Nov 2026", "0.9.6.6; exceptions "
    "E-2 to E-5; 0.9.7 ~26 Nov, 0.9.8 ~5 Dec, freeze ~12 Dec, Stable 1 Jan"],
   ["THS-RM-1005 REV D", "20 Nov 2026", "0.9.7 &ldquo;Mnemosyne&rdquo; "
    "released with E-1 to E-4, the first GitHub pre-release; other dates "
    "unchanged"],
   ["<b>THS-RM-1005 REV E</b>", "22 Nov 2026", "QEMU-tested releases; "
    "exception E-6, a 386 for Retro PC, in 0.9.7.2"],
 ], [26, 18, 56]),
 ("p", "Built by " + cmd("docs/rm1005/build.py") + " from "
       + cmd("docs/rm1005/content.py") + " in the house style: A5, cream "
       "stock, black and oxide red, Courier Prime and IBM Plex Mono, "
       "rendered with WeasyPrint. A new revision is a new REV letter and a "
       "row above."),
]),

]
