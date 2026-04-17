/* echo [-n] args... — print args separated by spaces, optionally
 * suppress the trailing newline with -n. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int newline = 1;
    int arg = 1;
    if (arg < argc && u_strcmp(argv[arg], "-n") == 0) { newline = 0; arg++; }
    for (int i = arg; i < argc; i++) {
        if (i > arg) u_putc(' ');
        u_puts_n(argv[i]);
    }
    if (newline) u_putc('\n');
    return 0;
}
