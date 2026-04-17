/* time CMD [ARGS...] — fork + execve CMD, then print real/user/sys
 * CPU time for the child in 100 Hz ticks (1 tick = 10 ms).
 *
 *   real: wall-clock elapsed ticks
 *   user: ticks the child spent in ring 3
 *   sys:  ticks the child spent in ring 0
 *
 * Child's time lands in our cutime/cstime after waitpid reaps it, so
 * the delta of sys_times() across fork+waitpid holds the child total. */
#include "ulib_x64.h"

static void emit(const char *label, uint64_t ticks) {
    u_puts_n(label);
    u_putdec((long)ticks);
    u_puts_n(" ticks\n");
}

int main(int argc, char **argv, char **envp) {
    if (argc < 2) {
        u_puts_n("usage: time CMD [ARGS...]\n");
        return 2;
    }

    struct tms t0, t1;
    long r0 = sys_times(&t0);

    long pid = sys_fork();
    if (pid == 0) {
        /* Try the path as given first; then fall back to /bin/<name>
           for bare command names. Mirrors what shell does before
           spawning external binaries. */
        sys_execve(argv[1], &argv[1], envp);
        if (argv[1][0] != '/') {
            char alt[256];
            const char *pfx = "/bin/";
            int j = 0;
            while (*pfx && j < (int)sizeof(alt) - 1) alt[j++] = *pfx++;
            const char *n = argv[1];
            while (*n && j < (int)sizeof(alt) - 1) alt[j++] = *n++;
            alt[j] = 0;
            sys_execve(alt, &argv[1], envp);
        }
        u_puts_n("time: execve failed: ");
        u_puts_n(argv[1]);
        u_putc('\n');
        sys_exit(127);
    }
    if (pid < 0) {
        u_puts_n("time: fork failed\n");
        return 1;
    }

    int status = 0;
    sys_waitpid((int)pid, &status);
    long r1 = sys_times(&t1);

    uint64_t real = (uint64_t)(r1 - r0);
    uint64_t user = t1.cutime - t0.cutime;
    uint64_t sys  = t1.cstime - t0.cstime;

    emit("real ", real);
    emit("user ", user);
    emit("sys  ", sys);
    /* Propagate the child's exit status so callers can chain on it
       just as they would with a plain execve. Caller observes the
       child's return value, not /bin/time's bookkeeping. */
    return status;
}

