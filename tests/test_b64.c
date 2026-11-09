/* ================================================================
   tests/test_b64.c -- base64, judged by Python's base64 module
   ================================================================
   net/b64.c replaced FOUR implementations: two in
   shell/shell_v073.c (both of which took no output capacity at all),
   one in usb/lockdown.c and one in net/plist.c. Consolidating a codec
   is only safe if the replacement is checked against something
   outside the tree, because the thing a shared codec can now do that
   four private ones could not is be wrong in four places at once.

   So the encoder is compared against `base64.b64encode` for every
   input length from 0 to 200 at five wrap settings -- a thousand
   cases -- and the decoder is fed the encoder's own output and
   compared against the input pattern, which Python regenerates
   independently. tests/b64_judge.py prints how many cases it looked
   at and this suite asserts that number, because a judge that
   silently examined nothing would pass forever.

   The local checks are the edges: the capacity refusals, the measure
   mode, the trailing-group rules that usb/lockdown.c settled, and a
   0xAA canary proving nothing is written past the capacity given.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "b64.h"
#include "../net/b64.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

#define CAP 4096
static uint8_t g_in[256];
static char    g_out[CAP];
static uint8_t g_back[CAP];

static void pattern(uint8_t *p, int n){
    int i;
    for(i = 0; i < n; i++) p[i] = (uint8_t)((i * 37 + 11) & 255);
}

/* ── the canary ──────────────────────────────────────────────────
   b64_encode is told a capacity smaller than the buffer and every
   byte past it is 0xAA. A codec that honours the capacity cannot
   touch them. */
static char g_canary[CAP];
static int canary_clean(int cap){
    int i;
    for(i = cap; i < CAP; i++)
        if((unsigned char)g_canary[i] != 0xAAu) return 0;
    return 1;
}

int main(void){
    int i, n, wrap;
    const int WRAPS[5] = { 0, 4, 64, 68, 76 };

    printf("\n=== test_b64: base64, judged by Python ===\n");

    if(system("python3 -c 'import base64' >/dev/null 2>&1") != 0){
        printf("\n  python3 base64 unavailable -- SKIP\n");
        return 77;
    }

    /* ── the thousand cases, judged outside the tree ─────────────── */
    head("every length 0..200 at five wrap settings, against Python");
    {
        FILE *f = fopen("/tmp/hd_b64_cases.txt", "wb");
        int cases = 0;
        ck("the case file opened", f != 0, 0);
        if(!f) goto done;
        for(wrap = 0; wrap < 5; wrap++){
            for(n = 0; n <= 200; n++){
                int w = WRAPS[wrap], e, d, j;
                pattern(g_in, n > 200 ? 200 : n);
                /* g_in is 256 bytes; n never exceeds 200. */
                e = b64_encode(g_in, n, g_out, CAP, w);
                if(e < 0){
                    printf("    encode refused len=%d wrap=%d\n", n, w);
                    fails++; total++; continue;
                }
                fprintf(f, "E %d %d ", n, w);
                for(j = 0; j < e; j++)
                    if(g_out[j] == '\n') fputs("\\n", f);
                    else fputc(g_out[j], f);
                fputc('\n', f);
                cases++;

                /* And straight back, which is the other direction the
                   judge checks: Python regenerates the pattern. */
                d = b64_decode((const uint8_t *)g_out, e, g_back, CAP);
                if(d != n){
                    printf("    decode len=%d wrap=%d gave %d\n", n, w, d);
                    fails++; total++; continue;
                }
                fprintf(f, "D %d ", n);
                if(!n) fputs("-", f);
                else for(j = 0; j < d; j++) fprintf(f, "%02x", g_back[j]);
                fputc('\n', f);
                cases++;
            }
        }
        fclose(f);
        ck("2010 cases were written", cases == 2010, cases);
        {
            int rc = system("python3 -I tests/b64_judge.py "
                            "/tmp/hd_b64_cases.txt "
                            ">/tmp/hd_b64.out 2>/tmp/hd_b64.err");
            int seen = 0;
            FILE *o = fopen("/tmp/hd_b64.out", "rb");
            if(o){ if(fscanf(o, "%d", &seen) != 1) seen = -1; fclose(o); }
            ck("Python agrees with every one of them",
               WIFEXITED(rc) && WEXITSTATUS(rc) == 0, 
               WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
            /* A judge that looked at nothing would pass forever. */
            ck("...and it actually looked at all 2010", seen == 2010, seen);
            if(!(WIFEXITED(rc) && WEXITSTATUS(rc) == 0))
                system("sed 's/^/          | /' /tmp/hd_b64.err | head -12");
        }
    }

    /* ── b64_encoded_len must be exact, not an upper bound ───────── */
    head("b64_encoded_len is exact");
    {
        int bad = -1, badw = 0;
        for(wrap = 0; wrap < 5 && bad < 0; wrap++){
            for(n = 0; n <= 200; n++){
                int w = WRAPS[wrap];
                int want = b64_encoded_len(n, w);
                pattern(g_in, n);
                if(b64_encode(g_in, n, g_out, CAP, w) != want){
                    bad = n; badw = w; break;
                }
            }
        }
        /* It is used to size buffers. An upper bound would waste
           memory; a lower bound would overflow one. */
        ck("it equals what the encoder writes, every length, every wrap",
           bad < 0, bad);
        if(bad >= 0) printf("          at len=%d wrap=%d\n", bad, badw);
        ck("zero bytes is zero characters", b64_encoded_len(0, 0) == 0,
           b64_encoded_len(0, 0));
        ck("one byte is four", b64_encoded_len(1, 0) == 4,
           b64_encoded_len(1, 0));
        ck("three bytes is four", b64_encoded_len(3, 0) == 4,
           b64_encoded_len(3, 0));
        ck("a negative length is refused",
           b64_encoded_len(-1, 0) == -1, b64_encoded_len(-1, 0));
    }

    /* ── wrap rounding ───────────────────────────────────────────── */
    head("the wrap column is rounded down to a whole quad");
    pattern(g_in, 96);
    {
        int a = b64_encode(g_in, 96, g_out, CAP, 68);
        int b;
        pattern(g_in, 96);
        b = b64_encode(g_in, 96, g_back ? (char *)g_back : 0, CAP, 70);
        /* 70 rounds to 68: a quad is never split, because a decoder
           that cared would be entitled to. */
        ck("wrap 70 behaves as wrap 68", a == b, (long)b);
        ck("...and so does 71",
           b64_encode(g_in, 96, (char *)g_back, CAP, 71) == a, 0);
        ck("wrap 3 is no wrapping at all",
           b64_encode(g_in, 96, (char *)g_back, CAP, 3)
           == b64_encode(g_in, 96, g_out, CAP, 0), 0);
        n = b64_encode(g_in, 96, g_out, CAP, 68);
        ck("a wrapped line is exactly 68 characters",
           n > 68 && g_out[68] == '\n', n);
        ck("and there is no trailing newline",
           n > 0 && g_out[n-1] != '\n', g_out[n-1]);
    }

    /* ── capacity ────────────────────────────────────────────────── */
    head("capacity is a limit, not a hint");
    pattern(g_in, 100);
    n = b64_encoded_len(100, 0);
    memset(g_canary, 0xAA, CAP);
    ck("exactly enough room succeeds",
       b64_encode(g_in, 100, g_canary, n, 0) == n, 0);
    ck("...and wrote nothing past it", canary_clean(n), 0);
    memset(g_canary, 0xAA, CAP);
    ck("one byte short is refused",
       b64_encode(g_in, 100, g_canary, n - 1, 0) < 0, 0);
    /* The refusal must come BEFORE the first write, not part way
       through: the two implementations this replaced could not refuse
       at all, so this is the property that made them unsafe. */
    ck("...and refused without writing a single byte",
       canary_clean(0), 0);
    memset(g_canary, 0xAA, CAP);
    ck("zero capacity with zero input is zero characters",
       b64_encode(g_in, 0, g_canary, 0, 0) == 0, 0);
    ck("...and touched nothing", canary_clean(0), 0);
    ck("a NULL output is refused",
       b64_encode(g_in, 10, 0, 100, 0) < 0, 0);
    ck("a negative input length is refused",
       b64_encode(g_in, -1, g_out, CAP, 0) < 0, 0);

    /* ── the decoder's edges, which usb/lockdown.c settled ───────── */
    head("the decoder's trailing-group rule");
    ck("padded decodes",
       b64_decode((const uint8_t *)"aGVsbG8=", 8, g_back, CAP) == 5
       && !memcmp(g_back, "hello", 5), 0);
    ck("wrapped across lines with a tab decodes",
       b64_decode((const uint8_t *)"aGVs\n\tbG8=", 10, g_back, CAP) == 5
       && !memcmp(g_back, "hello", 5), 0);
    ck("unpadded decodes",
       b64_decode((const uint8_t *)"aGVsbG8", 7, g_back, CAP) == 5, 0);
    /* Two characters are twelve bits: one byte and four left over,
       which is what an unpadded single byte looks like. */
    ck("a two-character tail is one byte",
       b64_decode((const uint8_t *)"aG", 2, g_back, CAP) == 1, 0);
    ck("a three-character tail is two bytes",
       b64_decode((const uint8_t *)"aGV", 3, g_back, CAP) == 2, 0);
    /* One is the impossible case: six bits and nothing to put them in. */
    ck("a one-character tail is refused",
       b64_decode((const uint8_t *)"a", 1, g_back, CAP) < 0, 0);
    ck("a five-character value is refused (a one-character tail)",
       b64_decode((const uint8_t *)"aGVsb", 5, g_back, CAP) < 0, 0);
    ck("a character outside the alphabet is refused",
       b64_decode((const uint8_t *)"aGV!bG8=", 8, g_back, CAP) < 0, 0);
    ck("anything after the padding is refused",
       b64_decode((const uint8_t *)"aGVsbG8=X", 9, g_back, CAP) < 0, 0);
    ck("a second group after the padding is refused",
       b64_decode((const uint8_t *)"aGVsbG8=aGVs", 12, g_back, CAP) < 0, 0);
    ck("three padding characters are refused",
       b64_decode((const uint8_t *)"aGVsbG===", 9, g_back, CAP) < 0, 0);
    /* The one above is refused by the trailing-group rule as well, so
       it does not actually test the padding count. This one lands on
       a quad boundary with four pads and nothing else catches it. */
    ck("four padding characters on a quad boundary are refused",
       b64_decode((const uint8_t *)"aGVs====", 8, g_back, CAP) < 0, 0);
    ck("...and two is still fine",
       b64_decode((const uint8_t *)"aGVsbA==", 8, g_back, CAP) == 4, 0);

    /* ── bytes with the top bit set ──────────────────────────────
       The decode table has 128 entries. A byte of 0xFF indexed into
       it reads twelve bytes past the end of the array and trusts
       whatever is there -- which, with the guard removed, is whatever
       the linker put next in .rodata. Every value is checked rather
       than one, so the refusal cannot be an accident of what happens
       to follow the table today. */
    head("a byte with the top bit set is refused, all 128 of them");
    {
        int bad = -1;
        for(i = 128; i < 256; i++){
            uint8_t v[4];
            v[0] = 'a'; v[1] = 'G'; v[2] = (uint8_t)i; v[3] = 's';
            if(b64_decode(v, 4, g_back, CAP) >= 0){ bad = i; break; }
        }
        ck("not one of them decodes", bad < 0, bad);
        /* And in every position of the quad, because a guard applied
           after the first character is not a guard. */
        bad = -1;
        for(i = 0; i < 4; i++){
            uint8_t v[4] = { 'a','G','V','s' };
            v[i] = 0xFF;
            if(b64_decode(v, 4, g_back, CAP) >= 0){ bad = i; break; }
        }
        ck("in any position of the quad", bad < 0, bad);
        ck("a high byte in measure mode is refused too",
           b64_decode((const uint8_t *)"\xffGVs", 4, 0, 0) < 0, 0);
    }
    ck("an empty input is zero bytes, not an error",
       b64_decode((const uint8_t *)"", 0, g_back, CAP) == 0, 0);
    ck("whitespace only is zero bytes",
       b64_decode((const uint8_t *)"  \n\t ", 5, g_back, CAP) == 0, 0);
    ck("a NULL input is refused",
       b64_decode(0, 4, g_back, CAP) < 0, 0);
    ck("a negative input length is refused",
       b64_decode((const uint8_t *)"aGVs", -1, g_back, CAP) < 0, 0);

    head("a value too big for the buffer is refused, never cut");
    {
        uint8_t small[2];
        memset(small, 0xAA, sizeof small);
        ck("the refusal happens",
           b64_decode((const uint8_t *)"aGVsbG8=", 8, small, 2) < 0, 0);
        /* It may write before it notices -- what it must NOT do is
           write past the end, and what it must not do is return a
           short count that the caller believes. */
        ck("...and the count is not a short success",
           b64_decode((const uint8_t *)"aGVsbG8=", 8, small, 2) != 2, 0);
    }

    head("measure mode: validate and report the length, write nothing");
    pattern(g_in, 64);
    n = b64_encode(g_in, 64, g_out, CAP, 0);
    ck("it reports the decoded length",
       b64_decode((const uint8_t *)g_out, n, 0, 0) == 64, 0);
    ck("it still refuses bad input",
       b64_decode((const uint8_t *)"aGV!", 4, 0, 0) < 0, 0);
    ck("it still refuses a one-character tail",
       b64_decode((const uint8_t *)"a", 1, 0, 0) < 0, 0);
    ck("a NULL output with a nonzero capacity is refused",
       b64_decode((const uint8_t *)"aGVs", 4, 0, 16) < 0, 0);

    head("all 256 byte values survive a round trip");
    {
        uint8_t all[256];
        int d;
        for(i = 0; i < 256; i++) all[i] = (uint8_t)i;
        n = b64_encode(all, 256, g_out, CAP, 68);
        ck("it encodes", n > 0, n);
        d = b64_decode((const uint8_t *)g_out, n, g_back, CAP);
        ck("it decodes to 256 bytes", d == 256, d);
        ck("byte for byte", d == 256 && !memcmp(g_back, all, 256), 0);
    }

    /* ── v0.8.9: two gaps the injection harness found ────────────────
       Both survived an injection because nothing here asked the
       question, and both are properties every caller depends on. */
    head("b64_encoded_len() agrees with what b64_encode() writes");
    {
        /* AT EVERY WRAP COLUMN, including the ones that are not a
           multiple of four. The column is rounded DOWN so a quad is
           never split, and the rounding lives in both functions -- so
           a wrap of 67 is the case where they can disagree while
           every multiple-of-four case passes. A caller sizing its
           buffer from one and filling it with the other is a buffer
           overrun with a correct-looking bound. */
        static const int WRAPS[] = { 0, 4, 7, 16, 60, 64, 67, 68, 70, 76 };
        static const int LENS[]  = { 0, 1, 2, 3, 47, 48, 49, 200, 419 };
        uint8_t src[512];
        int w, L, bad = 0, worst_w = -1, worst_l = -1;
        for(i = 0; i < (int)sizeof src; i++) src[i] = (uint8_t)(i * 7 + 3);
        for(w = 0; w < (int)(sizeof WRAPS / sizeof WRAPS[0]); w++){
            for(L = 0; L < (int)(sizeof LENS / sizeof LENS[0]); L++){
                int want = b64_encoded_len(LENS[L], WRAPS[w]);
                int got  = b64_encode(src, LENS[L], g_out, CAP, WRAPS[w]);
                if(want != got){
                    bad++;
                    if(worst_w < 0){ worst_w = WRAPS[w]; worst_l = LENS[L]; }
                }
            }
        }
        ck("they agree on every length at every wrap column",
           bad == 0, bad);
        if(bad){
            printf("      first disagreement: %d bytes at wrap %d\n",
                   worst_l, worst_w);
        }
    }

    head("the encoder refuses what it cannot read");
    ck("a NULL input with a nonzero length is refused",
       b64_encode(0, 8, g_out, CAP, 0) < 0, 0);
    ck("a NULL input with length zero is fine -- nothing to read",
       b64_encode(0, 0, g_out, CAP, 0) == 0, 0);
    ck("a NULL output is refused",
       b64_encode((const uint8_t *)"abc", 3, 0, CAP, 0) < 0, 0);
    ck("a negative length is refused",
       b64_encode((const uint8_t *)"abc", -1, g_out, CAP, 0) < 0, 0);
    ck("a negative capacity is refused",
       b64_encode((const uint8_t *)"abc", 3, g_out, -1, 0) < 0, 0);

done:
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
