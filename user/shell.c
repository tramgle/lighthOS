#include "syscall.h"
#include "ulib.h"

#define LINE_MAX  256
#define ARGC_MAX  16
#define CWD_MAX   256
#define HIST_MAX  32
#define HIST_FILE "/disk/.history"

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

static char cwd[CWD_MAX];

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
    char name[64];
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
        /* Path completion: split token into dir + basename. */
        char dir[CWD_MAX];
        char baseprefix[128];
        int last_slash = -1;
        for (int i = 0; i < tlen; i++) if (buf[tstart + i] == '/') last_slash = i;

        int bplen;
        if (last_slash < 0) {
            sys_getcwd(dir, CWD_MAX);
            bplen = tlen;
            for (int i = 0; i < bplen; i++) baseprefix[i] = buf[tstart + i];
        } else {
            int dlen = last_slash;
            if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
            else {
                if (buf[tstart] == '/') {
                    for (int i = 0; i < dlen; i++) dir[i] = buf[tstart + i];
                    dir[dlen] = '\0';
                } else {
                    sys_getcwd(dir, CWD_MAX);
                    int cwd_len = strlen(dir);
                    if (cwd_len > 0 && dir[cwd_len - 1] != '/') dir[cwd_len++] = '/';
                    for (int i = 0; i < dlen; i++) dir[cwd_len + i] = buf[tstart + i];
                    dir[cwd_len + dlen] = '\0';
                }
            }
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
    sys_getcwd(cwd, CWD_MAX);
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
        sys_getcwd(cwd, CWD_MAX);
    }
}

struct stat_info {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
};

static void cmd_ls(int argc, char **argv) {
    /* Parse flags: -l long listing, -a include dotfiles. */
    int want_long = 0, want_all = 0;
    const char *path_arg = ".";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'l') want_long = 1;
                else if (*f == 'a') want_all = 1;
                else { printf("ls: unknown flag -%c\n", *f); return; }
            }
        } else {
            path_arg = argv[i];
        }
    }

    /* Kernel resolves relative paths for readdir/stat, so we pass the
       user's path verbatim plus `/name` for each stat call. */
    char name[64];
    uint32_t type;
    uint32_t shown = 0;
    for (uint32_t idx = 0; sys_readdir(path_arg, idx, name, &type) == 0; idx++) {
        if (!want_all && name[0] == '.') continue;

        if (want_long) {
            char full[CWD_MAX];
            int plen = 0;
            while (path_arg[plen] && plen < CWD_MAX - 2) {
                full[plen] = path_arg[plen]; plen++;
            }
            if (plen == 0 || full[plen - 1] != '/') full[plen++] = '/';
            int j = 0;
            while (name[j] && plen < CWD_MAX - 1) full[plen++] = name[j++];
            full[plen] = '\0';

            struct stat_info st = {0, 0, 0};
            sys_stat(full, &st);
            char tch = (type == 2) ? 'd' : 'f';
            printf("%c  %8u  %s%s\n", tch, st.size, name, (type == 2) ? "/" : "");
        } else {
            if (type == 2) printf("  %s/\n", name);
            else            printf("  %s\n", name);
        }
        shown++;
    }
    if (shown == 0) puts("  (empty)\n");
}

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
    case 3: return "DEAD";
    default: return "?";
    }
}

static void cmd_ps(void) {
    puts("PID  PPID  STATE  NAME\n");
    struct proc_info info;
    for (uint32_t i = 0; sys_ps(i, &info) == 0; i++) {
        printf("%u    %u     %s  %s\n",
               info.pid, info.parent_pid, state_str(info.state), info.name);
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

int main(void) {
    char line[LINE_MAX];
    char line_copy[LINE_MAX];
    char *argv[ARGC_MAX];

    sys_getcwd(cwd, CWD_MAX);
    hist_load();

    for (;;) {
        sys_getcwd(cwd, CWD_MAX);
        /* "vibeos:" + cwd + "$ " — track length so readline can
           position the cursor past the prompt without reprinting it. */
        prompt_len = 8 /* "vibeos:" + "$" */ + 1 /* trailing space */
                   + (int)strlen(cwd);
        printf("vibeos:%s$ ", cwd);

        int len = readline(line, LINE_MAX);
        if (len == 0) continue;

        /* Keep a pristine copy for history — parse() mutates `line`
           by NUL-terminating each token in place. */
        strncpy(line_copy, line, LINE_MAX - 1);
        line_copy[LINE_MAX - 1] = '\0';

        int argc = parse(line, argv, ARGC_MAX);
        if (argc == 0) continue;

        hist_add(line_copy);

        /* Detect trailing `>` / `>>` redirection. Strips the operator
           and filename from argv so built-ins and external commands
           both see a clean argument list, and drives a sys_dup2 on
           stdout for the duration of the command. */
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
                continue;
            }
            /* Save the current stdout into a high fd, swap in the
               file fd as stdout, then close the temporary. Restored
               at the end of this iteration. */
            saved_stdout = sys_dup2(1, 10);
            sys_dup2(out, 1);
            sys_close(out);
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
        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc, argv);
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
        } else {
            /* External program. When redirection is active we need
               the child to inherit the shell's remapped stdout — that
               works naturally through fork+execve (fork copies the fd
               table). Without redirection we keep the cheaper
               sys_spawn path (child gets fresh console fds). */
            char path[CWD_MAX];
            char *child_argv[ARGC_MAX + 2];
            int ci = 1;
            for (int i = 1; i < argc && ci < ARGC_MAX + 1; i++) {
                child_argv[ci++] = argv[i];
            }
            child_argv[ci] = 0;

            int pid = -1;
            if (argv[0][0] == '/') {
                strncpy(path, argv[0], CWD_MAX - 1);
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
                int status;
                sys_waitpid(pid, &status);
            }
        }

        /* Restore stdout if we redirected earlier this iteration. */
        if (saved_stdout >= 0) {
            sys_dup2(saved_stdout, 1);
            sys_close(saved_stdout);
        }
    }
}
