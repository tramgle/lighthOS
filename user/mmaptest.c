/* mmaptest — exercise SYS_MMAP_ANON + SYS_MPROTECT and print an
 * "OK <sum>" line the test harness greps for. */
#include "ulib_x64.h"

#define REGION_A 0x30000000UL
#define REGION_B 0x30100000UL   /* far from A to avoid any coalesce */
#define REGION_SIZE 0x2000      /* 8 KiB, 2 pages */

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    long r = sys_mmap_anon((void *)REGION_A, REGION_SIZE, PROT_READ | PROT_WRITE);
    if (r != (long)REGION_A) { u_puts_n("FAIL mmap A\n"); return 1; }

    volatile unsigned char *p = (unsigned char *)REGION_A;
    for (long i = 0; i < REGION_SIZE; i++) if (p[i] != 0) { u_puts_n("FAIL nonzero\n"); return 1; }

    unsigned long sum = 0;
    for (long i = 0; i < REGION_SIZE; i++) { p[i] = (unsigned char)(i & 0xFF); sum += p[i]; }

    /* overlap should fail */
    if (sys_mmap_anon((void *)REGION_A, REGION_SIZE, PROT_READ | PROT_WRITE) != -1) {
        u_puts_n("FAIL overlap allowed\n"); return 1;
    }

    /* mprotect to RO should still let us read */
    if (sys_mprotect((void *)REGION_A, 0x1000, PROT_READ) != 0) {
        u_puts_n("FAIL mprotect\n"); return 1;
    }
    if (p[0] != 0) { /* data still present */ }

    /* non-overlapping second region */
    long r2 = sys_mmap_anon((void *)REGION_B, REGION_SIZE, PROT_READ | PROT_WRITE);
    if (r2 != (long)REGION_B) { u_puts_n("FAIL mmap B\n"); return 1; }

    u_puts_n("OK ");
    u_putdec((long)sum);
    u_putc('\n');
    return 0;
}
