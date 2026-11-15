# -*- coding: utf-8 -*-
"""THS-RM-1004 — the content.

Plain structures only; docs/rm1004/build.py turns these into the HTML the
house stylesheet expects. Same block vocabulary as RM-1003 and the manual,
so the documents cannot drift in appearance:

    ("p", html) ("h3", text) ("ul", [...]) ("ol", [...]) ("crt", text)
    ("note", html) ("caution", html) ("box", label, html)
    ("table", [heads], [[cells], ...]) ("spec", [(k, v), ...]) ("raw", html)

Every decision here was taken with Kyan on 15 November 2026, question by
question. The editable copy of the same text is the Claude doc "HavenDOS
Versioning Specification and Roadmap to 29H2"; where they differ, the one
changed last is right and the other is to be brought into line.
"""

META = dict(
    ver="1004", channel="", codename="",
    title="HAVENDOS 1<br/>VERSIONS AND<br/>ROADMAP",
    subtitle=("The numbering standard from HavenDOS 1 onward,<br/>"
              "and six releases from 27H1 to 29H2<br/>"
              "Decided question by question"),
    part="THS-RM-1004 REV A",
    date="15 NOV 2026",
    author="Kyan Hoekstra / TechHaven Studios",
    baseline="v0.9.6 Beta &ldquo;Daedalus&rdquo;, 7214 checks across 69 suites",
    supersedes="THS-RM-1003 from 1.0 onward (1.1 Aegis, 1.2 Janus)",
    status="DECIDED 15 NOV 2026 &mdash; six questions open (section 22)",
    cover_bars=[
        ("VERSIONING",   3, 20, "one dotted number"),
        ("HTTPS",        4, 20, "no certificate checks"),
        ("TASK LIFE",   12, 20, "kill leaks, one UDP slot"),
        ("UPDATER",     14, 20, "CRC-checked, no channels"),
        ("HW GATES",    15, 20, "never all on one build"),
    ],
)


# ── the release timeline, drawn from the dates rather than typed ─────
# One character is one month, from November 2026. A typed picture of a
# schedule is a second copy of the schedule, and it drifts the first time
# a date does; this one is computed from the same table the text quotes.
_RELEASES = [
    # name, dev from, testing from, stable (all as (year, month))
    ("27H1 BRANDARIS",   (2026, 11), (2026, 12), (2027, 1)),
    ("27H2 LANGE JAAP",  (2027, 1),  (2027, 9),  (2027, 12)),
    ("28H1 WESTHOOFD",   (2027, 7),  (2028, 3),  (2028, 6)),
    ("28H2 EIERLAND",    (2028, 1),  (2028, 9),  (2028, 12)),
    ("29H1 BORNRIF",     (2028, 7),  (2029, 3),  (2029, 6)),
    ("29H2 WESTKAPELLE", (2029, 1),  (2029, 9),  (2029, 12)),
]
_MON = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"]


def _idx(ym):
    return (ym[0] - 2026) * 12 + (ym[1] - 11)


def _timeline():
    lab, span = 18, _idx((2029, 12)) + 1
    axis = [" "] * span
    marks = [" "] * span
    for y in (2027, 2028, 2029):
        for m, ch in ((1, "J"), (7, "J")):
            marks[_idx((y, m))] = ch
        s = str(y)
        i = _idx((y, 1))
        axis[i:i + 4] = list(s)
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
        when = "%s %d" % (_MON[stable[1] - 1], stable[0])
        lines.append(name.ljust(lab) + "".join(row) + "  " + when)
    lines.append("")
    lines.append(" " * lab + "- development   = Beta to Release Preview   * Stable")
    return "\n".join(lines)


SECTIONS = [

# ══════════════════════════════════════════════════════════════════ 1
dict(title="What this document decides", body=[
 ("p", "HavenDOS 1, version 27H1 &ldquo;Brandaris&rdquo;, OS Build "
       "27100.1000, goes to Release Preview on 15 December 2026 and to "
       "Stable on 1 January 2027. From then on a release is named by product, "
       "half-year and build, never by a dotted version."),
 ("ul", [
   "<b>Until then nothing changes in the numbering.</b> 0.9.6, 0.9.7, 0.9.8 "
   "and 0.9.9 each get up to ten maintenance updates (.1 to .10), only as "
   "many as are needed.",
   "<b>The last 0.9.9.x build is rebuilt once as 27100.1000</b> and then "
   "only promoted: Public Beta, Release Preview, Stable. From Release "
   "Preview on, the kernel is byte-identical.",
   "<b>HavenDOS 1 ships fully ready, or not at all.</b> It needs audit "
   "hardening, real HTTPS, the new versioning in the updater, the six gates "
   "recorded on the HP, every app opened on the HP, no known serious bugs, "
   "and a second tester.",
   "<b>Five half-year releases follow,</b> each still HavenDOS 1: the web, "
   "daily use on the HP, Wi-Fi, desktop and files, then polish and "
   "groundwork.",
   "<b>HavenDOS 2 stays 32-bit and is not planned yet.</b> The HavenOS port "
   "is dropped.",
 ]),
 ("box", "WHAT A RELEASE IS CALLED FROM 1 JANUARY 2027",
  "<b>HavenDOS 1, version 27H1 (OS Build 27100.1000)</b><br/>"
  "Brandaris &middot; Stable (CH50)"),
 ("p", "Six questions are still open; [[Decisions still open]] lists them."),
]),

# ══════════════════════════════════════════════════════════════════ 2
dict(title="Why the dotted numbers are retired", body=[
 ("p", "The proposal Kyan brought is sound, and the 0.9.x updater already "
       "shipped can follow it without a change. Six points in it had to "
       "change before it could be adopted."),
 ("h3", "What the tree does today"),
 ("ul", [
   "<b>One source of truth.</b> <span class=\"cmd\">include/version.h</span> "
   "holds <span class=\"cmd\">HAVENDOS_VER_NUM</span> (&ldquo;0.9.6&rdquo;), "
   "the channel (&ldquo;Beta&rdquo;), the codename and the early-access "
   "switch. <span class=\"cmd\">tools/make_version.py --check</span> refuses "
   "a version literal anywhere else.",
   "<b><span class=\"cmd\">update.inf</span></b> is "
   "<span class=\"cmd\">HAVENDOS-UPDATE 1</span> then "
   "<span class=\"cmd\">version=</span>, <span class=\"cmd\">size=</span>, "
   "<span class=\"cmd\">crc32=</span> and <span class=\"cmd\">kernel=</span>. "
   "<span class=\"cmd\">fs/update_src.c</span> skips any other key.",
   "<b><span class=\"cmd\">ver_cmp()</span></b> compares up to four numbers, "
   "left to right, past a leading <span class=\"cmd\">v</span>; "
   "<span class=\"cmd\">tests/test_vercmp.c</span> holds it to that.",
   "<b>The release workflow</b> publishes when the version has no GitHub "
   "release yet and its changelog exists.",
 ]),
 ("box", "BACKWARD COMPATIBILITY: ALREADY THERE",
  "An installed 0.9.x system reads <span class=\"cmd\">version=27100.1000"
  "</span> as {27100, 1000, 0, 0} and 0.9.9.10 as {0, 9, 9, 10}. The first "
  "number decides, so <b>every 0.9.x machine offers HavenDOS 1 as an "
  "upgrade with no updater change</b> &mdash; provided "
  "<span class=\"cmd\">version=</span> stays numbers only. Product, release "
  "and channel go in new keys, which old updaters skip."),
 ("h3", "What had to change"),
 ("table", ["The proposal said", "The problem", "Resolved as"], [
   ["Every half-year cycle ends with a June Stable",
    "HavenDOS 1 is 27H1 but ships 1 January; an H2 release cannot ship in June",
    "The ID is the half a release reaches Stable in: H1 January to June, H2 July to December"],
   ["Channel IDs are separate from the binary",
    "The channel and the early-access banner are compiled in, so promotion changes the kernel",
    "The channel lives in <span class=\"cmd\">/BOOT/CHANNEL.INF</span> and in "
    "<span class=\"cmd\">update.inf</span>; the kernel reads it at boot"],
   ["OS build 27100, rules for when it rises",
    "Nothing stopped a number going down at a new half or at HavenDOS 2",
    "Builds only rise; each half starts at a fixed base above the last"],
   ["Revision +1 for each fix",
    "Silent on what the revision does when the build rises",
    "It returns to 1000 at every new build"],
   ["Six channels",
    "One developer, one test laptop",
    "CH00 and CH10 are local builds; GitHub carries CH20 and up"],
   ["Ten updates per 0.9.x line",
    "Forty releases in six and a half weeks",
    "Up to ten per line, as many as needed"],
 ], [30, 35, 35]),
]),

# ══════════════════════════════════════════════════════════════════ 3
dict(title="The identity of a build", body=[
 ("p", "Every build carries six fields. Two of them &mdash; the OS build and "
       "the revision &mdash; are the only ones ever compared to decide which "
       "build is newer."),
 ("table", ["Field", "Example", "Meaning", "Compared"], [
   ["Product", "HavenDOS 1", "The generation; changes only for a new architecture", "no"],
   ["Release ID", "27H1", "The half-year release: YY, then H1 or H2", "no"],
   ["Codename", "Brandaris", "One Dutch lighthouse per half-year release", "no"],
   ["OS build", "27100", "Rises with every feature update", "first"],
   ["Revision", "1000", "Rises with every maintenance update", "second"],
   ["Channel", "CH50 Stable", "Which ring it is in now; metadata, not the binary", "no"],
 ], [17, 18, 49, 16]),
 ("p", "The full number is <span class=\"cmd\">27100.1000</span>. It is the "
       "only thing in <span class=\"cmd\">version=</span>, in the Git tag and "
       "in <span class=\"cmd\">ver_cmp()</span>. No word ever goes into it."),
 ("crt", """
A:\\> ver
HavenDOS 1, version 27H1 (OS Build 27100.1000)
Brandaris - Stable (CH50)
TechHaven Studios
"""),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 3-1</span> '
         '<span class="cmd">ver</span> on a Stable build, as section 10 '
         'specifies it.</div>'),
]),

# ══════════════════════════════════════════════════════════════════ 4
dict(title="OS build numbers", body=[
 ("p", "Each half-year owns a fixed range of build numbers, and the numbers "
       "only ever rise."),
 ("spec", [
   ("H1 RANGE", "YY100 to YY499 &mdash; 27H1 is 27100&ndash;27499"),
   ("H2 RANGE", "YY500 to YY999 &mdash; 27H2 is 27500&ndash;27999"),
   ("NEVER", "goes down, resets, or is used twice &mdash; not even for a pulled build"),
   ("THE YEAR", "is the release&rsquo;s year, not the day it was built"),
   ("LOCAL", "CH00/CH10 builds use revisions 0001&ndash;0999 of the build they head for"),
 ]),
 ("p", "A build made in December 2027 for 28H1 is therefore a 28100-something, "
       "and a local <span class=\"cmd\">27101.0042</span> always compares "
       "older than the first public <span class=\"cmd\">27101.1000</span>."),
 ("caution", "A generation does not reset the numbers. HavenDOS 2 starts at "
             "the next half-year&rsquo;s base, above every HavenDOS 1 build, "
             "or the updater on a HavenDOS 1 machine would call it older."),
]),

# ══════════════════════════════════════════════════════════════════ 5
dict(title="When each number moves", body=[
 ("p", "This table is the rule. If a change is not in it, it is the row it "
       "most resembles."),
 ("table", ["Change", "Level", "Build", "Revision", "Example"], [
   ["Security, bug, performance, small UI or compatibility fix", "1", "same", "+1",
    "27100.1000 &rarr; .1001"],
   ["Several fixes shipped together", "1", "same", "+1 once", ".1001 &rarr; .1002"],
   ["Same source, new toolchain (binary differs)", "1", "same", "+1", ".1002 &rarr; .1003"],
   ["Promotion, kernel identical", "&mdash;", "same", "same", "stays .1003"],
   ["New app, setting, hardware support or system function", "2", "+1",
    "1000", "27100.1003 &rarr; 27101.1000"],
   ["Half-year release", "3", "next base", "1000", "27107.1004 &rarr; 27500.1000"],
   ["New generation", "&mdash;", "next base", "1000", "product 1 &rarr; 2"],
 ], [40, 9, 13, 13, 25]),
 ("ul", [
   "<b>A Level 2 update is one feature,</b> or a few small ones shipped "
   "together. If it cannot be named in one line, it is two updates.",
   "<b>A Level 3 release is the half-year release itself,</b> at most one per "
   "half. A collection that big, finished mid-half, waits for the next "
   "release ID.",
   "<b>Once the next half&rsquo;s first public build exists, the older release "
   "gets Level 1 only.</b> Its build stops; only its revision moves.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 6
dict(title="Release IDs and codenames", body=[
 ("p", "The ID is the half a release reaches Stable in. It, the codename and "
       "the build base are fixed when the release&rsquo;s first public build "
       "(CH20) appears, and a release that slips keeps them: renaming one "
       "mid-flight costs more than a slightly wrong half."),
 ("p", "&ldquo;Haven&rdquo; is Dutch for harbour, so the codenames are the "
       "lighthouses that bring ships into one. Greek names end with 0.9.x."),
 ("table", ["Release", "Codename", "Build base", "Stable by"], [
   ["27H1", "Brandaris", "27100", "1 Jan 2027"],
   ["27H2", "Lange Jaap", "27500", "31 Dec 2027"],
   ["28H1", "Westhoofd", "28100", "30 Jun 2028"],
   ["28H2", "Eierland", "28500", "31 Dec 2028"],
   ["29H1", "Bornrif", "29100", "30 Jun 2029"],
   ["29H2", "Westkapelle", "29500", "31 Dec 2029"],
 ], [20, 30, 22, 28]),
]),

# ══════════════════════════════════════════════════════════════════ 7
dict(title="Channels", body=[
 ("p", "Six channels, each with a permanent ID. The ID is metadata: it is "
       "never part of the build number."),
 ("table", ["ID", "Channel", "Who gets it", "Published", "Stay before promotion"], [
   ["CH00", "Experimental", "Kyan", "never", "&mdash;"],
   ["CH10", "Development", "Kyan", "bundle zips only", "&mdash;"],
   ["CH20", "Beta", "subscribed to Beta", "GitHub pre-release", "3 days"],
   ["CH30", "Public Beta", "Public Beta or lower", "GitHub pre-release", "3 days"],
   ["CH40", "Release Preview", "Release Preview or lower", "GitHub pre-release",
    "14 days for a half-year release, else 3"],
   ["CH50", "Stable", "every machine", "GitHub, marked latest", "&mdash;"],
 ], [10, 18, 24, 23, 25]),
 ("h3", "Promotion rules"),
 ("ol", [
   "<b>Promotion never changes the kernel.</b> Its CRC32 and size must match "
   "the build promoted, or it is not a promotion.",
   "<b>A build may skip channels upward and never moves down.</b> One found "
   "broken is pulled and replaced by a new revision.",
   "<b>A fix found in a channel enters at that channel</b> and promotes up "
   "from there.",
   "<b>A security fix may go straight to Stable,</b> with the reason in its "
   "release notes.",
   "<b>A machine follows one channel,</b> chosen in Settings or with "
   "<span class=\"cmd\">update channel &lt;name&gt;</span>; Stable by default. "
   "It is offered builds from its channel and every more stable one, "
   "whichever number is highest.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 8
dict(title="Update levels", body=[
 ("table", ["Level", "Name", "What qualifies", "Moves"], [
   ["1", "Maintenance update", "Security patches, bug fixes, performance, "
    "small UI fixes, compatibility fixes", "revision"],
   ["2", "Feature update", "A new system app, a new setting, new hardware "
    "support, new system functionality, a subsystem improved", "OS build"],
   ["3", "Half-year release", "Several substantial apps or subsystems, "
    "changes across the OS, a changed user experience",
    "release ID, codename, build base"],
 ], [10, 19, 50, 21]),
 ("note", "One new application is Level 2. It never makes a new half-year "
          "release on its own."),
]),

# ══════════════════════════════════════════════════════════════════ 9
dict(title="Generations", body=[
 ("p", "A new product number needs a change to how HavenDOS fundamentally "
       "works: a different kernel architecture, memory protection or "
       "preemptive scheduling that changes how every app is written, or an "
       "overhaul that breaks the previous generation&rsquo;s apps or installs. "
       "Time passing, or a big half-year release, is never enough."),
 ("box", "HAVENDOS 2",
  "Stays part of the HavenDOS project and its <b>32-bit</b> architecture. "
  "29H2 lays groundwork &mdash; preemptive task switching and memory owned "
  "per task &mdash; but HavenDOS 2 itself is not planned in this document. "
  "Its work starts when it is needed or convenient."),
]),

# ══════════════════════════════════════════════════════════════════ 10
dict(title="How the version is shown", body=[
 ("table", ["Where", "Shows"], [
   ["Boot screen", "HavenDOS 1 / Version 27H1 (OS Build 27100.1000), and "
    "the channel underneath when not Stable"],
   ["<span class=\"cmd\">ver</span>", "As FIGURE 3-1"],
   ["System, Settings", "Product, version, OS build, channel and codename on "
    "separate labelled lines"],
   ["Early-access warning", "Whenever the channel is below CH50, read from "
    "the channel file"],
   ["GRUB menu", "HavenDOS 1 (27H1)"],
   ["ISO file", "<span class=\"cmd\">HavenDOS-1-27H1.iso</span>"],
   ["Bundle zip", "<span class=\"cmd\">HavenDOS-1-27H1-27100.1000-bundle.zip"
    "</span> &mdash; one per build"],
   ["Git tag", "<span class=\"cmd\">27100.1000</span>, never moved"],
   ["GitHub release", "HavenDOS 1, version 27H1 (OS Build 27100.1000), plus "
    "&ldquo; - Release Preview&rdquo; while not Stable"],
   ["Release notes", "<span class=\"cmd\">changelogs/CHANGELOG-27100.1000.md"
    "</span>, headed like the release"],
   ["<span class=\"cmd\">$VER</span>", "27100.1000"],
 ], [26, 74]),
 ("crt", """
HAVENDOS 1
Version 27H1 (OS Build 27100.1000)
Public Beta (CH30)

EARLY ACCESS -- BETA SOFTWARE
"""),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 10-1</span> '
         'The boot screen on a build that is not yet Stable. On CH50 the '
         'last three lines are gone.</div>'),
]),

# ══════════════════════════════════════════════════════════════════ 11
dict(title="The updater manifest", body=[
 ("p", "<span class=\"cmd\">update.inf</span> keeps its first line, because "
       "every installed parser requires it, and gains seven keys."),
 ("crt", """
HAVENDOS-UPDATE 1
version=27100.1000
product=1
release=27H1
codename=Brandaris
build=27100
revision=1000
level=1
channel=CH50
size=4062372
crc32=0a1b2c3d
kernel=havendos.elf
"""),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 11-1</span> '
         'A HavenDOS 1 manifest. The size and checksum are examples.</div>'),
 ("ul", [
   "<b>Old updaters (0.9.6 and before)</b> read version, size, crc32 and "
   "kernel, skip the rest, and see a newer version.",
   "<b>New updaters (0.9.7 on)</b> refuse a manifest whose "
   "<span class=\"cmd\">version</span> is not exactly build.revision, whose "
   "channel is not one of the six, or whose channel is less stable than the "
   "machine&rsquo;s subscription.",
   "<b>The channel also lives beside the kernel</b> in "
   "<span class=\"cmd\">/BOOT/CHANNEL.INF</span>, written by the installer "
   "and by <span class=\"cmd\">update</span>.",
   "<b>Downgrades are refused</b> unless "
   "<span class=\"cmd\">update --allow-older</span> is given and confirmed.",
 ]),
 ("caution", "A missing channel file makes the kernel show Development "
             "(CH10) and the early-access warning. An unknown build is never "
             "presented as Stable."),
]),

# ══════════════════════════════════════════════════════════════════ 12
dict(title="From 0.9.6 to 27100.1000", body=[
 ("p", "The dotted numbering runs unchanged until the 0.9.9 freeze. Then the "
       "frozen build is rebuilt once with the new identity, and only promoted "
       "after that."),
 ("h3", "The 0.9.x rules"),
 ("ul", [
   "<b>A line is 0.9.6, 0.9.7, 0.9.8 or 0.9.9.</b> Each gets .1 up to .10, "
   "only as many as are needed, fixes only.",
   "<b>A new feature starts the next line,</b> never a .N.",
   "<b>Four-part numbers already compare correctly:</b> 0.9.6.10 is newer "
   "than 0.9.6.9, and 0.9.7 newer than 0.9.6.10. The one trap stays as "
   "<span class=\"cmd\">version.h</span> warns: never publish a shorter number "
   "at the same prefix.",
   "<b>From 0.9.7 every release is a GitHub pre-release with channel CH20,</b> "
   "so by 1 January every updated machine understands channels.",
 ]),
 ("h3", "The switch"),
 ("ol", [
   "<b>0.9.9.N is frozen</b>, about 10 December. Only release-blocking fixes "
   "after this.",
   "<b><span class=\"cmd\">version.h</span> changes once:</b> product 1, "
   "release 27H1, codename Brandaris, build 27100, revision 1000. Nothing "
   "else in the source changes.",
   "<b>27100.1000 is published as CH30 Public Beta.</b> An installed 0.9.9 "
   "sees 27100 &gt; 0 and offers it.",
   "<b>From 15 December it is CH40 Release Preview</b> &mdash; the same "
   "kernel; only the manifest, the channel file and the release title change.",
   "<b>On 1 January it is CH50 Stable,</b> marked latest, the early-access "
   "warning gone. A fix found in Release Preview becomes 27100.1001 and "
   "restarts the 14 days: the date moves rather than a bug ships.",
 ]),
 ("h3", "What changes in the tree &mdash; 0.9.7&rsquo;s work"),
 ("ul", [
   "<span class=\"cmd\">include/version.h</span>: product, release, build, "
   "revision and codename; the channel macro becomes a fallback only.",
   "<span class=\"cmd\">fs/update_src.c</span>, <span class=\"cmd\">fs/update.c"
   "</span>: the new keys, the refusals, the subscription, "
   "<span class=\"cmd\">update channel</span>.",
   "The boot splash, <span class=\"cmd\">ver</span>, System and Settings: "
   "section 10, through one function.",
   "<span class=\"cmd\">make_iso_manifest.py</span>, <span class=\"cmd\">"
   "make_version.py</span>, <span class=\"cmd\">make_release_bundle.py</span>, "
   "the release workflow: keys, names, tags, the pre-release flag.",
   "The installer and <span class=\"cmd\">update</span>: write "
   "<span class=\"cmd\">/BOOT/CHANNEL.INF</span>.",
   "Tests: <span class=\"cmd\">test_vercmp</span> for the 0.9.x &rarr; 27100 "
   "crossing and the 0999/1000 boundary, <span class=\"cmd\">test_update"
   "</span> for keys, refusals and subscriptions, and an injection suite.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 13
dict(title="The road to HavenDOS 1", body=[
 ("table", ["Release", "Target", "What is in it"], [
   ["0.9.6.1 &ndash; .x", "now &ndash; 21 Nov",
    "What the HP shows for Alt+Tab, the chips, music behind and "
    "<span class=\"cmd\">migrate</span>; the external monitor; the Welcome "
    "chooser on every boot; BOOT Chat hidden, saying its replacements "
    "arrive in 27H2"],
   ["0.9.7", "~22 Nov", "The new versioning (section 12)"],
   ["0.9.8", "~3 Dec", "Real HTTPS: TLS 1.2, X25519, AES-128-GCM, RSA and "
    "ECDSA P-256 signatures, the chain checked against about ten bundled "
    "roots, hostname and expiry, every refusal named on screen"],
   ["0.9.9", "~10 Dec, freeze", "Hardening, below; then the ship gate"],
   ["27100.1000", "11 Dec CH30<br/>15 Dec CH40<br/>1 Jan CH50",
    "The frozen 0.9.9.N, rebuilt once"],
 ], [19, 19, 62]),
 ("h3", "0.9.9&rsquo;s hardening, one item per audit finding"),
 ("ul", [
   "<b>Memory a task allocated is freed</b> on exit, kill and crash; each "
   "allocation records its owner (F-004, gate C).",
   "<b>One UDP receive slot per task</b> (F-003, gate E).",
   "<b><span class=\"cmd\">wget</span> and <span class=\"cmd\">http_get()</span> "
   "yield between reads</b>, so a download is a real background job (F-001).",
   "<b>A stall watchdog</b> names any task that runs too long without "
   "yielding (gate D).",
   "<b>A spawn-and-kill stress test</b> to heap exhaustion, with all eight "
   "slots full (gate C).",
   "<b><span class=\"cmd\">docs/SECURITY.md</span></b> states how passwords "
   "are stored, what HTTPS checks, where randomness comes from, and that "
   "apps are not isolated (gate G, F-002).",
 ]),
 ("box", "THE SHIP GATE &mdash; ALL FOUR, ON ONE BUILD",
  "1. The six gates of RM-1003 &mdash; installs, boots, fills the screen, "
  "networks, takes input, harms nothing &mdash; recorded on the HP with "
  "photos.<br/>2. Every app opened, used and closed on the HP.<br/>"
  "3. No known bug that loses data, hangs or crashes; every known limit "
  "written down.<br/>4. A second tester runs the Release Preview on their "
  "own machine."),
]),

# ══════════════════════════════════════════════════════════════════ 14
dict(title="Six releases, one every half-year", body=[
 ("p", "Brandaris on 1 January 2027, then one release every six months to "
       "Westkapelle in December 2029. Each one&rsquo;s development overlaps "
       "the previous one&rsquo;s testing."),
 ("crt", _timeline()),
 ("raw", '<div class="figcap"><span class="lab">FIGURE 14-1</span> '
         'One character is one month, from November 2026. Drawn by '
         '<span class="cmd">content.py</span> from the same dates as the '
         'text.</div>'),
 ("spec", [
   ("H1 RELEASE", "development Jul&ndash;Feb, Beta Mar (feature freeze), "
                  "Public Beta Apr, Release Preview May&ndash;Jun, Stable by 30 Jun"),
   ("H2 RELEASE", "development Jan&ndash;Aug, Beta Sep (feature freeze), "
                  "Public Beta Oct, Release Preview Nov&ndash;Dec, Stable by 31 Dec"),
   ("BETWEEN", "the Stable release gets Level 1 updates, and Level 2 until "
               "the next release&rsquo;s first public build exists"),
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 15
dict(title="27H2 Lange Jaap: the web", body=[
 ("spec", [("STABLE BY", "31 December 2027"), ("BUILDS", "from 27500"),
           ("RULE", "ships when all of it is done; nothing moves to 28H1")]),
 ("ol", [
   "<b>Update over the internet.</b> <span class=\"cmd\">update</span> checks "
   "GitHub Releases for the machine&rsquo;s channel and downloads it. "
   "Everything after arrives without a USB stick.",
   "<b>Background downloads:</b> a Downloads app, "
   "<span class=\"cmd\">wget</span> as a job with progress.",
   "<b>Browser, stage 1 &mdash; text:</b> headings, links, lists, tables, "
   "forms and colours, in the HavenDOS look.",
   "<b>Haven News:</b> what is new, release notes, and this build against "
   "the newest in your channel.",
   "<b>IRC:</b> Libera.Chat over TLS, with a #havendos channel.",
   "<b>LAN chat:</b> HavenDOS machines on one network find each other and "
   "chat directly, with no server.",
   "<b>Package store:</b> <span class=\"cmd\">bpkg</span> installs from a "
   "list on GitHub, over HTTPS.",
   "<b>Browser, stage 2 &mdash; images:</b> PNG, JPEG and GIF.",
   "<b>Email:</b> IMAP and SMTP over TLS.",
   "<b>Browser, stage 3 &mdash; CSS:</b> layout, fonts and colours from "
   "stylesheets.",
 ]),
 ("note", "IRC, Haven News and LAN chat together replace BOOT Chat, whose "
          "server is shut down until further notice."),
]),

# ══════════════════════════════════════════════════════════════════ 16
dict(title="28H1 Westhoofd: daily use on the HP", body=[
 ("spec", [("STABLE BY", "30 June 2028"), ("BUILDS", "from 28100")]),
 ("ol", [
   "<b>An ACPI interpreter (AML).</b> Battery, brightness, lid and sleep all "
   "live in the firmware&rsquo;s ACPI methods, so it comes first &mdash; and "
   "it is the largest single item.",
   "<b>Real sound:</b> Intel HD Audio and the HP&rsquo;s codec; the Music "
   "Player plays MP3s through the speakers and the jack.",
   "<b>Battery and charging</b> on the bar, with a low warning.",
   "<b>Brightness and volume keys.</b>",
   "<b>Sleep on lid close, and waking.</b> Every driver must restart after "
   "resume.",
   "<b>A Wi-Fi research build, CH00 only:</b> read the MediaTek chip and load "
   "its firmware, so 28H2 starts from facts.",
 ]),
 ("caution", "Sleep and resume is the riskiest item in this plan. Whether "
             "28H1 waits for it is an open decision."),
]),

# ══════════════════════════════════════════════════════════════════ 17
dict(title="28H2 Eierland: Wi-Fi", body=[
 ("spec", [("STABLE BY", "31 December 2028"), ("BUILDS", "from 28500"),
           ("CHIP", "MediaTek <span class=\"cmd\">14C3:7902</span>, the HP&rsquo;s only network card")]),
 ("ol", [
   "<b>The chip</b> up over PCIe, its firmware loaded.",
   "<b>Scanning:</b> the networks around, with signal strength.",
   "<b>Open networks,</b> and DHCP over Wi-Fi.",
   "<b>WPA2-Personal:</b> the four-way handshake, PBKDF2, AES-CCMP.",
   "<b>WPA3:</b> SAE, on 0.9.8&rsquo;s elliptic-curve code.",
   "<b>School and work (eduroam):</b> WPA2-Enterprise, EAP over 0.9.8&rsquo;s "
   "TLS.",
   "<b>Wi-Fi in Settings:</b> saved networks, joining by itself.",
 ]),
 ("note", "The firmware blob ships as it is, with MediaTek&rsquo;s licence "
          "beside it. The only reference for the chip is Linux&rsquo;s mt76 "
          "driver."),
]),

# ══════════════════════════════════════════════════════════════════ 18
dict(title="29H1 Bornrif: desktop and files", body=[
 ("spec", [("STABLE BY", "30 June 2029"), ("BUILDS", "from 29100")]),
 ("ol", [
   "<b>Overlapping windows:</b> move, resize, minimise, several visible at "
   "once. Every task has had its own console since 0.9.6, so a window is "
   "that console drawn somewhere other than full screen.",
   "<b>A file manager:</b> disks, sticks and folders; copy, move, rename, "
   "delete; open in the right app.",
   "<b>Open and Save dialogs,</b> one, shared by every app.",
   "<b>Wallpapers and themes,</b> on 27H2&rsquo;s image decoders.",
 ]),
]),

# ══════════════════════════════════════════════════════════════════ 19
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

# ══════════════════════════════════════════════════════════════════ 20
dict(title="The audit, by release", body=[
 ("p", "THS-HD-096-AUD reads 0.9.6 and mostly restates the limits its own "
       "changelog lists. F-007 is the finding that matters: "
       "<span class=\"cmd\">net/tls.c</span> checks no certificate at all and "
       "offers one RSA suite modern servers refuse."),
 ("table", ["Finding", "What", "Closed in"], [
   ["F-001", "A blocking task starves the rest", "0.9.9"],
   ["F-002", "No memory isolation", "stated in 0.9.9; groundwork 29H2"],
   ["F-003", "One UDP receive slot", "0.9.9"],
   ["F-004", "<span class=\"cmd\">kill</span> leaks memory", "0.9.9"],
   ["F-005", "Overflow caught after the fact", "stated; guard pages need paging"],
   ["F-006", "Eight tasks at most", "stays; the full-slots path tested in 0.9.9"],
   ["F-007", "HTTPS checks no certificate", "0.9.8"],
   ["F-008", "0.9.6 not yet on the HP", "0.9.6.x"],
   ["F-009", "Suite not reproduced", "the auditor&rsquo;s missing "
    "<span class=\"cmd\">mkfs.vfat</span>; nothing to fix"],
   ["F-010", "Evidence per release", "the bundle carries it; 0.9.9 adds the "
    "hardware record"],
 ], [14, 40, 46]),
]),

# ══════════════════════════════════════════════════════════════════ 21
dict(title="Risks", body=[
 ("table", ["Risk", "Where", "If it bites"], [
   ["HTTPS takes longer than eleven days", "0.9.8",
    "the freeze moves, and with 14 days of Release Preview, 1 January too"],
   ["The clock is wrong", "0.9.8", "every certificate fails; the refusal "
    "must say &ldquo;your clock reads&nbsp;&hellip;&rdquo;"],
   ["The AML interpreter is browser-sized", "28H1",
    "battery, brightness, lid and sleep all wait"],
   ["Sleep and resume", "28H1", "could take the half-year alone"],
   ["The MediaTek chip is undocumented", "28H2",
    "28H1&rsquo;s research build exists to learn this early"],
   ["Three large items in one half", "29H2", "the compiler spills"],
 ], [38, 12, 50]),
]),

# ══════════════════════════════════════════════════════════════════ 22
dict(title="Decisions still open", body=[
 ("ol", [
   "<b>If HTTPS slips past 3 December,</b> does 1 January move, or does "
   "HavenDOS 1 ship and HTTPS follow as 27101? HTTPS was chosen as a "
   "must-have, so the recommendation is that the date moves.",
   "<b>Early features:</b> do update-over-the-internet and background "
   "downloads ship to Stable in February or March as 27101 and 27102, "
   "before 27H2&rsquo;s Beta?",
   "<b>ISO names off Stable:</b> every 27H1 ISO is "
   "<span class=\"cmd\">HavenDOS-1-27H1.iso</span>. Should a Beta ISO say "
   "so, as <span class=\"cmd\">HavenDOS-1-27H2-Beta.iso</span>?",
   "<b>28H1 without sleep:</b> if sleep is not working by May 2028, does "
   "28H1 ship without it, or wait?",
   "<b>The ten roots,</b> decided at the start of 0.9.8 from the sites "
   "HavenDOS will actually visit.",
   "<b>The second tester:</b> who, on what machine, by 15 December.",
 ]),
]),

]
