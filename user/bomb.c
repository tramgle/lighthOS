#include "ulib_x64.h"

/* Bounded fork bomb.
 *
 *   bomb N           — fork 2^N processes, all exit immediately.
 *                      Stresses the exit/reap path.
 *   bomb N hold      — fork 2^N processes, every descendant yields
 *                      forever. Leaves the process table populated
 *                      so `ps` / `free` can observe the load and
 *                      PROCESS_MAX is easy to probe. Kill the tree
 *                      with Ctrl-C from the shell (the kernel sends
 *                      SIG_INT to the pipeline's whole pgid).
 *   bomb N <secs>    — same as `hold` but auto-exits after N seconds
 *                      of polling sleep. 0 = exit immediately.
 *   bomb             — unbounded; forks until sys_fork starts failing,
 *                      then exits. Don't `hold` without a bound.
 */

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int max_gen = -1;    /* -1 = unbounded */
    int hold_secs = 0;   /* 0 = exit immediately, -1 = forever */
    if (argc > 1 && argv[1]) max_gen = u_atoi(argv[1]);
    if (argc > 2 && argv[2]) {
        if (u_strcmp(argv[2], "hold") == 0) hold_secs = -1;
        else hold_secs = u_atoi(argv[2]);
    }

    long pid = sys_getpid();
    u_puts_n("[bomb] pid="); u_putdec(pid);
    u_puts_n(" gen="); u_putdec(max_gen);
    u_puts_n(" hold="); u_putdec(hold_secs); u_putc('\n');

    int gen = max_gen;
    int fails = 0;
    while (gen != 0) {
        int c = sys_fork();
        if (c < 0) {
            /* Out of slots — back off a few times, then give up so
               we don't spin forever against a saturated table. */
            if (++fails >= 64) {
                u_puts_n("[bomb] fork limit hit; stopping at gen=");
                u_putdec(gen); u_putc('\n');
                break;
            }
            sys_yield();
            continue;
        }
        fails = 0;
        if (gen > 0) gen--;
    }

    /* Every forked lineage falls through here. With max_gen=N we end
       up with 2^N processes at this line. hold_secs decides what they
       do from here:
         0  : return 0 — mass-exit, stresses reap path.
         >0 : poll sys_time for that many seconds then exit.
         <0 : yield forever until killed. */
    if (hold_secs == 0) return 0;

    if (hold_secs < 0) {
        for (;;) sys_yield();
    }

    long start = sys_time();
    long target = start + (long)hold_secs * 100;   /* 100 Hz */
    while (sys_time() < target) sys_yield();
    return 0;
}
