/* tests/pmm_host.inc -- the physical allocator, on the host (v0.9.9).
 *
 * drivers/wm_core.inc takes each window's surface and glyph table from
 * pmm_alloc_contig(). A suite that compiles drivers/vga.c gets this:
 * calloc'd blocks, counted, and a switch to refuse them -- so the
 * window manager's no-surface path (a machine out of memory) is
 * tested too. */
#include <stdlib.h>
static int  g_pmm_refuse;            /* 1: every allocation fails      */
static long g_pmm_live;              /* blocks handed out, not freed   */
__attribute__((weak)) void *pmm_alloc_contig(uint32_t n)
{
    if (g_pmm_refuse || !n) return 0;
    void *p = calloc(n, 4096);
    if (p) g_pmm_live += n;
    return p;
}
__attribute__((weak)) void pmm_free_contig(void *p, uint32_t n)
{
    if (!p) return;
    g_pmm_live -= n;
    free(p);
}
