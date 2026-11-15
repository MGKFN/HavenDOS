# HavenDOS v0.9.5.1 Beta "Hephaestus": the modern look, everywhere

*15 November 2026.*

Kyan liked the look of the new Settings and System apps in 0.9.5 and
asked for it on every app and every screen, including login, setup and
the installer. He also asked for new games and apps.

This release does both. Multitasking is still next, in 0.9.6.

## The look, written down

The rules are in a comment block at the top of `shell/ui.c`, and the
helpers that enforce them are new in the same file:

- **No dashed boxes.** A panel is a filled title band over a plain body
  (`ui_kit_panel`). `ui_draw_window()` now just calls it, so every old
  caller got the new look at once.
- **One top bar.** The app's name is on the left, "ESC close" and the
  clock on the right (`ui_draw_taskbar`).
- **One selection colour.** `ui_sel_colors()` gives the theme accent as
  a background, with black or white text chosen for contrast. Nothing
  uses a bright background any more; on VGA text those blink.
- **Keys go in a footer.** A dark grey strip on the bottom row
  (`ui_kit_footer`, `ui_kit_footer_at`).
- **Headings in the accent, labels dim, values bright** (`ui_kit_heading`,
  `ui_kit_kv`), plus `ui_kit_row`, `ui_kit_cell` and `ui_kit_bar` for
  lists, tables and meters.

## Every screen, restyled

- **Desktop.**
  - The top bar shows "HavenDOS" and the version in the accent colour,
    with today's date on the right.
  - The bottom bar has a "HavenDOS" start pill, the user and role, an
    alerts pill ("! 2 new", or "no alerts") and the clock.
  - Icons are solid tiles in a colour per category. The selected tile is
    drawn in the selection colour.
  - The info strip is just "RAM free / up", with no dashed rule.
- **Start menu, submenus and the right-click menu** have header bands,
  highlighted rows and footers that list their keys. The right-click
  menu's "Display" item was renamed "Next theme", which is what it does.
- **The notification centre** and the frame around boxed apps (solid
  edges, wallpaper around it).
- **Games.**
  - Snake has a solid border and cells, a footer and a game-over panel.
  - Tetris has solid pieces, a `[]` ghost, and a sidebar panel with
    score, lines, level and next piece. The old sidebar ran past the
    edge on an 80-column screen; the new one doesn't.
  - Pong has solid walls, paddles and ball, and its key hint moved from
    the title bar into a footer.
- **Text Editor** (renamed from "Editor"): the file line is quiet text,
  not a blue bar.
- **Paint, Music Player, Quiz and Calendar** have theme bars and
  footers. Calendar marks today in the selection colour. Quiz shows a
  green or red result band.
- **PCI, USB, Boot Log, Self Test and Neofetch** use the standard top
  bar. Their "press a key" lines are footers.
- **Terms and Privacy**: no dashed rules; a solid scrollbar.
- **Shell**: the welcome is a bar (a "HavenDOS" pill, "Shell" and the
  clock) and a fortune, without dashed rules. `ls` lost its 56-dash
  line.
- **Boot splash, setup, installer and login**: solid progress bars,
  solid bands, no dashed separators.

## New games

Open them from the Games menu, or from the shell by name or with `open`.

- **2048** (`2048`). Arrow keys slide the tiles; tiles are coloured by
  value. The best score is kept while the system runs.
- **Breakout** (`breakout`). Five rows of twelve bricks, three lives,
  Space to launch.
- **Tic-Tac-Toe** (`tictactoe`, `ttt`). The computer makes mistakes
  by default. `D` switches it to perfect play (minimax), which never
  loses.
- **Memory** (`memory`). Sixteen cards, eight pairs, with a move count.

## New apps

- **Notes** (`notes`). Up to 18 short notes, saved to
  `HOME/<user>/NOTES.TXT`. `A` adds a note, Enter edits it, `D` deletes it.
- **Stopwatch** (`stopwatch`). Big block digits, laps (`L`), and a
  countdown timer that beeps (`Tab` switches mode).
- **Converter** (`convert`): length, weight, temperature, data size,
  speed and time. It works in millionths with its own 64-bit division,
  because the kernel has no FPU code and no libgcc.

## `open`

`open` on its own lists every app by id. `open <id or name>` opens one,
matched by id or by a case-insensitive name prefix. For example,
`open tetris`, `open text` or `open sys`.

## Stale apps removed

Disk Info, Network Info and User Manager still described the 0.7-era
system: Disk Info reported RAMFS and Network Info reported VirtIO.

- Disk Info now opens System > Storage.
- Network Info now opens System > Network.
- User Manager now opens Settings > Accounts.

## Fixes

- The Haven theme's title text is now light cyan, to suit the new
  bars.
- Game scores in the top bar overlapped "ESC close". They have moved
  left.
- The login screen's account tiles and its "Welcome to HavenDOS" choice
  used the bright accent as a background. They now use the selection
  colour like everything else.
- Settings > About ran past the right edge on an 80-column screen. The
  three long lines are shorter.
- A toast that expired while the shell was open left a stripe of desktop
  colour across the shell. It now blanks to black; the desktop repaints
  that row itself.
- The shell now starts from a cleared screen, so nothing from the login
  screen shows through under the welcome.
- Pong showed the shell line it was opened from on row 1.
- The notification drawer was 25 columns wide and cut messages off. It
  is now 34.
- The login "welcome back" screen said "VirtIO mounted" or "Disk: not
  found", and only knew two network cards. It now says whether a disk
  is mounted and whether a network adapter is up, and it centres on any
  console size.
- Settings > Appearance previews a window the new way (title band, no
  dashed frame). Terms and Privacy has the standard bar, a panel and a
  footer.

## Tests and checks

- New suite **`test_games2`** (33 checks):
  - 2048 slides and merges, including that a tile merges only once per
    move;
  - Tic-Tac-Toe's perfect computer never loses, checked over every line
    of play;
  - the converter's conversions;
  - `udiv64` checked against the C library;
  - stopwatch formatting.
- **`test_chrome`** was rewritten for the new bars: 58 checks, floor
  raised from 42 to 58.
- `test_theme` gained the stubs the new bars need.
- `check_help.py` holds the eight new commands and `open` to the
  command reference, like every other command.
