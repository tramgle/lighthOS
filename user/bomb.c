#include "ulib_x64.h"

/* Bounded fork bomb. With argument N, each lineage forks N times before
   settling — so the final process count is 2^N. Without an argument,
   runs unbounded until the kernel's process table fills.

   Useful as a stress test: `bomb 5` -> 32 processes, `bomb 10` -> 1024,
   `bomb` -> as many as the kernel will grant. */

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int max_gen = -1;  /* -1 = unbounded */
    if (argc > 1 && argv[1]) max_gen = u_atoi(argv[1]);

    long pid = sys_getpid();
    u_puts_n("[bomb] pid="); u_putdec(pid);
    u_puts_n(" gen="); u_putdec(max_gen); u_putc('\n');

    int gen = max_gen;
    while (gen != 0) {
        int c = sys_fork();
        if (c < 0) {
            /* Out of slots/memory — back off and retry. */
            sys_yield();
            continue;
        }
        if (gen > 0) gen--;
    }

    /* Every forked lineage falls through here. With max_gen=N we end up
       with 2^N processes all exiting near-simultaneously, which stresses
       the exit/reap path. */
    return 0;
}
