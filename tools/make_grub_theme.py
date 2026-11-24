#!/usr/bin/env python3
"""
tools/make_grub_theme.py -- the boot menu's look, in the THS colours.

v0.9.7.8. Kyan, 24 Nov: the GRUB menu, installed and on the ISO, was plain
-- "no background, fancy text, nothing" -- and should follow the look of
the THS documents and website. That look is the studio's "Reference
Manual" system (MGKFN/techhavenstudios, src/styles.css): warm off-white
type on cool graphite, one oxide-red spot colour, hairline rules, no glow.
The background here is a graphite fade, top to bottom, like the glass of a
monitor.

One run writes the same theme to two places:

  isodir/boot/grub/themes/havendos/      for the ISO's own menu
  include/grub_theme_embed.h             the same bytes, built into the
                                         kernel, which fs/esp.c writes next
                                         to grub.cfg on the EFI partition
                                         at install time
  include/grub_theme.h                   just the file names, for
                                         fs/uninstall.c to remove

The outputs are committed, so a build needs neither PIL nor grub-mkfont.
Run it again after changing anything here:

    python3 tools/make_grub_theme.py

Nothing in the picture can go stale: no version number, no date. The menu
entries carry the version, written by tools/make_version.py.
"""
import io
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO_DIR = os.path.join(ROOT, "isodir", "boot", "grub", "themes", "havendos")
EMBED_H = os.path.join(ROOT, "include", "grub_theme_embed.h")
NAMES_H = os.path.join(ROOT, "include", "grub_theme.h")

# ── the THS console palette (src/styles.css, :root) ────────────────────
GRAPHITE_TOP = (0x1e, 0x23, 0x2c)   # --surface-raised
GRAPHITE     = (0x13, 0x16, 0x1c)   # --background
GRAPHITE_BOT = (0x0c, 0x0e, 0x12)   # a shade under it, for the fade
PAPER        = (0xe8, 0xe4, 0xda)   # --foreground: warm off-white
MUTED        = (0x9a, 0xa0, 0xaa)   # --muted-foreground
FAINT        = (0x8b, 0x91, 0x9b)   # --faint
OXIDE        = (0xe2, 0x67, 0x3f)   # --oxide: text, rules, icons
OXIDE_STRONG = (0xef, 0x7a, 0x52)   # --oxide-strong
OXIDE_FILL   = (0xb8, 0x3d, 0x22)   # --oxide-fill: a fill that carries white text
RULE         = (0x3a, 0x3e, 0x46)   # --rule, flattened onto graphite


def hexc(c):
    return "#%02x%02x%02x" % c


W, H = 1920, 1080

MONO      = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
MONO_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"


def ttf(path, size):
    return ImageFont.truetype(path, size)


def background():
    im = Image.new("RGB", (W, H))
    px = im.load()
    # The fade: graphite-raised at the top through graphite to a shade
    # under it at the foot, in two straight runs. One colour per row, so
    # the PNG compresses to almost nothing.
    for y in range(H):
        t = y / (H - 1)
        if t < 0.45:
            a, b, u = GRAPHITE_TOP, GRAPHITE, t / 0.45
        else:
            a, b, u = GRAPHITE, GRAPHITE_BOT, (t - 0.45) / 0.55
        c = tuple(round(a[i] + (b[i] - a[i]) * u) for i in range(3))
        for x in range(W):
            px[x, y] = c

    d = ImageDraw.Draw(im)
    L = round(W * 0.10)                      # the left margin, as in theme.txt

    # The masthead: a small oxide label, the wordmark, an oxide spot rule.
    d.text((L, round(H * 0.11)), "TECH HAVEN STUDIOS", font=ttf(MONO_BOLD, 22),
           fill=OXIDE)
    d.text((L - 4, round(H * 0.11) + 34), "HavenDOS", font=ttf(MONO_BOLD, 96),
           fill=PAPER)
    ry = round(H * 0.11) + 34 + 122
    d.rectangle([L, ry, L + 120, ry + 3], fill=OXIDE)
    d.line([(L + 132, ry + 1), (W - L, ry + 1)], fill=RULE, width=1)
    d.text((L, ry + 18), "BOOT SELECTOR", font=ttf(MONO, 20), fill=FAINT)

    # The foot: a hairline and a spec line, as on a manual's cover.
    fy = round(H * 0.90)
    d.line([(L, fy), (W - L, fy)], fill=RULE, width=1)
    spec = "THS-BOOT   32-BIT x86   BIOS + UEFI"
    f = ttf(MONO, 18)
    tw = d.textlength(spec, font=f)
    d.text((W - L - tw, fy + 14), spec, font=f, fill=FAINT)
    return im


def solid(w, h, c):
    im = Image.new("RGB", (w, h), c)
    return im


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


# The highlight behind the selected entry: an oxide fill with a brighter
# oxide rule down its left side. GRUB draws it as a nine-piece box; every
# piece is given, so no version of GRUB has to guess about a missing one.
def selection_pieces():
    p = {}
    for k in ("n", "ne", "e", "se", "s", "sw", "nw"):
        p[k] = solid(1, 1, OXIDE_FILL)
    p["w"] = solid(5, 1, OXIDE_STRONG)
    p["c"] = solid(4, 4, OXIDE_FILL)
    return {f"sel_{k}.png": png_bytes(v) for k, v in p.items()}


FONTS = [   # (file, ttf, size) -- ASCII and a middle dot, nothing more
    ("dvm20.pf2", MONO, 20),
    ("dvmb20.pf2", MONO_BOLD, 20),
    ("dvm16.pf2", MONO, 16),
]


def make_font(src, size):
    with tempfile.TemporaryDirectory() as t:
        out = os.path.join(t, "f.pf2")
        subprocess.run(["grub-mkfont", "-s", str(size), "-r", "0x20-0x7E,0xB7-0xB7",
                        "-o", out, src], check=True)
        return open(out, "rb").read()


def font_name(data):
    """The name GRUB knows a .pf2 by: its NAME section."""
    i = data.find(b"NAME")
    n = int.from_bytes(data[i + 4:i + 8], "big")
    return data[i + 8:i + 8 + n].rstrip(b"\0").decode()


def theme_txt(names):
    reg20, bold20, reg16 = names
    return f"""# HavenDOS boot menu -- generated by tools/make_grub_theme.py.
# The THS console look: off-white on graphite, one oxide-red spot colour.
title-text: ""
desktop-image: "bg.png"
desktop-image-scale-method: "stretch"
desktop-color: "{hexc(GRAPHITE)}"
terminal-font: "{reg20}"
terminal-left: "10%"
terminal-top: "36%"
terminal-width: "80%"
terminal-height: "50%"
terminal-border: "0"

+ boot_menu {{
    left = 10%
    top = 36%
    width = 80%
    height = 48%
    item_font = "{reg20}"
    item_color = "{hexc(MUTED)}"
    selected_item_font = "{bold20}"
    selected_item_color = "#ffffff"
    selected_item_pixmap_style = "sel_*.png"
    item_height = 38
    item_padding = 18
    item_spacing = 6
    icon_width = 0
    icon_height = 0
    item_icon_space = 0
    scrollbar = false
}}

+ label {{
    id = "__timeout__"
    left = 10%
    top = 92%
    width = 40%
    height = 24
    align = "left"
    font = "{reg16}"
    color = "{hexc(OXIDE)}"
    text = "Starting in %d s"
}}

+ label {{
    left = 10%
    top = 95%
    width = 80%
    height = 24
    align = "left"
    font = "{reg16}"
    color = "{hexc(FAINT)}"
    text = "Up/Down choose · Enter start · E edit · C command line"
}}
"""


def main():
    files = {}
    files["bg.png"] = png_bytes(background())
    files.update(selection_pieces())
    names = []
    for fn, src, size in FONTS:
        data = make_font(src, size)
        files[fn] = data
        names.append(font_name(data))
    files["theme.txt"] = theme_txt(names).encode("utf-8")

    os.makedirs(ISO_DIR, exist_ok=True)
    for fn, data in files.items():
        with open(os.path.join(ISO_DIR, fn), "wb") as f:
            f.write(data)

    order = sorted(files)
    with open(NAMES_H, "w") as h:
        h.write("/* GENERATED by tools/make_grub_theme.py -- do not edit.\n"
                "   The boot menu's theme files, as fs/esp.c writes them next to\n"
                "   grub.cfg in /EFI/HAVENDOS and fs/uninstall.c removes them. */\n"
                "#ifndef GRUB_THEME_H\n#define GRUB_THEME_H\n")
        h.write(f"#define GRUB_THEME_NFILES {len(order)}\n")
        h.write("static const char *const grub_theme_names[GRUB_THEME_NFILES] = {\n")
        for fn in order:
            h.write(f'    "{fn}",\n')
        h.write("};\n")
        h.write("/* the font lines a grub.cfg loads before `set theme` */\n")
        for fn, _, _ in FONTS:
            h.write(f'#define GRUB_THEME_FONT_{fn.split(".")[0].upper()} "{fn}"\n')
        h.write("#endif\n")

    with open(EMBED_H, "w") as h:
        h.write("/* GENERATED by tools/make_grub_theme.py -- do not edit.\n"
                "   The boot menu's theme, built in, for fs/esp.c ONLY: every\n"
                "   file that includes this carries a copy of these bytes. */\n"
                "#ifndef GRUB_THEME_EMBED_H\n#define GRUB_THEME_EMBED_H\n"
                '#include "grub_theme.h"\n')
        for i, fn in enumerate(order):
            data = files[fn]
            h.write(f"static const unsigned char grub_theme_d{i}[{len(data)}] = {{\n")
            for j in range(0, len(data), 16):
                h.write("    " + ",".join(f"0x{b:02x}" for b in data[j:j + 16]) + ",\n")
            h.write("};\n")
        h.write("static const unsigned char *const grub_theme_data[GRUB_THEME_NFILES] = {\n")
        for i in range(len(order)):
            h.write(f"    grub_theme_d{i},\n")
        h.write("};\n")
        h.write("static const unsigned int grub_theme_size[GRUB_THEME_NFILES] = {\n")
        for fn in order:
            h.write(f"    {len(files[fn])}u,\n")
        h.write("};\n#endif\n")

    total = sum(len(v) for v in files.values())
    print(f"make_grub_theme: {len(files)} files, {total} bytes; fonts: {', '.join(names)}")


if __name__ == "__main__":
    sys.exit(main())
