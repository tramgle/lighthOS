/* Minimal x86_64 shell — enough to run tests/*.vsh.
 *
 * Features:
 *   - `shell <script>` mode: one command per non-empty non-`#` line.
 *   - argv tokenization on whitespace, with `"..."` / `'...'` stripping.
 *   - Pipes: `a | b | c` — each stage runs in its own fork; stdin of
 *     the next stage is dup2'd from the previous pipe's read end.
 *   - Trailing `> file` or `>> file` redirects the last stage's stdout.
 *   - `exit`, `true`, `false` builtins.
 *
 * Deferred: input redirect (`<`), quoting escapes (`\"`), background
 * (`&`), env vars, multi-line, globbing.
 */

#include "ulib_x64.h"

#define MAX_ARGS   16
#define MAX_STAGES 8
#define LINE_MAX   512

struct stage { char *argv[MAX_ARGS]; int argc; };

/* Tokenize `line` in place. Splits on | to fill `stages`. Handles
   single/double quotes (no escapes inside). `>` / `>>` only makes
   sense at the end of the last stage — captured into redir_out /
   redir_path. Returns number of stages. */
static int parse_line(char *line, struct stage *stages, int max_stages,
                      int *redir_out, char **redir_path) {
    *redir_out = 0; *redir_path = 0;
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
        /* Token. Honor leading quote to include spaces. */
        char *tok;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            tok = p;
            while (*p && *p != q) p++;
            if (*p == q) { *p = 0; p++; }
        } else {
            tok = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '|' && *p != '>') p++;
            if (*p == '|' || *p == '>') { /* leave separator for next iter */
                char save = *p; *p = 0;
                /* Re-insert so the outer loop sees the separator. */
                if (save == '|' || save == '>') { /* restore */ *p = save; }
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
                        int redir_out, const char *redir_path) {
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

        /* Parent: close fds we don't need; keep read-end for next stage. */
        if (prev_read >= 0) sys_close(prev_read);
        if (!is_last) {
            sys_close(pfds[1]);
            prev_read = pfds[0];
        } else {
            prev_read = -1;
        }
        pids[npids++] = (int)pid;
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
    struct stage stages[MAX_STAGES];
    int redir_out = 0; char *redir_path = 0;
    int ns = parse_line(line, stages, MAX_STAGES, &redir_out, &redir_path);
    if (ns <= 0) return 0;
    if (ns == 1 && stages[0].argc == 0) return 0;

    /* Builtins — only meaningful in single-stage invocation. */
    if (ns == 1) {
        const char *cmd = stages[0].argv[0];
        if (u_strcmp(cmd, "exit") == 0) {
            sys_exit(stages[0].argc > 1 ? u_atoi(stages[0].argv[1]) : 0);
        }
        if (u_strcmp(cmd, "true") == 0)  return 0;
        if (u_strcmp(cmd, "false") == 0) return 1;
    }
    return run_pipeline(stages, ns, redir_out, redir_path);
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

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc >= 2) return run_script(argv[1]);
    u_puts_n("shell: interactive mode not supported yet\n");
    return 1;
}
