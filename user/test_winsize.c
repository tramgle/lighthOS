/* Exercise SYS_TTY_WINSZ: read default, write new, read back. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    uint16_t r = 0, c = 0;
    sys_tty_getsize(&r, &c);
    u_puts_n("default "); u_putdec(r); u_putc('x'); u_putdec(c); u_putc('\n');

    sys_tty_setsize(42, 137);
    sys_tty_getsize(&r, &c);
    u_puts_n("after-set "); u_putdec(r); u_putc('x'); u_putdec(c); u_putc('\n');

    /* Restore defaults so other tests don't inherit the oddball size. */
    sys_tty_setsize(24, 80);
    return 0;
}
