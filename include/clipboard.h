#ifndef CLIPBOARD_H
#define CLIPBOARD_H
/* shell/clipboard.c -- one clipboard for every app (v0.9.7, exception
   E-2 in THS-RM-1005). Plain text, lines separated by '\n'. */
#include "types.h"

#define CLIP_MAX 8192

int      clip_set(const char *text, int len);   /* bytes kept (cut at CLIP_MAX) */
int      clip_get(char *out, int max);          /* bytes copied, NUL-terminated */
int      clip_len(void);
uint32_t clip_serial(void);                     /* +1 on every clip_set */
#endif
