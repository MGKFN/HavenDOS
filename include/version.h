#pragma once
/* ============================================================
   HavenDOS version — SINGLE SOURCE OF TRUTH
   ============================================================

   Before v0.7.18 the version was typed out by hand in ~50 places and they
   had drifted apart: a v0.7.17 build shipped screens reading v0.7.0, v0.7.4
   and v0.7.11, the ISO's GRUB menu said v0.7.11, isodir/version.txt said
   v0.7.11, and the installer wrote a grub.cfg whose menu entries said
   v0.7.4.

   That was not only cosmetic.  fs/update.c carried

       #define HAVENDOS_VERSION "0.5.9.14"

   and fed it to ver_cmp() against the version read off the update ISO.  So
   "same version already installed" could never fire, and an OLDER ISO
   compared as NEWER and was applied as an upgrade with no warning.

   Everything now derives from the three numbers below.  tools/make_version.py
   regenerates isodir/version.txt and the ISO grub.cfg from this file at build
   time, so a release is a one-line change here.

   HAVENDOS_VER_NUM  — bare, for comparison (update.c, manifests)
   HAVENDOS_VER      — display form with a leading v, what the user sees
   HAVENDOS_FULL     — name and display version together

   No version literal is written anywhere in this comment on purpose: the
   Makefile banner used to scrape HAVENDOS_VER_NUM with an unanchored sed
   and printed EVERY match, so a literal here made the banner report the
   previous release beside the new one.  tools/make_version.py --check now
   fails the build if a version literal reappears outside this file.
*/

#define HAVENDOS_VER_MAJOR  0
#define HAVENDOS_VER_MINOR  9
#define HAVENDOS_VER_PATCH  7
/* A fourth number, for a patch release that fixes the release before it
   without adding a feature.  ver_cmp() in fs/update.c already parses four
   components, so 0.8.5.1 compares as newer than 0.8.5 and `update`
   behaves correctly across it.  Zero means "no patch", and VER_NUM below
   omits it in that case -- "0.8.6.0" would be a different string from
   "0.8.6" to ver_cmp()'s caller even though it parses the same. */
/* v0.8.7.5: these four had drifted out of step with VER_NUM below --
   they still read 0.8.6.7 through the whole of 0.8.7. Nothing in the tree
   reads them, which is why nothing complained, and a stale number nobody
   uses is still a number somebody will one day believe. */
/* ── v0.8.9: BACK TO THREE COMPONENTS, AND A WARNING ─────────────────
   0.8.8.11 was the end of a long fourth-number run. ver_cmp() compares
   component by component, so 0.8.9 is newer than 0.8.8.11 and `update`
   offers it -- verified against the parser in tests/test_vercmp.c
   rather than assumed.

   THE TRAP, which bites one way only and has already bitten once:
   0.8.8.11 vs 0.8.9 compares as a DOWNGRADE. A machine that was
   updated to a 0.8.9-numbered bundle during the 0.8.8.x series could
   never be offered anything in that series again. Nothing shipped as
   0.8.9 then; 0.8.9 is the release that took the number.

   0.8.9.1 adds a fourth component again, which is fine in this
   direction: 0.8.9.1 compares NEWER than 0.8.9, so `update` offers
   it. The rule is only ever broken by going from a longer number to a
   shorter one at the same prefix. */
/* ── v0.9.0: A NEW MINOR, AND THE FOURTH NUMBER RESETS ───────────────
   ver_cmp() compares component by component, so 0.9.0 is newer than
   0.8.9.9 at the second component and nothing after it is looked at.
   Zero here means "no patch" and VER_NUM omits it. */
/* v0.9.2.1 and .2: patches to 0.9.2 -- what its runs on the HP found.
   Each compares newer than the last under ver_cmp(). 0.9.3 is newer than
   0.9.2.2 at the third component, and the fourth goes back to zero. */
/* v0.9.6: a new third number, so the fourth goes back to zero. 0.9.6 is
   newer than 0.9.5.1 at the third component. */
/* v0.9.6.1: the first maintenance update of 0.9.6, under THS-RM-1004
   section 12 -- fixes only, up to .10 per line. */
/* v0.9.6.6: the fixes from Kyan's 0.9.6.5 test on the HP, 17 Nov. */
/* v0.9.7: a new line, so its maintenance number starts again. */
/* v0.9.7.1: the first maintenance update of 0.9.7, from Kyan's photo of
   the sign-in screen over the Start menu, 22 Nov. Tested in QEMU only. */
/* v0.9.7.2: Retro PC's 386 and BOOT OS, exception E-6. QEMU only. */
/* v0.9.7.3: /USB everywhere, faster stick reads, file types, replugging.
   QEMU only. */
/* v0.9.7.4: USB that comes back after more than one replug. QEMU only. */
/* v0.9.7.5: Retro PC as a program of its own, for Windows and Linux. */
/* v0.9.7.6: the first Stable release; a USB device slow after a replug is retried. */
/* v0.9.7.7: a bug sweep -- fuzzing and every QEMU harness; Retro PC 0.1.1's reader fixes.
   v0.9.7.8: FAT32 from 64 MB; Notepad's Ctrl+A/X, wrap and 4000 lines; the
   THS look -- a themed boot menu and Studio, the default theme. */
#define HAVENDOS_VER_BUILD  8

/* VER_NUM stays plain dotted numbers because fs/update.c compares it with
   ver_cmp() to decide whether an ISO is newer.  The channel and codename are
   separate macros so they can appear on screen without ever reaching that
   comparison -- a version string with a word in it would make every update
   check unpredictable. */
#define HAVENDOS_VER_NUM    "0.9.7.8"
#define HAVENDOS_VER        "v" HAVENDOS_VER_NUM
#define HAVENDOS_NAME       "HavenDOS"
#define HAVENDOS_CHANNEL    "Stable"
/* "Gaia", v0.9.3: the earth -- the ground an install stands on, and now
   all of it rather than the first 2047 MB. RM-1003 named it for 0.9.1;
   the display series ("Janus", 0.9.0-0.9.2.2) took those numbers first,
   so the road-to-1.0 releases each move up by two. */
/* "Prometheus", v0.9.4: the one who carried fire to make more of it -- an
   installed system that installs itself. */
/* "Hephaestus", v0.9.5: the smith -- the apps, rebuilt.
   "Daedalus", v0.9.6: who built the labyrinth, a place of many passages
   with more than one thing moving in it -- several apps open at once.
   RM-1003 gave the name to an app-port wave that Hephaestus finished;
   the handoff said it would take whatever number it reached. */
/* "Mnemosyne", v0.9.7: memory -- the release that keeps the oldest
   builds alive (Retro PC boots K-DOS Beta 1.1b) and teaches every
   install the numbers it will be known by. */
#define HAVENDOS_CODENAME   "Mnemosyne"
#define HAVENDOS_FULL       HAVENDOS_NAME " " HAVENDOS_VER " " HAVENDOS_CHANNEL \
                            " \"" HAVENDOS_CODENAME "\""

/* ── v0.9.7: THE NUMBERS HAVENDOS 1 WILL CARRY (THS-RM-1004 s.1-11) ────
   Taught to the updater now, so that every installed 0.9.x already
   understands them when 27100.1000 arrives.

   PRODUCT 0 is the 0.9.x line: it keeps HAVENDOS_VER_NUM and the
   display above. From HavenDOS 1, PRODUCT is 1, RELEASE "27H1",
   BUILD 27100, REVISION 1000, and HAVENDOS_VER_NUM is "27100.1000" --
   still plain dotted numbers, so ver_cmp() takes 27100.1000 as newer
   than any 0.9.9.N and the tools need nothing new to compare them.

   CHANNEL_ID: CH00 Experimental, CH10 Development (local only), CH20
   Beta, CH30 Public Beta, CH40 Release Preview, CH50 Stable. The
   HAVENDOS_CHANNEL word above is the fallback; kernel/verinfo.c names
   channels from the number. */
#define HAVENDOS_PRODUCT     0
#define HAVENDOS_RELEASE     ""
#define HAVENDOS_BUILD       0
#define HAVENDOS_REVISION    0
/* v0.9.7.6: Kyan, 23 Nov 2026: "0.9.7 releases are stable now". From
   here the 0.9.7 line is Stable: GitHub's "Latest", no early-access
   warnings, and an installed system with no /boot/channel.inf follows
   Stable -- so every later 0.9.7.N has to be Stable too, or it refuses
   it. A Beta system (0.9.7.5 and before) takes a Stable update. */
#define HAVENDOS_CHANNEL_ID  50
#define HAVENDOS_LEVEL       1      /* 1 maintenance, 2 feature update, 3 the half-year release */
#define HAVENDOS_VENDOR     "TechHaven Studios"

/* ── IS THIS A PRE-RELEASE? ──────────────────────────────────────────
   v0.8.8.8. One switch, here, next to the channel it follows, so that
   turning the warnings off is a single edit on the day there is a 1.0
   and not a hunt through four files.

   Everything that says "early access" reads this: the boot splash,
   the notice after it, the setup wizard and `ver`. Set it to 0 and
   every one of them goes quiet; nothing else changes. */
#define HAVENDOS_PRERELEASE 0

#define HAVENDOS_EA_LINE1   "EARLY ACCESS -- BETA SOFTWARE"
#define HAVENDOS_EA_LINE2   "Expect bugs. Do not keep anything here you cannot lose."
