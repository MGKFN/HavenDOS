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
#define HAVENDOS_VER_PATCH  4
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
#define HAVENDOS_VER_BUILD  0

/* VER_NUM stays plain dotted numbers because fs/update.c compares it with
   ver_cmp() to decide whether an ISO is newer.  The channel and codename are
   separate macros so they can appear on screen without ever reaching that
   comparison -- a version string with a word in it would make every update
   check unpredictable. */
#define HAVENDOS_VER_NUM    "0.9.4"
#define HAVENDOS_VER        "v" HAVENDOS_VER_NUM
#define HAVENDOS_NAME       "HavenDOS"
#define HAVENDOS_CHANNEL    "Beta"
/* "Gaia", v0.9.3: the earth -- the ground an install stands on, and now
   all of it rather than the first 2047 MB. RM-1003 named it for 0.9.1;
   the display series ("Janus", 0.9.0-0.9.2.2) took those numbers first,
   so the road-to-1.0 releases each move up by two. */
/* "Prometheus", v0.9.4: the one who carried fire to make more of it -- an
   installed system that installs itself. */
#define HAVENDOS_CODENAME   "Prometheus"
#define HAVENDOS_FULL       HAVENDOS_NAME " " HAVENDOS_VER " " HAVENDOS_CHANNEL \
                            " \"" HAVENDOS_CODENAME "\""
#define HAVENDOS_VENDOR     "TechHaven Studios"

/* ── IS THIS A PRE-RELEASE? ──────────────────────────────────────────
   v0.8.8.8. One switch, here, next to the channel it follows, so that
   turning the warnings off is a single edit on the day there is a 1.0
   and not a hunt through four files.

   Everything that says "early access" reads this: the boot splash,
   the notice after it, the setup wizard and `ver`. Set it to 0 and
   every one of them goes quiet; nothing else changes. */
#define HAVENDOS_PRERELEASE 1

#define HAVENDOS_EA_LINE1   "EARLY ACCESS -- BETA SOFTWARE"
#define HAVENDOS_EA_LINE2   "Expect bugs. Do not keep anything here you cannot lose."
