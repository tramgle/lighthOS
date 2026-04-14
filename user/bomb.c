#include "syscall.h"
#include "ulib.h"

/* Bounded fork bomb. With argument N, each lineage forks N times before
   settling — so the final process count is 2^N. Without an argument,
   runs unbounded until the kernel's process table fills.

   Useful as a stress test: `bomb 5` -> 32 processes, `bomb 10` -> 1024,
   `bomb` -> as many as the kernel will grant. */

static int parse_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char **argv) {
    int max_gen = -1;  /* -1 = unbounded */
    if (argc > 1 && argv[1]) max_gen = parse_int(argv[1]);

    int pid = sys_getpid();
    printf("[bomb] pid=%d gen=%d\n", pid, max_gen);

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
