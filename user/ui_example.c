/* ui_example — minimal PS/2 mouse + buttons demo in VGA mode 13h.
 *
 * Three color swatches across the top and a small quit button in the
 * corner. Clicking a swatch paints the bottom canvas with that color
 * and bumps a click counter. Clicking quit (or ESC) returns to text
 * mode and exits.
 *
 * Input:
 *   - PS/2 mouse via SYS_MOUSE_POLL. Button bit 0 is the left click.
 *   - PS/2 keyboard via sys_tty_poll / sys_read — ESC quits.
 *
 * Rendering pass per frame:
 *   1. fill background + canvas with the current color
 *   2. draw four buttons (+ hover highlight for whichever the
 *      cursor's over)
 *   3. draw the click counter
 *   4. draw the cursor last so it always sits on top
 */

#include "ulib_x64.h"

#define FB_W  320
#define FB_H  200
#define FB_VA 0x70000000ULL

/* Palette indices from the default mode-13h palette
 * (vga_set_palette_default). We reuse flappy's layout:
 *   0 sky/dark  1 ground  2 pipe-green  3 bird-yellow
 *   4 beak-orange  5 eye-black  6 cloud-white  7 text
 * — which conveniently gives us a handful of distinct hues to pick
 * the button fills from without writing a new palette. */
#define COL_BG        6   /* cloud white — frame */
#define COL_CANVAS    0   /* sky — starting canvas fill */
#define COL_TEXT      5   /* eye-black — readable on white */
#define COL_BORDER    5
#define COL_HOVER     4
#define COL_CURSOR    5
#define COL_QUIT_BG   4
#define COL_QUIT_MARK 7

static uint8_t *fb;

/* ---- drawing primitives (cribbed from flappy; identical shape) ---- */
static void px(int x, int y, uint8_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H) fb[y * FB_W + x] = c;
}
static void rect(int x, int y, int w, int h, uint8_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > FB_W) x1 = FB_W;
    int y1 = y + h; if (y1 > FB_H) y1 = FB_H;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            fb[yy * FB_W + xx] = c;
}
static void frame(int x, int y, int w, int h, uint8_t c) {
    rect(x, y, w, 1, c);
    rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c);
    rect(x + w - 1, y, 1, h, c);
}
static void line(int x0, int y0, int x1, int y1, uint8_t c) {
    /* Bresenham — just enough to draw the quit "X". */
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = ax - ay;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -ay) { err -= ay; x0 += sx; }
        if (e2 <  ax) { err += ax; y0 += sy; }
    }
}

/* 5x7 digit glyphs — same table as flappy, rendered here at 1x
 * (5 wide, 7 tall) because the click counter sits in a small
 * 40-pixel-wide strip. */
static const uint8_t digit_glyphs[10][7] = {
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },
    { 0x0E,0x11,0x01,0x06,0x08,0x10,0x1F },
    { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },
};

static void draw_digit(int x, int y, int d, uint8_t c) {
    if (d < 0 || d > 9) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = digit_glyphs[d][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) px(x + col, y + row, c);
        }
    }
}
static void draw_int(int x, int y, int v, uint8_t c) {
    if (v < 0) v = 0;
    int digits[10], n = 0;
    if (v == 0) digits[n++] = 0;
    while (v > 0) { digits[n++] = v % 10; v /= 10; }
    for (int i = n - 1; i >= 0; i--) { draw_digit(x, y, digits[i], c); x += 6; }
}

/* ---- buttons ---- */
struct button {
    int x, y, w, h;
    uint8_t fill;
    int     action;   /* >=0 palette index to paint canvas with; -1 = quit */
};

enum { ACTION_QUIT = -1 };

static struct button buttons[] = {
    {  20, 20, 60, 40,  2 /* green  */,  2 },
    { 100, 20, 60, 40,  4 /* orange */,  4 },
    { 180, 20, 60, 40,  1 /* brown  */,  1 },
    { 280, 10, 30, 20,  COL_QUIT_BG, ACTION_QUIT },
};
#define NBUTTONS ((int)(sizeof buttons / sizeof buttons[0]))

static int hit(const struct button *b, int mx, int my) {
    return mx >= b->x && mx < b->x + b->w &&
           my >= b->y && my < b->y + b->h;
}

static void draw_button(const struct button *b, int hover) {
    rect(b->x, b->y, b->w, b->h, b->fill);
    frame(b->x, b->y, b->w, b->h, COL_BORDER);
    if (hover) frame(b->x - 1, b->y - 1, b->w + 2, b->h + 2, COL_HOVER);
    if (b->action == ACTION_QUIT) {
        /* draw an X across the button — universally-understood
           "close" affordance that doesn't need a font. */
        line(b->x + 6, b->y + 4, b->x + b->w - 7, b->y + b->h - 5, COL_QUIT_MARK);
        line(b->x + b->w - 7, b->y + 4, b->x + 6, b->y + b->h - 5, COL_QUIT_MARK);
    }
}

/* ---- cursor — 7-pixel arrow pointing up-left ---- */
static const uint8_t cursor_glyph[12][7] = {
    {1,0,0,0,0,0,0},
    {1,1,0,0,0,0,0},
    {1,1,1,0,0,0,0},
    {1,1,1,1,0,0,0},
    {1,1,1,1,1,0,0},
    {1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1},
    {1,1,1,1,1,0,0},
    {1,1,0,1,1,0,0},
    {1,0,0,0,1,1,0},
    {0,0,0,0,1,1,0},
    {0,0,0,0,0,1,0},
};
static void draw_cursor(int x, int y, uint8_t c) {
    for (int row = 0; row < 12; row++)
        for (int col = 0; col < 7; col++)
            if (cursor_glyph[row][col]) px(x + col, y + row, c);
}

/* ---- ESC polling — identical to flappy's approach ---- */
static int esc_pressed(void) {
    while (sys_tty_poll()) {
        char c;
        if (sys_read(0, &c, 1) != 1) break;
        if ((unsigned char)c == 0x1B || c == 'q' || c == 'Q') return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    /* Same VGA-gate as flappy: only meaningful on a PS/2 console. */
    long src = sys_tty_lastsrc();
    if (src != 2) {
        u_puts_n("ui_example: needs a VGA console with PS/2.\n"
                 "            boot with `make run-vga` and run here.\n");
        return 1;
    }

    sys_tty_raw(1);
    long va = sys_vga_gfx(FB_VA);
    if (va < 0) { sys_tty_raw(0); return 1; }
    fb = (uint8_t *)(uintptr_t)va;

    int canvas_color = COL_CANVAS;
    int click_count  = 0;
    uint32_t prev_btns = 0;

    for (;;) {
        if (esc_pressed()) break;

        struct mouse_state m = {0};
        sys_mouse_poll(&m);

        /* Edge-detect left-button press. Comparing against prev_btns
           debounces a held-down button — we only count each press
           once, regardless of how many poll rounds cover the hold. */
        int left_edge = ((m.buttons & 1) && !(prev_btns & 1));
        prev_btns = m.buttons;

        if (left_edge) {
            for (int i = 0; i < NBUTTONS; i++) {
                if (!hit(&buttons[i], m.x, m.y)) continue;
                click_count++;
                if (buttons[i].action == ACTION_QUIT) goto quit;
                canvas_color = buttons[i].action;
                break;
            }
        }

        /* --- render --- */
        rect(0, 0, FB_W, FB_H, COL_BG);
        /* Canvas area fills below the button strip. */
        rect(10, 70, FB_W - 20, FB_H - 80, (uint8_t)canvas_color);
        frame(10, 70, FB_W - 20, FB_H - 80, COL_BORDER);

        for (int i = 0; i < NBUTTONS; i++) {
            int hover = hit(&buttons[i], m.x, m.y);
            draw_button(&buttons[i], hover);
        }

        /* counter at top-left, above the buttons */
        draw_int(4, 4, click_count, COL_TEXT);

        draw_cursor(m.x, m.y, COL_CURSOR);

        /* Cooperative 60 Hz-ish — yield until at least one tick has
           passed since the last iteration. 100 Hz kernel tick = at
           most 100 fps; draining mouse events per-frame is plenty. */
        long t0 = sys_time();
        while (sys_time() == t0) sys_yield();
    }

quit:
    sys_vga_text();
    sys_tty_raw(0);
    return 0;
}
