#include "drivers/vga.h"
#include "include/io.h"
#include "lib/string.h"
#include "mm/vmm.h"

/* VGA text framebuffer. Phys 0xB8000, reached through the kernel
   HHDM because PML4[0] is user-private once vmm_init runs. */
static uint16_t *vga_buffer = (uint16_t *)(KERNEL_HHDM_BASE + 0xB8000ULL);
static int vga_row;
static int vga_col;
static uint8_t vga_color_attr;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static inline uint8_t vga_make_color(uint8_t fg, uint8_t bg) {
    return fg | (bg << 4);
}

static void vga_update_cursor(void) {
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

static void vga_scroll(void) {
    if (vga_row < VGA_HEIGHT) return;

    memmove(vga_buffer, vga_buffer + VGA_WIDTH,
            VGA_WIDTH * (VGA_HEIGHT - 1) * sizeof(uint16_t));

    for (int i = 0; i < VGA_WIDTH; i++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + i] =
            vga_entry(' ', vga_color_attr);
    }
    vga_row = VGA_HEIGHT - 1;
}

void vga_init(void) {
    vga_color_attr = vga_make_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_row = 0;
    vga_col = 0;
    vga_clear();
}

void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = vga_entry(' ', vga_color_attr);
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

void vga_get_cursor(int *row, int *col) {
    *row = vga_row;
    *col = vga_col;
}

void vga_putchar_at(char c, int row, int col) {
    if (row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH) {
        vga_buffer[row * VGA_WIDTH + col] = vga_entry(c, vga_color_attr);
    }
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color_attr = vga_make_color(fg, bg);
}

void vga_set_cursor(int row, int col) {
    vga_row = row;
    vga_col = col;
    vga_update_cursor();
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\t') {
        vga_col = (vga_col + 8) & ~7;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        /* Cursor-left only. Our shell's raw-mode line editor sends
           \b both for navigation and as the first of a "\b \b"
           rub-out triple — in both cases the intent is "move one
           left", not "write a glyph". Match vga_backspace without
           erasing, since the rub-out's space will do that. */
        if (vga_col > 0) vga_col--;
        else if (vga_row > 0) { vga_row--; vga_col = VGA_WIDTH - 1; }
    } else if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
        /* Drop other C0 controls + DEL so they don't splatter glyphs. */
    } else {
        vga_buffer[vga_row * VGA_WIDTH + vga_col] =
            vga_entry(c, vga_color_attr);
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    }
    vga_scroll();
    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s) vga_putchar(*s++);
}

void vga_backspace(void) {
    if (vga_col > 0) {
        vga_col--;
    } else if (vga_row > 0) {
        vga_row--;
        vga_col = VGA_WIDTH - 1;
    }
    vga_buffer[vga_row * VGA_WIDTH + vga_col] =
        vga_entry(' ', vga_color_attr);
    vga_update_cursor();
}

/* -------- Mode 13h (320x200x256 linear framebuffer) ----------------
 *
 * Register tables lifted from the canonical IBM VGA / Abrash docs.
 * Sequencer, CRT controller, graphics controller, attribute
 * controller each get a specific run of values. After programming,
 * the framebuffer is at phys 0xA0000, one byte per pixel, row-major,
 * indexed into the DAC palette. We set a practical 256-color palette
 * (6-bit-per-channel DAC) afterwards. */

static const uint8_t mode13_seq[5]  = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const uint8_t mode13_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
};
static const uint8_t mode13_gc[9]   = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
};
static const uint8_t mode13_ac[21]  = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};

static void vga_set_palette_default(void) {
    /* Ramp of a few handy colors — enough for flappy:
         0 sky    1 ground 2 pipe   3 bird   4 beak
         5 eye    6 cloud  7 text   8..255 fill to avoid junk */
    struct { uint8_t r, g, b; } pal[] = {
        { 30, 50, 63 },    /* 0: sky blue  */
        { 40, 30, 10 },    /* 1: ground    */
        { 10, 45, 12 },    /* 2: pipe green*/
        { 63, 50,  5 },    /* 3: bird yellow */
        { 55, 20,  5 },    /* 4: beak red  */
        {  0,  0,  0 },    /* 5: eye black */
        { 60, 60, 63 },    /* 6: cloud white */
        { 63, 63, 63 },    /* 7: text      */
    };
    outb(0x3C8, 0);
    for (unsigned i = 0; i < sizeof(pal)/sizeof(pal[0]); i++) {
        outb(0x3C9, pal[i].r);
        outb(0x3C9, pal[i].g);
        outb(0x3C9, pal[i].b);
    }
    /* Fill remaining entries with a coarse ramp so unset indices
       aren't bright white if a program slips. */
    for (unsigned i = sizeof(pal)/sizeof(pal[0]); i < 256; i++) {
        uint8_t v = (uint8_t)(i >> 2);
        outb(0x3C9, v);
        outb(0x3C9, v);
        outb(0x3C9, v);
    }
}

void vga_mode13_enter(void) {
    /* Misc output — 0x63 picks the 0xA0000 mapping + 25 MHz clock +
       positive polarity for 320x200. */
    outb(0x3C2, 0x63);

    /* Sequencer. Reset off → regs 1..4 → reset on. */
    outb(0x3C4, 0); outb(0x3C5, 0x03);
    for (int i = 0; i < 5; i++) {
        outb(0x3C4, (uint8_t)i);
        outb(0x3C5, mode13_seq[i]);
    }

    /* CRTC: unlock regs 0..7 by clearing bit 7 of reg 0x11. */
    outb(0x3D4, 0x11);
    outb(0x3D5, mode13_crtc[0x11] & 0x7F);
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, (uint8_t)i);
        outb(0x3D5, mode13_crtc[i]);
    }

    /* Graphics controller. */
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, (uint8_t)i);
        outb(0x3CF, mode13_gc[i]);
    }

    /* Attribute controller — indexed via 0x3C0 with a flip-flop reset
       by reading 0x3DA. Writes toggle between index and data. Final
       0x20 to 0x3C0 re-enables the display. */
    (void)inb(0x3DA);
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, mode13_ac[i]);
    }
    outb(0x3C0, 0x20);

    vga_set_palette_default();

    /* Clear the framebuffer (phys 0xA0000, reached via HHDM). */
    uint8_t *fb = (uint8_t *)(uintptr_t)(KERNEL_HHDM_BASE + 0xA0000ULL);
    for (uint32_t i = 0; i < VGA_MODE13_BYTES; i++) fb[i] = 0;
}
