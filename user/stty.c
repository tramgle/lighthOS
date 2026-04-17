/* stty size — report the terminal's row/col dimensions.
 *
 * POSIX would do this via ioctl(TIOCGWINSZ) against a tty driver.
 * We don't have a tty subsystem, so we use the VT100 trick: move
 * the cursor to an absurd position (999;999), the terminal clamps
 * it to its actual bounds, then ESC[6n asks the terminal to report
 * the position — it replies ESC[row;colR over stdin.
 *
 * Caveats: the kernel's serial line-discipline will echo the
 * digits and 'R' of the reply back at the terminal, producing a
 * brief visible glitch. For a clean probe we'd need a raw-mode
 * flag on the serial driver, which is a separate project. */

#include "ulib_x64.h"

static int read_reply(int *rows, int *cols) {
    char c;
    /* Wait for ESC. */
    for (int i = 0; i < 64; i++) {
        if (sys_read(0, &c, 1) != 1) return -1;
        if (c == 0x1B) break;
        if (i == 63) return -1;
    }
    if (sys_read(0, &c, 1) != 1 || c != '[') return -1;

    int r = 0, co = 0;
    while (sys_read(0, &c, 1) == 1 && c >= '0' && c <= '9')
        r = r * 10 + (c - '0');
    if (c != ';') return -1;
    while (sys_read(0, &c, 1) == 1 && c >= '0' && c <= '9')
        co = co * 10 + (c - '0');
    if (c != 'R') return -1;
    *rows = r; *cols = co;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int want_size = (argc >= 2 && argv[1][0] == 's' && argv[1][1] == 'i');

    /* Save cursor, drive it to the bottom-right, request pos, restore. */
    u_puts_n("\033[s\033[999;999H\033[6n\033[u");

    int rows = 0, cols = 0;
    if (read_reply(&rows, &cols) != 0) {
        u_puts_n("stty: no reply (terminal doesn't support CSI 6n?)\n");
        return 1;
    }
    if (want_size) {
        u_putdec(rows); u_putc(' '); u_putdec(cols); u_putc('\n');
    } else {
        u_puts_n("rows "); u_putdec(rows);
        u_puts_n("; cols "); u_putdec(cols); u_putc('\n');
    }
    return 0;
}
