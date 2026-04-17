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
 * Not yet: quoting escapes (`\"`, `\\`), globbing (`*`, `?`).
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
    /* Resume the most-recently-added live job in the foreground:
       hand it the terminal, SIGCONT the whole pgid, wait for it to
       finish OR stop again. Ctrl-Z during fg drops us back to the
       prompt with the job re-stopped; normal exit reaps and frees
       the slot. */
    for (int i = MAX_JOBS - 1; i >= 0; i--) {
        if (!jobs[i].alive) continue;
        int pid = jobs[i].pid;
        u_puts_n(jobs[i].cmd); u_putc('\n');
        sys_tcsetpgrp(pid);                /* job's pgid leader == pid */
        sys_kill(-pid, SIG_CONT);          /* negative pid = whole pgid */
        int st = 0;
        sys_waitpid(pid, &st);
        sys_tcsetpgrp((int)sys_getpid());
        if (st & 0x100) {
            /* Stopped again — keep the job entry live so another
               fg/bg can pick it up. Print a banner like Ctrl-Z did. */
            u_puts_n("\n[stopped] "); u_puts_n(jobs[i].cmd); u_putc('\n');
            return st;
        }
        jobs[i].alive = 0;
        return st;
    }
    return 0;
}

static int parse_line(char *line, struct stage *stages, int max_stages,
                      int *redir_out, char **redir_path,
                      char **redir_in, int *background) {
    *redir_out = 0; *redir_path = 0; *redir_in = 0; *background = 0;
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
        if (*p == '<') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) { *p = 0; p++; }
            *redir_in = start;
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
                   *p != '|' && *p != '>' && *p != '<' && *p != '&') p++;
            if (*p == '|' || *p == '>' || *p == '<' || *p == '&') {
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
                        const char *redir_in,
                        int background, const char *raw_line) {
    int prev_read = -1;
    int pids[MAX_STAGES];
    int npids = 0;
    /* Every foreground pipeline runs in a fresh process group so
       Ctrl-C / Ctrl-Z delivered to the terminal's fg pgid hit the
       pipeline (not the shell). The first stage becomes the pgid
       leader; later stages join it. */
    int pgid_leader = 0;

    for (int i = 0; i < nstages; i++) {
        int pfds[2] = {-1, -1};
        int is_last = (i == nstages - 1);
        if (!is_last) {
            if (sys_pipe(pfds) != 0) return 1;
        }

        long pid = sys_fork();
        if (pid == 0) {
            /* Child: join the pipeline's pgid. For the first stage
               pgid_leader is 0 → setpgid(0,0) makes it its own group
               leader. Later stages setpgid(0, pgid_leader). */
            sys_setpgid(0, pgid_leader);
            if (prev_read >= 0) {
                sys_dup2(prev_read, 0);
                sys_close(prev_read);
            } else if (i == 0 && redir_in) {
                /* First stage gets its stdin from `< file`. */
                int ifd = sys_open(redir_in, O_RDONLY);
                if (ifd < 0) sys_exit(126);
                sys_dup2(ifd, 0);
                if (ifd != 0) sys_close(ifd);
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
            /* Foreground children get default SIG_INT back — shell
               has it ignored; children shouldn't inherit that. */
            if (!background) sys_signal(SIG_INT, SIG_DFL);
            char path[128];
            resolve_path(stages[i].argv[0], path, sizeof(path));
            sys_execve(path, stages[i].argv, 0);
            u_puts_n("shell: not found: "); u_puts_n(path); u_putc('\n');
            sys_exit(127);
        }
        if (pid < 0) {
            u_puts_n("shell: fork failed (process table full?)\n");
            return 1;
        }
        /* Parent also calls setpgid to close the race (child may not
           have run yet). First stage: pid is the leader. */
        if (i == 0) pgid_leader = (int)pid;
        sys_setpgid((int)pid, pgid_leader);
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
    /* Foreground: hand the terminal to the pipeline for the duration
       of the wait. Reclaim it on exit so the next prompt gets our
       signals again. */
    sys_tcsetpgrp(pgid_leader);
    int last_status = 0;
    int stopped = 0;
    for (int i = 0; i < npids; i++) {
        int st = 0;
        sys_waitpid(pids[i], &st);
        /* Stop-status sentinel: bit 0x100 set. If the first stage
           we were waiting on got Ctrl-Z'd, stop waiting for the
           rest of the pipeline — they'll either follow it down or
           we'll pick them up later via fg/bg. */
        if (st & 0x100) {
            stopped = 1;
            last_status = st;
            break;
        }
        if (i == npids - 1) last_status = st;
    }
    sys_tcsetpgrp((int)sys_getpid());
    if (stopped) {
        /* Register the pipeline leader as a job so fg/bg can find
           it. Print a newline + status so the next prompt lands on
           a fresh line (the ^Z byte we swallowed in the kernel
           never echoed). */
        job_add(pids[0], raw_line);
        u_puts_n("\n[stopped] "); u_puts_n(raw_line); u_putc('\n');
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
    char *redir_in = 0;
    int background = 0;
    int ns = parse_line(line, stages, MAX_STAGES, &redir_out, &redir_path,
                        &redir_in, &background);
    if (ns <= 0) return 0;
    if (ns == 1 && stages[0].argc == 0) return 0;

    if (ns == 1) {
        const char *cmd = stages[0].argv[0];
        if (u_strcmp(cmd, "exit") == 0) {
            sys_exit(stages[0].argc > 1 ? u_atoi(stages[0].argv[1]) : 0);
        }
        if (u_strcmp(cmd, "true") == 0)  return 0;
        if (u_strcmp(cmd, "false") == 0) return 1;
        if (u_strcmp(cmd, "cd") == 0) {
            const char *dest = (stages[0].argc > 1) ? stages[0].argv[1] : "/";
            if (sys_chdir(dest) != 0) {
                u_puts_n("cd: "); u_puts_n(dest); u_puts_n(": not a directory\n");
                return 1;
            }
            return 0;
        }
        if (u_strcmp(cmd, "pwd") == 0) {
            char buf[256];
            long n = sys_getcwd(buf, sizeof buf);
            if (n < 0) { u_puts_n("pwd: failed\n"); return 1; }
            u_puts_n(buf); u_putc('\n');
            return 0;
        }
        if (u_strcmp(cmd, "clear") == 0) {
            u_puts_n("\033[2J\033[H");
            return 0;
        }
        if (u_strcmp(cmd, "help") == 0) {
            u_puts_n(
                "Built-ins: cd pwd exit true false jobs fg bg clear help\n"
                "Programs:  see /bin — e.g. ls, cat, echo, grep, vi, ps, free,\n"
                "           lsblk, find, strace, lua, mount, umount, chroot.\n"
                "Pipes:   a | b | c   redirect: > file  >> file   bg: &\n"
                "History: up/down arrows   Tab: complete /bin name\n");
            return 0;
        }
        if (u_strcmp(cmd, "bg") == 0) {
            /* Resume the most recent stopped job in the background.
               jobs[i].pid is the pgid leader, so `kill(-pid, SIGCONT)`
               wakes every stage of the pipeline. No tcsetpgrp — the
               job stays in its own pgid but the shell keeps the
               terminal. */
            for (int i = MAX_JOBS - 1; i >= 0; i--) {
                if (jobs[i].alive) {
                    sys_kill(-jobs[i].pid, SIG_CONT);
                    u_puts_n("[bg] "); u_puts_n(jobs[i].cmd); u_putc('\n');
                    return 0;
                }
            }
            u_puts_n("bg: no jobs\n");
            return 1;
        }
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
    return run_pipeline(stages, ns, redir_out, redir_path, redir_in,
                        background, saved);
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

/* ---- Line editor with history + tab completion ------------------ */

/* Keystroke bytes emitted by the kernel's serial CSI decoder.
   Must match src/drivers/keyboard.h. */
#define K_UP     0x81
#define K_DOWN   0x82
#define K_LEFT   0x83
#define K_RIGHT  0x84
#define K_HOME   0x85
#define K_END    0x86
#define K_CUP    0x91
#define K_CDOWN  0x92
#define K_CLEFT  0x93
#define K_CRIGHT 0x94
#define K_DEL    0x95

#define HIST_MAX 32
static char hist[HIST_MAX][LINE_MAX];
static int  hist_count;          /* number of valid entries (0..HIST_MAX) */
static int  hist_head;           /* next write slot */

static void hist_push(const char *line) {
    /* Skip empty + duplicate-of-last. */
    if (!line[0]) return;
    int last = (hist_head - 1 + HIST_MAX) % HIST_MAX;
    if (hist_count > 0 && u_strcmp(hist[last], line) == 0) return;
    int i = 0;
    while (line[i] && i < LINE_MAX - 1) { hist[hist_head][i] = line[i]; i++; }
    hist[hist_head][i] = 0;
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
}

/* Return the i'th most recent entry (i=0 = most recent); 0 if out of range. */
static const char *hist_get(int i) {
    if (i < 0 || i >= hist_count) return 0;
    int slot = (hist_head - 1 - i + HIST_MAX * 2) % HIST_MAX;
    return hist[slot];
}

/* Redraw the editable region assuming the cursor was at `old_pos`
   characters past the start of input, the old buffer was `old_len`
   wide, and the new buffer is `new_len` wide. Positions the cursor
   at `new_pos` on exit. Uses only \b and ESC[K so it works on the
   most minimal terminal. */
static void repaint_at(const char *buf, int old_pos, int old_len,
                       int new_len, int new_pos) {
    /* Cursor back to start of input. */
    for (int i = 0; i < old_pos; i++) u_putc('\b');
    /* Write the new buffer. */
    for (int i = 0; i < new_len; i++) u_putc(buf[i]);
    /* Erase tail if old was longer. */
    if (old_len > new_len) {
        int tail = old_len - new_len;
        for (int i = 0; i < tail; i++) u_putc(' ');
        for (int i = 0; i < tail; i++) u_putc('\b');
    }
    /* Cursor back to new_pos. */
    for (int i = new_len; i > new_pos; i--) u_putc('\b');
}

/* Convenience: backward-compat wrapper for callers that used to
   repaint the whole editable region (cursor is always at end). */
static void repaint(const char *buf, int old_len, int new_len) {
    repaint_at(buf, old_len, old_len, new_len, new_len);
}

/* Word-boundary helpers. A "word" is a run of non-whitespace chars. */
static int word_left(const char *buf, int pos) {
    if (pos <= 0) return 0;
    int i = pos - 1;
    while (i > 0 && (buf[i] == ' ' || buf[i] == '\t')) i--;
    while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') i--;
    return i;
}
static int word_right(const char *buf, int pos, int len) {
    int i = pos;
    while (i < len && buf[i] != ' ' && buf[i] != '\t') i++;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
    return i;
}

/* Find the start of the last whitespace-delimited token in `buf[..len]`.
   Returns index into buf. */
static int last_token_start(const char *buf, int len) {
    int i = len;
    while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') i--;
    return i;
}

/* Tab completion: scan /bin for entries that start with the prefix
   and, if exactly one matches, append the rest into `buf`. If
   multiple match, print the list on a fresh line and redraw. */
struct readdir_ent { char name[64]; uint32_t type; };

static int tab_complete(char *buf, int *len) {
    int start = last_token_start(buf, *len);
    const char *pref = buf + start;
    int pref_len = *len - start;
    if (pref_len == 0) return 0;

    struct readdir_ent e;
    char matches[8][64];
    int  nmatches = 0;
    for (uint32_t idx = 0;
         nmatches < 8 &&
         _syscall3(SYS_READDIR, (long)(uintptr_t)"/bin", idx,
                   (long)(uintptr_t)&e) == 0;
         idx++) {
        int ok = 1;
        for (int i = 0; i < pref_len; i++) {
            if (e.name[i] != pref[i]) { ok = 0; break; }
        }
        if (!ok) continue;
        int n = 0;
        while (e.name[n] && n < 63) { matches[nmatches][n] = e.name[n]; n++; }
        matches[nmatches][n] = 0;
        nmatches++;
    }

    if (nmatches == 0) return 0;
    if (nmatches == 1) {
        /* Append the tail plus a space. */
        const char *m = matches[0];
        int i = pref_len;
        while (m[i] && *len < LINE_MAX - 2) {
            buf[(*len)++] = m[i];
            u_putc(m[i]);
            i++;
        }
        if (*len < LINE_MAX - 1) { buf[(*len)++] = ' '; u_putc(' '); }
        return 1;
    }
    /* Multiple: list them, redraw prompt+buffer. Explicit \r\n because
       the serial driver is in raw mode during readline — no \n→\r\n
       translation anymore. */
    u_putc('\r'); u_putc('\n');
    for (int i = 0; i < nmatches; i++) {
        u_puts_n(matches[i]); u_putc('\t');
    }
    u_putc('\r'); u_putc('\n');
    u_puts_n("$ ");
    for (int i = 0; i < *len; i++) u_putc(buf[i]);
    return 1;
}

/* Read a line with history + tab completion + line editing. The
   serial driver is in raw mode for the duration (see run_interactive),
   so we own the cursor and do all our own echo. Returns length (not
   counting NUL), 0 on EOF, -1 on read error. */
static long shell_readline(char *buf, int cap) {
    int len = 0;
    int pos = 0;                /* cursor index into buf */
    int hist_pos = -1;          /* -1 = live buffer; 0..n = history entry */
    char saved[LINE_MAX];       /* snapshot of live buffer when walking history */
    int  saved_len = 0;

    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);
        if (n < 0) return -1;
        if (n == 0) return 0;

        unsigned char uc = (unsigned char)c;

        /* Enter: commit the line. Print the newline ourselves (kernel
           doesn't in raw mode) and return. */
        if (uc == '\n' || uc == '\r') {
            u_putc('\r'); u_putc('\n');
            buf[len] = 0;
            return len;
        }

        /* Backspace / Ctrl-H: delete char before cursor. */
        if (uc == '\b' || uc == 0x7F) {
            if (pos > 0) {
                int old_len = len;
                for (int i = pos - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                repaint_at(buf, pos, old_len, len, pos - 1);
                pos--;
            }
            continue;
        }

        /* Delete: remove char under cursor. */
        if (uc == K_DEL) {
            if (pos < len) {
                int old_len = len;
                for (int i = pos; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                repaint_at(buf, pos, old_len, len, pos);
            }
            continue;
        }

        /* Ctrl-A / Home. */
        if (uc == K_HOME || uc == 0x01) {
            while (pos > 0) { u_putc('\b'); pos--; }
            continue;
        }
        /* Ctrl-E / End. */
        if (uc == K_END || uc == 0x05) {
            while (pos < len) { u_putc(buf[pos]); pos++; }
            continue;
        }
        /* Ctrl-U: kill whole line. */
        if (uc == 0x15) {
            int old_len = len;
            len = 0;
            repaint_at(buf, pos, old_len, 0, 0);
            pos = 0;
            continue;
        }
        /* Ctrl-K: kill to EOL. */
        if (uc == 0x0B) {
            int old_len = len;
            len = pos;
            repaint_at(buf, pos, old_len, len, pos);
            continue;
        }

        if (uc == K_LEFT) {
            if (pos > 0) { u_putc('\b'); pos--; }
            continue;
        }
        if (uc == K_RIGHT) {
            if (pos < len) { u_putc(buf[pos]); pos++; }
            continue;
        }

        /* Word navigation: Ctrl-Left / Ctrl-Right (modifier 5). */
        if (uc == K_CLEFT) {
            int target = word_left(buf, pos);
            while (pos > target) { u_putc('\b'); pos--; }
            continue;
        }
        if (uc == K_CRIGHT) {
            int target = word_right(buf, pos, len);
            while (pos < target) { u_putc(buf[pos]); pos++; }
            continue;
        }

        if (uc == '\t') {
            /* Tab only operates at end-of-line for now — move cursor
               there, then let the existing completer run. */
            while (pos < len) { u_putc(buf[pos]); pos++; }
            tab_complete(buf, &len);
            pos = len;
            continue;
        }

        if (uc == K_UP || uc == K_DOWN) {
            int new_hist = hist_pos + (uc == K_UP ? 1 : -1);
            if (new_hist < -1 || new_hist >= hist_count) continue;
            if (hist_pos == -1) {
                for (int i = 0; i < len; i++) saved[i] = buf[i];
                saved_len = len;
            }
            hist_pos = new_hist;
            int old_len = len;
            int old_pos = pos;
            if (hist_pos == -1) {
                for (int i = 0; i < saved_len; i++) buf[i] = saved[i];
                len = saved_len;
            } else {
                const char *h = hist_get(hist_pos);
                int i = 0;
                while (h[i] && i < cap - 1) { buf[i] = h[i]; i++; }
                len = i;
            }
            pos = len;
            repaint_at(buf, old_pos, old_len, len, pos);
            continue;
        }

        /* Drop remaining control / extended bytes. */
        if (uc < 0x20 || uc >= 0x80) continue;

        /* Printable: insert at cursor, echo the change. */
        if (len < cap - 1) {
            for (int i = len; i > pos; i--) buf[i] = buf[i - 1];
            buf[pos] = (char)uc;
            len++;
            if (pos == len - 1) {
                u_putc(uc);
            } else {
                /* Mid-line insert: rewrite from pos to end, then
                   restore cursor. */
                int old_len = len - 1;
                repaint_at(buf, pos, old_len, len, pos + 1);
            }
            pos++;
        }
    }
}

/* --- $VAR / ${VAR} expansion. Writes the expanded copy into `out`
   (cap bytes). Returns 0 on success, -1 on overflow. */
static int expand_vars(const char *in, char *out, int cap) {
    int oi = 0;
    for (int i = 0; in[i]; i++) {
        if (in[i] != '$') {
            if (oi >= cap - 1) return -1;
            out[oi++] = in[i];
            continue;
        }
        /* Parse name: {NAME} or bare alnum+underscore. */
        i++;
        char name[64];
        int ni = 0;
        int braced = (in[i] == '{');
        if (braced) i++;
        while (in[i] &&
               ((in[i] >= 'A' && in[i] <= 'Z') ||
                (in[i] >= 'a' && in[i] <= 'z') ||
                (in[i] >= '0' && in[i] <= '9') ||
                in[i] == '_')) {
            if (ni < 63) name[ni++] = in[i];
            i++;
        }
        name[ni] = 0;
        if (braced && in[i] == '}') i++;
        else i--;   /* step back; the outer loop will re-read this byte */
        extern char *getenv(const char *);
        const char *val = getenv(name);
        if (!val) continue;
        while (*val) {
            if (oi >= cap - 1) return -1;
            out[oi++] = *val++;
        }
    }
    out[oi] = 0;
    return 0;
}

/* Wait up to `yields` scheduler rounds for a byte to land on stdin,
   then read one byte. Returns 1 on success, 0 on timeout. Keeps the
   shell from hanging forever when the terminal doesn't answer
   CSI-6n (dumb console, redirected stdin, etc.). */
static int poll_read1(char *out, int yields) {
    for (int i = 0; i < yields && !sys_tty_poll(); i++) sys_yield();
    if (!sys_tty_poll()) return 0;
    return sys_read(0, out, 1) == 1;
}

/* Probe the host terminal via CSI-6n. On success, cache the size in
   the kernel (sys_tty_setsize) and export LINES/COLUMNS so children
   pick them up via getenv. */
static void probe_winsize(void) {
    sys_tty_raw(1);
    sys_write(1, "\033[s\033[999;999H\033[6n\033[u", 20);
    /* Read ESC [ rows ; cols R. Bail on anything unexpected so a
       non-VT terminal just leaves the defaults intact. Each byte
       waits at most a few scheduler rounds. */
    char c; int rows = 0, cols = 0;
    int saw_esc = 0;
    for (int tries = 0; tries < 64; tries++) {
        if (!poll_read1(&c, 200)) goto bail;
        if (c == 0x1B) { saw_esc = 1; break; }
    }
    if (!saw_esc) goto bail;
    if (!poll_read1(&c, 100) || c != '[') goto bail;
    while (poll_read1(&c, 100) && c >= '0' && c <= '9')
        rows = rows * 10 + (c - '0');
    if (c != ';') goto bail;
    while (poll_read1(&c, 100) && c >= '0' && c <= '9')
        cols = cols * 10 + (c - '0');
    if (c != 'R' || rows <= 0 || cols <= 0) goto bail;

    sys_tty_setsize(rows, cols);
    extern int setenv(const char *, const char *, int);
    char buf[8];
    int n = 0, v = rows;
    char tmp[8]; int tn = 0;
    if (v == 0) tmp[tn++] = '0';
    while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
    while (tn > 0) buf[n++] = tmp[--tn];
    buf[n] = 0;
    setenv("LINES", buf, 1);
    n = 0; v = cols; tn = 0;
    if (v == 0) tmp[tn++] = '0';
    while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
    while (tn > 0) buf[n++] = tmp[--tn];
    buf[n] = 0;
    setenv("COLUMNS", buf, 1);
bail:
    sys_tty_raw(0);
}

static int run_interactive(void) {
    /* Put the shell in its own process group and claim the terminal.
       Foreground pipelines will get handed their own pgid and reclaim
       it on exit. Ignore SIGINT/SIGTSTP (we model TSTP as STOP — not
       catchable anyway) so that if a Ctrl-C / Ctrl-Z arrives between
       the hand-off calls, the shell survives. */
    sys_setpgid(0, 0);
    sys_tcsetpgrp((int)sys_getpid());
    sys_signal(SIG_INT, SIG_IGN);
    probe_winsize();
    char line[LINE_MAX];
    char expanded[LINE_MAX];
    for (;;) {
        u_puts_n("$ ");
        sys_tty_raw(1);                /* own echo + line editing */
        long n = shell_readline(line, sizeof(line));
        sys_tty_raw(0);                /* spawned children want cooked mode */
        if (n < 0) return 1;
        if (n == 0) continue;
        hist_push(line);
        if (expand_vars(line, expanded, sizeof(expanded)) == 0) {
            run_line(expanded);
        } else {
            run_line(line);    /* overflow — run as-is */
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc >= 3 && u_strcmp(argv[1], "-c") == 0) {
        /* One-shot: run the string as a single shell line. Used by
           libvibc's system() / popen() so callers get shell globbing
           and redirection (>, >>, <, |, &) instead of having their
           tokens handed straight to execve. */
        char line[LINE_MAX];
        int i = 0;
        while (argv[2][i] && i < LINE_MAX - 1) { line[i] = argv[2][i]; i++; }
        line[i] = 0;
        return run_line(line);
    }
    if (argc >= 2) return run_script(argv[1]);
    return run_interactive();
}
