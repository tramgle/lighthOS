#include "syscall.h"
#include "ulib.h"

#define LINE_MAX  256
#define ARGC_MAX  16
#define HIST_MAX  32
/* Root-relative. Bootdisk boots have FAT at '/' so this persists;
   ISO boots leave it on ramfs (ephemeral but harmless). */
#define HIST_FILE "/.history"

/* Key bytes emitted by src/drivers/serial.c's ANSI state machine. */
#define KEY_UP       0x81
#define KEY_DOWN     0x82
#define KEY_LEFT     0x83
#define KEY_RIGHT    0x84
#define KEY_HOME     0x85
#define KEY_END      0x86
#define KEY_CUP      0x91
#define KEY_CDOWN    0x92
#define KEY_CLEFT    0x93
#define KEY_CRIGHT   0x94
#define KEY_DEL      0x95

static char cwd[VFS_MAX_PATH];

/* --- History ring --- */
static char  history[HIST_MAX][LINE_MAX];
static int   hist_count;
static int   hist_pos;   /* cursor into history; == hist_count means "new line" */

static void hist_add(const char *line) {
    if (!line[0]) return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) return;

    if (hist_count < HIST_MAX) {
        strncpy(history[hist_count], line, LINE_MAX - 1);
        history[hist_count][LINE_MAX - 1] = '\0';
        hist_count++;
    } else {
        /* Shift ring down; drop the oldest. */
        for (int i = 0; i < HIST_MAX - 1; i++) {
            strncpy(history[i], history[i + 1], LINE_MAX - 1);
            history[i][LINE_MAX - 1] = '\0';
        }
        strncpy(history[HIST_MAX - 1], line, LINE_MAX - 1);
        history[HIST_MAX - 1][LINE_MAX - 1] = '\0';
    }
    hist_pos = hist_count;
}

static void hist_load(void) {
    int fd = sys_open(HIST_FILE, O_RDONLY);
    if (fd < 0) return;
    char buf[LINE_MAX];
    int  pos = 0;
    char c;
    while (sys_read(fd, &c, 1) == 1) {
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) hist_add(buf);
            pos = 0;
        } else if (pos < LINE_MAX - 1) {
            buf[pos++] = c;
        }
    }
    if (pos > 0) { buf[pos] = '\0'; hist_add(buf); }
    sys_close(fd);
    hist_pos = hist_count;
}

static void hist_save(void) {
    int fd = sys_open(HIST_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < hist_count; i++) {
        sys_write(fd, history[i], strlen(history[i]));
        sys_write(fd, "\n", 1);
    }
    sys_close(fd);
}

static char getchar_blocking(void) {
    char c;
    while (sys_read(0, &c, 1) < 1)
        sys_yield();
    return c;
}

/* --- Line editor helpers --- */

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Columns occupied by the shell prompt when readline was entered.
   Used to position the cursor with ANSI CUF (\033[NC) during repaint. */
static int prompt_len;

/* Write an ANSI "cursor forward N columns" escape. */
static void cuf(int n) {
    if (n <= 0) return;
    char buf[16];
    int i = 0;
    buf[i++] = '\033';
    buf[i++] = '[';
    /* Render decimal N. */
    char digits[8];
    int d = 0;
    if (n == 0) digits[d++] = '0';
    while (n) { digits[d++] = '0' + (n % 10); n /= 10; }
    while (d > 0) buf[i++] = digits[--d];
    buf[i++] = 'C';
    sys_write(1, buf, i);
}

/* Repaint the editable portion of the line: move to start-of-line,
   skip past the prompt with CUF, write the buffer, erase to EOL, then
   position the cursor at prompt_len + cur. */
static void paint_line(const char *buf, int len, int cur) {
    sys_write(1, "\r", 1);
    cuf(prompt_len);
    if (len > 0) sys_write(1, buf, len);
    sys_write(1, "\033[K", 3);    /* erase to EOL */
    sys_write(1, "\r", 1);
    cuf(prompt_len + cur);
}

/* --- Tab completion --- */

static const char *const builtins[] = {
    "help", "echo", "clear", "exit", "pwd", "cd", "ls", "cat", "write",
    "rm", "mkdir", "ps", "shutdown", 0
};

/* Find the token the caret sits in. Returns its start offset; writes
   the length into *out_len. */
static int token_bounds(const char *buf, int cur, int *out_len) {
    int start = cur;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t') start--;
    int end = cur;
    while (buf[end] && buf[end] != ' ' && buf[end] != '\t') end++;
    *out_len = end - start;
    return start;
}

/* Tab-complete `prefix` against a list of candidate names. Returns the
   single match if unique, else NULL (and optionally prints the list to
   stderr via `show`). */
static int candidates_from_dir(const char *dir, const char *prefix, int prefix_len,
                               char *match, int match_size, int *found) {
    char name[VFS_MAX_NAME];
    uint32_t type;
    for (uint32_t idx = 0; sys_readdir(dir, idx, name, &type) == 0; idx++) {
        if (name[0] == '.') continue;
        int ok = 1;
        for (int i = 0; i < prefix_len; i++) {
            if (name[i] != prefix[i]) { ok = 0; break; }
        }
        if (!ok) continue;
        (*found)++;
        if (*found == 1) {
            strncpy(match, name, match_size - 1);
            match[match_size - 1] = '\0';
        } else {
            /* Trim `match` to the common prefix between itself and
               `name`, so multi-match still completes as far as possible. */
            int j = 0;
            while (match[j] && name[j] && match[j] == name[j]) j++;
            match[j] = '\0';
        }
    }
    return 0;
}

/* Try to complete at the caret. Returns number of chars inserted. */
static int tab_complete(char *buf, int *len, int *cur, int max) {
    int tlen = 0;
    int tstart = token_bounds(buf, *cur, &tlen);
    int is_cmd = (tstart == 0);

    char match[128];
    match[0] = '\0';
    int found = 0;

    if (is_cmd) {
        /* Match builtins + /bin + /disk/bin. */
        for (int i = 0; builtins[i]; i++) {
            int ok = 1;
            for (int j = 0; j < tlen; j++) {
                if (builtins[i][j] != buf[tstart + j]) { ok = 0; break; }
            }
            if (!ok || !builtins[i][tlen]) continue;  /* require strict prefix */
            if (!builtins[i][tlen] && tlen == 0) continue;
            found++;
            if (found == 1) {
                strncpy(match, builtins[i], sizeof match - 1);
                match[sizeof match - 1] = '\0';
            } else {
                int j = 0;
                while (match[j] && builtins[i][j] && match[j] == builtins[i][j]) j++;
                match[j] = '\0';
            }
        }
        candidates_from_dir("/bin",      buf + tstart, tlen, match, sizeof match, &found);
        candidates_from_dir("/disk/bin", buf + tstart, tlen, match, sizeof match, &found);
    } else {
        /* Path completion: split token into dir + basename. The kernel
           resolves relative paths for sys_readdir (process_resolve_path
           in src/kernel/process.c), so we just hand it the dir portion
           as-is — no cwd prepending or canonicalization needed here. */
        char dir[VFS_MAX_PATH];
        char baseprefix[128];
        int last_slash = -1;
        for (int i = 0; i < tlen; i++) if (buf[tstart + i] == '/') last_slash = i;

        int bplen;
        if (last_slash < 0) {
            /* Bare name — resolve against cwd via ".". */
            dir[0] = '.'; dir[1] = '\0';
            bplen = tlen;
            for (int i = 0; i < bplen; i++) baseprefix[i] = buf[tstart + i];
        } else {
            /* "foo/bar" or "/abs/path/bar" — take the leading dir as
               typed. `last_slash == 0` means leading '/' — pass "/". */
            int dlen = last_slash == 0 ? 1 : last_slash;
            for (int i = 0; i < dlen; i++) dir[i] = buf[tstart + i];
            dir[dlen] = '\0';
            bplen = tlen - last_slash - 1;
            for (int i = 0; i < bplen; i++) baseprefix[i] = buf[tstart + last_slash + 1 + i];
        }
        baseprefix[bplen] = '\0';

        candidates_from_dir(dir, baseprefix, bplen, match, sizeof match, &found);

        /* If we matched, preserve the dir/ prefix in the inserted text. */
        if (found > 0 && last_slash >= 0) {
            char combined[256];
            int ci = 0;
            for (int i = 0; i <= last_slash && ci < 255; i++) combined[ci++] = buf[tstart + i];
            for (int i = 0; match[i] && ci < 255; i++) combined[ci++] = match[i];
            combined[ci] = '\0';
            strncpy(match, combined, sizeof match - 1);
            match[sizeof match - 1] = '\0';
        }
    }

    if (found == 0) return 0;

    /* Replace the current token with `match`. */
    int mlen = strlen(match);
    if (mlen <= tlen) return 0;  /* nothing to insert */

    int insert = mlen - tlen;
    if (*len + insert >= max) return 0;

    /* Shift right past the token. */
    for (int i = *len - 1; i >= tstart + tlen; i--) buf[i + insert] = buf[i];
    /* Overwrite token with match. */
    for (int i = 0; i < mlen; i++) buf[tstart + i] = match[i];
    *len += insert;
    *cur = tstart + mlen;
    buf[*len] = '\0';
    return insert;
}

/* Jump-by-word: skip runs of whitespace then runs of word chars. */
static int word_back(const char *buf, int cur) {
    while (cur > 0 && !is_word_char(buf[cur - 1])) cur--;
    while (cur > 0 &&  is_word_char(buf[cur - 1])) cur--;
    return cur;
}
static int word_forward(const char *buf, int len, int cur) {
    while (cur < len && !is_word_char(buf[cur])) cur++;
    while (cur < len &&  is_word_char(buf[cur])) cur++;
    return cur;
}

static int readline(char *buf, int max) {
    int len = 0;
    int cur = 0;
    hist_pos = hist_count;

    for (;;) {
        unsigned char c = (unsigned char)getchar_blocking();

        if (c == '\n') {
            putchar('\n');
            buf[len] = '\0';
            return len;
        }

        if (c == '\b') {
            if (cur > 0) {
                /* Delete the char left of the caret. */
                for (int i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                cur--; len--;
                buf[len] = '\0';
                paint_line(buf, len, cur);
            }
            continue;
        }

        if (c == KEY_DEL) {
            if (cur < len) {
                for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                buf[len] = '\0';
                paint_line(buf, len, cur);
            }
            continue;
        }

        if (c == '\t') {
            int inserted = tab_complete(buf, &len, &cur, max);
            if (inserted > 0) paint_line(buf, len, cur);
            continue;
        }

        if (c == KEY_UP) {
            if (hist_pos > 0) {
                hist_pos--;
                int n = 0;
                while (history[hist_pos][n] && n < max - 1) {
                    buf[n] = history[hist_pos][n]; n++;
                }
                buf[n] = '\0';
                len = n; cur = n;
                paint_line(buf, len, cur);
            }
            continue;
        }

        if (c == KEY_DOWN) {
            if (hist_pos < hist_count) {
                hist_pos++;
                const char *line = (hist_pos == hist_count) ? "" : history[hist_pos];
                int n = 0;
                while (line[n] && n < max - 1) { buf[n] = line[n]; n++; }
                buf[n] = '\0';
                len = n; cur = n;
                paint_line(buf, len, cur);
            }
            continue;
        }

        if (c == KEY_LEFT) {
            if (cur > 0) { cur--; sys_write(1, "\b", 1); }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cur < len) { sys_write(1, &buf[cur], 1); cur++; }
            continue;
        }
        if (c == KEY_CLEFT) {
            int nc = word_back(buf, cur);
            if (nc != cur) { cur = nc; paint_line(buf, len, cur); }
            continue;
        }
        if (c == KEY_CRIGHT) {
            int nc = word_forward(buf, len, cur);
            if (nc != cur) { cur = nc; paint_line(buf, len, cur); }
            continue;
        }
        if (c == KEY_HOME) {
            if (cur != 0) { cur = 0; paint_line(buf, len, cur); }
            continue;
        }
        if (c == KEY_END) {
            if (cur != len) { cur = len; paint_line(buf, len, cur); }
            continue;
        }

        /* Printable: insert at caret. */
        if (c >= ' ' && c < 127 && len < max - 1) {
            for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
            buf[cur] = c;
            len++;
            cur++;
            buf[len] = '\0';
            if (cur == len) {
                /* Appending — cheap path, no repaint needed. */
                sys_write(1, &c, 1);
            } else {
                paint_line(buf, len, cur);
            }
        }
    }
}

/* Tokenize `line` in place. Supports:
     - whitespace-separated bare tokens
     - "double quoted" and 'single quoted' strings
     - backslash escapes (outside single quotes) for the next byte
   Closing quote is consumed; the text between quotes becomes part of
   the current token (quotes themselves are elided). Unterminated
   quote is treated as if the quote ran to end-of-line. */
static int parse(char *line, char **argv, int max) {
    int argc = 0;
    char *src = line;
    char *dst = line;

    for (;;) {
        while (*src == ' ' || *src == '\t') src++;
        if (*src == '\0') break;
        if (argc >= max) break;

        argv[argc++] = dst;
        while (*src && *src != ' ' && *src != '\t') {
            if (*src == '\\' && src[1]) {
                *dst++ = src[1];
                src += 2;
            } else if (*src == '"') {
                src++;
                while (*src && *src != '"') {
                    if (*src == '\\' && src[1]) {
                        *dst++ = src[1];
                        src += 2;
                    } else {
                        *dst++ = *src++;
                    }
                }
                if (*src == '"') src++;
            } else if (*src == '\'') {
                src++;
                while (*src && *src != '\'') *dst++ = *src++;
                if (*src == '\'') src++;
            } else {
                *dst++ = *src++;
            }
        }
        /* Advance src past the delimiter whitespace BEFORE writing the
           NUL at dst — since dst and src can coincide, writing '\0' at
           dst's position would otherwise clobber the very space we're
           about to skip, leaving the outer loop seeing '\0' and
           terminating one token early. */
        while (*src == ' ' || *src == '\t') src++;
        *dst++ = '\0';
    }
    if (argc < max) argv[argc] = 0;
    return argc;
}

/* --- Built-in commands --- */

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        puts(argv[i]);
    }
    putchar('\n');
}

static void cmd_pwd(void) {
    sys_getcwd(cwd, VFS_MAX_PATH);
    puts(cwd);
    putchar('\n');
}

static void cmd_cd(int argc, char **argv) {
    /* The kernel resolves relative paths + canonicalizes `.`/`..`
       since r55, so we pass the user's argument through unchanged. */
    const char *target = (argc > 1) ? argv[1] : "/";
    if (sys_chdir(target) != 0) {
        printf("cd: %s: not found\n", target);
    } else {
        sys_getcwd(cwd, VFS_MAX_PATH);
    }
}

/* cmd_ls moved to /bin/ls (user/ls.c) so it composes in pipes.
   Shell now routes `ls` through the external-command path. */

static void cmd_clear(void) {
    /* ANSI: clear screen + cursor home. */
    sys_write(1, "\033[2J\033[H", 7);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { puts("Usage: cat <file>\n"); return; }
    int fd = sys_open(argv[1], O_RDONLY);
    if (fd < 0) { printf("cat: %s: not found\n", argv[1]); return; }
    char buf[256];
    int32_t n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        sys_write(1, buf, n);
    }
    putchar('\n');
    sys_close(fd);
}

static void cmd_write(int argc, char **argv) {
    if (argc < 3) { puts("Usage: write <file> <text...>\n"); return; }
    int fd = sys_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("write: cannot create %s\n", argv[1]); return; }
    for (int i = 2; i < argc; i++) {
        if (i > 2) sys_write(fd, " ", 1);
        sys_write(fd, argv[i], strlen(argv[i]));
    }
    sys_close(fd);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { puts("Usage: rm <file>\n"); return; }
    if (sys_unlink(argv[1]) != 0) printf("rm: cannot remove %s\n", argv[1]);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { puts("Usage: mkdir <dir>\n"); return; }
    if (sys_mkdir(argv[1]) != 0) printf("mkdir: cannot create %s\n", argv[1]);
}

static const char *state_str(uint32_t s) {
    switch (s) {
    case 0: return "READY";
    case 1: return "RUN";
    case 2: return "BLOCK";
    case 3: return "STOP";
    case 4: return "DEAD";
    default: return "?";
    }
}

static void cmd_ps(void) {
    puts("PID  PPID  PGID  STATE  ROOT   NAME\n");
    struct proc_info info;
    for (uint32_t i = 0; sys_ps(i, &info) == 0; i++) {
        printf("%u    %u     %u    %s  %s  %s\n",
               info.pid, info.parent_pid, info.pgid,
               state_str(info.state), info.root, info.name);
    }
}

static void cmd_shutdown(void) {
    puts("Shutting down...\n");
    hist_save();
    sys_shutdown();
}

static void cmd_help(void) {
    puts("Commands: help echo clear exit pwd cd ls [-la] cat write rm mkdir ps shutdown <program>\n");
    puts("Run ELF programs by name (e.g., hello). Up/Down arrows browse history.\n");
}

/* Resolve an argv[0] to an absolute path, probing /bin then /disk/bin
   for bare names. `abspath_out` receives the full path. Returns the
   spawned pid or -1 if the command wasn't found. */
static int spawn_external(char **seg_argv) {
    if (!seg_argv || !seg_argv[0]) return -1;
    char path[VFS_MAX_PATH];
    char *child_argv[ARGC_MAX + 2];
    int ci = 1;
    for (int i = 1; seg_argv[i] && ci < ARGC_MAX + 1; i++) {
        child_argv[ci++] = seg_argv[i];
    }
    child_argv[ci] = 0;

    int pid = -1;
    if (seg_argv[0][0] == '/') {
        strncpy(path, seg_argv[0], VFS_MAX_PATH - 1);
        child_argv[0] = path;
        pid = sys_spawn(path, child_argv);
    } else {
        strcpy(path, "/bin/");
        strcat(path, seg_argv[0]);
        child_argv[0] = path;
        pid = sys_spawn(path, child_argv);
        if (pid < 0) {
            strcpy(path, "/disk/bin/");
            strcat(path, seg_argv[0]);
            child_argv[0] = path;
            pid = sys_spawn(path, child_argv);
        }
    }
    return pid;
}

/* Run a `foo | bar | baz` pipeline. Each segment runs as an external
   program; stdin of segment N+1 is wired to stdout of segment N via a
   sys_pipe. Builtins aren't supported in pipelines — they'd need to
   run in-process, which doesn't compose with dup2/spawn.

   We save the shell's stdin/stdout to temporary fds, rewrite fd 0 and
   fd 1 around each sys_spawn so the child inherits the right endpoints,
   then restore before waiting for completion. Pipe endpoints created
   here are closed in the shell as soon as they've been handed off —
   the child is the only process that should keep a reference. */
#define SAVE_STDIN_FD  11
#define SAVE_STDOUT_FD 12

/* --- Job control ----
   Each background/stopped pipeline becomes a job. We remember the
   group leader's pid (which is also the pgid for the whole pipeline,
   thanks to sys_setpgid calls after spawn). */

#define JOBS_MAX 16

typedef enum { JOB_FREE, JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

typedef struct {
    job_state_t state;
    int         pgid;       /* == leader pid */
    int         pids[ARGC_MAX];
    int         n_pids;
    char        cmd[LINE_MAX];
} job_t;

static job_t jobs[JOBS_MAX];

static int job_alloc(void) {
    for (int i = 0; i < JOBS_MAX; i++)
        if (jobs[i].state == JOB_FREE) return i;
    return -1;
}

/* Poll each pid via sys_ps so we can reflect DONE/STOPPED without
   blocking. Kernel state values: 0 READY, 1 RUN, 2 BLOCK, 3 STOP,
   4 DEAD. Returns 1 if at least one pid is still live (non-DEAD). */
static int job_refresh(job_t *j) {
    if (j->state == JOB_FREE) return 0;
    int any_alive = 0;
    int any_stopped = 0;
    for (int i = 0; i < j->n_pids; i++) {
        struct proc_info pi;
        int found = 0;
        for (uint32_t idx = 0; sys_ps(idx, &pi) == 0; idx++) {
            if ((int)pi.pid == j->pids[i]) { found = 1; break; }
        }
        if (!found) continue;
        if (pi.state == 4) continue;
        any_alive = 1;
        if (pi.state == 3) any_stopped = 1;
    }
    if (!any_alive) j->state = JOB_DONE;
    else if (any_stopped) j->state = JOB_STOPPED;
    else if (j->state == JOB_STOPPED) j->state = JOB_RUNNING;
    return any_alive;
}

static void run_pipeline_ex(char **argv, int argc, int background, const char *full_cmd);

static void run_pipeline(char **argv, int argc) {
    run_pipeline_ex(argv, argc, 0, 0);
}

/* Parse `%N` (1-based) or bare decimal into a jobs[] index. Returns
   -1 if no valid job matches. If no arg given, picks the most recent
   Stopped-or-Running job. */
static int job_index_from_arg(int argc, char **argv) {
    if (argc < 2) {
        for (int i = JOBS_MAX - 1; i >= 0; i--) {
            if (jobs[i].state == JOB_STOPPED || jobs[i].state == JOB_RUNNING) return i;
        }
        return -1;
    }
    const char *s = argv[1];
    if (s[0] == '%') s++;
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    int idx = n - 1;
    if (idx < 0 || idx >= JOBS_MAX) return -1;
    if (jobs[idx].state == JOB_FREE) return -1;
    return idx;
}

static const char *job_state_label(job_state_t s) {
    switch (s) {
    case JOB_RUNNING: return "Running";
    case JOB_STOPPED: return "Stopped";
    case JOB_DONE:    return "Done";
    default:          return "?";
    }
}

static void cmd_jobs(void) {
    for (int i = 0; i < JOBS_MAX; i++) {
        if (jobs[i].state == JOB_FREE) continue;
        job_refresh(&jobs[i]);
        printf("[%d] %-7s  pgid=%d  %s\n", i + 1,
               job_state_label(jobs[i].state), jobs[i].pgid, jobs[i].cmd);
        if (jobs[i].state == JOB_DONE) jobs[i].state = JOB_FREE;
    }
}

static void cmd_fg(int argc, char **argv, const char *raw_cmd) {
    (void)raw_cmd;
    int idx = job_index_from_arg(argc, argv);
    if (idx < 0) { puts("fg: no such job\n"); return; }
    job_t *j = &jobs[idx];

    /* Resume if stopped. */
    if (j->state == JOB_STOPPED) {
        sys_kill(-j->pgid, SIG_CONT);
        j->state = JOB_RUNNING;
    }

    /* Wait on the LAST pid (pipeline terminator). A fresh SIGSTOP
       would re-stop it — status == 0x7f — and we re-register. */
    printf("%s\n", j->cmd);
    int status = 0;
    sys_waitpid(j->pids[j->n_pids - 1], &status);
    if (status == 0x7f) {
        j->state = JOB_STOPPED;
        printf("\n[%d]+ Stopped  %s\n", idx + 1, j->cmd);
    } else {
        /* Reap the rest. */
        for (int i = 0; i < j->n_pids - 1; i++) {
            int st;
            sys_waitpid(j->pids[i], &st);
        }
        j->state = JOB_FREE;
    }
}

static void cmd_bg(int argc, char **argv) {
    int idx = job_index_from_arg(argc, argv);
    if (idx < 0) { puts("bg: no such job\n"); return; }
    job_t *j = &jobs[idx];
    if (j->state != JOB_STOPPED) { puts("bg: job not stopped\n"); return; }
    sys_kill(-j->pgid, SIG_CONT);
    j->state = JOB_RUNNING;
    printf("[%d] %s &\n", idx + 1, j->cmd);
}

static void run_pipeline_ex(char **argv, int argc, int background, const char *full_cmd) {
    char **segments[ARGC_MAX];
    int seg_count = 0;
    int n_pipes = 0;
    segments[seg_count++] = argv;
    for (int i = 0; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "|") == 0) {
            argv[i] = 0;  /* terminate preceding segment's argv list */
            n_pipes++;
            if (i + 1 < argc && argv[i + 1] && seg_count < ARGC_MAX) {
                segments[seg_count++] = &argv[i + 1];
            }
        }
    }

    /* Syntax checks: exactly one segment per pipe, and every segment
       starts with a real token. Catches "|foo", "foo |", "foo || bar". */
    if (seg_count != n_pipes + 1) {
        puts("shell: syntax error near '|'\n");
        return;
    }
    for (int s = 0; s < seg_count; s++) {
        if (!segments[s][0] || !segments[s][0][0]) {
            puts("shell: syntax error near '|'\n");
            return;
        }
    }

    int saved_in = sys_dup2(0, SAVE_STDIN_FD);
    int saved_out = sys_dup2(1, SAVE_STDOUT_FD);
    if (saved_in < 0 || saved_out < 0) {
        puts("shell: out of fds for pipeline\n");
        return;
    }

    int pids[ARGC_MAX];
    int n_pids = 0;
    int prev_read = -1;
    const char *missing_cmd = 0;
    int leader_pid = -1;

    for (int s = 0; s < seg_count; s++) {
        int is_last = (s == seg_count - 1);
        int pipe_fds[2] = { -1, -1 };

        if (!is_last) {
            if (sys_pipe(pipe_fds) < 0) {
                puts("shell: pipe() failed\n");
                break;
            }
        }

        /* Point the shell's fd 0 and fd 1 at this segment's endpoints
           so the child inherits them via sys_spawn's fd-table copy. */
        if (prev_read >= 0) sys_dup2(prev_read, 0);
        else                sys_dup2(saved_in, 0);

        if (!is_last) sys_dup2(pipe_fds[1], 1);
        else          sys_dup2(saved_out, 1);

        int pid = spawn_external(segments[s]);

        /* Shell drops its pipe references so child has the only ones. */
        if (prev_read >= 0) { sys_close(prev_read); prev_read = -1; }
        if (!is_last) {
            sys_close(pipe_fds[1]);
            prev_read = pipe_fds[0];
        }

        if (pid < 0) {
            /* Defer the error message — fd[1] is still the pipe at
               this point. Print after we restore stdio below. */
            missing_cmd = segments[s][0];
            break;
        }
        if (leader_pid < 0) leader_pid = pid;
        else sys_setpgid(pid, leader_pid);  /* join the pipeline group */
        if (n_pids < ARGC_MAX) pids[n_pids++] = pid;
    }

    /* Restore shell's stdio BEFORE waitpid, so any output (e.g. a
       Ctrl-C interrupt banner) goes to the console. */
    if (prev_read >= 0) sys_close(prev_read);
    sys_dup2(saved_in, 0);   sys_close(saved_in);
    sys_dup2(saved_out, 1);  sys_close(saved_out);

    if (missing_cmd) printf("%s: command not found\n", missing_cmd);

    if (background && n_pids > 0 && leader_pid > 0) {
        int jn = job_alloc();
        if (jn >= 0) {
            jobs[jn].state = JOB_RUNNING;
            jobs[jn].pgid  = leader_pid;
            jobs[jn].n_pids = n_pids;
            for (int i = 0; i < n_pids; i++) jobs[jn].pids[i] = pids[i];
            if (full_cmd) {
                int c = 0;
                while (full_cmd[c] && c < LINE_MAX - 1) {
                    jobs[jn].cmd[c] = full_cmd[c]; c++;
                }
                jobs[jn].cmd[c] = '\0';
            } else {
                jobs[jn].cmd[0] = '\0';
            }
            printf("[%d] %d\n", jn + 1, leader_pid);
        }
        return;
    }

    /* Foreground: wait on the LAST pid (pipeline terminator). If it
       stops via SIGSTOP, register as a stopped job. */
    if (n_pids == 0) return;
    int status = 0;
    sys_waitpid(pids[n_pids - 1], &status);
    if (status == 0x7f) {
        /* SIGSTOP reported by kernel. Stash as stopped job. */
        int jn = job_alloc();
        if (jn >= 0) {
            jobs[jn].state = JOB_STOPPED;
            jobs[jn].pgid  = leader_pid;
            jobs[jn].n_pids = n_pids;
            for (int i = 0; i < n_pids; i++) jobs[jn].pids[i] = pids[i];
            if (full_cmd) {
                int c = 0;
                while (full_cmd[c] && c < LINE_MAX - 1) {
                    jobs[jn].cmd[c] = full_cmd[c]; c++;
                }
                jobs[jn].cmd[c] = '\0';
            }
            printf("\n[%d]+ Stopped  %s\n", jn + 1,
                   full_cmd ? full_cmd : "(command)");
        }
        return;
    }
    /* Reap the earlier pipeline members that are dead too. */
    for (int i = 0; i < n_pids - 1; i++) {
        int st;
        sys_waitpid(pids[i], &st);
    }
}

/* Execute a single command line. `line` is mutated in place by parse().
   Returns after the command (or pipeline, or builtin) finishes. */
static void run_one_line(char *line) {
    char *argv[ARGC_MAX];
    char raw_cmd[LINE_MAX];

    /* Keep a pristine copy for jobs[] display (parse() mutates line). */
    int rc_i = 0;
    while (line[rc_i] && rc_i < LINE_MAX - 1) {
        raw_cmd[rc_i] = line[rc_i]; rc_i++;
    }
    raw_cmd[rc_i] = '\0';

    int argc = parse(line, argv, ARGC_MAX);
    if (argc == 0) return;

    /* Trailing `&` = background. Strip it before dispatch. */
    int background = 0;
    if (argc > 0 && argv[argc - 1] && strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argv[--argc] = 0;
    }

    /* Detect trailing `>` / `>>` redirection BEFORE dispatching to
       pipeline or builtin. Redirect applies to the last command in a
       pipeline as well as to simple commands. For pipelines the
       run_pipeline path uses our saved_out as the target for its last
       segment, so setting fd[1] here routes the final stage into the
       file transparently. */
    const char *redir_path = 0;
    int redir_append = 0;
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            redir_append = (argv[i][1] == '>');
            redir_path = argv[i + 1];
            argc = i;
            argv[i] = 0;
            break;
        }
    }

    int saved_stdout = -1;
    if (redir_path) {
        uint32_t flags = O_WRONLY | O_CREAT | (redir_append ? O_APPEND : O_TRUNC);
        int out = sys_open(redir_path, flags);
        if (out < 0) {
            printf("shell: cannot open %s for writing\n", redir_path);
            return;
        }
        saved_stdout = sys_dup2(1, 10);
        sys_dup2(out, 1);
        sys_close(out);
    }

    /* Detect `|` pipeline — pipelines compose externals only. */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            run_pipeline_ex(argv, argc, background, raw_cmd);
            if (saved_stdout >= 0) {
                sys_dup2(saved_stdout, 1);
                sys_close(saved_stdout);
            }
            return;
        }
    }

    /* Single-command background: route through run_pipeline_ex so the
       same jobs/pgid plumbing applies. */
    if (background) {
        run_pipeline_ex(argv, argc, 1, raw_cmd);
        if (saved_stdout >= 0) {
            sys_dup2(saved_stdout, 1);
            sys_close(saved_stdout);
        }
        return;
    }

    /* Built-in commands */
    if (strcmp(argv[0], "exit") == 0) {
        hist_save();
        sys_exit(0);
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(argv[0], "clear") == 0) {
        cmd_clear();
    } else if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(argv[0], "cd") == 0) {
        cmd_cd(argc, argv);
    } else if (strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(argv[0], "write") == 0) {
        cmd_write(argc, argv);
    } else if (strcmp(argv[0], "rm") == 0) {
        cmd_rm(argc, argv);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        cmd_mkdir(argc, argv);
    } else if (strcmp(argv[0], "ps") == 0) {
        cmd_ps();
    } else if (strcmp(argv[0], "shutdown") == 0) {
        cmd_shutdown();
    } else if (strcmp(argv[0], "jobs") == 0) {
        cmd_jobs();
    } else if (strcmp(argv[0], "fg") == 0) {
        cmd_fg(argc, argv, raw_cmd);
    } else if (strcmp(argv[0], "bg") == 0) {
        cmd_bg(argc, argv);
    } else {
        /* External program. sys_spawn inherits the shell's fd table
           (including any redirected stdout), so pipes and `>` both
           work without an explicit fork+execve dance. */
        char path[VFS_MAX_PATH];
        char *child_argv[ARGC_MAX + 2];
        int ci = 1;
        for (int i = 1; i < argc && ci < ARGC_MAX + 1; i++) {
            child_argv[ci++] = argv[i];
        }
        child_argv[ci] = 0;

        int pid = -1;
        if (argv[0][0] == '/') {
            strncpy(path, argv[0], VFS_MAX_PATH - 1);
            child_argv[0] = path;
            pid = sys_spawn(path, child_argv);
        } else {
            strcpy(path, "/bin/");
            strcat(path, argv[0]);
            child_argv[0] = path;
            pid = sys_spawn(path, child_argv);
            if (pid < 0) {
                strcpy(path, "/disk/bin/");
                strcat(path, argv[0]);
                child_argv[0] = path;
                pid = sys_spawn(path, child_argv);
            }
        }

        if (pid < 0) {
            printf("%s: command not found\n", argv[0]);
        } else {
            int status = 0;
            sys_waitpid(pid, &status);
            if (status == 0x7f) {
                /* Ctrl-Z → register as a stopped job. */
                int jn = job_alloc();
                if (jn >= 0) {
                    jobs[jn].state = JOB_STOPPED;
                    jobs[jn].pgid  = pid;
                    jobs[jn].pids[0] = pid;
                    jobs[jn].n_pids = 1;
                    int c = 0;
                    while (raw_cmd[c] && c < LINE_MAX - 1) {
                        jobs[jn].cmd[c] = raw_cmd[c]; c++;
                    }
                    jobs[jn].cmd[c] = '\0';
                    printf("\n[%d]+ Stopped  %s\n", jn + 1, raw_cmd);
                }
            }
        }
    }

    if (saved_stdout >= 0) {
        sys_dup2(saved_stdout, 1);
        sys_close(saved_stdout);
    }
}

/* Read one line from fd `in`. No editing, no escape processing —
   just bytes until '\n' or EOF. Used for script mode. Returns line
   length written to buf (not including NUL), -1 on EOF at start. */
static int read_script_line(int in, char *buf, int cap) {
    int n = 0;
    char c;
    int got_any = 0;
    while (n < cap - 1) {
        int32_t r = sys_read(in, &c, 1);
        if (r <= 0) break;
        got_any = 1;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return got_any ? n : -1;
}

static void run_script_fd(int fd) {
    char line[LINE_MAX];
    for (;;) {
        int n = read_script_line(fd, line, LINE_MAX);
        if (n < 0) break;  /* EOF */
        /* Skip blank lines and comments. */
        int i = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (line[i] == '\0' || line[i] == '#') continue;
        run_one_line(line);
    }
}

int main(int argc, char **argv) {
    /* Non-interactive modes: shell <file> or shell -c "<cmd>". Both
       bypass readline/history and exit on completion. */
    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        if (argc < 3) { puts("shell: -c needs a command\n"); return 1; }
        char line[LINE_MAX];
        int i = 0;
        while (argv[2][i] && i < LINE_MAX - 1) { line[i] = argv[2][i]; i++; }
        line[i] = '\0';
        run_one_line(line);
        return 0;
    }
    if (argc >= 2) {
        int fd = sys_open(argv[1], O_RDONLY);
        if (fd < 0) { printf("shell: cannot open %s\n", argv[1]); return 1; }
        run_script_fd(fd);
        sys_close(fd);
        return 0;
    }

    /* Interactive. */
    char line[LINE_MAX];
    char line_copy[LINE_MAX];

    sys_getcwd(cwd, VFS_MAX_PATH);
    hist_load();

    for (;;) {
        sys_getcwd(cwd, VFS_MAX_PATH);
        prompt_len = 10 /* "lighthos:" + "$" */ + 1 /* trailing space */
                   + (int)strlen(cwd);
        printf("lighthos:%s$ ", cwd);

        int len = readline(line, LINE_MAX);
        if (len == 0) continue;

        /* Pristine copy for history — parse() mutates `line`. */
        strncpy(line_copy, line, LINE_MAX - 1);
        line_copy[LINE_MAX - 1] = '\0';
        hist_add(line_copy);

        run_one_line(line);
    }
}
