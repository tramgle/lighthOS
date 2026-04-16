/* mmaptest: smoke test for SYS_MMAP_ANON + SYS_MPROTECT.
     1. mmap 8KB RW at 0x30000000, write pattern, read back, assert.
     2. mmap again at same addr — must fail (overlap).
     3. mprotect(RO) the first page; subsequent reads still work.
     4. mmap 4KB at 0x30100000 with PROT_READ only; write should still
        succeed since PROT_WRITE was not set... wait, we expect it to
        trap. Skip that sub-case to avoid relying on SIGSEGV handling,
        which this kernel doesn't surface to user space yet. Just
        assert the mapping succeeded.
   Prints "OK <checksum>" on success. */

#include "syscall.h"
#include "ulib.h"

int main(void) {
    /* Pick a base well above the dynamic-linker's library region
       (0x30000000-0x3FFFFFFF) so a dynamic build of this binary
       doesn't collide with its own loaded libulib.so.1. */
    const uint32_t addr = 0x60000000;
    const uint32_t len  = 8192;

    int32_t rc = sys_mmap_anon(addr, len, PROT_READ | PROT_WRITE);
    if (rc != (int32_t)addr) {
        printf("FAIL mmap %d\n", rc);
        return 1;
    }

    /* Fresh pages should read as zero. */
    volatile uint32_t *p = (uint32_t *)addr;
    for (uint32_t i = 0; i < len / 4; i++) {
        if (p[i] != 0) {
            printf("FAIL zero @%u = 0x%x\n", i, p[i]);
            return 1;
        }
    }

    /* Write a pattern, read back. */
    for (uint32_t i = 0; i < len / 4; i++) p[i] = 0xC0DE0000u + i;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len / 4; i++) sum += p[i];

    /* Overlap check: a second mmap at the same addr must fail. */
    int32_t rc2 = sys_mmap_anon(addr, 4096, PROT_READ | PROT_WRITE);
    if (rc2 != -1) {
        puts("FAIL overlap\n");
        return 1;
    }

    /* mprotect first page to RO — reads still work, pattern intact. */
    if (sys_mprotect(addr, 4096, PROT_READ) != 0) {
        puts("FAIL mprotect\n");
        return 1;
    }
    uint32_t sum_after = 0;
    for (uint32_t i = 0; i < len / 4; i++) sum_after += p[i];
    if (sum_after != sum) {
        puts("FAIL reread\n");
        return 1;
    }

    /* Mmap a non-overlapping range to prove the allocator advances. */
    int32_t rc3 = sys_mmap_anon(0x60100000, 4096, PROT_READ | PROT_WRITE);
    if (rc3 != 0x60100000) {
        printf("FAIL second %d\n", rc3);
        return 1;
    }

    printf("OK %u\n", sum);
    return 0;
}
