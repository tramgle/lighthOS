/* test_fork: fork, child exits 17, parent waitpids and checks status. */
#include "syscall_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    long parent_pid = sys_getpid();
    long pid = sys_fork();
    if (pid == 0) {
        if (sys_getpid() == parent_pid) sys_exit(20);  /* pids didn't diverge */
        sys_exit(17);
    }
    if (pid < 0) return 21;
    int status = 0;
    if (sys_waitpid((int)pid, &status) != pid) return 22;
    if (status != 17) return 23;
    return 0;
}
