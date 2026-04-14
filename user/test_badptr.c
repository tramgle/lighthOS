/* Regression test for user-pointer validation in the syscall boundary.
   Each case hands the kernel a knowingly-bad pointer and expects -1 —
   NOT a page fault. If any case panics the kernel, that's a bug. */

#include "syscall.h"
#include "ulib.h"

static int failures;

static void expect_fail(const char *name, int32_t rc) {
    if (rc < 0) {
        printf("PASS %s -> %d\n", name, rc);
    } else {
        printf("FAIL %s -> %d (expected negative)\n", name, rc);
        failures++;
    }
}

int main(void) {
    /* Path syscalls with a kernel-address pointer. Our user space is
       at 0x08000000+; 0x00100000 is kernel-only. */
    unsigned char stat_buf[64];
    expect_fail("sys_stat kernel path",   sys_stat((const char *)0x00100000, stat_buf));
    expect_fail("sys_stat NULL",          sys_stat((const char *)0, stat_buf));
    expect_fail("sys_stat wild ptr",      sys_stat((const char *)0xFFFFFFFF, stat_buf));

    /* sys_open with a kernel buffer as path. */
    expect_fail("sys_open kernel path",   sys_open((const char *)0x00100000, O_RDONLY));

    /* sys_write pointing at kernel memory — must not disclose. */
    expect_fail("sys_write kernel buf",   sys_write(1, (const void *)0x00100000, 16));

    /* sys_read into a kernel buffer — must not scribble. */
    expect_fail("sys_read kernel dst",    sys_read(0, (void *)0x00100000, 16));

    /* sys_stat with a valid path but bad stat-out pointer. */
    expect_fail("sys_stat bad out",       sys_stat("/", (void *)0x00100000));

    /* sys_getcwd into kernel memory. */
    expect_fail("sys_getcwd kernel dst",  sys_getcwd((char *)0x00100000, 64));

    /* sys_mkdir with kernel path. */
    expect_fail("sys_mkdir kernel path",  sys_mkdir((const char *)0x00100000));

    /* sys_unlink likewise. */
    expect_fail("sys_unlink kernel path", sys_unlink((const char *)0x00100000));

    /* sys_readdir with kernel path. */
    char name[64];
    uint32_t type;
    expect_fail("sys_readdir kernel path", sys_readdir((const char *)0x00100000, 0, name, &type));

    /* Sanity: a valid call should still succeed. */
    int32_t rc = sys_stat("/", stat_buf);
    if (rc == 0) printf("PASS sys_stat valid path\n");
    else { printf("FAIL sys_stat valid path -> %d\n", rc); failures++; }

    if (failures == 0) puts("[test_badptr] ALL PASSED\n");
    else printf("[test_badptr] %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
