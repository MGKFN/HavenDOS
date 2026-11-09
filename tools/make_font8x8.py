#!/usr/bin/env python3
"""Regenerate include/font8x8.h from font8x8_basic.h (public domain).
   Source: https://github.com/dhepper/font8x8
   Bit-reverses each row: font8x8 is LSB-first, VGA plane 2 is MSB-first."""
import re, sys
src = open(sys.argv[1]).read()
rows = re.findall(r'\{\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){7}0x[0-9A-Fa-f]{2})\s*\}', src)
glyphs = [[int(x, 0) for x in re.findall(r'0x[0-9A-Fa-f]{2}', r)] for r in rows]
rev = lambda b: sum((0x80 >> i) for i in range(8) if b & (1 << i))
print(len(glyphs), "glyphs converted")
