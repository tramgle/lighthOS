/* test_pgroup — verify setpgid/getpgid and group-kill (kill(-pgid)).
 *
 * Flow:
 *   1. Parent forks child.
 *   2. Child sys_setpgid(0, 0): leaves the parent's pgroup, joins
 *      its own.
 *   3. Parent reads child's pgid.
 *   4. Child installs SIGINT handler, busy-yields until flagged.
 *   5. Parent sys_kill(-child_pgid, SIG_INT) — group-kill.
 *   6. Child's handler fires → sets flag → sigreturn → exit 7.
 *   7. Parent waitpids, asserts exit status 7.
 *
 * Exercises: SYS_SETPGID, SYS_GETPGID, kill-by-negative-pid =
 * group signal. No interactive terminal needed.
 */
#include "syscall_x64.h"
#include "ulib_x64.h"

static volatile int got_signal;

static void on_sigint(int signo) {
    (void)signo;
    got_signal = 1;
    sys_sigreturn();
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    long child = sys_fork();
    if (child == 0) {
        /* Child: leave parent group. */
        if (sys_setpgid(0, 0) != 0) sys_exit(10);
        if (sys_getpgid(0) != sys_getpid()) sys_exit(11);

        sys_signal_raw(SIG_INT, on_sigint);
        long deadline = sys_time() + 300;        /* 3 s */
        while (!got_signal && sys_time() < deadline) sys_yield();
        if (!got_signal) sys_exit(12);
        sys_exit(7);
    }
    if (child < 0) return 20;

    /* Give the child a tick to setpgid. */
    long t0 = sys_time();
    while (sys_time() - t0 < 5) sys_yield();

    long cp = sys_getpgid((int)child);
    if (cp != child) {
        u_puts_n("FAIL child pgid != pid: "); u_putdec(cp); u_putc('\n');
        return 21;
    }

    /* Group-kill the child's pgid. */
    if (sys_kill(-(int)cp, SIG_INT) != 0) return 22;

    int status = 0;
    if (sys_waitpid((int)child, &status) != child) return 23;
    if (status != 7) {
        u_puts_n("FAIL child status="); u_putdec(status); u_putc('\n');
        return 24;
    }
    u_puts_n("pgroup ok\n");
    return 0;
}
