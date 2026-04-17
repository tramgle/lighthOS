/* test_xmm — fork three ways; each child loads a distinctive
 * XMM0 / XMM1 pattern and spins verifying it stays intact while
 * the scheduler preempts. If the kernel doesn't FXSAVE/FXRSTOR
 * on context switch, at least one child's XMM will get stomped
 * by a sibling and fail the verify.
 *
 * Uses movdqu with 16-byte patterns; no SSE math, just load +
 * compare. Runs for a fixed tick budget to give the timer IRQ
 * multiple chances to switch tasks mid-loop. */

#include "syscall_x64.h"
#include "ulib_x64.h"

static void child_spin(uint64_t lo, uint64_t hi, int exit_ok) {
    uint64_t expected[2] __attribute__((aligned(16))) = {lo, hi};
    uint64_t seen[2]     __attribute__((aligned(16)));
    __asm__ volatile ("movdqa (%0), %%xmm0\n" :: "r"(expected) : "xmm0", "memory");
    long deadline = sys_time() + 20;          /* 0.2 s */
    long loops = 0;
    while (sys_time() < deadline) {
        __asm__ volatile ("movdqa %%xmm0, (%0)\n" :: "r"(seen) : "memory");
        if (seen[0] != lo || seen[1] != hi) sys_exit(1);
        sys_yield();
        loops++;
    }
    (void)loops;
    sys_exit(exit_ok);
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    /* Parent keeps its own pattern too. */
    int pids[3];
    long p1 = sys_fork();
    if (p1 == 0) child_spin(0x1111111111111111ULL, 0x2222222222222222ULL, 11);
    long p2 = sys_fork();
    if (p2 == 0) child_spin(0x3333333333333333ULL, 0x4444444444444444ULL, 12);
    long p3 = sys_fork();
    if (p3 == 0) child_spin(0x5555555555555555ULL, 0x6666666666666666ULL, 13);

    /* Parent also verifies its own XMM. */
    uint64_t pp[2] __attribute__((aligned(16))) = {0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL};
    uint64_t ps[2] __attribute__((aligned(16)));
    __asm__ volatile ("movdqa (%0), %%xmm0\n" :: "r"(pp) : "xmm0", "memory");

    long deadline = sys_time() + 20;
    while (sys_time() < deadline) {
        __asm__ volatile ("movdqa %%xmm0, (%0)\n" :: "r"(ps) : "memory");
        if (ps[0] != pp[0] || ps[1] != pp[1]) { u_puts_n("FAIL parent\n"); return 1; }
        sys_yield();
    }
    pids[0] = (int)p1; pids[1] = (int)p2; pids[2] = (int)p3;
    int want[3] = {11, 12, 13};
    for (int i = 0; i < 3; i++) {
        int st = 0;
        sys_waitpid(pids[i], &st);
        if (st != want[i]) {
            u_puts_n("FAIL child "); u_putdec(i); u_puts_n(" status=");
            u_putdec(st); u_putc('\n');
            return 1;
        }
    }
    u_puts_n("xmm ok\n");
    return 0;
}
