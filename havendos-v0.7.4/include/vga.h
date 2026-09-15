#pragma once
#include "types.h"
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
typedef enum {
    VGA_BLACK=0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
    VGA_YELLOW, VGA_WHITE
} vga_color_t;
void vga_init(void);
void vga_clear(void);
void vga_flush(void);
void vga_scroll(void);
void vga_putchar(char c);
void vga_puts(const char *s);
void vga_printf(const char *fmt, ...);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_set_cursor(int x, int y);
void vga_get_cursor(int *x, int *y);
void vga_putchar_at(char c, int x, int y, vga_color_t fg, vga_color_t bg);
void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg);
void vga_fill_row(int y, char c, vga_color_t fg, vga_color_t bg);
void vga_getchar_at(int x, int y, uint8_t *ch, uint8_t *attr);
void vga_putraw_at(uint8_t ch, uint8_t attr, int x, int y);
void vga_shadow_write(int x, int y, uint16_t val);
uint16_t vga_shadow_read(int x, int y);
