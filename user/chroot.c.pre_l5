/* chroot <dir> [cmd [args...]]: change root to <dir>, then exec the
   given command (default /bin/shell). We execve rather than spawn so
   that the caller's process struct keeps the new root — spawn would
   fork a child with its own inherited root but the parent would have
   the new chroot too, and there'd be no clean return. */

#include "syscall.h"
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: chroot <dir> [cmd [args...]]\n"); return 1; }

    if (sys_chroot(argv[1]) != 0) {
        printf("chroot: %s: not a directory or not found\n", argv[1]);
        return 1;
    }

    char *cmd = (argc >= 3) ? argv[2] : "/bin/shell";
    char *default_argv[] = { cmd, 0 };
    char **child_argv = (argc >= 3) ? &argv[2] : default_argv;

    if (sys_execve(cmd, child_argv) != 0) {
        printf("chroot: cannot exec %s\n", cmd);
        return 1;
    }
    /* unreachable */
    return 0;
}
