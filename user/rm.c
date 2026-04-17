/* rm <file>...: remove files. Directory removal isn't wired up
   on the kernel side (no SYS_RMDIR), so rm on a directory fails
   with -1 — use that as the signal to teach SYS_RMDIR when
   we need it. */

#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) {
        u_puts_n("rm: missing operand\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_unlink(argv[i]) != 0) {
            u_puts_n("rm: "); u_puts_n(argv[i]);
            u_puts_n(": cannot remove\n");
            rc = 1;
        }
    }
    return rc;
}
