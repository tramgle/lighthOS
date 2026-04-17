/* segv — deliberately dereference NULL to trigger a page fault.
 * Companion to tests/segv.vsh: kernel should terminate just this
 * process (via the ring-3 fault → SIGSEGV path) and return to the
 * caller's shell, rather than panicking. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    u_puts_n("segv: about to NULL-deref\n");
    volatile int *bad = (volatile int *)0;
    *bad = 42;
    u_puts_n("segv: still alive (!!!)\n");
    return 0;
}
