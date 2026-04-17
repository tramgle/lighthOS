/* alarmtest — register SIGALRM handler, test alarm cancel + fire.
 *   A1: alarm(1), alarm(0) → no fire.
 *   A2: alarm(1), yield until fired → handler runs within ~1s.
 * Prints "OK <elapsed>" on success. */

#include "ulib_x64.h"

static volatile int fired;
static volatile long fire_tick;

static void on_sigalrm(int signo) {
    (void)signo;
    fired = 1;
    fire_tick = sys_time();
    sys_sigreturn();
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    sys_signal_raw(SIG_ALRM, on_sigalrm);

    /* A1: arm then cancel. */
    sys_alarm(1);
    sys_alarm(0);
    long t0 = sys_time();
    while (sys_time() - t0 < 20) sys_yield();
    if (fired) { u_puts_n("FAIL canceled fired\n"); return 1; }

    /* A2: real fire. */
    long start = sys_time();
    sys_alarm(1);
    long deadline = start + 200;                 /* 2 seconds of slack */
    while (!fired && sys_time() < deadline) sys_yield();
    if (!fired) { u_puts_n("FAIL not fired\n"); return 1; }

    long elapsed = fire_tick - start;
    if (elapsed < 80 || elapsed > 150) {
        u_puts_n("FAIL elapsed="); u_putdec(elapsed); u_putc('\n');
        return 1;
    }
    u_puts_n("OK "); u_putdec(elapsed); u_putc('\n');
    return 0;
}
