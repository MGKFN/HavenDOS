#!/usr/bin/env python3
"""make_icons.py -- the HavenDOS icon family (v0.9.8).

Every app and a handful of system functions get one icon: a rounded
square tile in its category's colour with a white symbol on it. The
symbols are Material Symbols Rounded (Apache 2.0, Google), filled,
weight 500 -- tools/assets/msr-icons.ttf is the subset this file uses
(make it with --subset from the full font).

Each icon is drawn at 8x its size and averaged down, at 16, 24, 32 and
48 pixels. What is stored is not colour but two coverages a pixel, four
bits each: how much of the pixel is TILE and how much is SYMBOL. The
kernel (drivers/vga.c, wm_icon) mixes those over whatever is behind --
the gradient, a menu, the taskbar -- with the tile colour shaded top to
bottom, so one icon works on every background and every theme without
reading the screen back. 4160 bytes an icon.

    python3 tools/make_icons.py            -> include/icons.h
    python3 tools/make_icons.py --subset FULL.ttf   (refresh the subset)
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONT = os.path.join(HERE, "assets", "msr-icons.ttf")
# v0.9.10: symbols added after the subset above was cut -- instanced from
# the full variable font at FILL 1, wght 500, GRAD 0, opsz 48 and kept in
# a second file, so every icon before them renders byte for byte as it did
EXTRA = os.path.join(HERE, "assets", "msr-icons-extra.ttf")
OUT = os.path.join(ROOT, "include", "icons.h")
SIZES = (16, 24, 32, 48)

# THS colours (the Studio theme's palette, see shell/themecmd.c)
CAT = {
    "apps":     0x3d78a8,
    "system":   0x4a7f99,
    "hardware": 0x4f8a45,
    "games":    0x76598f,
    "media":    0xb07a33,
    "settings": 0x5b6675,
    "oxide":    0xc0462c,
    "graphite": 0x2c3442,
}

# id, symbol, colour. The id is the app id in shell/desktop.c's appreg[]
# (so the desktop finds an app's icon by name), or a system name.
ICONS = [
    ("shell",     "terminal",            "graphite"),
    ("edit",      "description",         "apps"),
    ("calc",      "calculate",           "apps"),
    ("files",     "folder",              "media"),
    ("todo",      "checklist",           "apps"),
    ("havencode", "code",                "apps"),
    ("hex",       "data_object",         "apps"),
    ("paint",     "brush",               "oxide"),
    ("calendar",  "calendar_month",      "apps"),
    ("asciiart",  "text_fields",         "apps"),
    ("notes",     "sticky_note_2",       "media"),
    ("stopwatch", "timer",               "apps"),
    ("convert",   "swap_horiz",          "apps"),
    ("retro",     "computer",            "graphite"),
    ("system",    "monitor_heart",       "system"),
    ("clock",     "schedule",            "system"),
    ("neofetch",  "info",                "system"),
    ("notif",     "notifications",       "system"),
    ("users",     "group",               "system"),
    ("audit",     "receipt_long",        "system"),
    ("bpkg",      "inventory_2",         "system"),
    ("install",   "install_desktop",     "oxide"),
    ("selftest",  "fact_check",          "system"),
    ("disk",      "hard_drive",          "hardware"),
    ("net",       "lan",                 "hardware"),
    ("pci",       "developer_board",     "hardware"),
    ("usb",       "usb",                 "hardware"),
    ("bootlog",   "article",             "hardware"),
    ("snake",     "gesture",             "games"),
    ("tetris",    "view_quilt",          "games"),
    ("pong",      "sports_tennis",       "games"),
    ("quiz",      "quiz",                "games"),
    ("mines",     "flag",                "games"),
    ("2048",      "grid_view",           "games"),
    ("breakout",  "view_week",           "games"),
    ("tictactoe", "tag",                 "games"),
    ("memory",    "style",               "games"),
    ("music",     "music_note",          "media"),
    ("matrix",    "grain",               "media"),
    ("stars",     "star",                "media"),
    ("pipes",     "plumbing",            "media"),
    ("screensvr", "bedtime",             "media"),
    ("hacker",    "security",            "media"),
    ("fortune",   "auto_awesome",        "media"),
    ("settings",  "settings",            "settings"),
    ("themes",    "palette",             "settings"),
    ("guide",     "help",                "settings"),
    ("manual",    "menu_book",           "settings"),
    ("legal",     "gavel",               "settings"),
    ("power",     "power_settings_new",  "oxide"),
    # system functions
    ("_app",      "apps",                "settings"),   # an app with no icon of its own
    ("_start",    "grid_view",           "oxide"),
    ("_search",   "search",              "settings"),
    ("_lock",     "lock",                "settings"),
    ("_logout",   "logout",              "settings"),
    ("_restart",  "restart_alt",         "settings"),
    ("_user",     "person",              "apps"),
    ("_bell",     "notifications",       "settings"),
    ("_star",     "star",                "media"),
    ("_history",  "history",             "settings"),
    ("_folder",   "folder",              "media"),
    ("_file",     "article",               "settings"),
    ("_usbdrive", "usb",                 "hardware"),
    ("_hdd",      "hard_drive",          "hardware"),
    ("_desktop",  "desktop_windows",     "settings"),
    ("images",    "image",               "media"),       # v0.9.10
    ("python",    "code_blocks",         "graphite"),    # v0.9.10
]

def codepoints(font_path):
    from fontTools.ttLib import TTFont
    f = TTFont(font_path)
    rev = {}
    for cp, g in f.getBestCmap().items():
        rev.setdefault(g, cp)
    return rev

def subset(full):
    from fontTools import subset as fsub
    rev = codepoints(full)
    cps = sorted({rev[s] for _, s, _ in ICONS if s in rev})
    opts = fsub.Options(); opts.layout_features = []; opts.notdef_outline = True; opts.glyph_names = True; opts.legacy_kern = False
    font = fsub.load_font(full, opts)
    sub = fsub.Subsetter(opts); sub.populate(unicodes=cps); sub.subset(font)
    os.makedirs(os.path.dirname(FONT), exist_ok=True)
    fsub.save_font(font, FONT, opts)
    print("wrote", FONT, os.path.getsize(FONT), "bytes,", len(cps), "symbols")

def render(sym_cp, size, font_path=None):
    S = 8 * size
    tile = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(tile)
    r = int(S * 0.24)
    inset = max(1, S // 32)
    d.rounded_rectangle((inset, inset, S - 1 - inset, S - 1 - inset), radius=r, fill=255)
    glyph = Image.new("L", (S, S), 0)
    if sym_cp is not None:
        fs = int(S * (0.70 if size <= 16 else 0.64))
        font = ImageFont.truetype(font_path or FONT, fs)
        ch = chr(sym_cp)
        bb = font.getbbox(ch)
        gw, gh = bb[2] - bb[0], bb[3] - bb[1]
        gd = ImageDraw.Draw(glyph)
        gd.text(((S - gw) / 2 - bb[0], (S - gh) / 2 - bb[1]), ch, font=font, fill=255)
    tile = tile.resize((size, size), Image.BOX)
    glyph = glyph.resize((size, size), Image.BOX)
    out = bytearray()
    tp, gp = tile.load(), glyph.load()
    for y in range(size):
        for x in range(size):
            t = (tp[x, y] * 15 + 127) // 255
            g = (gp[x, y] * 15 + 127) // 255
            out.append((t << 4) | g)
    return out

def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--subset":
        subset(sys.argv[2]); return
    rev = codepoints(FONT)
    rev2 = codepoints(EXTRA) if os.path.exists(EXTRA) else {}
    lines = ["/* include/icons.h -- GENERATED by tools/make_icons.py, do not edit.",
             " * The HavenDOS icon family: symbols from Material Symbols Rounded",
             " * (Apache License 2.0, Google). Each pixel is two 4-bit coverages:",
             " * high nibble the tile, low nibble the white symbol. */",
             "#ifndef ICONS_H", "#define ICONS_H", "#include \"types.h\"",
             "#define ICON_COUNT %d" % len(ICONS),
             "#define ICON_SIZES 4",
             "static const uint8_t icon_size_px[ICON_SIZES] = {%s};" % ",".join(map(str, SIZES)),
             "static const uint32_t icon_size_off[ICON_SIZES] = {%s};" % ",".join(
                 str(sum(s * s for s in SIZES[:i])) for i in range(len(SIZES))),
             "#define ICON_STRIDE %d" % sum(s * s for s in SIZES),
             "static const char *const icon_name[ICON_COUNT] = {"]
    lines += ["    \"%s\"," % n for n, _, _ in ICONS]
    lines += ["};", "static const uint32_t icon_rgb[ICON_COUNT] = {"]
    lines += ["    0x%06x," % CAT[c] for _, _, c in ICONS]
    lines += ["};", "static const uint8_t icon_px[ICON_COUNT * ICON_STRIDE] = {"]
    missing = []
    for n, sym, _ in ICONS:
        cp, fp = rev.get(sym), None
        if cp is None and sym in rev2:
            cp, fp = rev2[sym], EXTRA
        if cp is None:
            missing.append(sym)
        data = bytearray()
        for s in SIZES:
            data += render(cp, s, fp)
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    lines += ["};", "#endif", ""]
    open(OUT, "w").write("\n".join(lines))
    print("wrote", OUT, len(ICONS), "icons", "missing symbols: %s" % missing if missing else "")

if __name__ == "__main__":
    main()
