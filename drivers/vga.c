#include "../include/vga.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/font8x8.h"
#include "../include/font8x8_ext.h"
#include "../include/font8x16.h"

/* ── THE FRAMEBUFFER CELL IS 8 WIDE AND 16 TALL, v0.8.8.2 ────────────
   It was 8x8, and the console stretched every character to twice its
   proper height on every framebuffer machine this has ever run on.

   fbc_fit() picks a scale per axis. 80 columns and 25 rows of 8x8
   cells is 640x200, an aspect ratio of 16:5, and no panel is that
   shape -- so on 1280x800 the scales came out sx=2, sy=4 and a
   character was drawn sixteen pixels wide and thirty-two tall. On
   1920x1080 it was 3 and 5. It was reported as "resolution works but
   is bugged and small" and "seems a bit stretched", which is exactly
   what it was.

   80x25 of 8x16 cells is 640x400 -- 16:10, which divides 1280x800 by
   exactly two in both axes. That is why every VGA text mode used an
   8x16 font for 80x25 and an 8x8 one only for 80x50.

   The TEXT console is untouched: its 80x50 mode uploads 8 rows into
   plane 2 of the character generator and that hardware wants exactly
   8 rows. font8x8[] still goes there. */
#define FBC_CW  8
#define FBC_CH  16
#include <stdarg.h>

/* ── v0.8.8: the text window is a seam, the same idea as io.h's ────────
 *
 * The framebuffer console has been testable since it was written -- it
 * takes the address it draws into as an argument.  The TEXT console never
 * has, because this line is the address, and a native suite that lets one
 * successful 80x25 mode change through dies writing to 0xB8000.  So the
 * only console with sixteen years of VGA register programming behind it
 * was the one no suite could compile, and the five VGA_WIDTH stride bugs
 * v0.7.12 fixed were all found by eye.
 *
 * One override and both consoles come under test, including the thing
 * worth testing most: that the two of them agree.  io.h, mmio.h and
 * machine_asm.h all earn their seams the same way. */
#ifndef VGA_MEM_PTR
#define VGA_MEM_PTR ((uint16_t*)0xB8000)
#endif
#define VGA_MEM VGA_MEM_PTR
static int cur_x=0, cur_y=0;
static vga_color_t cur_fg=VGA_LIGHT_GREY, cur_bg=VGA_BLACK;

/* ── Shadow buffer — anti-flicker double-buffering ──────────────────
   ARCHITECTURE:
   vga_write()  — write-through: shadow + real VGA together, skip if
                  cell unchanged.  Fast path for all normal drawing.

   vga_clear()  — shadow-only: blanks the shadow, does NOT touch real
                  VGA.  This eliminates the classic "white flash":
                  without this fix, clearing VGA then redrawing took
                  1-2 full 14ms VGA refresh cycles on PC hardware and
                  accelerated VM backends (VirtualBox, VMware, QEMU
                  KVM/HVF) — visible as obvious flicker.  On UTM SE
                  TCG the bug was invisible because the software
                  renderer committed every frame atomically.

   vga_flush()  — diffs shadow against real VGA and writes only cells
                  that differ.  Called at the END of full_draw() after
                  the entire new frame is in the shadow buffer.  This
                  means VGA is updated from old frame → new frame in
                  one pass; the user never sees a blank intermediate
                  state.  Also called from keyboard_waitchar() to push
                  any buffered content before blocking.

   vga_shadow_write() / vga_shadow_read() — cursor bypass: the mouse
                  cursor must read and write individual cells without
                  going through the normal path, so it uses these
                  direct accessors that update both shadow and VGA.   */
/* ── v0.7.11: runtime row count (80x25 / 80x50) ───────────────────────────
 * VGA_HEIGHT is now the MAXIMUM (50).  g_rows is the active row count, so the
 * shadow buffer is allocated once at full size and the extra rows simply go
 * unused in 25-row mode.  Everything that scrolls, clears or flushes iterates
 * g_rows rather than the compile-time constant. */
static int g_rows = 25;
static int g_cols = 80;
/* The viewport's state. Declared here rather than beside the block
   that explains it, because the accessors just below are the first
   thing that reads it. See "THE VIEWPORT" further down this file for
   what it is for and why the transform lives in vga_write(). */
static int g_vp_x = 0, g_vp_y = 0;      /* origin on the physical grid */
static int g_vp_w = 0, g_vp_h = 0;      /* 0 = no viewport, full grid  */

/* ── the LOGICAL grid: what an app is told it has ────────────────────
   While a viewport is set these report its size, so an app laid out
   for 80x25 gets 80x25 and everything it derives from them lands
   inside its own box. The physical grid is g_rows/g_cols and is
   reached only from the flush, the framebuffer renderer and the
   hardware cursor -- see the viewport block below vga_write(). */
int  vga_rows(void){ return (g_vp_h > 0) ? g_vp_h : g_rows; }
int  vga_cols(void){ return (g_vp_w > 0) ? g_vp_w : g_cols; }
int  vga_phys_rows(void){ return g_rows; }
int  vga_phys_cols(void){ return g_cols; }

static uint16_t shadow[VGA_CELLS_MAX];

/* ── the framebuffer console ───────────────────────────────────────────
 *
 * HavenDOS looked like 1985 because it was writing characters into VGA text
 * memory at 0xB8000, which is 720x400 and the only thing a BIOS gives you
 * for free.  Under UEFI there is no text memory at all, so that path does
 * not merely look dated -- it shows nothing whatsoever.
 *
 * The fix is cheap here only because of how this file is already built.
 * Every character the OS draws goes through shadow[], and there is exactly
 * ONE place where a cell becomes pixels: the writes to VGA_MEM.  Route those
 * through put_cell() and the whole system -- desktop, every app, the shell,
 * the boot log -- comes up at the monitor's real resolution without a line
 * changing above this driver.  It is how Linux's framebuffer console works,
 * and the seam was already in the right place.
 *
 * The grid stays a character grid.  That is the point: it keeps the retro
 * look, and it keeps every existing layout pixel-correct, instead of
 * requiring 3500 lines of desktop and every app to be rewritten for pixels
 * before anything at all can be displayed.
 *
 * Scaling is independent in X and Y.  An 8x8 font cannot give 80x25 on a
 * 1280x800 screen without stretching -- 80 columns needs 16 px each (2x) and
 * 25 rows needs 32 px each (4x).  That 1:2 pixel aspect is exactly how an
 * 8x8 CGA font looked on a 400-line display, so it is period-correct rather
 * than a mistake, and uniform 2x is still available as 80x50 for anyone who
 * would rather have square pixels and more rows.
 */

static uint32_t *fbc_addr;        /* linear framebuffer, 32bpp            */
static uint32_t  fbc_pitch;       /* in PIXELS, not bytes                 */
static uint32_t  fbc_w, fbc_h;
static int       fbc_sx = 2, fbc_sy = 4;
static uint32_t  fbc_ox = 0, fbc_oy = 0;   /* top-left of the centred grid */
static int       fbc_active;

/* v0.8.9.6: one cell's size in pixels on the glass -- for the mouse,
   which has to move the same distance across the SCREEN for the same
   hand movement whatever grid is on it. A text mode is 720x400 or
   640x400 drawn by the card; 8 wide and 400/rows high is near enough
   for a ratio, which is all the caller uses. */
void vga_cell_px(int *w, int *h)
{
    if (fbc_active) {
        if (w) *w = FBC_CW * fbc_sx;
        if (h) *h = FBC_CH * fbc_sy;
        return;
    }
    if (w) *w = 8;
    if (h) *h = g_rows > 0 ? 400 / g_rows : 16;
}

/* What is actually on the glass.
 *
 * The text path gets this for free: it can read 0xB8000 back and diff
 * against it.  A framebuffer cannot be read back as characters, so the
 * record has to be kept here -- and it has to be kept by EVERY draw, not
 * just by vga_flush().
 *
 * That matters because vga_clear() is deliberately shadow-only: it blanks
 * shadow[] and never touches the screen, so that a redraw followed by one
 * flush repaints only the cells that really changed and no blank frame is
 * ever visible.  If this record were updated only inside flush, a cell drawn
 * by the write-through path would leave it stale, flush would conclude the
 * cell was already correct, and the PREVIOUS screen would show through the
 * gaps of the new one. */
static uint16_t  fbc_painted[VGA_CELLS_MAX];
static int       fbc_painted_valid;

static void fbc_forget(void)
{
    for(int i=0;i<VGA_CELLS_MAX;i++) fbc_painted[i]=0xFFFF;
    fbc_painted_valid=1;
}

/* The 16 VGA attributes as RGB.  These are the standard values, so a theme
   written against the text console looks the same here. */
static const uint32_t fbc_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

int vga_fb_active(void) { return fbc_active; }
uint32_t vga_fb_width(void)  { return fbc_w; }
uint32_t vga_fb_height(void) { return fbc_h; }

/* Pick the glyph scale that gets closest to a requested grid without
   overflowing the panel, and report the grid that actually results. */
/* Pick the scale for a requested grid, then GIVE THAT GRID -- and centre it.
 *
 * This used to compute sx and sy from the grid that was asked for and then
 * recompute g_cols and g_rows from the framebuffer, which is not the same
 * thing.  1280x800 is the one size where the two agree: 1280/(8*2) is exactly
 * 80 and 800/(8*4) is exactly 25.  Every other panel came out bigger --
 * 1366x768 gives 85x32, 1600x900 gives 90x28 -- and since the setup wizard,
 * the installer and every desktop app lay themselves out for 80x25, they drew
 * in the top-left corner of a larger console and left the rest black.  On
 * QEMU at 1280x800 that could never be seen, so it survived until HavenDOS
 * first ran on a real panel.
 *
 * Now the requested grid is honoured and scaled up as far as it fits, and the
 * leftover pixels become an even margin instead of a half-used screen.  At
 * 1280x800 asking for 80x25 still gives sx=2, sy=4, offset 0,0 -- byte for
 * byte what it did before. */
/* ── v0.8.8: and the clamps are the PANEL's and the BUDGET's ────────────
 *
 * This clamped to VGA_COLS_MAX/VGA_ROWS_MAX when those were 90 and 60 --
 * the largest grid a CRTC can be programmed to.  Nothing in this function
 * touches a CRTC.  A 1920x1080 panel asked for 240x135 got 90x60 back,
 * centred, with 240 pixels of black down each side and 300 across the
 * bottom, and no screen in the system could say why: `resolution` printed
 * the grid it had been GIVEN rather than the grid it got.  It prints both
 * now, and vga_grid_capped() says whether this panel is bigger than the
 * console can express at one pixel per pixel.
 *
 * The two clamps are not interchangeable and the distinction is the whole
 * value of the report.  The PANEL clamp means "those pixels do not exist".
 * The BUDGET clamp means "the buffers are two bytes a cell and the pair is
 * sized for 1080p" -- a different sentence, with a different answer (ask
 * for fewer cells at a larger scale, which vga_fb_best_grid() does).
 *
 * ── THE CLAMPS HERE ARE DEFENCE IN DEPTH, AND THAT IS RECORDED ────────
 *
 * vga_set_mode() validates the range, the budget and the panel before it
 * calls this, so none of the clamps below can fire on that path.  They
 * are not dead code -- vga_fb_init() also calls this, and so could a
 * later caller -- but they are UNREACHABLE THROUGH THE PUBLIC ENTRY
 * POINT, and tools/check_console_suite.py's injection that deletes the
 * area clamp was MISSED for exactly that reason.
 *
 * Recorded rather than removed, and the suite calls fbc_fit() directly
 * instead -- it compiles this file, so it can.  An injection that proves
 * not to be a defect is recorded, not deleted (v0.8.6.3 proved two of
 * ten); the same goes for a guard that proves to be unreachable.  What
 * changed is that the suite now reaches it on purpose rather than
 * believing it was covered.
 */
static void fbc_fit(int want_cols, int want_rows)
{
    if (want_cols < 1) want_cols = 1;
    if (want_rows < 1) want_rows = 1;

    /* ── ONE SCALE FOR BOTH AXES ──────────────────────────────────
       This used to compute sx and sy separately and use both, which
       is what stretched the text: the smaller of the two is the only
       one that preserves the glyph's shape, and the 8x16 cell is
       drawn to be used at it.

       The cost is a border. On 1920x1080 at 80x25 the fit is scale 2,
       which uses 1280x800 of the panel and centres it -- so there are
       black bars rather than characters one and a half times taller
       than they are wide. `resolution fit` is the answer for somebody
       who wants the whole panel used: it chooses a GRID that fills it
       at a square scale, instead of distorting a grid that does not.

       A square cell is not negotiable and filling the screen is, so
       this takes the minimum and vga_fb_readable_grid() does the
       rest. */
    int sx = (int)(fbc_w / (uint32_t)(want_cols * FBC_CW));
    int sy = (int)(fbc_h / (uint32_t)(want_rows * FBC_CH));
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (sx > sy) sx = sy;
    else         sy = sx;
    fbc_sx = sx; fbc_sy = sy;

    g_cols = want_cols;
    g_rows = want_rows;

    /* Only shrink if the grid genuinely will not fit even at scale 1. */
    if ((uint32_t)(g_cols * FBC_CW * sx) > fbc_w)
        g_cols = (int)(fbc_w / (uint32_t)(FBC_CW * sx));
    if ((uint32_t)(g_rows * FBC_CH * sy) > fbc_h)
        g_rows = (int)(fbc_h / (uint32_t)(FBC_CH * sy));

    /* The per-axis caps are the buffers' addressing limit, not the
       hardware's -- see include/vga.h. */
    if (g_cols > VGA_COLS_MAX) g_cols = VGA_COLS_MAX;
    if (g_rows > VGA_ROWS_MAX) g_rows = VGA_ROWS_MAX;
    if (g_cols < 1) g_cols = 1;
    if (g_rows < 1) g_rows = 1;

    /* And the AREA, which is the one that actually bounds the buffers.
       Rows go first because a console that loses columns truncates every
       line it has already written, and a console that loses rows scrolls.
       Neither is good; one is recoverable. */
    while (g_rows > 1 && g_cols * g_rows > VGA_CELLS_MAX) g_rows--;
    while (g_cols > 1 && g_cols * g_rows > VGA_CELLS_MAX) g_cols--;

    fbc_ox = (fbc_w - (uint32_t)(g_cols * FBC_CW * fbc_sx)) / 2u;
    fbc_oy = (fbc_h - (uint32_t)(g_rows * FBC_CH * fbc_sy)) / 2u;
}

/* Is this PANEL bigger than the console can express at one pixel per
   pixel?  A statement about the hardware, not about the last fit -- see
   the note in include/vga.h on why the first version was dead. */
int vga_grid_capped(void)
{
    if (!fbc_active || !fbc_w || !fbc_h) return 0;
    uint32_t c = fbc_w / (uint32_t)FBC_CW, r = fbc_h / (uint32_t)FBC_CH;
    if (c > (uint32_t)VGA_COLS_MAX) return 1;
    if (r > (uint32_t)VGA_ROWS_MAX) return 1;
    if (c * r > (uint32_t)VGA_CELLS_MAX) return 1;
    return 0;
}

/* The largest grid that FILLS this panel and stays inside the budget.
 *
 * Not "the panel divided by the glyph", which on anything above 1080p is
 * over the cap and comes back letterboxed.  The scale is raised until the
 * quotient fits, so the glyphs grow instead of the screen shrinking --
 * and because 2560, 1440, 3840 and 2160 all divide by 16, the common
 * panels above 1080p come out exact rather than nearly exact. */
/* ── v0.8.8.1: THE LARGEST READABLE GRID, WHICH IS NOT THE LARGEST ───
 *
 * `resolution max` on a 1080p panel gives 240x135 at a glyph scale of
 * 1, which is a cell 8 physical pixels square. That is the size an 8x8
 * font is drawn at on a 320x200 screen, on a panel six times as wide,
 * and the first thing anybody said about it was that it is too small
 * to use. They were right.
 *
 * The knob is the SCALE, not the font. At 80x25 today the same 8x8
 * glyphs are drawn into cells of 24x40 physical pixels on 1080p --
 * enormous. Between those two there is a grid whose cells are about
 * the size of a normal terminal's, and that is what this finds: the
 * most cells whose glyph is still at least `min_px` pixels in each
 * axis.
 *
 * On 1920x1080 with the default 16: 120x67. Half again the columns of
 * 80x25 and nearly three times the rows, with glyphs at 16x16 -- which
 * is the size a 16-pixel console font is drawn at everywhere else.
 *
 * WHAT THIS IS NOT. It is not RM-1003's item 5. That asks for a real
 * 8x16 GLYPH TABLE -- more detail in the letterforms, descenders,
 * properly proportioned lowercase -- and there is none in this tree:
 * include/font8x8.h is the only glyph data there is. Scaling an 8x8
 * glyph to 16 pixels makes it bigger, not better drawn, and generating
 * a 16-row font by doubling rows is literally what the renderer
 * already does. A real one has to come from somewhere, and where it
 * comes from is a licensing decision that belongs to whoever owns this
 * project, not to the person writing the scaler. Recorded as open. */
int vga_fb_readable_grid(int min_px, int *cols, int *rows)
{
    if (min_px < 8) min_px = 8;
    if (cols) *cols = 80;
    if (rows) *rows = 25;
    if (!fbc_active || !fbc_w || !fbc_h) return 0;

    /* The smallest scale whose cell is wide enough, used for BOTH
       axes -- the cell is 8x16 and its shape is the thing being
       preserved, so there is only one scale to choose. min_px is a
       WIDTH: at VGA_READABLE_PX the cell is 16 wide and 32 tall,
       which is the classic VGA text cell and what a person means by
       "not tiny". */
    int sx = (min_px + FBC_CW - 1) / FBC_CW;
    int sy = sx;
    int c = (int)(fbc_w / (uint32_t)(FBC_CW * sx));
    int r = (int)(fbc_h / (uint32_t)(FBC_CH * sy));
    if (c < 1 || r < 1) return 0;

    /* Then the same two bounds every other grid obeys. Shrinking by
       AREA keeps the cell size the caller asked for, which is the
       whole point of this function -- raising the scale instead would
       give them back a smaller grid with bigger text, which is what
       they would have got from vga_fb_best_grid(). */
    if (c > VGA_COLS_MAX) c = VGA_COLS_MAX;
    if (r > VGA_ROWS_MAX) r = VGA_ROWS_MAX;
    while (r > 1 && c * r > VGA_CELLS_MAX) r--;
    while (c > 1 && c * r > VGA_CELLS_MAX) c--;

    if (cols) *cols = c;
    if (rows) *rows = r;
    return 1;
}

int vga_fb_best_grid(int *cols, int *rows)
{
    /* ONE contract for a refusal, not two.  The first version returned
       early here and filled nothing, while the overflow path at the
       bottom filled 80x25 -- so "there is no grid" meant two different
       things to a caller that ignores the return value, and one of them
       was an uninitialised variable it would then pass to
       vga_set_mode().  Both paths report the stock grid now. */
    if (!fbc_active || !fbc_w || !fbc_h) {
        if (cols) *cols = 80;
        if (rows) *rows = 25;
        return 0;
    }
    for (int s = 1; s <= 16; s++) {
        int c = (int)(fbc_w / (uint32_t)(FBC_CW * s));
        int r = (int)(fbc_h / (uint32_t)(FBC_CH * s));
        if (c < 1 || r < 1) break;
        if (c > VGA_COLS_MAX || r > VGA_ROWS_MAX) continue;
        if (c * r > VGA_CELLS_MAX) continue;
        if (cols) *cols = c;
        if (rows) *rows = r;
        return 1;
    }
    /* A panel so large that even a 16x scale overflows the cap, or so
       small that one cell does not fit.  Report the stock grid rather
       than an arithmetic leftover. */
    if (cols) *cols = 80;
    if (rows) *rows = 25;
    return 0;
}

/* The margin around the centred grid.  Painted once when the geometry
   changes: nothing else ever writes there, so whatever the firmware left in
   the framebuffer would otherwise stay on screen forever. */
static void fbc_clear_margin(void)
{
    if (!fbc_active || !fbc_addr) return;
    uint32_t gx0 = fbc_ox, gy0 = fbc_oy;
    uint32_t gx1 = fbc_ox + (uint32_t)(g_cols * FBC_CW * fbc_sx);
    uint32_t gy1 = fbc_oy + (uint32_t)(g_rows * FBC_CH * fbc_sy);
    for (uint32_t y = 0; y < fbc_h; y++) {
        uint32_t *line = fbc_addr + y * fbc_pitch;
        if (y < gy0 || y >= gy1) {
            for (uint32_t x = 0; x < fbc_w; x++) line[x] = 0;
        } else {
            for (uint32_t x = 0; x < gx0; x++) line[x] = 0;
            for (uint32_t x = gx1; x < fbc_w; x++) line[x] = 0;
        }
    }
}

/* Called from kernel_main once the multiboot framebuffer tag has been read,
   BEFORE vga_init(), so the text-mode register programming is never run on
   a machine where GRUB has already set a graphics mode. */
int vga_fb_init(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch_bytes,
                uint32_t bpp)
{
    if (!addr || !w || !h || bpp != 32) return 0;
    if (w > 4096 || h > 4096) return 0;
    fbc_addr  = (uint32_t *)addr;
    fbc_pitch = pitch_bytes / 4;
    fbc_w = w; fbc_h = h;
    fbc_active = 1;
    fbc_forget();
    fbc_fit(80, 25);
    fbc_clear_margin();
    return 1;
}

/* One character cell, as pixels.  The inner loop writes a run of fbc_sx
   pixels per source pixel, repeated fbc_sy times.
   `record` is 0 for the cursor overlay, which paints a cell that is not what
   the shadow buffer says is there; recording it would make the next flush
   fight the caret. */
static void fbc_put_ex(int x, int y, uint16_t entry, int record)
{
    if ((unsigned)x >= (unsigned)g_cols || (unsigned)y >= (unsigned)g_rows) return;
    if (record) {
        if (!fbc_painted_valid) fbc_forget();
        fbc_painted[y*g_cols+x] = entry;
    }
    uint8_t ch   = (uint8_t)(entry & 0xFF);
    uint8_t attr = (uint8_t)(entry >> 8);
    uint32_t fg = fbc_rgb[attr & 0x0F];
    uint32_t bg = fbc_rgb[(attr >> 4) & 0x0F];
    const uint8_t *glyph = &font8x16[(uint32_t)ch * FBC_CH];

    uint32_t px0 = fbc_ox + (uint32_t)x * (uint32_t)FBC_CW * (uint32_t)fbc_sx;
    uint32_t py0 = fbc_oy + (uint32_t)y * (uint32_t)FBC_CH * (uint32_t)fbc_sy;

    for (int row = 0; row < FBC_CH; row++) {
        uint8_t bits = glyph[row];
        for (int ry = 0; ry < fbc_sy; ry++) {
            uint32_t *line = fbc_addr + (py0 + (uint32_t)(row * fbc_sy + ry)) * fbc_pitch + px0;
            for (int col = 0; col < FBC_CW; col++) {
                uint32_t c = ((bits >> (FBC_CW - 1 - col)) & 1) ? fg : bg;
                for (int rx = 0; rx < fbc_sx; rx++) *line++ = c;
            }
        }
    }
}

static void fbc_put(int x, int y, uint16_t entry) { fbc_put_ex(x, y, entry, 1); }

/* The single place a cell becomes pixels, whichever console is in use.
 *
 * Both branches skip a cell the screen already shows.  The text branch always
 * did; the framebuffer branch did not, and that asymmetry was the whole of the
 * flicker.  Writing a character to VGA memory costs one 16-bit store, so
 * repainting an unchanged cell there is invisible.  Drawing the same character
 * into a framebuffer costs 8 x fbc_sy rows of 8 x fbc_sx pixels - 512 stores
 * per cell at 80x25 on a 1280x800 screen - and it happens with the screen
 * live, so a loop that redraws a static label twice a second was re-rendering
 * it twice a second in front of the user.  app_clock() rewrites "Uptime:" and
 * "Time from CMOS RTC" on every pass; so do the dashboard, the monitor, the
 * games and the screensavers.
 */
static inline void put_cell(int idx, uint16_t entry)
{
    if (fbc_active) {
        if (!fbc_painted_valid) fbc_forget();
        if (fbc_painted[idx] == entry) return;      /* already on screen */
        fbc_put(idx % g_cols, idx / g_cols, entry);
    }
    else if (VGA_MEM[idx] != entry) VGA_MEM[idx] = entry;
}


static uint16_t make_entry(char c, vga_color_t fg, vga_color_t bg);
static void     update_hw_cursor(void);

/* ── v0.7.11: full text-mode switching (80x25 / 80x50 / 90x30 / 90x60) ────
 *
 * v0.7.11 only changed the CRTC character height, which doubled the rows but
 * left the 720x400 timing and the 9-dot character clock alone.  The result was
 * 8x8 glyphs in cells that are still full width and only half height, so every
 * character came out visibly squashed - correct, but ugly.
 *
 * The fix is to move both axes together.  Dropping the character clock from 9
 * dots to 8 gives 720/8 = 90 columns, and retiming vertically to 480 lines
 * gives 480/8 = 60 rows.  90x60 over 720x480 is the classic maximal VGA text
 * mode and its cells have sane proportions again.
 *
 * Each mode is applied as a COMPLETE register set rather than a delta.  That
 * matters for recovery: if a mode ever fails to sync on real hardware, typing
 * `resolution 25` blind reprograms every register back to stock mode 3 instead
 * of trying to undo a half-applied change.  The keyboard path is completely
 * independent of the video mode, so blind recovery always works.
 *
 * v0.7.11 BUG, fixed here: vga_upload_font8x8() wrote the 8x8 glyphs into the
 * font plane AND ZEROED bytes 8..31 of every glyph slot - which is where the
 * BIOS 8x16 font lives.  Switching back to 25 rows therefore restored the cell
 * height but not the glyphs, leaving every character drawn in the top half of
 * its cell with a blank band underneath.  The original font plane is now saved
 * once, before the first overwrite, and restored whenever a 16-pixel cell
 * height is selected.
 * ───────────────────────────────────────────────────────────────────────── */
#define SEQ_IDX   0x3C4
#define SEQ_DAT   0x3C5
#define GC_IDX    0x3CE
#define GC_DAT    0x3CF
#define CRTC_IDX  0x3D4
#define CRTC_DAT  0x3D5
#define AC_IDX    0x3C0
#define AC_DAT    0x3C1
#define MISC_W    0x3C2
#define INSTAT_1  0x3DA

/* Stock mode 3 (80x25, 720x400, 9-dot chars) */
static const uint8_t M3_MISC = 0x67;
static const uint8_t M3_SEQ[5]  = {0x03,0x00,0x03,0x00,0x02};
static const uint8_t M3_CRTC[25]= {
    0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
    0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
    0x9C,0x8E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF };

/* 90x60 (720x480, 8-dot chars, 8-line cells) */
static const uint8_t M90_MISC = 0xE7;
static const uint8_t M90_SEQ[5]  = {0x03,0x01,0x03,0x00,0x02};
static const uint8_t M90_CRTC[25]= {
    0x6B,0x59,0x5A,0x82,0x60,0x8D,0x0B,0x3E,
    0x00,0x47,0x06,0x07,0x00,0x00,0x00,0x00,
    0xEA,0x0C,0xDF,0x2D,0x08,0xE8,0x05,0xA3,0xFF };

static const uint8_t GC_TEXT[9] = {0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF};
static const uint8_t AC_TEXT[21]= {
    0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
    0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x0C,0x00,0x0F,0x08,0x00 };

static uint8_t  font_backup[256*32];
static int      font_saved = 0;

/* Open the font plane for linear CPU access at A0000 */
static void font_plane_open(void){
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x01);
    outb(SEQ_IDX,0x02); outb(SEQ_DAT,0x04);   /* write plane 2 only    */
    outb(SEQ_IDX,0x04); outb(SEQ_DAT,0x07);   /* extended, no odd/even */
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x03);
    outb(GC_IDX,0x04);  outb(GC_DAT,0x02);    /* read plane 2          */
    outb(GC_IDX,0x05);  outb(GC_DAT,0x00);
    outb(GC_IDX,0x06);  outb(GC_DAT,0x04);    /* map at A0000          */
}
static void font_plane_close(void){
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x01);
    outb(SEQ_IDX,0x02); outb(SEQ_DAT,0x03);
    outb(SEQ_IDX,0x04); outb(SEQ_DAT,0x03);
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x03);
    outb(GC_IDX,0x04);  outb(GC_DAT,0x00);
    outb(GC_IDX,0x05);  outb(GC_DAT,0x10);
    outb(GC_IDX,0x06);  outb(GC_DAT,0x0E);
}

/* ══ v0.7.15: CP437 symbol glyphs for the 8x8 font ════════════════════════
 *
 * font8x8.h leaves glyphs 0x01-0x1F blank — its header says HavenDOS "only
 * ever draws 7-bit ASCII".  BOOT Chat's emoji layer maps incoming emoji onto
 * exactly those slots (☺ ♥ ♪ ☼ and the arrows), which the BIOS font provides
 * in 80x25 but the 8x8 font does not.  Without this the emoji would appear in
 * 80x25 and vanish into blank cells in 80x50 / 90x30 / 90x60.
 *
 * Rows are MSB-first (bit 7 = leftmost pixel), matching the pre-reversed
 * convention font8x8.h already uses, so these go straight into the plane.
 * ═══════════════════════════════════════════════════════════════════════ */
typedef struct { uint8_t idx; uint8_t rows[8]; } cp437_glyph_t;

static const cp437_glyph_t g_cp437_syms[] = {
 {0x01,{0x3C,0x42,0xA5,0x81,0xA5,0x99,0x42,0x3C}}, /* ☺ */
 {0x02,{0x3C,0x7E,0xDB,0xFF,0xDB,0xE7,0x7E,0x3C}}, /* ☻ */
 {0x03,{0x6C,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00}}, /* ♥ */
 {0x04,{0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00}}, /* ♦ */
 {0x05,{0x38,0x38,0xD6,0xFE,0xD6,0x10,0x38,0x00}}, /* ♣ */
 {0x06,{0x10,0x38,0x7C,0xFE,0x7C,0x10,0x38,0x00}}, /* ♠ */
 {0x0D,{0x0C,0x0E,0x0B,0x09,0x0B,0x3B,0x78,0x30}}, /* ♪ */
 {0x0E,{0x3F,0x33,0x3F,0x33,0x33,0xF3,0xE0,0x00}}, /* ♫ */
 {0x0F,{0x18,0xDB,0x3C,0xE7,0xE7,0x3C,0xDB,0x18}}, /* ☼ */
 {0x18,{0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00}}, /* ↑ */
 {0x19,{0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00}}, /* ↓ */
 {0x1A,{0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00}}, /* → */
 {0x1B,{0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00}}, /* ← */
 {0x1E,{0x00,0x10,0x38,0x7C,0xFE,0x00,0x00,0x00}}, /* ▲ */
 {0x1F,{0x00,0x00,0xFE,0x7C,0x38,0x10,0x00,0x00}}, /* ▼ */
};

/* Overwrite the blank slots in the already-uploaded 8x8 font. */
static void vga_font_patch_symbols(void){
    font_plane_open();
    volatile uint8_t *p2 = (volatile uint8_t*)0xA0000;
    int n = (int)(sizeof(g_cp437_syms)/sizeof(g_cp437_syms[0]));
    for(int g=0; g<n; g++){
        volatile uint8_t *dst = p2 + (uint32_t)g_cp437_syms[g].idx * 32;
        for(int r=0; r<8; r++)  dst[r] = g_cp437_syms[g].rows[r];
        for(int r=8; r<32; r++) dst[r] = 0x00;
    }
    /* v0.7.15: accented letters (128-255), generated by tools/make_font_ext.py
       from the base glyphs already in this font.  Without these, "café" would
       render correctly in 80x25 (BIOS font) and as blanks in 80x50/90x30/90x60
       — which looks like corruption rather than a missing glyph. */
    for(int g=0; g<FONT8X8_EXT_N; g++){
        volatile uint8_t *dst = p2 + (uint32_t)FONT8X8_EXT[g].idx * 32;
        for(int r=0; r<8; r++)  dst[r] = FONT8X8_EXT[g].rows[r];
        for(int r=8; r<32; r++) dst[r] = 0x00;
    }
    font_plane_close();
}

static void vga_font_save(void){
    if(font_saved) return;
    const uint8_t *p2=(const uint8_t*)0xA0000;
    font_plane_open();
    for(int i=0;i<256*32;i++) font_backup[i]=p2[i];
    font_plane_close();
    font_saved=1;
}
static void vga_font_restore(void){
    if(!font_saved) return;
    uint8_t *p2=(uint8_t*)0xA0000;
    font_plane_open();
    for(int i=0;i<256*32;i++) p2[i]=font_backup[i];
    font_plane_close();
}
static void vga_font_load8x8(void){
    vga_font_save();                       /* preserve the BIOS 8x16 font */
    uint8_t *p2=(uint8_t*)0xA0000;
    font_plane_open();
    for(int c=0;c<256;c++){
        for(int r=0;r<8;r++)  p2[c*32+r]=font8x8[c*8+r];
        for(int r=8;r<32;r++) p2[c*32+r]=0;
    }
    font_plane_close();
    vga_font_patch_symbols();   /* v0.7.15: fill the blank CP437 slots */
}

static void crtc_program(const uint8_t *tbl){
    /* unlock CRTC registers 0-7 (clear protect bit in index 0x11) */
    outb(CRTC_IDX,0x11);
    outb(CRTC_DAT,(uint8_t)(inb(CRTC_DAT)&0x7F));
    /* 25 CRTC REGISTERS, which is not 25 rows: the two numbers are
       equal by coincidence and M3_CRTC[] is 25 entries long.  This was
       the first thing tools/check_console_grid.py found, and a good
       example of why that check wants a reason written out rather than
       a bare marker. */
    /* grid-ok: 25 is the CRTC register count, not a row count */
    for(int i=0;i<25;i++){ outb(CRTC_IDX,(uint8_t)i); outb(CRTC_DAT,tbl[i]); }
}
/* ── v0.7.11: THIS IS WHAT CAUSED THE BLACK SCREEN ──────────────────────
 * The Attribute Controller is the one VGA block that uses a SINGLE port for
 * both index and data.  You write the index to 0x3C0 and then the data to
 * 0x3C0 as well; an internal flip-flop decides which one a write means, and it
 * toggles on every write.  Port 0x3C1 is READ-ONLY for AC data.
 *
 * v0.7.11 wrote the data to 0x3C1.  Those writes went nowhere, and because they
 * never toggled the flip-flop, every subsequent write to 0x3C0 was interpreted
 * as DATA for the previous index instead of a new index.  The whole sequence
 * desynchronised, and the final `outb(0x3C0, 0x20)` - which sets the Palette
 * Address Source bit and is the thing that RE-ENABLES VIDEO OUTPUT - landed as
 * a data byte instead.
 *
 * PAS therefore stayed 0, which blanks the display. That is the black screen,
 * and it is why `resolution 25` could not recover: the recovery path ran the
 * exact same broken routine and blanked the screen again.
 *
 * Reading 0x3DA before starting forces the flip-flop to the index state.
 * ─────────────────────────────────────────────────────────────────────── */
static void ac_program(int eight_dot){
    (void)inb(INSTAT_1);                   /* force flip-flop to INDEX */
    for(int i=0;i<21;i++){
        uint8_t v = AC_TEXT[i];
        /* AC[0x13] is Horizontal Pixel Panning: 8 for 9-dot cells, 0 for
           8-dot cells.  Using the 9-dot value in a 90-column mode shifts the
           whole display sideways by a character. */
        if(i==0x13) v = eight_dot ? 0x00 : 0x08;
        outb(AC_IDX,(uint8_t)i);           /* index */
        outb(AC_IDX,v);                    /* data  - SAME PORT */
    }
    (void)inb(INSTAT_1);
    outb(AC_IDX,0x20);                     /* PAS=1: video output ON */
}

/* Unconditional video re-enable.  Cheap, and it means no future mistake in
   the block above can leave the user staring at a dark screen. */
static void ac_video_on(void){
    (void)inb(INSTAT_1);
    outb(AC_IDX,0x20);
}
static void seq_program(const uint8_t *tbl){
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x01);        /* sync reset */
    for(int i=1;i<5;i++){ outb(SEQ_IDX,(uint8_t)i); outb(SEQ_DAT,tbl[i]); }
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x03);        /* end reset  */
}
static void gc_program(void){
    for(int i=0;i<9;i++){ outb(GC_IDX,(uint8_t)i); outb(GC_DAT,GC_TEXT[i]); }
}
static void vga_set_char_height(int h){
    outb(CRTC_IDX,0x09);
    outb(CRTC_DAT,(uint8_t)((inb(CRTC_DAT)&0xE0)|((h-1)&0x1F)));
    outb(CRTC_IDX,0x0A); outb(CRTC_DAT,(uint8_t)(h-2));
    outb(CRTC_IDX,0x0B); outb(CRTC_DAT,(uint8_t)(h-1));
}

static int g_mode_why = VGA_MODE_OK;
int vga_mode_why(void){ return g_mode_why; }
const char *vga_mode_why_name(void){
    switch(g_mode_why){
        case VGA_MODE_OK:      return "ok";
        case VGA_MODE_ETEXT:   return "not a VGA text mode";
        case VGA_MODE_ERANGE:  return "outside the grid limits";
        case VGA_MODE_EBUDGET: return "over the cell budget";
        case VGA_MODE_EPANEL:  return "will not fit this panel";
    }
    return "unknown";
}

/* ── v0.8.8: THE TEXT-MODE TABLE NO LONGER GATES THE FRAMEBUFFER ────────
 *
 * The four-way validation below used to run FIRST, for both consoles.  It
 * is a register-table question -- each of 80x25, 80x50, 90x30 and 90x60 is
 * a complete CRTC/sequencer/attribute set, and a fifth does not exist
 * because nobody has written one.  On a framebuffer no register is
 * touched: the branch a few lines down says so in as many words, and then
 * returns having been let through by a test about a dot clock.
 *
 * So the order is inverted.  The framebuffer path validates against the
 * PANEL and the BUFFERS, which are the only two things that bound it, and
 * the text path keeps its table untouched.  Same reason usb_hc_ops_t has a
 * configure() that is a no-op on one back-end: the asymmetry is real, and
 * writing it down beats having one branch quietly carry the other's
 * constraint.
 *
 * The two pre-existing register writes that happened BEFORE the fbc_active
 * branch -- the sequencer reset and MISC_W -- moved with it.  They were
 * being issued to a card GRUB had already put in a graphics mode, on the
 * way to a branch whose own comment explains that doing exactly that
 * "would tear it down and leave a blank screen".  Nothing ever saw it
 * because the two writes are harmless on their own and the real teardown
 * is in seq_program()/crtc_program() further down, which the branch did
 * skip.  A guard that stops three of five writes is not a guard. */
int vga_set_mode(int cols, int rows){
    g_mode_why = VGA_MODE_OK;

    if(fbc_active){
        if(cols < 1 || rows < 1 ||
           cols > VGA_COLS_MAX || rows > VGA_ROWS_MAX){
            g_mode_why = VGA_MODE_ERANGE; return 0;
        }
        if(cols * rows > VGA_CELLS_MAX){
            g_mode_why = VGA_MODE_EBUDGET; return 0;
        }
        if((uint32_t)(cols * FBC_CW) > fbc_w ||
           (uint32_t)(rows * FBC_CH) > fbc_h){
            g_mode_why = VGA_MODE_EPANEL; return 0;
        }
        if(cols==g_cols && rows==g_rows) return 1;

        /* No font plane here: the glyphs come from font8x8[] in this file,
           so there is nothing to upload and nothing to restore.  A mode
           change is a change of glyph scale and margin. */
        fbc_fit(cols, rows);
        fbc_forget();
        uint16_t blank_fb = make_entry(' ',cur_fg,cur_bg);
        for(int i=0;i<VGA_CELLS_MAX;i++) shadow[i]=blank_fb;
        for(uint32_t py=0;py<fbc_h;py++)
            for(uint32_t px=0;px<fbc_w;px++)
                fbc_addr[py*fbc_pitch+px]=fbc_rgb[(blank_fb>>12)&0x0F];
        for(int i=0;i<g_rows*g_cols;i++) fbc_put(i%g_cols,i/g_cols,blank_fb);
        if(cur_y>=g_rows) cur_y=g_rows-1;
        if(cur_x>=g_cols) cur_x=g_cols-1;
        return 1;
    }

    int wide  = (cols==90);
    int small = (rows==50 || rows==60);
    if(cols!=80 && cols!=90){ g_mode_why = VGA_MODE_ETEXT; return 0; }
    if(rows!=25 && rows!=50 && rows!=30 && rows!=60){
        g_mode_why = VGA_MODE_ETEXT; return 0; }
    /* 480 lines needs the 8-dot character clock, i.e. 90 columns */
    if(!wide && (rows==30||rows==60)){ g_mode_why = VGA_MODE_ETEXT; return 0; }
    if(wide  && (rows==25||rows==50)){ g_mode_why = VGA_MODE_ETEXT; return 0; }
    if(cols==g_cols && rows==g_rows) return 1;

    if(small) vga_font_load8x8(); else vga_font_restore();

    /* Hold the sequencer in synchronous reset across the dot-clock change. */
    outb(SEQ_IDX,0x00); outb(SEQ_DAT,0x01);
    outb(MISC_W, wide ? M90_MISC : M3_MISC);
    seq_program(wide ? M90_SEQ : M3_SEQ);
    crtc_program(wide ? M90_CRTC : M3_CRTC);
    gc_program();
    ac_program(wide);
    vga_set_char_height(small ? 8 : 16);

    g_cols = cols;
    g_rows = rows;

    uint16_t blank = make_entry(' ',cur_fg,cur_bg);
    for(int i=0;i<VGA_CELLS_MAX;i++) shadow[i]=blank;
    for(int i=0;i<g_rows*g_cols;i++) put_cell(i,blank);
    if(cur_y>=g_rows) cur_y=g_rows-1;
    if(cur_x>=g_cols) cur_x=g_cols-1;
    update_hw_cursor();
    ac_video_on();          /* belt and braces - never leave PAS clear */
    return 1;
}

/* Reprogram the current mode from scratch even if nothing changed.  This is
   the blind-recovery path: it never short-circuits, so it works when the
   screen is already dark and vga_set_mode() would have returned early. */
void vga_force_text_mode(void){
    g_cols=0; g_rows=0;           /* defeat the no-op check */
    vga_set_mode(80,25);
}

/* Back-compat: rows only. 25/50 stay at 80 columns; 30/60 imply 90. */
int vga_set_rows(int rows){
    if(rows==30||rows==60) return vga_set_mode(90,rows);
    return vga_set_mode(80,rows);
}

static uint16_t make_entry(char c, vga_color_t fg, vga_color_t bg){
    return (uint16_t)(uint8_t)c | ((uint16_t)((bg<<4)|fg)<<8);
}

/* ── v0.7.6 FIX: two defects in the v0.7.4 anti-flicker design ────────────
 *
 * 1. THE DIRTY CHECK COMPARED AGAINST THE WRONG BUFFER.
 *    The old body was:
 *        if(shadow[idx]!=entry){ shadow[idx]=entry; VGA_MEM[idx]=entry; }
 *    i.e. "skip the write if the SHADOW already holds this value".  That is
 *    only safe while shadow and VGA are identical — but vga_clear() is
 *    deliberately shadow-only, so the two diverge by design on every screen
 *    change.  Once diverged, any cell whose new value happened to match the
 *    stale shadow was skipped and NEVER REACHED THE SCREEN.  Unchanged UI
 *    elements — the "HavenDOS v0.7.11" label, the two-character icon glyphs,
 *    the taskbar username / NOTIF / clock — are exactly the cells most likely
 *    to match, which is why those and not the surrounding dashes and labels
 *    were the parts that blinked out.  They stayed missing until something
 *    forced a full vga_flush().
 *
 *    The dirty check now tests VGA memory, which is the thing we are actually
 *    trying to avoid rewriting.  A skip can now only happen when the screen
 *    genuinely already shows the right cell.
 *
 * 2. THERE WAS NO ACTUAL DOUBLE BUFFERING.
 *    Because vga_write() wrote straight through to 0xB8000, a frame was
 *    composed live on screen element by element and vga_flush() at the end of
 *    full_draw() only ever mopped up cells that had gone blank.  The
 *    begin/end frame pair below gives real atomicity: between them writes land
 *    in the shadow only, and vga_end_frame() commits the finished frame in one
 *    pass.  Nesting is counted so an inner pair cannot commit early.
 *    Outside a frame, behaviour is unchanged write-through, so none of the
 *    ~40 app draw sites in desktop.c need to be touched.
 * ─────────────────────────────────────────────────────────────────────── */
static int defer_depth = 0;
/* Set once vga_init() has run; before that there is no console to commit to. */
static int g_console_ready = 0;

/* ════════════════════════════════════════════════════════════════════
   THE VIEWPORT.  v0.8.8.4.

   WHAT PROBLEM THIS SOLVES.  v0.7.18 made the desktop's own chrome
   derive from vga_rows()/vga_cols(), and its header records what it
   could not do:

     "Apps launched FROM the desktop still lay themselves out for
      80x25 and render in the top-left of a larger screen --
      converting those is a separate pass and is not claimed here."

   There are forty-three apps and they place text at absolute columns
   and rows -- `vga_puts_at("CPU Activity",8,4,...)`, a 60-cell bar
   drawn from column 8, a window at `ui_draw_window(4,2,72,20,...)`.
   Rewriting all of that by hand is several thousand lines of blind
   diff over layouts that can only be checked by opening each app on
   each grid, and the likely result is forty-three apps that are
   slightly wrong in new ways instead of consistently wrong in one old
   way.

   SO THE GRID LIES TO THEM INSTEAD. An app that lays out for 80x25
   gets an 80x25 console: vga_cols() returns 80, vga_rows() returns
   25, and its coordinates are translated into a centred box on the
   real panel. The app is unchanged and correct, and it is in the
   middle of the screen instead of the corner.

   THIS IS NOT A WINDOW SYSTEM. There is one viewport, it does not
   nest, and nothing clips one app against another because only one
   app runs at a time. It is a coordinate transform with a clip, which
   is the whole of what was needed.

   ── WHY THE TRANSFORM IS HERE AND NOWHERE ELSE ──────────────────────

   Every cell this driver ever writes goes through vga_write(), and
   every app that asks how big the screen is calls vga_rows() or
   vga_cols(). Three functions. Putting the offset anywhere else --
   in ui.c, in each app, in a wrapper -- means finding every call site
   again, which is the problem this was meant to avoid.

   The PHYSICAL grid stays in g_rows/g_cols and the flush, the
   framebuffer renderer and the hardware cursor keep using it. Mixing
   those two up is the one way this can go wrong, so the physical ones
   are never reached through the accessors.
   ════════════════════════════════════════════════════════════════════ */
int vga_viewport_active(void){ return g_vp_w > 0 && g_vp_h > 0; }

void vga_viewport_get(int *x, int *y, int *w, int *h)
{
    if(x) *x = g_vp_x;
    if(y) *y = g_vp_y;
    if(w) *w = vga_viewport_active() ? g_vp_w : g_cols;
    if(h) *h = vga_viewport_active() ? g_vp_h : g_rows;
}

void vga_viewport_clear(void){ g_vp_x = g_vp_y = g_vp_w = g_vp_h = 0; }

int vga_viewport_centre(int w, int h)
{
    /* A viewport bigger than the panel is refused rather than clamped.
       Clamping would hand the app a console of a size it did not ask
       for and had no way to find out about, and an app laid out for
       80 columns in a 70-column box draws its right-hand edge into
       nothing -- silently, because the clip below would absorb it.
       Refusing means the caller gets `0` and runs the app full-screen,
       which is exactly what happened before this existed. */
    if(w <= 0 || h <= 0) return 0;
    if(w > g_cols || h > g_rows){ vga_viewport_clear(); return 0; }

    g_vp_w = w;
    g_vp_h = h;
    g_vp_x = (g_cols - w) / 2;
    g_vp_y = (g_rows - h) / 2;
    return 1;
}

/* Translate a logical coordinate to a physical one. Returns 0 if it
   falls outside the viewport, in which case the caller must not touch
   the cell -- the clip is load-bearing: an out-of-bounds y from a long
   theme list in Settings used to write past shadow[] into kernel heap
   and stack and hang the machine. */
/* ════════════════════════════════════════════════════════════════════
   A GRID THE APPS CAN ACTUALLY USE.  v0.8.8.6

   FOUND ON THE HP, and it is the whole of "some apps still go off the
   screen". His `dmesg` after beta 6:

     warn layout: System Monitor drew 1466 cell(s) outside its 64x24
                  console
     warn layout: Dashboard drew 3855 cell(s) outside its 64x24 console

   SIXTY-FOUR BY TWENTY-FOUR. His panel is 1024x768, and
   vga_fb_readable_grid() at the readable 16-pixel cell gives
   1024/16 = 64 columns and 768/32 = 24 rows. `resolution fit` offered
   it, he took it, and it was saved.

   Every app in the registry lays out for 80x25. A console SMALLER
   than that cannot be fixed by the viewport -- there is nowhere to
   centre an 80x25 box inside a 64x24 screen -- so the apps draw off
   the edge and there is nothing to catch them. The console was the
   problem again, one size further down than last time.

   ── THE POLICY ──────────────────────────────────────────────────────

   A readable cell IF the grid is still at least 80x25, and a smaller
   cell if that is what it takes to get there. On 1024x768 the
   16-pixel cell gives 64x24 and the 8-pixel cell gives 128x48 -- the
   classic VGA text cell, on a panel that shows it comfortably.

     1024x768   64x24 readable, so 128x48 at an 8-pixel cell
     1280x800   80x25 readable, exactly; taken
     1920x1080  120x33 readable; taken
     640x384    40x12 readable, 80x24 at 8 pixels -- still under 25
                rows, so REFUSED. That panel cannot show 80x25 at all
                and nothing here can change it.

   Refusing is right for the last one. A caller that gets 0 leaves the
   console alone, and the apps overflow as they always did on a panel
   that small -- which is honest, and visible in `layout`.
   ════════════════════════════════════════════════════════════════════ */
int vga_fb_app_grid(int *cols, int *rows)
{
    int c = 0, r = 0;

    if(!fbc_active) return 0;

    /* The readable cell first: the text stays the size a console font
       is anywhere else, and on most panels this is the answer. */
    if(vga_fb_readable_grid(VGA_READABLE_PX, &c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }

    /* Otherwise the smallest cell this console draws, which is the
       8x16 of a VGA text mode. Smaller text than the line above
       wanted, and a console the apps fit in -- which they do not
       otherwise, at any size. */
    if(vga_fb_readable_grid(8, &c, &r) && c >= 80 && r >= 25){
        if(cols) *cols = c;
        if(rows) *rows = r;
        return 1;
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
   THE GRID A MACHINE SHOULD COME UP WITH.  v0.8.8.5

   Returns 1 and fills cols/rows when the panel can do better than the
   grid it is currently showing; 0 when it cannot, and then nothing is
   changed.

   ── WHY IT IS A FUNCTION AND NOT THREE LINES IN kernel/main.c ───────

   Because the two guards in it are the whole of the decision, and
   neither can be tested where the decision was first written. A
   boot-path `if` is reachable only by booting, on a panel of the
   right size, and QEMU hands this project 1280x800 -- which
   tests/test_console.c's own header calls "the one panel on which
   every v0.8.8 console change could be a no-op and look perfect",
   and which is exactly what happened: on 1280x800 the readable grid
   IS 80x25, so the first version of this appeared to do nothing and
   there was no way to tell that from it being broken.

   Here it is twelve lines with no hardware in it, and
   tests/test_console.c drives it against four simulated panels.

   ── THE TWO GUARDS ──────────────────────────────────────────────────

   NEVER SMALLER THAN 80x25. FOUND BY BOOTING IT on a 640x384
   framebuffer: 80x25 of an 8x16 cell needs 640x400, so that panel
   cannot show the stock grid at all, and vga_fb_readable_grid()
   dutifully returned 40x12 -- the biggest grid whose glyphs stay 16
   pixels wide. Taking it would have been the complaint this exists to
   fix, backwards: a 40x12 console is SMALLER than the 80x25 every app
   lays out for, so nothing would be boxed and every app would draw
   past the right-hand edge with only the grid to clip it.

   ONLY UPWARDS. A panel whose readable grid equals the current one
   gets no mode change, because a mode change that lands on the same
   numbers is a flicker and a log line saying nothing happened.

   READABLE AND NOT MAX. vga_fb_readable_grid() keeps the glyph at
   least VGA_READABLE_PX wide, so the text stays the size it already
   is and there is simply more of it. vga_fb_best_grid() is the
   8-pixel cell people complained about.
   ════════════════════════════════════════════════════════════════════ */
int vga_fb_boot_grid(int *cols, int *rows)
{
    int c = 0, r = 0;

    /* ── REDUNDANT, AND KEPT ──────────────────────────────────────────
       vga_fb_readable_grid() already returns 0 when !fbc_active, so
       removing this line changes nothing -- the injection harness
       planted exactly that and the suite correctly did not notice.
       Recorded as a non-defect rather than deleted.

       It stays because it states the precondition where the decision
       is made instead of two functions away, and because the day
       readable_grid grows an answer for a machine with no panel, this
       is the line that stops a CRTC register table being rewritten at
       boot with nobody watching. */
    if(!fbc_active) return 0;                  /* no panel to fit */
    /* The 80x25 floor lives in vga_fb_app_grid() now, together with
       the fallback to a smaller cell that makes it reachable on a
       1024x768 panel. This function is that policy plus one rule of
       its own. */
    if(!vga_fb_app_grid(&c, &r)) return 0;
    if(c <= g_cols && r <= g_rows) return 0;   /* only upwards */

    if(cols) *cols = c;
    if(rows) *rows = r;
    return 1;
}

static inline int vp_xlate(int *x, int *y){
    if(g_vp_w > 0){
        if(*x < 0 || *x >= g_vp_w || *y < 0 || *y >= g_vp_h) return 0;
        *x += g_vp_x;
        *y += g_vp_y;
    }
    if(*x < 0 || *x >= g_cols || *y < 0 || *y >= g_rows) return 0;
    return 1;
}

/* ── WHAT TRIED TO DRAW OUTSIDE ──────────────────────────────────────
   v0.8.8.5. Kyan, after running beta 5 on the HP: "some apps still go
   off the screen and aren't show fully maybe bc of 16:10".

   There are forty-three apps and guessing which ones is how a release
   fixes the wrong three. The clip in vga_write() is the one place
   that already knows, because every write that lands outside the grid
   or outside an app's viewport passes through it -- so it counts them
   and keeps the worst coordinate it saw.

   shell/desktop.c resets this before it calls an app and reads it
   afterwards, so `dmesg` names the app and says by how much. A
   measurement instead of a theory. */
static uint32_t g_clip_n    = 0;
static int      g_clip_maxx = -1;
static int      g_clip_maxy = -1;

void vga_clip_reset(void){ g_clip_n = 0; g_clip_maxx = -1; g_clip_maxy = -1; }

void vga_clip_stats(uint32_t *n, int *maxx, int *maxy)
{
    if(n)    *n    = g_clip_n;
    if(maxx) *maxx = g_clip_maxx;
    if(maxy) *maxy = g_clip_maxy;
}

static inline void vga_write(int x, int y, uint16_t entry){
    int idx;
    if(!vp_xlate(&x,&y)){
        /* Counted here and not in vp_xlate(), because that function
           also serves the two READ accessors and the hardware cursor.
           A read off the edge is a question, not an app drawing where
           it cannot; counting those would make every app look broken. */
        g_clip_n++;
        if(x > g_clip_maxx) g_clip_maxx = x;
        if(y > g_clip_maxy) g_clip_maxy = y;
        return;
    }
    idx = y*g_cols+x;
    shadow[idx]=entry;
    if(!defer_depth) put_cell(idx,entry);
}

/* Cursor bypass — same rules as vga_write, minus the dirty check semantics
   the cursor does not want (it always rewrites the cell it owns). */
void vga_shadow_write(int x, int y, uint16_t val){
    int idx;
    if(!vp_xlate(&x,&y)) return;
    idx=y*g_cols+x;
    shadow[idx]=val;
    if(!defer_depth) put_cell(idx,val);
}

/* Begin an atomic frame: writes go to the shadow only until the matching
   vga_end_frame().  Safe to nest. */
void vga_begin_frame(void){ defer_depth++; }
void vga_end_frame(void){
    if(defer_depth > 0) defer_depth--;
    if(defer_depth == 0) vga_flush();
}
uint16_t vga_shadow_read(int x, int y){
    if(!vp_xlate(&x,&y)) return 0x0720;
    return shadow[y*g_cols+x];
}

/* Sync shadow → VGA.  Called at the end of full_draw() after all UI
   elements have been drawn into the shadow buffer.  Any cell that
   differs between shadow and real VGA (including background cells
   left blank by the shadow-only vga_clear()) gets written here in
   one pass, so the screen updates atomically from the user's
   perspective — no intermediate blank frame is ever visible.       */
void vga_flush(void){
    if(fbc_active){
        if(!fbc_painted_valid) fbc_forget();
        for(int i=0;i<g_rows*g_cols;i++)
            if(fbc_painted[i]!=shadow[i]) fbc_put(i%g_cols,i/g_cols,shadow[i]);
        return;
    }
    uint16_t *vmem=VGA_MEM;
    for(int i=0;i<g_rows*g_cols;i++)
        if(vmem[i]!=shadow[i]) vmem[i]=shadow[i];
}

/* Commit whatever is pending, for callers that are about to stop and wait.
   keyboard_waitchar() flushes before it blocks, which is what makes a frame
   atomic for anything that reads a key.  A program that polls instead - every
   draw-poll-sleep loop in the system - only ever reaches sleep_ms(), so that
   calls this.  It is a no-op inside an open frame, where committing early is
   exactly what vga_begin_frame() exists to prevent, and before the console is
   up, where there is no screen to commit to. */
void vga_present(void){
    if(!g_console_ready || defer_depth) return;
    vga_flush();
}

static void update_hw_cursor(void){
    /* cur_x/cur_y are LOGICAL. The CRTC and the framebuffer caret are
       both physical, so the caret of an app in a centred viewport
       would otherwise blink in the top-left of the panel while its
       text appeared in the middle. */
    int cpx=cur_x, cpy=cur_y;
    uint16_t pos;
    if(!vp_xlate(&cpx,&cpy)){ cpx=0; cpy=0; }
    pos=(uint16_t)(cpy*g_cols+cpx);
    if(fbc_active){
        /* No CRTC cursor on a framebuffer.  The cell under the cursor is
           redrawn with its colours swapped, and the previous one is put
           back, so the caret is visible without the rest of the system
           knowing anything changed. */
        /* The caret is an overlay: it is not in the shadow, so fbc_painted -
           which records what the console believes is on screen - must not be
           told the swapped cell is the real one.  The cell it covers is marked
           unknown instead, so a later write of the same character still
           repaints and cannot leave an inverted block behind; putting the old
           cell back records normally, because that value IS the true one. */
        static int last_idx=-1;
        if(last_idx>=0 && last_idx<g_rows*g_cols)
            fbc_put_ex(last_idx%g_cols,last_idx/g_cols,shadow[last_idx],1);
        int idx=cpy*g_cols+cpx;     /* physical: see the note above */
        if(idx>=0 && idx<g_rows*g_cols){
            uint16_t e=shadow[idx];
            uint16_t sw=(uint16_t)((e&0x00FF)|((uint16_t)(((e>>12)&0x0F)|(((e>>8)&0x0F)<<4))<<8));
            fbc_put_ex(cpx,cpy,sw,0);
            if(!fbc_painted_valid) fbc_forget();
            fbc_painted[idx]=0xFFFF;
            last_idx=idx;
        }
        return;
    }
    outb(0x3D4,14); outb(0x3D5,(uint8_t)(pos>>8));
    outb(0x3D4,15); outb(0x3D5,(uint8_t)(pos&0xFF));
}

void vga_init(void){
    cur_x=0;cur_y=0;cur_fg=VGA_LIGHT_GREY;cur_bg=VGA_BLACK;
    for(int i=0;i<VGA_CELLS_MAX;i++) shadow[i]=0xFFFF;
    vga_clear(); vga_flush(); update_hw_cursor();
    g_console_ready = 1;
}

/* Shadow-only clear: writes to shadow buffer ONLY, never touches real VGA.
   This is the key anti-flicker fix for PC VM environments (VirtualBox,
   VMware, QEMU with hardware-accelerated VGA):
     - Old behaviour: clear → VGA blank (visible white flash) → redraw
     - New behaviour: clear → shadow blank → redraw → vga_flush() diffs
       only the cells that are STILL blank after the new frame is drawn.
   On UTM SE TCG the difference was invisible (TCG renders every frame
   in one shot), but on accelerated PC VGA the blank frame was visible
   for 1-2 full VGA refresh cycles (~14ms each) = obvious flicker.
   vga_flush() at the end of full_draw() handles any cells that stayed
   blank (background areas not covered by UI elements). */
void vga_clear(void){
    uint16_t blank=make_entry(' ',cur_fg,cur_bg);
    /* THE VIEWPORT, NOT THE SCREEN. An app clearing its own console
       must not wipe what the desktop drew around it -- that surround
       is how a centred 80x25 app reads as framed rather than as a
       screen that failed to finish drawing.
       Through vga_write() and not a straight loop over shadow[],
       because the translation and the clip live there; a direct loop
       was how this function ignored the grid before and it is how it
       would ignore the viewport now. */
    if(g_vp_w > 0){
        for(int y=0;y<g_vp_h;y++)
            for(int x=0;x<g_vp_w;x++) vga_write(x,y,blank);
    } else {
        for(int i=0;i<g_rows*g_cols;i++) shadow[i]=blank;
    }
    cur_x=0;cur_y=0;update_hw_cursor();
}

/* v0.7.12 FIX — the shadow stride is g_cols, not the compile-time VGA_WIDTH.
   In any 90-column mode the old `(y+1)*VGA_WIDTH+x` read from 10 cells before
   the start of the source row, so scrolling smeared the screen diagonally.
   Same root cause as the VGA_WIDTH uses in vga_putchar() and vga_puts_at()
   below: v0.7.11 made the column count dynamic but left five call sites
   pinned to 80.  Those modes have never been exercised, so this was never
   seen. */
void vga_scroll(void){
    /* Scrolls the LOGICAL grid, which is the viewport when one is set.
       The source row is read with vga_shadow_read(), which translates
       the same way vga_write() does -- reading shadow[] directly with
       a g_cols stride would pull in cells from outside the viewport
       and smear the surround into the app. That is the same class of
       mistake as the v0.7.12 bug this function already carries a note
       about, where the stride was the compile-time 80 and scrolling a
       90-column mode smeared the screen diagonally. */
    int w = vga_cols(), h = vga_rows();
    for(int y=0;y<h-1;y++)
        for(int x=0;x<w;x++)
            vga_write(x,y,vga_shadow_read(x,y+1));
    for(int x=0;x<w;x++)
        vga_write(x,h-1,make_entry(' ',cur_fg,cur_bg));
    cur_y=h-1;
}
void vga_putchar(char c){
    if(c=='\n'){cur_x=0;cur_y++;if(cur_y>=g_rows)vga_scroll();update_hw_cursor();return;}
    if(c=='\r'){cur_x=0;update_hw_cursor();return;}
    if(c=='\b'){if(cur_x>0){cur_x--;vga_write(cur_x,cur_y,make_entry(' ',cur_fg,cur_bg));}update_hw_cursor();return;}
    if(c=='\t'){int ns=(cur_x+8)&~7;while(cur_x<ns&&cur_x<g_cols)vga_write(cur_x++,cur_y,make_entry(' ',cur_fg,cur_bg));update_hw_cursor();return;}
    vga_write(cur_x,cur_y,make_entry(c,cur_fg,cur_bg));
    if(++cur_x>=g_cols){cur_x=0;cur_y++;}
    if(cur_y>=g_rows)vga_scroll();
    update_hw_cursor();
}
void vga_puts(const char *s){ while(*s)vga_putchar(*s++); }
void vga_set_color(vga_color_t fg, vga_color_t bg){ cur_fg=fg; cur_bg=bg; }
void vga_set_cursor(int x, int y){ cur_x=x;cur_y=y;update_hw_cursor(); }
void vga_get_cursor(int *x, int *y){ *x=cur_x;*y=cur_y; }
void vga_putchar_at(char c, int x, int y, vga_color_t fg, vga_color_t bg){
    vga_write(x,y,make_entry(c,fg,bg));
}
void vga_puts_at(const char *s, int x, int y, vga_color_t fg, vga_color_t bg){
    while(*s&&x<g_cols)vga_putchar_at(*s++,x++,y,fg,bg);
}
void vga_fill_row(int y, char c, vga_color_t fg, vga_color_t bg){
    for(int x=0;x<g_cols;x++)vga_putchar_at(c,x,y,fg,bg);
}
/* v0.9.4.1: flags and width. This handled exactly one character after the
   '%', so "%02u" printed nothing for the '0', then the literal "2u" -- and
   never consumed the argument, so every conversion after it printed the
   wrong value. `sysinfo` showed "Uptime 0:2u:2u"; 18 format strings in the
   tree use a width or a flag (%02u, %04x, %6d, %-16s, %02X ...). Found in
   the 1.0 promo screenshots. Supported now: '-' and '0' flags, a width,
   an ignored 'l', and %X. Precision is not used anywhere and not parsed. */
static void pf_emit(const char *s, int width, int left, char pad){
    int n = (int)strlen(s), i;
    /* zero padding goes after a minus sign, not before it */
    if(pad == '0' && !left && s[0] == '-' && n < width){
        vga_putchar('-'); s++; width--; n--;
    }
    if(!left) for(i = n; i < width; i++) vga_putchar(pad);
    vga_puts(s);
    if(left)  for(i = n; i < width; i++) vga_putchar(' ');
}
void vga_printf(const char *fmt,...){
    va_list args; va_start(args,fmt); char buf[34];
    while(*fmt){
        if(*fmt=='%'){
            int left = 0, width = 0; char pad = ' ';
            fmt++;
            while(*fmt=='-' || *fmt=='0'){
                if(*fmt=='-') left = 1; else pad = '0';
                fmt++;
            }
            while(*fmt>='0' && *fmt<='9'){ width = width*10 + (*fmt-'0'); fmt++; }
            while(*fmt=='l') fmt++;
            if(left) pad = ' ';
            switch(*fmt){
                case 'd':{ int v=va_arg(args,int); itoa(v,buf,10); pf_emit(buf,width,left,pad); break;}
                case 'u':{ uint32_t v=va_arg(args,uint32_t); utoa(v,buf,10); pf_emit(buf,width,left,pad); break;}
                case 'x':{ uint32_t v=va_arg(args,uint32_t); utoa(v,buf,16); pf_emit(buf,width,left,pad); break;}
                case 'X':{ uint32_t v=va_arg(args,uint32_t); utoa(v,buf,16);
                           for(char *c=buf;*c;c++) if(*c>='a'&&*c<='f') *c=(char)(*c-32);
                           pf_emit(buf,width,left,pad); break;}
                case 's':{ char *s=va_arg(args,char*); pf_emit(s?s:"(null)",width,left,' '); break;}
                case 'c':{ buf[0]=(char)va_arg(args,int); buf[1]=0; pf_emit(buf,width,left,' '); break;}
                case '%': vga_putchar('%'); break;
                case 0:   va_end(args); return;          /* a lone '%' at the end */
            }
        } else vga_putchar(*fmt);
        fmt++;
    }
    va_end(args);
}
void vga_getchar_at(int x, int y, uint8_t *ch, uint8_t *attr){
    /* v0.7.12: was the only shadow accessor with no bounds check.  Callers in
       desktop.c read the cell under the mouse, which can be off-screen for a
       frame after a mode change shrinks the grid. */
    if(!vp_xlate(&x,&y)){
        if(ch)   *ch=' ';
        if(attr) *attr=0x07;
        return;
    }
    uint16_t v=shadow[y*g_cols+x];
    *ch=(uint8_t)(v&0xFF); *attr=(uint8_t)(v>>8);
}
void vga_putraw_at(uint8_t ch, uint8_t attr, int x, int y){
    vga_write(x,y,(uint16_t)((attr<<8)|ch));
}
