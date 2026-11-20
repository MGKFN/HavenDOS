#ifndef VERINFO_H
#define VERINFO_H
/* kernel/verinfo.c -- the version as people see it, and the channels
   (v0.9.7, THS-RM-1004 sections 1-11). Pure functions first, so
   tests/test_verinfo.c drives every case; the running system's own
   values after. */
#include "types.h"

#define CH_EXPERIMENTAL  0
#define CH_DEVELOPMENT  10
#define CH_BETA         20
#define CH_PUBLIC_BETA  30
#define CH_PREVIEW      40
#define CH_STABLE       50

const char *ver_channel_name(int ch);           /* "Beta"; "?" for none */
int         ver_channel_parse(const char *s);    /* "beta", "CH20", "20" -> 20; -1 */
/* May a system subscribed to `sub` take an update on channel `upd`?
   upd < 0 means the manifest names none (anything before 0.9.7): yes. */
int         ver_channel_accepts(int sub, int upd);
/* "HavenDOS v0.9.7 Beta "Daedalus"" for product 0; from product 1
   "HavenDOS 1, version 27H1 (OS Build 27100.1000)", with ", Beta" (or
   the channel's name) inside the brackets when it is not Stable. */
void        ver_format(char *out, int max, int product, const char *release,
                       int build, int revision, int channel,
                       const char *legacy);

/* The running system. */
const char *ver_display(void);
int         ver_channel(void);                   /* this build's */
/* The subscription: /boot/channel.inf, else this build's own channel. */
int         ver_subscription(void);
int         ver_set_subscription(int ch);        /* 1 saved, 0 could not */
#endif
