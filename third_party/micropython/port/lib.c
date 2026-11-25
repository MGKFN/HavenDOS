/* What MicroPython needs from libc that the HavenDOS kernel does not
   already have (kernel/string.c has memcpy, memset, memmove, memcmp,
   strlen, strcmp, strncmp, strchr, strcpy, strcat ...). */
#include <stddef.h>
#include <stdint.h>

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return 0;
}
char *strrchr(const char *s, int c) {
    const char *r = 0;
    do { if (*s == (char)c) r = s; } while (*s++);
    return (char *)r;
}
void abort(void) {
    extern void __fatal_error(const char *msg);
    __fatal_error("python: abort\n");
    for (;;) { }
}
float fabsf(float x) { union { float f; uint32_t u; } v = { x }; v.u &= 0x7FFFFFFFu; return v.f; }
float copysignf(float x, float y) {
    union { float f; uint32_t u; } a = { x }, b = { y };
    a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u); return a.f;
}
float nanf(const char *tag) { (void)tag; union { uint32_t u; float f; } v = { 0x7FC00000u }; return v.f; }
/* gcc joins sinf(x) and cosf(x) of one x into a call to this */
float sinf(float x);
float cosf(float x);
void sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
