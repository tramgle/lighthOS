/* runtests [-k PATTERN] [-v] [dir]
 *
 * Walks `dir` (default /tests) and runs every script we recognize:
 *     *.vsh  via /bin/shell
 *     *.lua  via /bin/lua
 * Tallies PASS/FAIL assertion lines each script emits, reports a
 * summary, then shuts down. The host-side harness greps for
 * "=== Summary:" to decide success; `=== OK <name>` / `=== FAIL <name>`
 * prefixes feed the per-script regression gate.
 *
 * Flags:
 *   -k PATTERN   only run scripts whose filename contains PATTERN
 *                (substring match — not glob; keeps ulib lean)
 *   -v           extra chatter (noise from tests themselves is fine
 *                either way; flag is reserved for future use)
 *
 * Output shape:
 *   === RUN  <name>
 *   ... whatever the script prints ...
 *   === OK   <name> (Nt)     or     === FAIL <name> (Nt) status=C
 *   === Summary: P passed, F failed
 *
 * The (Nt) suffix after a script name is wall-clock ticks for that
 * script, computed from sys_times deltas. 1 tick = 10 ms. */

#include "ulib_x64.h"

#define TESTS_MAX      128
#define NAME_MAX       64

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

/* Substring match — returns 1 if `needle` appears in `hay`. */
static int substr(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i+j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
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

static int u_strcmp2(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* In-place insertion sort — N <= TESTS_MAX is small so O(N^2) is fine
 * and we avoid pulling qsort into ulib. */
static void sort_names(char names[][NAME_MAX], int n) {
    for (int i = 1; i < n; i++) {
        char key[NAME_MAX];
        int k = 0;
        while (names[i][k] && k < NAME_MAX - 1) { key[k] = names[i][k]; k++; }
        key[k] = 0;
        int j = i - 1;
        while (j >= 0 && u_strcmp2(names[j], key) > 0) {
            int m = 0;
            while (names[j][m] && m < NAME_MAX - 1) {
                names[j+1][m] = names[j][m]; m++;
            }
            names[j+1][m] = 0;
            j--;
        }
        int m = 0;
        while (key[m] && m < NAME_MAX - 1) { names[j+1][m] = key[m]; m++; }
        names[j+1][m] = 0;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    const char *pattern = 0;
    const char *dir = "/tests";
    for (int i = 1; i < argc; i++) {
        if (u_strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            pattern = argv[++i];
        } else if (u_strcmp(argv[i], "-v") == 0) {
            /* reserved */
        } else {
            dir = argv[i];
        }
    }

    /* Ensure /scratch exists; tests stash intermediate files there. */
    sys_mkdir("/scratch");

    /* Collect + filter + sort. readdir order is filesystem-dependent;
       an alphabetical run makes failures more predictable. */
    static char names[TESTS_MAX][NAME_MAX];
    int n = 0;

    struct readdir_out e;
    uint32_t idx = 0;
    while (_syscall3(SYS_READDIR, (long)(uintptr_t)dir, idx, (long)(uintptr_t)&e) == 0) {
        idx++;
        int kind = test_kind(e.name);
        if (!kind) continue;
        if (pattern && !substr(e.name, pattern)) continue;
        if (n >= TESTS_MAX) break;
        int k = 0;
        while (e.name[k] && k < NAME_MAX - 1) { names[n][k] = e.name[k]; k++; }
        names[n][k] = 0;
        n++;
    }
    sort_names(names, n);

    int passed = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        int kind = test_kind(names[i]);
        const char *runner = (kind == 1) ? "/bin/shell" : "/bin/lua";
        u_puts_n("=== RUN  "); u_puts_n(names[i]); u_putc('\n');

        struct tms t0, t1;
        long r0 = sys_times(&t0);
        int s = run_under(runner, dir, names[i]);
        long r1 = sys_times(&t1);
        long ticks = r1 - r0;

        if (s == 0) {
            u_puts_n("=== OK   ");
            u_puts_n(names[i]);
            u_puts_n(" (");   u_putdec(ticks); u_puts_n("t)\n");
            passed++;
        } else {
            u_puts_n("=== FAIL ");
            u_puts_n(names[i]);
            u_puts_n(" (");   u_putdec(ticks);
            u_puts_n("t) status="); u_putdec(s); u_putc('\n');
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
