/* runtests [<dir>]: walk dir for *.vsh files, spawn /bin/shell on
   each with stdout captured. Forward captured output to our own
   stdout AND scan lines for PASS / FAIL prefixes for a tally. Default
   dir is /tests. Exit code is non-zero if any FAIL was seen. */

#include "syscall.h"
#include "ulib.h"

static int has_vsh_suffix(const char *name) {
    int n = 0;
    while (name[n]) n++;
    if (n < 5) return 0;  /* need at least "a.vsh" */
    return name[n - 4] == '.' && name[n - 3] == 'v' &&
           name[n - 2] == 's' && name[n - 1] == 'h';
}

static void join(char *out, int cap, const char *a, const char *b) {
    int oi = 0;
    int need_sep = 1;
    while (*a && oi < cap - 1) {
        out[oi++] = *a;
        need_sep = (*a != '/');
        a++;
    }
    if (need_sep && oi < cap - 1) out[oi++] = '/';
    while (*b && oi < cap - 1) out[oi++] = *b++;
    out[oi] = '\0';
}

static int line_starts_with(const char *line, int len, const char *pfx) {
    int i = 0;
    while (pfx[i]) {
        if (i >= len || line[i] != pfx[i]) return 0;
        i++;
    }
    return 1;
}

/* Run one test. Returns 1 on any FAIL line in its output, 0 otherwise.
   Also updates *pass_out / *fail_out with per-line counts. */
static int run_one(const char *path, int *pass_out, int *fail_out) {
    int pipe_fds[2];
    if (sys_pipe(pipe_fds) < 0) { puts("runtests: pipe() failed\n"); return 1; }

    /* Save our stdout, install pipe write end, spawn, restore. */
    int saved_out = sys_dup2(1, 12);
    sys_dup2(pipe_fds[1], 1);

    char *child_argv[] = { "/bin/shell", (char *)path, 0 };
    int pid = sys_spawn("/bin/shell", child_argv);

    sys_dup2(saved_out, 1); sys_close(saved_out);
    sys_close(pipe_fds[1]);

    if (pid < 0) {
        sys_close(pipe_fds[0]);
        printf("runtests: cannot spawn shell for %s\n", path);
        return 1;
    }

    /* Drain the pipe: echo to our real stdout, scan for PASS/FAIL. */
    char buf[1024];
    char line[512];
    int line_len = 0;
    int saw_fail = 0;
    int32_t n;
    while ((n = sys_read(pipe_fds[0], buf, sizeof buf)) > 0) {
        sys_write(1, buf, n);
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (line_starts_with(line, line_len, "PASS")) (*pass_out)++;
                else if (line_starts_with(line, line_len, "FAIL")) {
                    (*fail_out)++; saw_fail = 1;
                }
                line_len = 0;
            } else if (line_len < (int)sizeof line - 1) {
                line[line_len++] = c;
            }
        }
    }
    sys_close(pipe_fds[0]);
    sys_waitpid(pid, 0);
    return saw_fail;
}

int main(int argc, char **argv) {
    const char *dir = (argc >= 2) ? argv[1] : "/tests";

    /* Ensure /scratch exists for tests to use. Ignore "already
       exists" — tests will complain about real problems. */
    sys_mkdir("/scratch");

    int tests = 0, pass = 0, fail = 0;
    char name[VFS_MAX_NAME];
    uint32_t type;
    for (uint32_t idx = 0; sys_readdir(dir, idx, name, &type) == 0; idx++) {
        if (name[0] == '.') continue;
        if (type != VFS_FILE) continue;
        if (!has_vsh_suffix(name)) continue;

        char path[VFS_MAX_PATH];
        join(path, sizeof path, dir, name);

        printf("=== %s ===\n", name);
        tests++;
        run_one(path, &pass, &fail);
    }

    printf("=== Summary: %d tests, %d PASS, %d FAIL ===\n", tests, pass, fail);
    return fail > 0 ? 1 : 0;
}
