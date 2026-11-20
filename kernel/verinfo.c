/*
 * kernel/verinfo.c -- the version as people see it, and the channels.
 * v0.9.7, THS-RM-1004 sections 1-11 and THS-RM-1005 section 7.
 *
 * HavenDOS 1 will show itself the way Windows does:
 *
 *     HavenDOS 1, version 27H1 (OS Build 27100.1000)
 *
 * with the channel inside the brackets only when it is not Stable. The
 * 0.9.x line keeps what it has always shown. ONE function makes both,
 * so the splash, `ver`, the System app, neofetch and Settings' About
 * page can never disagree -- the lesson of v0.7.18, when the boot menu,
 * version.txt and the kernel each said a different version.
 *
 * The CHANNEL a system follows is a file beside its kernel,
 * /boot/channel.inf, so promoting a build from Beta to Release Preview
 * to Stable never changes the kernel's bytes. With no file, a system
 * follows its OWN channel: a Beta follows Beta, and HavenDOS 1 on
 * Stable follows Stable, which is RM-1004's "default Stable" for every
 * system that matters to it. (A 0.9.x Beta that defaulted to Stable
 * would refuse the next 0.9.x -- every one of them is a Beta.)
 */
#include "../include/types.h"
#include "../include/version.h"
#include "../include/verinfo.h"

static int lc(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* compare ignoring case, spaces, '-' and '_' ("Public Beta",
   "public-beta", "publicbeta" are one name) */
static int same_word(const char *a, const char *b)
{
    for(;;){
        while(*a == ' ' || *a == '-' || *a == '_') a++;
        while(*b == ' ' || *b == '-' || *b == '_') b++;
        if(!*a || !*b) return !*a && !*b;
        if(lc((unsigned char)*a) != lc((unsigned char)*b)) return 0;
        a++; b++;
    }
}

static const struct { int ch; const char *name; const char *alias; } CH[] = {
    { CH_EXPERIMENTAL, "Experimental",    "exp"     },
    { CH_DEVELOPMENT,  "Development",     "dev"     },
    { CH_BETA,         "Beta",            "beta"    },
    { CH_PUBLIC_BETA,  "Public Beta",     "public"  },
    { CH_PREVIEW,      "Release Preview", "preview" },
    { CH_STABLE,       "Stable",          "release" },
};
#define NCH ((int)(sizeof CH / sizeof CH[0]))

const char *ver_channel_name(int ch)
{
    for(int i = 0; i < NCH; i++) if(CH[i].ch == ch) return CH[i].name;
    return "?";
}

int ver_channel_parse(const char *s)
{
    int v = 0, any = 0;
    if(!s) return -1;
    while(*s == ' ') s++;
    if((s[0] == 'C' || s[0] == 'c') && (s[1] == 'H' || s[1] == 'h')) s += 2;
    {
        const char *p = s;
        while(*p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; any = 1; if(v > 99) return -1; }
        while(*p == ' ' || *p == '\r' || *p == '\n') p++;
        if(any && !*p){
            for(int i = 0; i < NCH; i++) if(CH[i].ch == v) return v;
            return -1;
        }
    }
    for(int i = 0; i < NCH; i++)
        if(same_word(s, CH[i].name) || same_word(s, CH[i].alias)) return CH[i].ch;
    return -1;
}

int ver_channel_accepts(int sub, int upd)
{
    if(upd < 0) return 1;           /* a manifest from before 0.9.7 names none */
    return upd >= sub;
}

static int put(char *o, int at, int max, const char *s)
{
    while(*s && at < max - 1) o[at++] = *s++;
    o[at < max ? at : max - 1] = 0;
    return at;
}
static int putn(char *o, int at, int max, int v, int width)
{
    char t[12]; int n = 0;
    if(v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while(v && n < 11);
    while(n < width && n < 11) t[n++] = '0';
    while(n) { if(at < max - 1) o[at++] = t[--n]; else n--; }
    o[at < max ? at : max - 1] = 0;
    return at;
}

void ver_format(char *out, int max, int product, const char *release,
                int build, int revision, int channel, const char *legacy)
{
    int at = 0;
    if(!out || max <= 0) return;
    out[0] = 0;
    if(product < 1){                         /* the 0.9.x line, as ever */
        put(out, 0, max, legacy ? legacy : "");
        return;
    }
    at = put(out, at, max, "HavenDOS ");
    at = putn(out, at, max, product, 1);
    at = put(out, at, max, ", version ");
    at = put(out, at, max, release ? release : "");
    at = put(out, at, max, " (OS Build ");
    at = putn(out, at, max, build, 1);
    at = put(out, at, max, ".");
    at = putn(out, at, max, revision, 4);    /* 27100.0999 is a local build */
    if(channel != CH_STABLE){
        at = put(out, at, max, ", ");
        at = put(out, at, max, ver_channel_name(channel));
    }
    put(out, at, max, ")");
}

/* ── the running system ─────────────────────────────────────────────── */
#ifndef VERINFO_NO_SYSTEM
extern int vfs_read (const char*, char*, uint32_t);
extern int vfs_write(const char*, const char*, uint32_t);

#define CHANNEL_FILE "/boot/channel.inf"

const char *ver_display(void)
{
    static char s[96];
    if(!s[0])
        ver_format(s, (int)sizeof s, HAVENDOS_PRODUCT, HAVENDOS_RELEASE,
                   HAVENDOS_BUILD, HAVENDOS_REVISION, HAVENDOS_CHANNEL_ID,
                   HAVENDOS_FULL);
    return s;
}

int ver_channel(void){ return HAVENDOS_CHANNEL_ID; }

int ver_subscription(void)
{
    char b[64]; int n, ch;
    n = vfs_read(CHANNEL_FILE, b, sizeof b - 1);
    if(n > 0){
        b[n] = 0;
        /* "channel=CH20", or just the name a person typed */
        {
            char *p = b;
            while(*p && *p != '=') p++;
            ch = ver_channel_parse(*p ? p + 1 : b);
        }
        if(ch >= 0) return ch;
    }
    return HAVENDOS_CHANNEL_ID;
}

int ver_set_subscription(int ch)
{
    char b[32]; int at = 0;
    if(ver_channel_parse(ver_channel_name(ch)) != ch) return 0;
    at = put(b, at, sizeof b, "channel=CH");
    at = putn(b, at, sizeof b, ch, 2);
    at = put(b, at, sizeof b, "\n");
    return vfs_write(CHANNEL_FILE, b, (uint32_t)at) == at;
}
#endif
