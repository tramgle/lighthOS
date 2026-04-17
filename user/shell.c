/* Minimal x86_64 shell — enough to run tests/*.vsh.
 *
 * Features:
 *   - `shell <script>` mode: one command per non-empty non-`#` line.
 *   - argv tokenization on whitespace, with `"..."` / `'...'` stripping.
 *   - Pipes: `a | b | c` — each stage runs in its own fork; stdin of
 *     the next stage is dup2'd from the previous pipe's read end.
 *   - Trailing `> file` or `>> file` redirects the last stage's stdout.
 *   - Trailing `&` backgrounds the command (pid tracked for jobs/fg).
 *   - `jobs` and `fg` builtins; `exit`, `true`, `false`.
 *
 * Deferred: input redirect (`<`), quoting escapes, real `bg`,
 * env vars, globbing.
 */

#include "ulib_x64.h"

#define MAX_ARGS   16
#define MAX_STAGES 8
#define LINE_MAX   512
#define MAX_JOBS   8

struct stage { char *argv[MAX_ARGS]; int argc; };

struct job { int pid; int alive; char cmd[64]; };
static struct job jobs[MAX_JOBS];

static void job_add(int pid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].alive) {
            jobs[i].pid = pid;
            jobs[i].alive = 1;
            int n = 0;
            while (cmd[n] && n < (int)sizeof(jobs[i].cmd) - 1) {
                jobs[i].cmd[n] = cmd[n]; n++;
            }
            jobs[i].cmd[n] = 0;
            return;
        }
    }
}

static void jobs_builtin(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].alive) {
            u_puts_n("[");
            u_putdec(i + 1);
            u_puts_n("]  Running  ");
            u_puts_n(jobs[i].cmd);
            u_putc('\n');
        }
    }
}

static int fg_builtin(void) {
    /* Wait on the most-recently-added live job. */
    for (int i = MAX_JOBS - 1; i >= 0; i--) {
        if (jobs[i].alive) {
            int st = 0;
            sys_waitpid(jobs[i].pid, &st);
            jobs[i].alive = 0;
            u_puts_n(jobs[i].cmd); u_putc('\n');
            return st;
        }
    }
    return 0;
}

static int parse_line(char *line, struct stage *stages, int max_stages,
                      int *redir_out, char **redir_path, int *background) {
    *redir_out = 0; *redir_path = 0; *background = 0;
    int ns = 0;
    stages[0].argc = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '#') break;
        if (*p == '|') {
            p++;
            if (ns + 1 >= max_stages) return -1;
            stages[ns].argv[stages[ns].argc] = 0;
            ns++;
            stages[ns].argc = 0;
            continue;
        }
        if (*p == '&') {
            *background = 1;
            p++;
            continue;
        }
        if (*p == '>') {
            int mode = 1;
            p++;
            if (*p == '>') { mode = 2; p++; }
            while (*p == ' ' || *p == '\t') p++;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) { *p = 0; p++; }
            *redir_out = mode;
            *redir_path = start;
            continue;
        }
        char *tok;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            tok = p;
            while (*p && *p != q) p++;
            if (*p == q) { *p = 0; p++; }
        } else {
            tok = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '|' && *p != '>' && *p != '&') p++;
            if (*p == '|' || *p == '>' || *p == '&') {
                /* leave separator for next iter; null-terminate in place */
                char save = *p; *p = 0; *p = save;
            } else if (*p) {
                *p = 0; p++;
            }
        }
        if (stages[ns].argc < MAX_ARGS - 1) stages[ns].argv[stages[ns].argc++] = tok;
    }
    stages[ns].argv[stages[ns].argc] = 0;
    if (stages[0].argc == 0 && ns == 0) return 0;
    return ns + 1;
}

static void resolve_path(const char *name, char *out, size_t cap) {
    if (name[0] == '/') {
        size_t i = 0;
        while (name[i] && i < cap - 1) { out[i] = name[i]; i++; }
        out[i] = 0;
        return;
    }
    const char *pfx = "/bin/";
    size_t pi = 0;
    while (pfx[pi]) { out[pi] = pfx[pi]; pi++; }
    for (size_t j = 0; name[j] && pi < cap - 1; j++) out[pi++] = name[j];
    out[pi] = 0;
}

static int run_pipeline(struct stage *stages, int nstages,
                        int redir_out, const char *redir_path,
                        int background, const char *raw_line) {
    int prev_read = -1;
    int pids[MAX_STAGES];
    int npids = 0;

    for (int i = 0; i < nstages; i++) {
        int pfds[2] = {-1, -1};
        int is_last = (i == nstages - 1);
        if (!is_last) {
            if (sys_pipe(pfds) != 0) return 1;
        }

        long pid = sys_fork();
        if (pid == 0) {
            if (prev_read >= 0) {
                sys_dup2(prev_read, 0);
                sys_close(prev_read);
            }
            if (!is_last) {
                sys_dup2(pfds[1], 1);
                sys_close(pfds[0]);
                sys_close(pfds[1]);
            } else if (redir_out) {
                long flags = O_WRONLY | O_CREAT;
                flags |= (redir_out == 1) ? O_TRUNC : O_APPEND;
                int ofd = sys_open(redir_path, flags);
                if (ofd < 0) sys_exit(125);
                sys_dup2(ofd, 1);
                if (ofd != 1) sys_close(ofd);
            }
            char path[128];
            resolve_path(stages[i].argv[0], path, sizeof(path));
            sys_execve(path, stages[i].argv, 0);
            u_puts_n("shell: not found: "); u_puts_n(path); u_putc('\n');
            sys_exit(127);
        }
        if (pid < 0) return 1;
        if (prev_read >= 0) sys_close(prev_read);
        if (!is_last) { sys_close(pfds[1]); prev_read = pfds[0]; }
        else          prev_read = -1;
        pids[npids++] = (int)pid;
    }

    if (background) {
        /* Don't waitpid; register last stage as a job. */
        job_add(pids[npids - 1], raw_line);
        return 0;
    }
    int last_status = 0;
    for (int i = 0; i < npids; i++) {
        int st = 0;
        sys_waitpid(pids[i], &st);
        if (i == npids - 1) last_status = st;
    }
    return last_status;
}

static int run_line(char *line) {
    /* Save a copy of the raw line for job tracking before the
       parser rewrites in place. */
    char saved[LINE_MAX];
    int si = 0;
    for (; line[si] && si < LINE_MAX - 1; si++) saved[si] = line[si];
    saved[si] = 0;
    /* Trim trailing newline. */
    while (si > 0 && (saved[si-1] == '\n' || saved[si-1] == '\r' ||
                      saved[si-1] == ' '  || saved[si-1] == '\t')) {
        saved[--si] = 0;
    }

    struct stage stages[MAX_STAGES];
    int redir_out = 0; char *redir_path = 0;
    int background = 0;
    int ns = parse_line(line, stages, MAX_STAGES, &redir_out, &redir_path, &background);
    if (ns <= 0) return 0;
    if (ns == 1 && stages[0].argc == 0) return 0;

    if (ns == 1) {
        const char *cmd = stages[0].argv[0];
        if (u_strcmp(cmd, "exit") == 0) {
            sys_exit(stages[0].argc > 1 ? u_atoi(stages[0].argv[1]) : 0);
        }
        if (u_strcmp(cmd, "true") == 0)  return 0;
        if (u_strcmp(cmd, "false") == 0) return 1;
        if (u_strcmp(cmd, "jobs") == 0) {
            /* `jobs` with redirect: fork so we can redirect stdout. */
            if (redir_out) {
                long pid = sys_fork();
                if (pid == 0) {
                    long flags = O_WRONLY | O_CREAT;
                    flags |= (redir_out == 1) ? O_TRUNC : O_APPEND;
                    int ofd = sys_open(redir_path, flags);
                    if (ofd < 0) sys_exit(125);
                    sys_dup2(ofd, 1);
                    if (ofd != 1) sys_close(ofd);
                    jobs_builtin();
                    sys_exit(0);
                }
                int st = 0; sys_waitpid((int)pid, &st);
                return st;
            }
            jobs_builtin();
            return 0;
        }
        if (u_strcmp(cmd, "fg") == 0) {
            /* fg's waitpid + jobs[].alive clear MUST happen in the
               shell process itself so subsequent `jobs` sees the
               reaped state. Only the output is redirected. */
            int saved_stdout = -1;
            if (redir_out) {
                long flags = O_WRONLY | O_CREAT;
                flags |= (redir_out == 1) ? O_TRUNC : O_APPEND;
                int ofd = sys_open(redir_path, flags);
                if (ofd < 0) return 1;
                saved_stdout = 9;              /* unused high fd */
                sys_dup2(1, saved_stdout);
                sys_dup2(ofd, 1);
                if (ofd != 1) sys_close(ofd);
            }
            int rc = fg_builtin();
            if (saved_stdout != -1) {
                sys_dup2(saved_stdout, 1);
                sys_close(saved_stdout);
            }
            return rc;
        }
    }
    return run_pipeline(stages, ns, redir_out, redir_path, background, saved);
}

static int run_script(const char *path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { u_puts_n("shell: cannot open "); u_puts_n(path); u_putc('\n'); return 1; }
    char line[LINE_MAX];
    int any_fail = 0;
    for (;;) {
        long n = u_readline(fd, line, sizeof(line));
        if (n <= 0) break;
        if (run_line(line) != 0) any_fail = 1;
    }
    sys_close(fd);
    return any_fail;
}

static int run_interactive(void) {
    char line[LINE_MAX];
    for (;;) {
        u_puts_n("$ ");
        long n = u_readline(0, line, sizeof(line));
        if (n <= 0) return 0;          /* EOF or read error */
        run_line(line);
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp; (void)argv;
    if (argc >= 2) return run_script(argv[1]);
    return run_interactive();
}
