/* tests/test_ramfs.c -- fs/ramfs.c, the live session's filesystem.
   v0.9.7.1: a file over 2 KB was cut at 2 KB without a word. It now
   goes to the heap above that, and comes back whole. */
/* No libc headers: fs/ramfs.c brings the kernel's string.h, whose
   strncat disagrees with glibc's. The libc functions link all the same. */
#include "../include/types.h"
int   printf(const char *, ...);
void *malloc(size_t);
void  free(void *);
static int g_live, g_fail_alloc;
void *kmalloc(size_t n){ if(g_fail_alloc) return 0; g_live++; return malloc(n); }
void  kfree(void *p){ if(p){ g_live--; free(p); } }
#include "../fs/ramfs.c"

static int total, fails;
static void ck(const char *w, int ok, long got)
{ total++; if(ok) printf("  ok    %s\n", w); else { fails++; printf("  FAIL  %s  (%ld)\n", w, got); } }

int main(void)
{
    static char big[20000], out[20000];
    int i, n;
    printf("test_ramfs -- the live session's files, small and big\n");
    ramfs_init();
    for(i = 0; i < (int)sizeof big; i++) big[i] = (char)('a' + i % 26);

    ck("a small file writes", ramfs_write("A.TXT", "hello", 5) == 5, 0);
    n = ramfs_read("A.TXT", out, sizeof out);
    ck("  ... and reads back", n == 5 && !memcmp(out, "hello", 5), n);
    ck("  ... without the heap", g_live == 0, g_live);

    ck("a 20000-byte file writes whole (was cut at 2048)", ramfs_write("B.IMG", big, 20000) == 20000, 0);
    memset(out, 0, sizeof out);
    n = ramfs_read("B.IMG", out, sizeof out);
    ck("  ... and reads back whole", n == 20000 && !memcmp(out, big, 20000), n);
    ck("  ... on the heap, once", g_live == 1, g_live);
    ck("exactly 2048 stays in the entry", ramfs_write("C", big, 2048) == 2048 && g_live == 1, g_live);
    ck("2049 goes to the heap", ramfs_write("C", big, 2049) == 2049 && g_live == 2, g_live);
    n = ramfs_read("C", out, sizeof out);
    ck("  ... and reads back", n == 2049 && !memcmp(out, big, 2049), n);
    ck("rewriting it smaller frees the heap copy", ramfs_write("C", "x", 1) == 1 && g_live == 1, g_live);
    n = ramfs_read("C", out, sizeof out);
    ck("  ... and reads the new contents", n == 1 && out[0] == 'x', n);
    ck("rewriting a big one smaller than its block reuses it",
       ramfs_write("B.IMG", big, 9000) == 9000 && g_live == 1, g_live);
    n = ramfs_read("B.IMG", out, sizeof out);
    ck("  ... and reads 9000", n == 9000 && !memcmp(out, big, 9000), n);
    ck("a short read buffer gets what fits", ramfs_read("B.IMG", out, 100) == 100, 0);
    ck("deleting it frees the heap copy", ramfs_delete("B.IMG") == 0 && g_live == 0, g_live);
    ck("  ... and it is gone", !ramfs_exists("B.IMG") && ramfs_read("B.IMG", out, 10) == -1, 0);
    g_fail_alloc = 1;
    ck("out of memory: the write fails and says so", ramfs_write("D", big, 5000) == -1, 0);
    ck("  ... and an existing file is left as it was",
       ramfs_write("A.TXT", big, 5000) == -1 && ramfs_read("A.TXT", out, sizeof out) == 5 && !memcmp(out, "hello", 5), 0);
    g_fail_alloc = 0;

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
