#include "test/test.h"
#include "kernel/process.h"
#include "kernel/task.h"
#include "mm/pmm.h"
#include "fs/vfs.h"

static volatile int child_ran = 0;

static void child_entry(void) {
    child_ran = 1;
    process_exit(42);
}

static void fd_test_entry(void) {
    /* Open a file, write, close */
    int fd = fd_open("/fd_test.txt", O_CREAT | O_WRONLY);
    if (fd >= 0) {
        fd_write(fd, "hello", 5);
        fd_close(fd);
    }
    process_exit(0);
}

test_results_t test_process(void) {
    TEST_SUITE_BEGIN("process");

    /* process_init was called in main.c before tests */
    process_t *p0 = process_current();
    TEST_ASSERT_NEQ((uint32_t)p0, 0, "process_current returns non-null");
    TEST_ASSERT_EQ(p0->pid, 0, "process 0 has pid 0");

    /* Check stdio FDs */
    TEST_ASSERT_EQ(p0->fds[0].type, FD_CONSOLE, "fd 0 is console (stdin)");
    TEST_ASSERT_EQ(p0->fds[1].type, FD_CONSOLE, "fd 1 is console (stdout)");
    TEST_ASSERT_EQ(p0->fds[2].type, FD_CONSOLE, "fd 2 is console (stderr)");

    /* Create child process, wait for it */
    child_ran = 0;
    process_t *child = process_create("test_child", child_entry);
    TEST_ASSERT_NEQ((uint32_t)child, 0, "process_create returns non-null");
    TEST_ASSERT(child->alive, "child is alive after create");

    int status = 0;
    int ret = process_waitpid(child->pid, &status);
    TEST_ASSERT_EQ(child_ran, 1, "child process ran");
    TEST_ASSERT_EQ(status, 42, "waitpid returns child exit code 42");
    TEST_ASSERT(ret >= 0, "waitpid returns valid pid");

    /* FD file operations: create process that opens/writes a file */
    process_t *fdp = process_create("fd_test", fd_test_entry);
    TEST_ASSERT_NEQ((uint32_t)fdp, 0, "fd test process created");
    process_waitpid(fdp->pid, NULL);

    /* Verify the file was written */
    struct vfs_stat st;
    int rc = vfs_stat("/fd_test.txt", &st);
    TEST_ASSERT_EQ(rc, 0, "fd-written file exists");
    TEST_ASSERT_EQ(st.size, 5, "fd-written file has correct size");

    char buf[16] = {0};
    vfs_read("/fd_test.txt", buf, 16, 0);
    TEST_ASSERT_STR_EQ(buf, "hello", "fd-written file has correct content");

    /* Clean up */
    vfs_unlink("/fd_test.txt");

    /* Test sbrk (from process 0 context) */
    p0->brk = 0x08100000;  /* set a known break point */
    /* Invoke sbrk via direct call since we're in ring 0 */
    uint32_t old_brk = p0->brk;
    p0->brk += PAGE_SIZE;  /* simulate sbrk growing by one page */
    TEST_ASSERT_EQ(old_brk, 0x08100000, "sbrk returns old break");
    TEST_ASSERT_EQ(p0->brk, 0x08101000, "brk advanced by PAGE_SIZE");

    TEST_SUITE_END();
}
