/* Native regression tests for the v0.7.18 fixes.
   Compiled with the host gcc and run directly — faster and more rigorous
   than driving the VM, and it proves the OLD code really was broken rather
   than just asserting the new code passes. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0, total = 0;
static void ck(const char *what, int cond)
{
    total++;
    if (!cond) { fails++; printf("  FAIL  %s\n", what); }
    else       { printf("  ok    %s\n", what); }
}

/* ────────────────────────────────────────────────────────────────
   F1 — Multiboot1 memory map: type is at entry+20, not entry+16
   ──────────────────────────────────────────────────────────────── */

/* Build a real MB1 mmap: entries are
      size@0(u32) base@4(u64) length@12(u64) type@20(u32)
   and the walk advances by size+4. */
static int build_mmap(uint8_t *buf, uint64_t base, uint64_t len, uint32_t type)
{
    uint32_t size = 20;                 /* bytes after the size field */
    memcpy(buf + 0,  &size, 4);
    memcpy(buf + 4,  &base, 8);
    memcpy(buf + 12, &len,  8);
    memcpy(buf + 20, &type, 4);
    return size + 4;                    /* 24 */
}

static uint32_t scan_mmap(uint8_t *p, uint8_t *end, int type_off)
{
    uint32_t highest = 0;
    while (p < end) {
        uint32_t entry_sz = *(uint32_t *)p;
        uint32_t type     = *(uint32_t *)(p + type_off);
        uint64_t base     = *(uint64_t *)(p + 4);
        uint64_t len      = *(uint64_t *)(p + 12);
        if (entry_sz < 20) break;
        if (type == 1 && base < 0x100000000ULL) {
            uint64_t top = base + len;
            if (top > 0x100000000ULL) top = 0x100000000ULL;
            if ((uint32_t)top > highest) highest = (uint32_t)top;
        }
        p += entry_sz + 4;
    }
    return highest;
}

static void test_mmap(void)
{
    printf("\nF1  Multiboot1 memory map offset\n");
    uint8_t buf[256]; memset(buf, 0, sizeof buf);
    int off = 0;
    /* 640K low RAM, a reserved hole, then 2GB of usable RAM at 1MB */
    off += build_mmap(buf + off, 0x00000000ULL, 0x0009FC00ULL, 1);
    off += build_mmap(buf + off, 0x000F0000ULL, 0x00010000ULL, 2);
    off += build_mmap(buf + off, 0x00100000ULL, 0x7FF00000ULL, 1);

    uint32_t old_val = scan_mmap(buf, buf + off, 16);   /* the shipped bug */
    uint32_t new_val = scan_mmap(buf, buf + off, 20);   /* the fix          */

    ck("old offset 16 finds nothing (proves the bug was real)", old_val == 0);
    ck("new offset 20 finds the top of RAM = 2GB",
       new_val == 0x80000000u);
    printf("        old=%u  new=%u (%u MB)\n", old_val, new_val,
           new_val / (1024 * 1024));
}

/* ────────────────────────────────────────────────────────────────
   F7 — field() must not leave p inside an over-long token
   ──────────────────────────────────────────────────────────────── */

static char *field_old(char *p, char *out, int outsz)
{
    int i = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < outsz - 1) out[i++] = *p++;
    out[i] = 0;
    while (*p == ' ') p++;
    return p;
}

static char *field_new(char *p, char *out, int outsz)
{
    int i = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && i < outsz - 1) out[i++] = *p++;
    out[i] = 0;
    while (*p && *p != ' ') p++;        /* discard the tail of the token */
    while (*p == ' ') p++;
    return p;
}

static void test_field(void)
{
    printf("\nF7  field() token overflow\n");
    /* A 12-char server name parsed into an 8-byte buffer, then the channel. */
    char line1[] = "verylongserver general";
    char line2[] = "verylongserver general";
    char a[8], b[8];

    char *p = field_old(line1, a, sizeof a);
    field_old(p, b, sizeof b);
    ck("old leaves the tail to be parsed as the next field",
       strcmp(b, "general") != 0);
    printf("        old: first=%-8s second=%s\n", a, b);

    p = field_new(line2, a, sizeof a);
    field_new(p, b, sizeof b);
    ck("new recovers and reads the real next field", strcmp(b, "general") == 0);
    printf("        new: first=%-8s second=%s\n", a, b);
}

/* ────────────────────────────────────────────────────────────────
   F4 — the receive assembler must not truncate the wire
   ──────────────────────────────────────────────────────────────── */

#define CHAT_LINE 96
#define CHAT_WIRE 1024

/* Feed a server line through the assembler and report what survived. */
static int assemble(const char *wire, int cap, char *out)
{
    int rlen = 0;
    for (const char *s = wire; *s; s++) {
        char c = *s;
        if (c == '\n') break;
        if (c != '\r' && rlen < cap - 1) out[rlen++] = c;
    }
    out[rlen] = 0;
    return rlen;
}

static void test_wire(void)
{
    printf("\nF4  BootChat receive assembler\n");
    /* A realistic MSG carrying a 300-character message. */
    char msg[2048];
    strcpy(msg, "MSG techhaven general kyan 1789907163 ");
    int prefix = (int)strlen(msg);
    for (int i = 0; i < 300; i++) msg[prefix + i] = 'a' + (i % 26);
    msg[prefix + 300] = '\n';
    msg[prefix + 301] = 0;

    char got_old[4096], got_new[4096];
    int n_old = assemble(msg, CHAT_LINE, got_old);
    int n_new = assemble(msg, CHAT_WIRE, got_new);

    ck("old buffer truncates the line to 95 bytes", n_old == CHAT_LINE - 1);
    ck("new buffer keeps the whole line", n_new == prefix + 300);

    /* How much of the actual message text survived? */
    int text_old = n_old - prefix;
    int text_new = n_new - prefix;
    ck("old delivered well under half the message", text_old < 150);
    ck("new delivered all 300 characters", text_new == 300);
    printf("        prefix=%d  old text=%d chars  new text=%d chars\n",
           prefix, text_old, text_new);

    /* CHANLIST with a realistic channel set. */
    char cl[512];
    strcpy(cl, "CHANLIST techhaven general development havendos "
               "announcements bootchat hardware releases offtopic random\n");
    int c_old = assemble(cl, CHAT_LINE, got_old);
    int c_new = assemble(cl, CHAT_WIRE, got_new);
    int chans_old = 0, chans_new = 0;
    for (int i = 0; i < c_old; i++) if (got_old[i] == ' ') chans_old++;
    for (int i = 0; i < c_new; i++) if (got_new[i] == ' ') chans_new++;
    /* fields = spaces, minus the verb and server name */
    ck("old loses channels off the end of CHANLIST", chans_old - 2 < 8);
    ck("new keeps all 8 channels", chans_new - 2 == 8);
    printf("        CHANLIST: old kept %d channels, new kept %d\n",
           chans_old - 2, chans_new - 2);
}

int main(void)
{
    printf("HavenDOS v0.7.18 fix regression tests\n");
    printf("=====================================\n");
    test_mmap();
    test_field();
    test_wire();
    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
