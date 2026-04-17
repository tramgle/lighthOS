/* chroot <dir> [cmd [args...]]
 * Call sys_chroot(dir), then execve the command (default /bin/shell).
 * Resulting process inherits the new root via the kernel's chroot
 * field. Run inside a fork() so the parent's root is untouched. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) { u_puts_n("chroot: usage: dir [cmd [args...]]\n"); return 2; }
    if (sys_chroot(argv[1]) != 0) { u_puts_n("chroot: failed\n"); return 1; }
    const char *cmd = (argc >= 3) ? argv[2] : "/bin/shell";
    char *exec_argv[16];
    int ai = 0;
    exec_argv[ai++] = (char *)cmd;
    for (int i = 3; i < argc && ai < 15; i++) exec_argv[ai++] = argv[i];
    exec_argv[ai] = 0;
    sys_execve(cmd, exec_argv, 0);
    u_puts_n("chroot: execve failed\n");
    return 1;
}
