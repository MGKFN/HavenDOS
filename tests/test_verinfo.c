/* tests/test_verinfo.c -- kernel/verinfo.c: the version as people see
 * it, and the channels. v0.9.7, THS-RM-1004 sections 1-11.
 *
 * What matters: HavenDOS 1's string is exactly the one the roadmap
 * promised; a channel is understood however a person types it; and a
 * subscription never lets a less stable update through -- while a
 * manifest from before 0.9.7, which names no channel, is still taken.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define VERINFO_NO_SYSTEM
#include "../kernel/verinfo.c"

static int total, fails;
static void ck(const char *what, int ok, const char *got)
{
    total++;
    if(ok) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s  (got '%s')\n", what, got ? got : ""); }
}
static void head(const char *s){ printf("\n%s\n", s); }

int main(void)
{
    char b[96]; char n[8];
    printf("test_verinfo -- the version string and the channels\n");

    head("the display, as THS-RM-1004 promised it");
    ver_format(b, sizeof b, 1, "27H1", 27100, 1000, CH_STABLE, "x");
    ck("Stable: no channel shown", !strcmp(b, "HavenDOS 1, version 27H1 (OS Build 27100.1000)"), b);
    ver_format(b, sizeof b, 1, "27H1", 27100, 1000, CH_PREVIEW, "x");
    ck("Release Preview: named inside the brackets",
       !strcmp(b, "HavenDOS 1, version 27H1 (OS Build 27100.1000, Release Preview)"), b);
    ver_format(b, sizeof b, 1, "27H1", 27100, 1000, CH_PUBLIC_BETA, "x");
    ck("Public Beta too", !strcmp(b, "HavenDOS 1, version 27H1 (OS Build 27100.1000, Public Beta)"), b);
    ver_format(b, sizeof b, 1, "27H2", 27501, 1003, CH_STABLE, "x");
    ck("a maintenance update of 27H2", !strcmp(b, "HavenDOS 1, version 27H2 (OS Build 27501.1003)"), b);
    ver_format(b, sizeof b, 1, "27H1", 27100, 7, CH_DEVELOPMENT, "x");
    ck("a local build's revision keeps four digits",
       !strcmp(b, "HavenDOS 1, version 27H1 (OS Build 27100.0007, Development)"), b);
    ver_format(b, sizeof b, 0, "", 0, 0, CH_BETA, "HavenDOS v0.9.7 Beta \"Daedalus\"");
    ck("product 0, the 0.9.x line: shown as it always was",
       !strcmp(b, "HavenDOS v0.9.7 Beta \"Daedalus\""), b);
    ver_format(b, 20, 1, "27H1", 27100, 1000, CH_STABLE, "x");
    ck("a short buffer is cut, never overrun", strlen(b) == 19 && !strncmp(b, "HavenDOS 1, version", 19), b);

    head("the channels, however they are typed");
    {
        static const struct { const char *in; int want; } T[] = {
            { "beta", 20 }, { "Beta", 20 }, { "CH20", 20 }, { "ch20", 20 }, { "20", 20 },
            { "public-beta", 30 }, { "Public Beta", 30 }, { "publicbeta", 30 }, { "public", 30 },
            { "preview", 40 }, { "Release Preview", 40 }, { "release-preview", 40 }, { "CH40", 40 },
            { "stable", 50 }, { "STABLE", 50 }, { "CH50", 50 }, { "50\r\n", 50 },
            { "dev", 10 }, { "Development", 10 }, { "experimental", 0 }, { "CH00", 0 },
            { "", -1 }, { "gamma", -1 }, { "CH25", -1 }, { "60", -1 }, { "betaa", -1 }, { "999", -1 },
        };
        for(unsigned i = 0; i < sizeof T / sizeof T[0]; i++){
            char w[64]; int got = ver_channel_parse(T[i].in);
            snprintf(w, sizeof w, "'%s' -> %d", T[i].in, T[i].want);
            snprintf(n, sizeof n, "%d", got);
            ck(w, got == T[i].want, n);
        }
    }
    ck("every channel's name parses back to it",
       ver_channel_parse(ver_channel_name(0)) == 0 && ver_channel_parse(ver_channel_name(10)) == 10 &&
       ver_channel_parse(ver_channel_name(20)) == 20 && ver_channel_parse(ver_channel_name(30)) == 30 &&
       ver_channel_parse(ver_channel_name(40)) == 40 && ver_channel_parse(ver_channel_name(50)) == 50, "");
    ck("a number that is no channel has no name", !strcmp(ver_channel_name(25), "?"), ver_channel_name(25));

    head("the subscription: at least as stable, never less");
    ck("Stable takes Stable", ver_channel_accepts(50, 50), "");
    ck("Stable refuses Release Preview", !ver_channel_accepts(50, 40), "");
    ck("Stable refuses Beta", !ver_channel_accepts(50, 20), "");
    ck("Beta takes Beta", ver_channel_accepts(20, 20), "");
    ck("Beta takes Stable (more stable is always fine)", ver_channel_accepts(20, 50), "");
    ck("Release Preview takes Stable, refuses Public Beta",
       ver_channel_accepts(40, 50) && !ver_channel_accepts(40, 30), "");
    ck("a manifest that names no channel (before 0.9.7) is taken", ver_channel_accepts(50, -1), "");

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
