/* stty — small terminal-control shim.
 *
 *   stty size         — report rows cols
 *   stty raw          — enable kernel raw mode (no echo, no BS magic)
 *   stty cooked       — restore default cooked-mode line discipline
 *
 * `size` uses the VT100 trick: move the cursor to an absurd position
 * (999;999), the terminal clamps, then ESC[6n asks for the position.
 * The reply comes back over stdin as ESC[row;colR. We flip raw mode
 * on for the duration of the probe so the reply bytes don't get
 * echoed back to the terminal. */

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
    if (argc >= 2 && u_strcmp(argv[1], "raw") == 0) {
        sys_tty_raw(1);
        return 0;
    }
    if (argc >= 2 && u_strcmp(argv[1], "cooked") == 0) {
        sys_tty_raw(0);
        return 0;
    }
    int want_size = (argc >= 2 && argv[1][0] == 's' && argv[1][1] == 'i');

    /* Probe in raw mode so the terminal's reply doesn't echo back. */
    sys_tty_raw(1);
    u_puts_n("\033[s\033[999;999H\033[6n\033[u");

    int rows = 0, cols = 0;
    int rc = read_reply(&rows, &cols);
    sys_tty_raw(0);
    if (rc != 0) {
        /* Terminal didn't answer. Fall back to the kernel's cached
           winsize (24x80 default, or whatever someone set earlier). */
        uint16_t cr = 0, cc = 0;
        sys_tty_getsize(&cr, &cc);
        rows = cr; cols = cc;
    } else {
        /* Probe succeeded — update the kernel cache so other programs
           can read the size without re-probing (which would echo-glitch
           again on a non-raw reader). */
        sys_tty_setsize(rows, cols);
    }
    if (want_size) {
        u_putdec(rows); u_putc(' '); u_putdec(cols); u_putc('\n');
    } else {
        u_puts_n("rows "); u_putdec(rows);
        u_puts_n("; cols "); u_putdec(cols); u_putc('\n');
    }
    return 0;
}
