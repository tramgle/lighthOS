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
