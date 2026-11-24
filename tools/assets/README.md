# tools/assets -- fonts the build reads (v0.9.8)

- `plexsans-400.ttf`, `plexsans-600.ttf` -- IBM Plex Sans Regular and
  SemiBold (from @fontsource/ibm-plex-sans 5.3.0). Copyright IBM Corp.,
  SIL Open Font License 1.1. Read by tools/make_font_ui.py.
- `msr-icons.ttf` -- a subset of Material Symbols Rounded (material-symbols
  0.49.0, filled, weight 500): only the symbols tools/make_icons.py uses.
  Copyright Google LLC, Apache License 2.0. Regenerate with
  `python3 tools/make_icons.py --subset <full font instance>`.

The generated headers (include/font_ui.h, include/icons.h) are committed,
so a build does not need these files; they are here so the headers can be
made again.
