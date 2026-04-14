#include "test/test.h"
#include "drivers/console.h"
#include "drivers/vga.h"

test_results_t test_ansi(void) {
    TEST_SUITE_BEGIN("ansi");

    /* Clear screen via ANSI */
    vga_putchar('X');  /* put something on screen first */
    console_write("\033[2J", 4);
    int row, col;
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 0 && col == 0, "ANSI clear screen resets cursor to 0,0");

    /* Cursor positioning */
    console_write("\033[5;10H", 7);
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 4 && col == 9, "ANSI cursor position 5;10 -> row=4,col=9");

    /* Cursor home */
    console_write("\033[H", 3);
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 0 && col == 0, "ANSI cursor home");

    /* Write text then clear to EOL */
    console_write("\033[3;1H", 6);   /* move to row 3, col 1 */
    console_write("ABCDEF", 6);       /* write text */
    console_write("\033[3;4H", 6);   /* move to row 3, col 4 (on the 'D') */
    console_write("\033[K", 3);       /* clear to EOL */
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 2 && col == 3, "ANSI clear to EOL preserves cursor");

    /* Cursor movement: up/down/left/right */
    console_write("\033[10;10H", 8);
    console_write("\033[3A", 4);  /* up 3 */
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 6 && col == 9, "ANSI cursor up");

    console_write("\033[2B", 4);  /* down 2 */
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 8 && col == 9, "ANSI cursor down");

    console_write("\033[5C", 4);  /* right 5 */
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 8 && col == 14, "ANSI cursor right");

    console_write("\033[3D", 4);  /* left 3 */
    vga_get_cursor(&row, &col);
    TEST_ASSERT(row == 8 && col == 11, "ANSI cursor left");

    TEST_SUITE_END();
}
