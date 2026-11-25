/* tests/test_png.c -- kernel/png.c (v0.9.10).
 *
 * Judged by PIL: tests/png_fixtures.inc holds pictures PIL wrote in every
 * colour type the decoder claims (zlib level 9, 1 and 0 -- dynamic,
 * fixed and stored blocks) and the pixels PIL reads back from them.
 * Then round trips through HavenDOS's own encoder, which only makes
 * fixed-Huffman blocks -- so a decoder that only understood its own
 * encoder would fail the fixtures, and an encoder that only its own
 * decoder understood is checked by `python3 -c` in the release notes'
 * method (and by test_png's CRC and Adler checks below).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../kernel/string.c"
void *kmalloc(uint32_t n){ return malloc(n ? n : 1); }
void  kfree(void *p){ free(p); }
#include "../kernel/png.c"
#include "png_fixtures.inc"

static int pass, fail;
static void ck(const char *what, int ok, long got)
{
    if (ok) { pass++; printf("  ok    %s\n", what); }
    else { fail++; printf("  FAIL  %s  (got %ld)\n", what, got); }
}

int main(void)
{
    printf("test_png: kernel/png.c\n");
    ck("CRC-32 of \"IEND\" is the one every PNG ends with",
       png_crc32(0, (const uint8_t *)"IEND", 4) == 0xAE426082u, (long)png_crc32(0, (const uint8_t *)"IEND", 4));
    ck("Adler-32 of \"Wikipedia\" is 0x11E60398", png_adler32(1, (const uint8_t *)"Wikipedia", 9) == 0x11E60398u, 0);

    /* every fixture PIL wrote */
    for (unsigned k = 0; k < sizeof fx_all / sizeof fx_all[0]; k++) {
        char what[96]; int w = 0, h = 0;
        uint32_t *img = image_decode(fx_all[k].f, fx_all[k].n, &w, &h, FX_BG);
        if (!strcmp(fx_all[k].name, "interlaced")) {
            snprintf(what, sizeof what, "interlaced: refused, and says so");
            ck(what, !img && png_error && strstr(png_error, "interlaced"), 0);
            continue;
        }
        long bad = -1;
        if (img && w == FX_W && h == FX_H) {
            bad = 0;
            for (int i = 0; i < FX_W * FX_H; i++) if (img[i] != fx_all[k].px[i]) bad++;
        }
        snprintf(what, sizeof what, "%s: every pixel what PIL reads", fx_all[k].name);
        ck(what, bad == 0, bad);
        free(img);
    }

    /* round trips through our own encoder */
    {
        int w = 301, h = 97;
        uint32_t *px = malloc((size_t)w * h * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            px[y * w + x] = (uint32_t)((x * 2654435761u) ^ (y * 40503u)) & 0xFFFFFF;
        uint32_t n = 0; uint8_t *f = png_encode(px, w, h, w, &n);
        int dw, dh; uint32_t *back = f ? png_decode(f, n, &dw, &dh, 0) : 0;
        long bad = -1;
        if (back && dw == w && dh == h) { bad = 0; for (int i = 0; i < w * h; i++) if (back[i] != px[i]) bad++; }
        ck("noise: encoded and decoded, every pixel the same", bad == 0, bad);
        /* the CRC of every chunk is right (a viewer checks them, we do not) */
        int crcs = 1;
        for (uint32_t p = 8; f && p + 12 <= n; ) {
            uint32_t len = be32(f + p);
            if (png_crc32(0, f + p + 4, len + 4) != be32(f + p + 8 + len)) crcs = 0;
            p += 12 + len;
        }
        ck("  ... and every chunk's CRC is right", f && crcs, 0);
        free(f); free(back);

        /* a screenshot-like picture: flat panels, a little text */
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            px[y * w + x] = y < 20 ? 0x2c3a50 : ((x / 7 + y / 9) % 23 == 0 ? 0xe8e4da : 0x1e232c);
        f = png_encode(px, w, h, w, &n);
        back = f ? png_decode(f, n, &dw, &dh, 0) : 0;
        bad = -1;
        if (back) { bad = 0; for (int i = 0; i < w * h; i++) if (back[i] != px[i]) bad++; }
        ck("a flat UI picture round-trips", bad == 0, bad);
        ck("  ... at under a tenth of its raw size", f && n * 10 < (uint32_t)(w * h * 3), (long)n);
        free(f); free(back); free(px);
    }

    /* damage */
    {
        uint8_t *c = malloc(sizeof fx_rgb); memcpy(c, fx_rgb, sizeof fx_rgb);
        int w, h;
        uint32_t *img = png_decode(c, sizeof fx_rgb / 2, &w, &h, 0);
        ck("cut in half: refused, not a crash", img == 0 && png_error, 0);
        free(img);
        c[sizeof fx_rgb / 2] ^= 0x55;
        img = png_decode(c, sizeof fx_rgb, &w, &h, 0);
        ck("a byte of its picture data changed: refused (Adler-32), not a crash", img == 0 && png_error, 0);
        free(img); free(c);
        img = image_decode((const uint8_t *)"GIF89a....", 10, &w, &h, 0);
        ck("not a PNG or BMP: refused by name", img == 0 && png_error && strstr(png_error, "PNG or BMP"), 0);
    }
    printf("\n%d/%d passed\n", pass, pass + fail);
    return fail ? 1 : 0;
}
