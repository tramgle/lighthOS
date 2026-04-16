/* sigtest: install a SIGINT handler, raise SIGINT against ourselves,
   verify the handler ran. Writes "CAUGHT N" on success (N = signo).
   Used by tests/signal.vsh to assert user-handler delivery works. */

#include "syscall.h"
#include "ulib.h"

static volatile int caught;

static void on_sigint(int signo) {
    caught = signo;
}

int main(void) {
    if (signal(SIG_INT, on_sigint) == SIG_ERR) {
        puts("FAIL install\n");
        return 1;
    }
    /* Send SIGINT to self. Queued by the kernel, then delivered on
       the iret return from sys_kill — so by the time control reaches
       the next line, the handler has already run. */
    int32_t pid = sys_getpid();
    sys_kill(pid, SIG_INT);

    if (caught != SIG_INT) {
        puts("FAIL notrun\n");
        return 1;
    }

    /* Also verify SIG_IGN silences delivery. */
    signal(SIG_INT, SIG_IGN);
    caught = 0;
    sys_kill(pid, SIG_INT);
    if (caught != 0) {
        puts("FAIL ignored\n");
        return 1;
    }

    /* Restore default + confirm reinstall works. */
    signal(SIG_INT, on_sigint);
    sys_kill(pid, SIG_INT);
    if (caught != SIG_INT) {
        puts("FAIL reinstall\n");
        return 1;
    }

    printf("CAUGHT %d\n", caught);
    return 0;
}
