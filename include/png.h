/* include/png.h -- PNG and BMP (kernel/png.c), v0.9.10.
   Pixels are 0x00RRGGBB. Every buffer returned is kmalloc()'d. */
#pragma once
#include "types.h"

extern const char *png_error;          /* why the last decode failed, in words */

uint32_t  png_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
uint32_t  png_adler32(uint32_t a, const uint8_t *p, uint32_t n);
int       png_inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap);
int       png_inflate_zlib(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap);
int       png_deflate_zlib(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap);

int       png_info(const uint8_t *f, uint32_t n, int *w, int *h);
/* bg: what a transparent pixel is blended over */
uint32_t *png_decode(const uint8_t *f, uint32_t n, int *w, int *h, uint32_t bg);
uint8_t  *png_encode(const uint32_t *px, int w, int h, int pitch, uint32_t *outlen);
uint32_t *bmp_decode(const uint8_t *f, uint32_t n, int *w, int *h);
uint32_t *image_decode(const uint8_t *f, uint32_t n, int *w, int *h, uint32_t bg);
