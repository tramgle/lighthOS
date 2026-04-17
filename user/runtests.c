/* runtests [dir] — walk `dir` (default /tests) for *.vsh files,
 * spawn /bin/shell on each, tally the PASS/FAIL assertion lines
 * emitted by `assert` in the scripts, then sys_shutdown.
 *
 * The host-side harness greps for "=== Summary:" to decide success.
 * Each script writes its own PASS/FAIL lines directly to stdout;
 * runtests doesn't intercept shell's output (no pipes yet), so the
 * summary is derived from shell exit codes: 0 = all asserts passed,
 * non-zero = at least one FAILed.
 */

#include "ulib_x64.h"

struct readdir_out {
    char name[64];
    uint32_t type;
};

static int has_vsh(const char *name) {
    int n = 0; while (name[n]) n++;
    if (n < 5) return 0;
    return name[n-4] == '.' && name[n-3] == 'v' &&
           name[n-2] == 's' && name[n-1] == 'h';
}

static int run_script(const char *dir, const char *name) {
    char path[128];
    int p = 0;
    while (dir[p] && p < (int)sizeof(path) - 1) { path[p] = dir[p]; p++; }
    if (p > 0 && path[p-1] != '/' && p < (int)sizeof(path) - 1) path[p++] = '/';
    for (int i = 0; name[i] && p < (int)sizeof(path) - 1; i++) path[p++] = name[i];
    path[p] = 0;

    char *argv[] = { (char *)"shell", path, 0 };
    long pid = sys_fork();
    if (pid == 0) { sys_execve("/bin/shell", argv, 0); sys_exit(127); }
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
        if (!has_vsh(e.name)) continue;
        u_puts_n("=== RUN "); u_puts_n(e.name); u_putc('\n');
        int s = run_script(dir, e.name);
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
