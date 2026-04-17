/* env — print each envp entry on its own line. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    if (!envp) return 0;
    for (int i = 0; envp[i]; i++) {
        u_puts_n(envp[i]);
        u_putc('\n');
    }
    return 0;
}
