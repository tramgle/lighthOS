/* mv <src> <dst> — copy then unlink the source. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 3) return 2;
    char *cp_argv[] = { (char *)"cp", argv[1], argv[2], 0 };
    long pid = sys_fork();
    if (pid == 0) { sys_execve("/bin/cp", cp_argv, 0); sys_exit(127); }
    if (pid < 0) return 1;
    int st = 0;
    sys_waitpid((int)pid, &st);
    if (st != 0) return st;
    return sys_unlink(argv[1]) == 0 ? 0 : 1;
}
