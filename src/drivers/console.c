#include "drivers/console.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "kernel/task.h"
#include "lib/string.h"

void console_init(void) {
    /* Nothing extra — keyboard, serial, VGA already initialized */
}

ssize_t console_read(void *buf, size_t count) {
    /* Non-blocking: returns 0 if no data is ready. Callers that need to
       block must yield and retry (the syscall int-gate clears IF, so
       halting here would deadlock — nothing could wake us). */
    char *cbuf = (char *)buf;
    if (count == 0) return 0;

    if (keyboard_has_key()) {
        cbuf[0] = keyboard_getchar();
        return 1;
    }
    if (serial_has_data()) {
        cbuf[0] = serial_getchar();
        return 1;
    }
    return 0;
}

/* ANSI escape sequence state machine */
enum ansi_state {
    ANSI_NORMAL,
    ANSI_ESC,       /* got \033 */
    ANSI_BRACKET,   /* got \033[ */
};

static enum ansi_state ansi = ANSI_NORMAL;
#define ANSI_PARAM_MAX 4
static int ansi_params[ANSI_PARAM_MAX];
static int ansi_param_count;
static int ansi_cur_param;

static void ansi_reset(void) {
    ansi = ANSI_NORMAL;
    ansi_param_count = 0;
    ansi_cur_param = 0;
    memset(ansi_params, 0, sizeof(ansi_params));
}

static void ansi_execute(char cmd) {
    int p0 = (ansi_param_count > 0) ? ansi_params[0] : 0;
    int p1 = (ansi_param_count > 1) ? ansi_params[1] : 0;

    switch (cmd) {
    case 'H': /* Cursor position: \033[row;colH */
    case 'f': {
        int row = (p0 > 0) ? p0 - 1 : 0;  /* 1-based to 0-based */
        int col = (p1 > 0) ? p1 - 1 : 0;
        if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
        if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
        vga_set_cursor(row, col);
        break;
    }
    case 'J': /* Erase display */
        if (p0 == 2) {
            vga_clear();
        }
        break;
    case 'K': { /* Erase line from cursor to end */
        /* We need access to cursor position — use vga internals.
           For now, write spaces from cursor to end of line. */
        int row, col;
        vga_get_cursor(&row, &col);
        for (int c = col; c < VGA_WIDTH; c++) {
            vga_putchar_at(' ', row, c);
        }
        vga_set_cursor(row, col);  /* restore cursor */
        break;
    }
    case 'A': { /* Cursor up */
        int n = (p0 > 0) ? p0 : 1;
        int row, col;
        vga_get_cursor(&row, &col);
        row -= n;
        if (row < 0) row = 0;
        vga_set_cursor(row, col);
        break;
    }
    case 'B': { /* Cursor down */
        int n = (p0 > 0) ? p0 : 1;
        int row, col;
        vga_get_cursor(&row, &col);
        row += n;
        if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
        vga_set_cursor(row, col);
        break;
    }
    case 'C': { /* Cursor forward */
        int n = (p0 > 0) ? p0 : 1;
        int row, col;
        vga_get_cursor(&row, &col);
        col += n;
        if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;
        vga_set_cursor(row, col);
        break;
    }
    case 'D': { /* Cursor back */
        int n = (p0 > 0) ? p0 : 1;
        int row, col;
        vga_get_cursor(&row, &col);
        col -= n;
        if (col < 0) col = 0;
        vga_set_cursor(row, col);
        break;
    }
    default:
        break;
    }
}

static void console_putchar(char c) {
    /* Always forward the raw byte to serial — real terminals handle ANSI
       themselves, and stripping the escapes here (as we must for VGA, which
       doesn't) means a serial user sees nothing useful from programs like
       vi. The state machine below drives the VGA side only. */
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);

    switch (ansi) {
    case ANSI_NORMAL:
        if (c == '\033') {
            ansi = ANSI_ESC;
        } else {
            vga_putchar(c);
        }
        break;

    case ANSI_ESC:
        if (c == '[') {
            ansi = ANSI_BRACKET;
            ansi_param_count = 0;
            ansi_cur_param = 0;
            memset(ansi_params, 0, sizeof(ansi_params));
        } else {
            ansi_reset();
        }
        break;

    case ANSI_BRACKET:
        if (c >= '0' && c <= '9') {
            ansi_cur_param = ansi_cur_param * 10 + (c - '0');
        } else if (c == ';') {
            if (ansi_param_count < ANSI_PARAM_MAX) {
                ansi_params[ansi_param_count++] = ansi_cur_param;
            }
            ansi_cur_param = 0;
        } else {
            /* End of sequence — store last param and execute */
            if (ansi_param_count < ANSI_PARAM_MAX) {
                ansi_params[ansi_param_count++] = ansi_cur_param;
            }
            ansi_execute(c);
            ansi_reset();
        }
        break;
    }
}

ssize_t console_write(const void *buf, size_t count) {
    const char *cbuf = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        console_putchar(cbuf[i]);
    }
    return (ssize_t)count;
}
