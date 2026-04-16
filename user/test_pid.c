/* test_pid: verify getpid and write round-trip. Exit 0 on pass. */
#include "syscall_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    long pid = sys_getpid();
    if (pid <= 0) return 10;                     /* must be positive */
    if (sys_write(1, "pid-ok\n", 7) != 7) return 11;
    return 0;
}
