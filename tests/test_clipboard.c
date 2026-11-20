/* tests/test_clipboard.c -- shell/clipboard.c, the one clipboard every
   app shares (v0.9.7, exception E-2). */
#include <stdio.h>
#include <string.h>
#include "../shell/clipboard.c"

static int total, fails;
static void ck(const char *w, int ok, long got)
{ total++; if(ok) printf("  ok    %s\n", w); else { fails++; printf("  FAIL  %s  (%ld)\n", w, got); } }

int main(void)
{
    static char big[CLIP_MAX + 100], out[CLIP_MAX + 100];
    char small[6];
    uint32_t s0;
    printf("test_clipboard -- one clipboard for every app\n");
    ck("empty at first", clip_len() == 0 && clip_get(out, sizeof out) == 0 && out[0] == 0, clip_len());
    s0 = clip_serial();
    ck("set two lines", clip_set("hello\nworld", 11) == 11 && clip_len() == 11, clip_len());
    ck("  ... the serial moved", clip_serial() == s0 + 1, (long)clip_serial());
    ck("  ... read back exactly", clip_get(out, sizeof out) == 11 && !strcmp(out, "hello\nworld"), 0);
    ck("a short buffer gets what fits, terminated",
       clip_get(small, sizeof small) == 5 && !strcmp(small, "hello"), 0);
    memset(big, 'x', sizeof big);
    ck("more than CLIP_MAX is cut at CLIP_MAX", clip_set(big, sizeof big) == CLIP_MAX && clip_len() == CLIP_MAX, clip_len());
    ck("  ... and reads back terminated", clip_get(out, sizeof out) == CLIP_MAX && out[CLIP_MAX] == 0, 0);
    ck("a negative length is an empty clipboard, not a crash", clip_set("abc", -4) == 0 && clip_len() == 0, clip_len());
    ck("NULL text likewise", clip_set(0, 9) == 0 && clip_len() == 0, clip_len());
    ck("clip_get into nothing copies nothing", clip_get(0, 10) == 0 && clip_get(out, 0) == 0, 0);
    s0 = clip_serial();
    clip_set("a", 1); clip_set("a", 1);
    ck("every set moves the serial, even the same text (another app set it)", clip_serial() == s0 + 2, (long)clip_serial());
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
