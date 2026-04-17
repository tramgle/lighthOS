/* runtests [dir] — walk `dir` (default /tests) and run every script
 * we recognize: *.vsh via /bin/shell, *.lua via /bin/lua. Tally the
 * PASS/FAIL assertion lines each script emits, report a summary,
 * shutdown.
 *
 * The host-side harness greps for "=== Summary:" to decide success.
 * PASS/FAIL lines for individual assertions go through stdout from
 * whichever runner the test is using (assert binary for .vsh;
 * tests/lua/testlib.lua for .lua). Pass/fail at the script level
 * here is derived from the child's exit code.
 */

#include "ulib_x64.h"

struct readdir_out {
    char name[64];
    uint32_t type;
};

/* Returns 1 if `name` ends with `.vsh`, 2 if `.lua`, 0 otherwise. */
static int test_kind(const char *name) {
    int n = 0; while (name[n]) n++;
    if (n >= 5 && name[n-4] == '.' && name[n-3] == 'v' &&
        name[n-2] == 's' && name[n-1] == 'h') return 1;
    if (n >= 5 && name[n-4] == '.' && name[n-3] == 'l' &&
        name[n-2] == 'u' && name[n-1] == 'a') return 2;
    return 0;
}

static int run_under(const char *runner, const char *dir, const char *name) {
    char path[128];
    int p = 0;
    while (dir[p] && p < (int)sizeof(path) - 1) { path[p] = dir[p]; p++; }
    if (p > 0 && path[p-1] != '/' && p < (int)sizeof(path) - 1) path[p++] = '/';
    for (int i = 0; name[i] && p < (int)sizeof(path) - 1; i++) path[p++] = name[i];
    path[p] = 0;

    char runner_base[32];
    const char *b = runner;
    for (const char *s = runner; *s; s++) if (*s == '/') b = s + 1;
    int bi = 0;
    while (b[bi] && bi < (int)sizeof(runner_base) - 1) {
        runner_base[bi] = b[bi]; bi++;
    }
    runner_base[bi] = 0;

    char *argv[] = { runner_base, path, 0 };
    long pid = sys_fork();
    if (pid == 0) { sys_execve(runner, argv, 0); sys_exit(127); }
    if (pid < 0) return -1;
    int status = 0;
    sys_waitpid((int)pid, &status);
    return status;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *dir = (argc > 1) ? argv[1] : "/tests";

    /* Ensure /scratch exists; tests stash intermediate files there. */
    sys_mkdir("/scratch");

    int passed = 0, failed = 0;
    struct readdir_out e;
    uint32_t idx = 0;
    while (_syscall3(SYS_READDIR, (long)(uintptr_t)dir, idx, (long)(uintptr_t)&e) == 0) {
        idx++;
        int kind = test_kind(e.name);
        if (!kind) continue;
        const char *runner = (kind == 1) ? "/bin/shell" : "/bin/lua";
        u_puts_n("=== RUN "); u_puts_n(e.name); u_putc('\n');
        int s = run_under(runner, dir, e.name);
        if (s == 0) {
            u_puts_n("=== OK  "); u_puts_n(e.name); u_putc('\n');
            passed++;
        } else {
            u_puts_n("=== FAIL "); u_puts_n(e.name);
            u_puts_n(" status="); u_putdec(s); u_putc('\n');
            failed++;
        }
    }

    u_puts_n("=== Summary: ");
    u_putdec(passed);
    u_puts_n(" passed, ");
    u_putdec(failed);
    u_puts_n(" failed\n");
    sys_shutdown();
    return failed ? 1 : 0;
}
