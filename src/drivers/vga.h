#ifndef VGA_H
#define VGA_H

#include "include/types.h"

enum vga_color {
    VGA_BLACK        = 0,
    VGA_BLUE         = 1,
    VGA_GREEN        = 2,
    VGA_CYAN         = 3,
    VGA_RED          = 4,
    VGA_MAGENTA      = 5,
    VGA_BROWN        = 6,
    VGA_LIGHT_GREY   = 7,
    VGA_DARK_GREY    = 8,
    VGA_LIGHT_BLUE   = 9,
    VGA_LIGHT_GREEN  = 10,
    VGA_LIGHT_CYAN   = 11,
    VGA_LIGHT_RED    = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW       = 14,
    VGA_WHITE        = 15,
};

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

void vga_init(void);
void vga_putchar(char c);
void vga_puts(const char *s);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_clear(void);
void vga_set_cursor(int row, int col);
void vga_get_cursor(int *row, int *col);
void vga_putchar_at(char c, int row, int col);
void vga_backspace(void);

/* Switch to VGA mode 13h: 320x200 256-color linear framebuffer at
   phys 0xA0000. After this point the text-mode primitives above
   are no-ops (they'd poke 0xB8000 which is unmapped in graphics
   mode). No vga_mode13_exit for now — the game runs until shutdown. */
void vga_mode13_enter(void);

#define VGA_MODE13_PHYS    0xA0000ULL
#define VGA_MODE13_BYTES   (320 * 200)
#define VGA_MODE13_PAGES   16        /* 64 KiB rounded up from 64000 */

#endif
