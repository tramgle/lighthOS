#include "drivers/console.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "kernel/task.h"
#include "lib/string.h"

void console_init(void) {
    /* Nothing extra — keyboard, serial, VGA already initialized */
}

/* Cooked-mode line discipline state. line_len counts visible chars
   on the current line so BS can't chew into the prompt. Reset on \n. */
static uint32_t line_len;

ssize_t console_read(void *buf, size_t count) {
    char *cbuf = (char *)buf;
    if (count == 0) return 0;

    /* Loop until we have a byte to deliver. In cooked mode we may
       swallow a prompt-level BS and keep waiting. */
    for (;;) {
        /* Syscall entry clears IF (via IA32_FMASK). `sti; hlt; cli`
           atomically re-enables interrupts just long enough for the
           UART or keyboard IRQ to wake the halt. */
        while (!keyboard_has_key() && !serial_has_data()) {
            __asm__ volatile ("sti; hlt; cli");
        }
        char c = keyboard_has_key() ? keyboard_getchar() : serial_getchar();

        /* Raw mode: pass bytes through verbatim. Shell's readline,
           vi, and anything else driving its own cursor want this. */
        if (serial_get_raw()) {
            cbuf[0] = c;
            return 1;
        }

        /* Cooked mode: echo + BS rubout + line_len guard, mirrored
           to both serial and VGA via console_write. */
        if (c == '\b') {
            if (line_len == 0) continue;     /* don't eat into prompt */
            line_len--;
            console_write("\b \b", 3);
        } else if (c == '\n') {
            line_len = 0;
            console_write("\r\n", 2);
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            line_len++;
            console_write(&c, 1);
        }
        cbuf[0] = c;
        return 1;
    }
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

/* ESC[s / ESC[u — save / restore cursor. stty's CSI-6n probe and vi's
   status-line updates both rely on this pair; without it the cursor
   gets left wherever the probe parked it. */
static int saved_row, saved_col;

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
    case 's': { /* Save cursor */
        vga_get_cursor(&saved_row, &saved_col);
        break;
    }
    case 'u': { /* Restore cursor */
        vga_set_cursor(saved_row, saved_col);
        break;
    }
    case 'n':
        /* CSI-6n device status report. A real terminal answers with
           ESC[row;colR; VGA has no way to shove bytes back into the
           input stream, so we just drop it. Callers that want the
           live dimensions either read serial or use SYS_TTY_WINSZ. */
        break;
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
