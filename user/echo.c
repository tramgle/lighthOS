/* echo: print args separated by spaces, terminate with newline. */
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
