/* The update manager's version comparison, against the real function.
 *
 * "The updater never worked." This is why.
 *
 * The ISO carries its version in /version.txt, written by make_version.py in
 * DISPLAY form with a leading v — "v0.7.20". The running system compares that
 * against HAVENDOS_VER_NUM, which is bare — "0.7.19". The old parser started
 * at the first character, consumed only digits, and advanced p only on a '.'.
 * For "v0.7.20" it matched no digits, so n stayed 0, and *p was still 'v' so p
 * never moved: the outer loop ran four times on the same character and the
 * whole string parsed as 0.0.0.0.
 *
 * A NEWER ISO therefore compared as OLDER, and the update manager asked
 * "Downgrade to older version?" for every single upgrade. Answering no — the
 * sensible answer — cancelled the update. Answering yes worked, which is
 * probably why this survived: it looked like the updater simply refused.
 *
 * The functions below are extracted from fs/update.c at build time by
 * tools/make_bars_inc.py, so this cannot pass against a stale copy.
 *
 * version-literal-file-ok — this file is ABOUT version strings, so it is
 * exempt from make_version.py --check. Nothing here is a version the OS
 * reports; they are all inputs to the comparison under test.
 */
#include <stdio.h>
#include <string.h>

#include "vercmp.inc"

static int fails = 0, total = 0;
static void ck(const char *what, int cond, long got)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s  (got %ld)\n", what, got); }
}
static void head(const char *t){ printf("\n%s\n", t); }

/* Name the outcome the way the update manager reads it. */
static const char *verdict(int c)
{
    return c > 0 ? "newer" : c < 0 ? "older" : "same";
}
static void cmp_is(const char *iso, const char *running, int want)
{
    int got = ver_cmp(iso, running);
    char w[128];
    sprintf(w, "ISO %-10s vs running %-10s -> %s", iso, running, verdict(want));
    ck(w, got == want, got);
}

int main(void)
{
    printf("Update manager version comparison\n");
    printf("=================================\n");

    head("the regression: the ISO's v prefix");
    /* This is the exact pair that shipped: version.txt says v0.7.20, the
       running system says 0.7.19. */
    cmp_is("v0.7.20", "0.7.19",  1);
    cmp_is("v0.7.19", "0.7.19",  0);
    cmp_is("v0.7.18", "0.7.19", -1);
    {
        int p[4];
        ck("a leading v is skipped, not parsed as a component",
           ver_parse("v0.7.20", p) == 3 && p[0] == 0 && p[1] == 7 && p[2] == 20,
           p[1]);
        ck("an uppercase V works too",
           ver_parse("V1.2.3", p) == 3 && p[0] == 1 && p[1] == 2 && p[2] == 3,
           p[0]);
    }

    head("both sides may carry the prefix, or neither");
    cmp_is("v0.7.20", "v0.7.19",  1);
    cmp_is("0.7.20",  "0.7.19",   1);
    cmp_is("0.7.20",  "v0.7.19",  1);

    head("ordinary ordering");
    cmp_is("0.7.20",   "0.7.19",   1);
    cmp_is("0.8.0",    "0.7.19",   1);
    cmp_is("1.0.0",    "0.9.9",    1);
    cmp_is("0.7.9",    "0.7.10",  -1);   /* not a string comparison */
    cmp_is("0.7.100",  "0.7.99",   1);
    cmp_is("0.5.9.14", "0.5.9.13", 1);   /* the old four-part scheme */
    cmp_is("0.5.9.13", "0.5.9.14", -1);
    cmp_is("0.7.19",   "0.5.9.14", 1);   /* the jump that broke update.c once */

    head("missing components count as zero");
    cmp_is("0.7",     "0.7.0",   0);
    cmp_is("0.7.1",   "0.7",     1);
    cmp_is("1",       "0.9.9",   1);

    head("junk does not become version 0.0.0.0");
    /* The old parser turned anything non-numeric into 0.0.0.0, which compares
       as older than every real version — so a corrupt version.txt silently
       looked like a downgrade rather than like nonsense. */
    ck("an empty string is not a version", ver_cmp("", "0.7.19") < 0,
       ver_cmp("", "0.7.19"));
    ck("a word is not a version", ver_cmp("unknown", "0.7.19") < 0,
       ver_cmp("unknown", "0.7.19"));
    ck("nonsense on both sides compares equal",
       ver_cmp("unknown", "") == 0, ver_cmp("unknown", ""));
    ck("a real version beats nonsense",
       ver_cmp("0.7.19", "unknown") > 0, ver_cmp("0.7.19", "unknown"));
    {
        int p[4];
        ck("a string with no digits parses zero components",
           ver_parse("unknown", p) == 0, ver_parse("unknown", p));
        ck("NULL is handled", ver_parse(0, p) == 0, 0);
    }

    head("trailing text after the number");
    /* version.txt is stripped of its newline before it gets here, but a
       hand-edited one may carry anything. */
    cmp_is("0.7.20-rc1", "0.7.19",  1);
    cmp_is("v0.7.20 ",   "0.7.19",  1);
    {
        int p[4];
        ck("a suffix ends the version rather than spinning",
           ver_parse("0.7.20-rc1", p) == 3 && p[2] == 20, p[2]);
    }

    head("the display form the ISO actually ships");
    /* make_version.py writes "v" + HAVENDOS_VER_NUM, so this is the real
       shape of the string the updater reads off a HavenDOS ISO. */
    cmp_is("v0.7.19", "0.7.19", 0);
    ck("so 'same version already installed' can actually fire",
       ver_cmp("v0.7.19", "0.7.19") == 0, ver_cmp("v0.7.19", "0.7.19"));

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
