/*
 * shell/clipboard.c -- one clipboard for every app. v0.9.7.
 *
 * Exception E-2 (THS-RM-1005 REV C): select text with the mouse in the
 * editor, copy it, paste it -- in the editor, or at the Shell's prompt.
 * Before this the editor's clipboard was its own: nothing copied there
 * was anywhere else.
 *
 * Plain text, '\n' between lines, at most CLIP_MAX bytes. A serial
 * number moves on every set, so an app holding its own copy (the
 * editor keeps lines) knows when another app has replaced it.
 */
#include "../include/types.h"
#include "../include/clipboard.h"

static char     g_clip[CLIP_MAX + 1];
static int      g_len;
static uint32_t g_serial;

int clip_set(const char *text, int len)
{
    int i;
    if(!text || len < 0) len = 0;
    if(len > CLIP_MAX) len = CLIP_MAX;
    for(i = 0; i < len; i++) g_clip[i] = text[i];
    g_clip[len] = 0;
    g_len = len;
    g_serial++;
    return len;
}

int clip_get(char *out, int max)
{
    int n = g_len, i;
    if(!out || max <= 0) return 0;
    if(n > max - 1) n = max - 1;
    for(i = 0; i < n; i++) out[i] = g_clip[i];
    out[n] = 0;
    return n;
}

int      clip_len(void)    { return g_len; }
uint32_t clip_serial(void) { return g_serial; }
