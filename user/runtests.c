/* Minimal x86_64 test harness.
 *
 * Spawns each of a hardcoded list of test binaries (each lives at
 * /bin/<name>), waits for it, reports PASS if it exited 0 or FAIL
 * with the exit code otherwise. Prints a summary line the host
 * harness greps for, then calls sys_shutdown.
 *
 * Tests exit 0 for success; any non-zero exit counts as a fail.
 */

#include "syscall_x64.h"

static void put_dec(long v) {
    char b[24]; int i = 0;
    if (v == 0) { uputs("0"); return; }
    if (v < 0) { uputs("-"); v = -v; }
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c = b[--i]; sys_write(1, &c, 1); }
}

static int run_one(const char *name) {
    char path[64] = "/bin/";
    int plen = 5;
    for (int i = 0; name[i] && plen < (int)sizeof(path) - 1; i++) path[plen++] = name[i];
    path[plen] = 0;

    long pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { (char *)name, 0 };
        sys_execve(path, argv, 0);
        /* execve returned — it failed; exit with a marker. */
        sys_exit(127);
    }
    if (pid < 0) return -1;

    int status = 0;
    sys_waitpid((int)pid, &status);
    return status;
}

static const char *tests[] = {
    "test_pid",
    "test_fork",
    "test_fs",
    "test_stream",
    0,
};

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    int passed = 0, failed = 0;
    for (int i = 0; tests[i]; i++) {
        uputs("RUN "); uputs(tests[i]); uputs("\n");
        int s = run_one(tests[i]);
        if (s == 0) {
            uputs("PASS "); uputs(tests[i]); uputs("\n");
            passed++;
        } else {
            uputs("FAIL "); uputs(tests[i]);
            uputs(" status="); put_dec(s); uputs("\n");
            failed++;
        }
    }

    uputs("=== Summary: ");
    put_dec(passed);
    uputs(" passed, ");
    put_dec(failed);
    uputs(" failed\n");

    sys_shutdown();
    return failed ? 1 : 0;
}
