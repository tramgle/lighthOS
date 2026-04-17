/* dyn_echo — like /bin/echo but nominally dynamically linked
 * through libvibc. TEMPORARILY static (see dynhello.c for the
 * ld.so port note). */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    for (int i = 1; i < argc; i++) {
        if (i > 1) u_putc(' ');
        u_puts_n(argv[i]);
    }
    u_putc('\n');
    return 0;
}
