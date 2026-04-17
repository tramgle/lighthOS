/* sigtest — register a SIGINT handler, raise SIGINT against self,
 * confirm the handler ran. Print "CAUGHT <signo>" and exit 0. */

#include "ulib_x64.h"

static volatile int caught_count;
static volatile int last_signo;

static void on_sigint(int signo) {
    last_signo = signo;
    caught_count++;
    sys_sigreturn();               /* never returns */
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    sys_signal_raw(SIG_INT, on_sigint);
    sys_kill((int)sys_getpid(), SIG_INT);

    if (caught_count != 1) { u_puts_n("FAIL not caught\n"); return 1; }
    u_puts_n("CAUGHT ");
    u_putdec(last_signo);
    u_putc('\n');
    return 0;
}
