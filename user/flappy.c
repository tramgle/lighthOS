/* flappy — minimalist Flappy Bird in VGA mode 13h (320x200x256).
 *
 * Controls:
 *   space / up-arrow / any printable / PS/2 Up   — flap
 *   q / ESC                                      — quit (shutdown)
 *
 * The game runs at a fixed 30 Hz via sys_time (100 Hz tick). Input
 * is polled non-blockingly with sys_tty_poll. Uses sys_vga_gfx to
 * flip into mode 13h and map the 64 KiB framebuffer at 0x70000000.
 *
 * Palette indices (see vga_set_palette_default in kernel vga.c):
 *   0 sky  1 ground  2 pipe  3 bird  4 beak  5 eye  6 cloud  7 text
 */

#include "ulib_x64.h"

#define FB_W     320
#define FB_H     200
#define FB_VA    0x70000000ULL

#define GROUND_Y 180         /* top of ground strip */
#define BIRD_X   60
#define BIRD_R   8
/* Physics is integer, scaled by 10 to fake one decimal of precision.
   GRAVITY = 3 → 0.3 px/tick/tick; FLAP_V = -45 → -4.5 px/tick.
   All position + velocity math stays in scaled units; divide by 10
   when you actually draw. */
#define PHY_SCALE 10
#define GRAVITY   3
#define FLAP_V   -45
#define PIPE_W   28
#define GAP_H    60          /* vertical gap between upper/lower pipe */
#define PIPE_SPEED 2         /* pixels per tick */
#define PIPE_COUNT 3
#define PIPE_SPACING 110     /* horizontal spacing between pipes */

static uint8_t *fb;

/* ---- random ---- */
static uint32_t rng_state;
static int rnd(int lo, int hi) {
    rng_state = rng_state * 1103515245u + 12345u;
    int range = hi - lo + 1;
    return lo + (int)((rng_state >> 8) % (uint32_t)range);
}

/* ---- drawing primitives ---- */
static void px(int x, int y, uint8_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H) fb[y * FB_W + x] = c;
}
static void rect(int x, int y, int w, int h, uint8_t c) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > FB_W) x1 = FB_W;
    int y1 = y + h; if (y1 > FB_H) y1 = FB_H;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            fb[yy * FB_W + xx] = c;
}
static void disc(int cx, int cy, int r, uint8_t c) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r2) px(cx + dx, cy + dy, c);
        }
    }
}

/* 5x7 digit glyphs for the score. Row-major, MSB-left. */
static const uint8_t digit_glyphs[10][7] = {
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }, /* 0 */
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }, /* 1 */
    { 0x0E,0x11,0x01,0x06,0x08,0x10,0x1F }, /* 2 */
    { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E }, /* 3 */
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }, /* 4 */
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E }, /* 5 */
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E }, /* 6 */
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }, /* 7 */
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }, /* 8 */
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C }, /* 9 */
};

static void draw_digit(int x, int y, int d, uint8_t c) {
    if (d < 0 || d > 9) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = digit_glyphs[d][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col))
                rect(x + col * 2, y + row * 2, 2, 2, c);
        }
    }
}
static void draw_int(int x, int y, int v, uint8_t c) {
    if (v < 0) v = 0;
    char buf[8]; int n = 0;
    if (v == 0) buf[n++] = 0;
    while (v > 0) { buf[n++] = v % 10; v /= 10; }
    for (int i = n - 1; i >= 0; i--) { draw_digit(x, y, buf[i], c); x += 12; }
}

/* ---- bird + pipes ---- */
/* bird_y and bird_vy are in scaled units — divide by PHY_SCALE to
   get pixels. */
static int bird_y;
static int bird_vy;

struct pipe { int x; int gap_top; int scored; };
static struct pipe pipes[PIPE_COUNT];

static void pipes_init(void) {
    for (int i = 0; i < PIPE_COUNT; i++) {
        pipes[i].x = FB_W + i * PIPE_SPACING;
        pipes[i].gap_top = rnd(30, GROUND_Y - GAP_H - 30);
        pipes[i].scored = 0;
    }
}

static void draw_bird(int x, int y) {
    disc(x, y, BIRD_R, 3);           /* body */
    disc(x + 3, y - 2, 2, 6);        /* wing highlight */
    disc(x + 4, y - 2, 1, 5);        /* eye */
    rect(x + BIRD_R - 2, y + 1, 5, 2, 4);  /* beak */
}

static void draw_pipe(struct pipe *p) {
    /* upper */
    rect(p->x, 0, PIPE_W, p->gap_top, 2);
    rect(p->x - 2, p->gap_top - 4, PIPE_W + 4, 4, 2);
    /* lower */
    int ly = p->gap_top + GAP_H;
    rect(p->x, ly, PIPE_W, GROUND_Y - ly, 2);
    rect(p->x - 2, ly, PIPE_W + 4, 4, 2);
}

static void draw_background(void) {
    rect(0, 0, FB_W, GROUND_Y, 0);     /* sky */
    /* a few clouds */
    disc(40, 30, 8, 6); disc(48, 28, 6, 6);
    disc(200, 50, 10, 6); disc(212, 48, 7, 6);
    rect(0, GROUND_Y, FB_W, FB_H - GROUND_Y, 1);
}

/* ---- collision ---- */
static int collides(int score) {
    int bx = BIRD_X, by = bird_y / PHY_SCALE;
    if (by - BIRD_R < 0) return 1;
    if (by + BIRD_R >= GROUND_Y) return 1;
    for (int i = 0; i < PIPE_COUNT; i++) {
        struct pipe *p = &pipes[i];
        if (bx + BIRD_R < p->x || bx - BIRD_R > p->x + PIPE_W) continue;
        if (by - BIRD_R < p->gap_top) return 1;
        if (by + BIRD_R > p->gap_top + GAP_H) return 1;
    }
    (void)score;
    return 0;
}

/* ---- input ---- */
enum { INPUT_NONE, INPUT_FLAP, INPUT_QUIT };

static int poll_input(void) {
    int action = INPUT_NONE;
    /* Drain every pending byte so queued presses don't pile up. */
    while (sys_tty_poll()) {
        char c;
        if (sys_read(0, &c, 1) != 1) break;
        unsigned char u = (unsigned char)c;
        if (u == 'q' || u == 'Q' || u == 0x1B) return INPUT_QUIT;
        /* Space, enter, any printable, or the kernel KEY_UP (0x81)
           counts as a flap. Debounce: only one flap per poll round. */
        if (u == ' ' || u == '\n' || u == '\r' || u == 0x81 ||
            (u >= 0x20 && u < 0x7F)) {
            action = INPUT_FLAP;
        }
    }
    return action;
}

/* ---- main loop ---- */
int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    /* Raw mode so keystrokes come through byte-at-a-time without
       echoing onto the (now-graphical) framebuffer. */
    sys_tty_raw(1);
    rng_state = (uint32_t)sys_time() ^ 0xDEADBEEFu;

    long va = sys_vga_gfx(FB_VA);
    if (va < 0) return 1;
    fb = (uint8_t *)(uintptr_t)va;

    int score = 0;
    int state = 0;   /* 0 = title, 1 = playing, 2 = dead */

    bird_y = (GROUND_Y / 2) * PHY_SCALE;
    bird_vy = 0;
    pipes_init();

    long last = sys_time();
    for (;;) {
        /* 30 Hz tick. 100Hz / 3 ≈ 33ms/frame. */
        while ((sys_time() - last) < 3) sys_yield();
        last = sys_time();

        int in = poll_input();
        if (in == INPUT_QUIT) break;

        if (state == 0) {
            draw_background();
            draw_bird(BIRD_X, bird_y / PHY_SCALE);
            /* center-ish "TAP" cue: three blinking blocks */
            if (((sys_time() / 30) & 1) == 0) {
                rect(FB_W/2 - 30, 60, 60, 8, 7);
            }
            draw_int(FB_W/2 - 6, 30, 0, 7);
            if (in == INPUT_FLAP) { state = 1; bird_vy = FLAP_V; }
            continue;
        }

        if (state == 1) {
            if (in == INPUT_FLAP) bird_vy = FLAP_V;
            bird_vy += GRAVITY;
            bird_y  += bird_vy;

            for (int i = 0; i < PIPE_COUNT; i++) {
                pipes[i].x -= PIPE_SPEED;
                if (pipes[i].x + PIPE_W < 0) {
                    /* Respawn at the far right. */
                    int max_x = 0;
                    for (int j = 0; j < PIPE_COUNT; j++)
                        if (pipes[j].x > max_x) max_x = pipes[j].x;
                    pipes[i].x = max_x + PIPE_SPACING;
                    pipes[i].gap_top = rnd(30, GROUND_Y - GAP_H - 30);
                    pipes[i].scored = 0;
                }
                if (!pipes[i].scored && pipes[i].x + PIPE_W < BIRD_X) {
                    pipes[i].scored = 1;
                    score++;
                }
            }

            if (collides(score)) {
                state = 2;
                bird_vy = 0;
            }
        } else if (state == 2) {
            /* Dead: let the bird fall, then on flap reset. */
            bird_vy += GRAVITY;
            bird_y += bird_vy;
            int floor_y = (GROUND_Y - BIRD_R) * PHY_SCALE;
            if (bird_y > floor_y) bird_y = floor_y;
            if (in == INPUT_FLAP) {
                score = 0;
                bird_y = (GROUND_Y / 2) * PHY_SCALE;
                bird_vy = 0;
                pipes_init();
                state = 1;
            }
        }

        draw_background();
        for (int i = 0; i < PIPE_COUNT; i++) draw_pipe(&pipes[i]);
        draw_bird(BIRD_X, bird_y / PHY_SCALE);
        draw_int(FB_W/2 - 6, 10, score, 7);

        if (state == 2) {
            /* "GAME OVER" block banner — a few solid rectangles so
               we don't need a full font. */
            int bx = FB_W/2 - 40, by = 80;
            rect(bx, by, 80, 30, 1);
            rect(bx + 2, by + 2, 76, 26, 0);
            draw_int(bx + 28, by + 10, score, 7);
        }
    }

    sys_shutdown();
    return 0;
}
