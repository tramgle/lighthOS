/* alarmtest: install a SIGALRM handler, request alarm(1), busy-yield
   for ~1.5 seconds, verify the handler fired. Also verifies alarm(0)
   cancels a pending alarm. Prints "OK N" on success with elapsed
   ticks; used by tests/alarm.vsh. */

#include "syscall.h"
#include "ulib.h"

static volatile int fired;

static void on_alrm(int signo) { fired = signo; }

int main(void) {
    if (signal(SIG_ALRM, on_alrm) == SIG_ERR) {
        puts("FAIL install\n");
        return 1;
    }

    /* Cancel-before-fire: alarm(1) then alarm(0) — handler should
       not run. */
    sys_alarm(1);
    uint32_t prev = sys_alarm(0);
    if (prev != 1) {
        printf("FAIL cancel_prev %u\n", prev);
        return 1;
    }
    /* Wait past when the first alarm would have fired. */
    uint32_t t0 = sys_time();
    while (sys_time() - t0 < 120) sys_yield();
    if (fired) { puts("FAIL cancel\n"); return 1; }

    /* Real alarm. alarm(1) should fire ~100 ticks from now. */
    fired = 0;
    sys_alarm(1);
    t0 = sys_time();
    while (!fired && sys_time() - t0 < 200) sys_yield();

    if (fired != SIG_ALRM) {
        printf("FAIL notfired %d\n", fired);
        return 1;
    }
    uint32_t elapsed = sys_time() - t0;
    /* Accept anywhere in [80, 150] ticks — scheduling jitter + the
       sys_yield path costs a few ticks. */
    if (elapsed < 80 || elapsed > 150) {
        printf("FAIL timing %u\n", elapsed);
        return 1;
    }
    printf("OK %u\n", elapsed);
    return 0;
}
