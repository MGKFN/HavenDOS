#!/usr/bin/env python3
"""tools/make_png_fixtures.py -- the pictures tests/test_png.c judges
kernel/png.c by (v0.9.10). Written by PIL (zlib level 9, so DYNAMIC
Huffman blocks -- the part of inflate HavenDOS's own deflate never
makes), in every colour type the decoder claims, with the pixels PIL
itself reads back as the expected answer.

    python3 tools/make_png_fixtures.py tests/png_fixtures.inc
"""
import io
import sys
from PIL import Image

W, H = 37, 23          # odd sizes: row strides that are not a multiple of 4


def picture():
    im = Image.new("RGBA", (W, H))
    p = im.load()
    for y in range(H):
        for x in range(W):
            r = (x * 7 + y * 3) & 255
            g = (x * x + y * 11) & 255
            b = 255 if (x // 5 + y // 4) % 2 else 40
            a = 255 if x < W - 6 else (x * 40) & 255
            p[x, y] = (r, g, b, a)
    return im


BG = (0x20, 0x40, 0x60)


def expect(im):
    """What the decoder must produce: alpha over BG, 0x00RRGGBB."""
    rgba = im.convert("RGBA")
    out = []
    for y in range(H):
        for x in range(W):
            r, g, b, a = rgba.getpixel((x, y))
            def mix(c, k):
                return (c * a + k * (255 - a) + 127) // 255
            out.append((mix(r, BG[0]) << 16) | (mix(g, BG[1]) << 8) | mix(b, BG[2]))
    return out


def blob(im, **kw):
    f = io.BytesIO()
    im.save(f, "PNG", optimize=False, compress_level=9, **kw)
    return f.getvalue()


def cases():
    base = picture()
    rgb = base.convert("RGB")
    yield "rgb", blob(rgb), expect(rgb)
    f = io.BytesIO(); rgb.save(f, "PNG", compress_level=0)
    yield "rgb_stored", f.getvalue(), expect(rgb)        # stored blocks
    f = io.BytesIO(); rgb.save(f, "PNG", compress_level=1)
    yield "rgb_fast", f.getvalue(), expect(rgb)          # zlib's fastest (fixed + dynamic)
    yield "rgba", blob(base), expect(base)
    grey = rgb.convert("L")
    yield "grey", blob(grey), expect(grey)
    la = base.convert("LA")
    yield "grey_alpha", blob(la), expect(la)
    pal = rgb.quantize(colors=200)
    yield "palette", blob(pal), expect(pal)
    p16 = rgb.quantize(colors=16)
    yield "palette4", blob(p16, bits=4), expect(p16)
    bw = grey.point(lambda v: 255 if v > 128 else 0).convert("1")
    yield "bilevel", blob(bw), expect(bw)
    f = io.BytesIO(); rgb.save(f, "BMP")
    yield "bmp24", f.getvalue(), expect(rgb)
    # PIL cannot WRITE Adam7: the header's interlace byte set by hand (and
    # its CRC fixed) is enough -- the decoder must refuse before the data
    import zlib, struct
    d = bytearray(blob(rgb)); d[28] = 1
    d[29:33] = struct.pack(">I", zlib.crc32(bytes(d[12:29])) & 0xFFFFFFFF)
    yield "interlaced", bytes(d), []                      # refused, by name


def carr(name, data, ty="uint8_t", per=16, fmt="0x%02x"):
    s = ["static const %s %s[%d] = {" % (ty, name, len(data))]
    for i in range(0, len(data), per):
        s.append("    " + ",".join(fmt % v for v in data[i:i + per]) + ",")
    s.append("};")
    return "\n".join(s)


def main():
    out = ["/* tests/png_fixtures.inc -- written by tools/make_png_fixtures.py; do not edit */",
           "#define FX_W %d" % W, "#define FX_H %d" % H,
           "#define FX_BG 0x%06x" % ((BG[0] << 16) | (BG[1] << 8) | BG[2])]
    names = []
    for name, data, px in cases():
        out.append(carr("fx_%s" % name, data))
        out.append(carr("fx_%s_px" % name, px or [0], "uint32_t", 8, "0x%06x"))
        names.append(name)
    out.append("static const struct { const char *name; const uint8_t *f; uint32_t n; const uint32_t *px; } fx_all[] = {")
    for n in names:
        out.append('    { "%s", fx_%s, sizeof fx_%s, fx_%s_px },' % (n, n, n, n))
    out.append("};")
    open(sys.argv[1], "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
