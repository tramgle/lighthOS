#include "syscall.h"

int main(void) {
    int c = sys_fork();
    if (c < 0) {
        sys_write(1, "fork failed\n", 12);
        return 1;
    }
    if (c == 0) {
        /* Child: exec hello so we see both fork AND execve work together. */
        char *argv[] = { "/bin/hello", 0 };
        sys_execve("/bin/hello", argv);
        sys_write(1, "execve failed\n", 14);
        sys_exit(1);
    }
    int status = 0;
    sys_waitpid(c, &status);
    sys_write(1, "parent done\n", 12);
    return 0;
}
