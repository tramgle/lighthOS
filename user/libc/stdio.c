/* Minimal FILE* layer over our fd-based syscalls.
 *
 * Buffering strategy:
 *  - stdin: unbuffered (ungetc supported via a 1-char pushback slot)
 *  - stdout: line-buffered when it's a terminal (which is always, for now)
 *  - stderr: unbuffered
 *  - disk files: block-buffered (1 KB)
 *
 * Lua's io lib uses fread/fwrite/fgets/fputs/fgetc/ungetc/fseek/ftell
 * /fflush/feof/ferror/clearerr/setvbuf. We cover all of those. */

#include "ulib.h"
#include "syscall.h"
#include <stdint.h>
#include <string.h>

typedef int ssize_t;

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)

int vsnprintf(char *out, size_t size, const char *fmt, va_list ap);

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 1024
#define EOF    (-1)

typedef long fpos_t;

typedef struct FILE_s {
    int    fd;
    int    mode;        /* O_RDONLY, O_WRONLY, O_RDWR */
    int    bufmode;     /* _IOFBF/_IOLBF/_IONBF */
    char  *buf;
    size_t bufsize;
    size_t bufpos;      /* next byte to flush (write) or next to read (read) */
    size_t buflen;      /* total valid bytes in buf (read only) */
    int    flags;       /* bit 0 = writing, bit 1 = reading, bit 2 = eof, bit 3 = error, bit 4 = ungot */
    int    ungot_ch;
    int    owns_buf;    /* 1 if we allocated buf and should free on close */
} FILE;

#define F_WRITING 0x01
#define F_READING 0x02
#define F_EOF     0x04
#define F_ERR     0x08
#define F_UNGOT   0x10

static char stdin_buf[BUFSIZ];
static char stdout_buf[BUFSIZ];
static char stderr_buf[BUFSIZ];

static FILE _stdin  = { 0, 0 /*RDONLY*/, _IONBF, stdin_buf,  BUFSIZ, 0, 0, 0, 0, 0 };
/* stdout is unbuffered by default: terminal sessions interleave raw
   sys_writes (shell echo, u_puts_n) with stdio writes (vi's printf),
   and line-buffering reorders them. Callers that want batched output
   can setvbuf themselves. */
static FILE _stdout = { 1, 1 /*WRONLY*/, _IONBF, stdout_buf, BUFSIZ, 0, 0, 0, 0, 0 };
static FILE _stderr = { 2, 1 /*WRONLY*/, _IONBF, stderr_buf, BUFSIZ, 0, 0, 0, 0, 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

void *malloc(uint32_t);
void  free(void *);

static int parse_mode(const char *mode, int *flags) {
    int o = 0;
    int want_read = 0, want_write = 0, want_append = 0;
    for (; *mode; mode++) {
        switch (*mode) {
        case 'r': want_read = 1; break;
        case 'w': want_write = 1; break;
        case 'a': want_append = 1; break;
        case '+': want_read = want_write = 1; break;
        case 'b': case 't': break;  /* binary flag is a no-op here */
        }
    }
    if (want_read && want_write) o = O_RDWR | O_CREAT;
    else if (want_write) o = O_WRONLY | O_CREAT | O_TRUNC;
    else if (want_append) o = O_WRONLY | O_CREAT;
    else o = O_RDONLY;
    *flags = o;
    if (want_write && !want_append) return 1;  /* truncate/create */
    if (want_append) return 2;
    if (want_read) return 0;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;
    parse_mode(mode, &flags);
    int fd = sys_open(path, (uint32_t)flags);
    if (fd < 0) return 0;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); return 0; }

    f->fd = fd;
    f->mode = flags;
    f->bufmode = _IOFBF;
    f->bufsize = BUFSIZ;
    f->buf = (char *)malloc(BUFSIZ);
    if (!f->buf) { free(f); sys_close(fd); return 0; }
    f->bufpos = 0;
    f->buflen = 0;
    f->flags = 0;
    f->ungot_ch = 0;
    f->owns_buf = 1;

    /* Append mode: seek to end. */
    if (mode[0] == 'a' || (mode[0] == 'a' && mode[1] == '+')) {
        sys_lseek(fd, 0, 2);
    }
    return f;
}

int fflush(FILE *f) {
    if (!f) return 0;
    if ((f->flags & F_WRITING) && f->bufpos > 0) {
        ssize_t n = sys_write(f->fd, f->buf, f->bufpos);
        if (n < 0 || (size_t)n != f->bufpos) {
            f->flags |= F_ERR;
            return EOF;
        }
        f->bufpos = 0;
    }
    f->flags &= ~F_WRITING;
    return 0;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int rc = 0;
    if (fflush(f) != 0) rc = EOF;
    if (f->fd >= 3) sys_close(f->fd);
    if (f->owns_buf && f->buf) free(f->buf);
    if (f != stdin && f != stdout && f != stderr) free(f);
    return rc;
}

/* Read up to `count` bytes into `dst`. Handles any buffered data first. */
size_t fread(void *dst, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    if (f->flags & F_WRITING) fflush(f);
    f->flags |= F_READING;

    size_t total = size * nmemb;
    char *out = (char *)dst;
    size_t done = 0;

    /* Drain ungot char. */
    if (f->flags & F_UNGOT) {
        out[done++] = (char)f->ungot_ch;
        f->flags &= ~F_UNGOT;
    }

    /* Drain buffered bytes. */
    size_t bufavail = f->buflen - f->bufpos;
    if (bufavail > 0 && done < total) {
        size_t n = total - done;
        if (n > bufavail) n = bufavail;
        memcpy(out + done, f->buf + f->bufpos, n);
        f->bufpos += n;
        done += n;
    }

    /* For large reads, bypass the buffer. For small reads, refill.
       stdin (fd 0) is the console — a 0-byte read means "no data yet",
       not EOF, so we yield and retry. Disk files return 0 only at true
       EOF. */
    int is_stdin = (f->fd == 0);
    while (done < total) {
        size_t remaining = total - done;
        if (f->bufmode == _IONBF || remaining >= f->bufsize) {
            ssize_t n = sys_read(f->fd, out + done, remaining);
            if (n < 0) { f->flags |= F_ERR; break; }
            if (n == 0) {
                if (is_stdin) { sys_yield(); continue; }
                f->flags |= F_EOF;
                break;
            }
            done += (size_t)n;
        } else {
            ssize_t n = sys_read(f->fd, f->buf, f->bufsize);
            if (n < 0) { f->flags |= F_ERR; break; }
            if (n == 0) {
                if (is_stdin) { sys_yield(); continue; }
                f->flags |= F_EOF;
                break;
            }
            f->bufpos = 0;
            f->buflen = (size_t)n;
            size_t take = (remaining < (size_t)n) ? remaining : (size_t)n;
            memcpy(out + done, f->buf, take);
            f->bufpos = take;
            done += take;
        }
    }
    return done / size;
}

size_t fwrite(const void *src, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;
    if (f->flags & F_READING) { f->bufpos = f->buflen = 0; f->flags &= ~F_READING; }
    f->flags |= F_WRITING;

    size_t total = size * nmemb;
    const char *in = (const char *)src;
    size_t done = 0;

    while (done < total) {
        if (f->bufmode == _IONBF) {
            ssize_t n = sys_write(f->fd, in + done, total - done);
            if (n < 0) { f->flags |= F_ERR; return done / size; }
            done += (size_t)n;
            continue;
        }
        size_t room = f->bufsize - f->bufpos;
        if (room == 0) {
            if (fflush(f) != 0) return done / size;
            f->flags |= F_WRITING;
            continue;
        }
        size_t take = (total - done < room) ? (total - done) : room;
        memcpy(f->buf + f->bufpos, in + done, take);
        f->bufpos += take;
        done += take;

        if (f->bufmode == _IOLBF) {
            /* Flush through the last newline we just wrote. */
            for (ssize_t i = (ssize_t)f->bufpos - 1; i >= 0; i--) {
                if (f->buf[i] == '\n') {
                    sys_write(f->fd, f->buf, i + 1);
                    size_t leftover = f->bufpos - (i + 1);
                    memmove(f->buf, f->buf + i + 1, leftover);
                    f->bufpos = leftover;
                    break;
                }
            }
        } else if (f->bufpos == f->bufsize) {
            if (fflush(f) != 0) return done / size;
            f->flags |= F_WRITING;
        }
    }
    return done / size;
}

int fgetc(FILE *f) {
    unsigned char c;
    size_t n = fread(&c, 1, 1, f);
    return (n == 1) ? (int)c : EOF;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    if (f->flags & F_UNGOT) return EOF;  /* only one slot */
    f->ungot_ch = (unsigned char)c;
    f->flags |= F_UNGOT;
    f->flags &= ~F_EOF;
    return c;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    return (fwrite(&ch, 1, 1, f) == 1) ? c : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return (fwrite(s, 1, n, f) == n) ? 0 : EOF;
}

/* ISO C: puts() writes the string AND a trailing newline. */
int puts(const char *s) {
    if (fputs(s, stdout) != 0) return EOF;
    return fputc('\n', stdout);
}

char *fgets(char *out, int size, FILE *f) {
    if (!f || size <= 0) return 0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return 0;
            break;
        }
        out[i++] = (char)c;
        if (c == '\n') break;
    }
    out[i] = '\0';
    return out;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f) return -1;
    if (f->flags & F_WRITING) fflush(f);
    f->bufpos = f->buflen = 0;
    f->flags &= ~(F_READING | F_EOF | F_UNGOT);
    int32_t r = sys_lseek(f->fd, (int32_t)offset, whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    if (f->flags & F_WRITING) fflush(f);
    /* We don't have a pure "tell" syscall, so SEEK_CUR with 0 works. */
    int32_t r = sys_lseek(f->fd, 0, 1);
    if (r < 0) return -1;
    /* Adjust for any buffered-but-not-consumed read bytes. */
    long pending = (long)(f->buflen - f->bufpos);
    return (long)r - pending;
}

void clearerr(FILE *f) { if (f) f->flags &= ~(F_EOF | F_ERR); }
void rewind(FILE *f) { fseek(f, 0, 0); clearerr(f); }

int fgetpos(FILE *f, fpos_t *pos) { long t = ftell(f); if (t < 0) return -1; *pos = t; return 0; }
int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, 0); }

int feof(FILE *f)    { return f && (f->flags & F_EOF); }
int ferror(FILE *f)  { return f && (f->flags & F_ERR); }
int fileno(FILE *f)  { return f ? f->fd : -1; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (!f) return -1;
    fflush(f);
    if (buf) {
        if (f->owns_buf && f->buf) free(f->buf);
        f->buf = buf;
        f->bufsize = size;
        f->owns_buf = 0;
    } else if (size != f->bufsize && size > 0) {
        char *nb = (char *)malloc(size);
        if (!nb) return -1;
        if (f->owns_buf && f->buf) free(f->buf);
        f->buf = nb;
        f->bufsize = size;
        f->owns_buf = 1;
    }
    f->bufmode = mode;
    f->bufpos = f->buflen = 0;
    return 0;
}

/* Stubs for features we don't support. */
FILE *tmpfile(void) { return 0; }
char *tmpnam(char *buf) { (void)buf; return 0; }
FILE *freopen(const char *path, const char *mode, FILE *f) { (void)path; (void)mode; (void)f; return 0; }

/* popen: fork + pipe + execve. "r" mode: parent reads child's stdout.
   "w" mode: parent writes to child's stdin. No shell quoting —
   tokenized the same way as libc system(). The child's pid is
   stashed in a small side table so pclose can reap and return the
   exit status. */
#define POPEN_SLOTS 8
static struct { FILE *f; long pid; } popen_tab[POPEN_SLOTS];

static void popen_remember(FILE *f, long pid) {
    for (int i = 0; i < POPEN_SLOTS; i++) {
        if (popen_tab[i].f == 0) { popen_tab[i].f = f; popen_tab[i].pid = pid; return; }
    }
}
static long popen_forget(FILE *f) {
    for (int i = 0; i < POPEN_SLOTS; i++) {
        if (popen_tab[i].f == f) {
            long p = popen_tab[i].pid;
            popen_tab[i].f = 0; popen_tab[i].pid = 0;
            return p;
        }
    }
    return -1;
}

FILE *popen(const char *cmd, const char *mode) {
    if (!cmd || !mode || !*mode) return 0;
    int read_mode = (mode[0] == 'r');
    if (!read_mode && mode[0] != 'w') return 0;

    /* Tokenize cmd into argv. */
    #define POPEN_ARGC 16
    static char buf[256];
    static char *argv[POPEN_ARGC + 1];
    size_t n = 0;
    while (cmd[n] && n < sizeof(buf) - 1) { buf[n] = cmd[n]; n++; }
    buf[n] = 0;
    int argc = 0; size_t i = 0;
    while (i < n && argc < POPEN_ARGC) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        if (i >= n) break;
        argv[argc++] = &buf[i];
        while (i < n && buf[i] != ' ' && buf[i] != '\t') i++;
        if (i < n) { buf[i++] = 0; }
    }
    argv[argc] = 0;
    if (argc == 0) return 0;

    char path[128];
    if (argv[0][0] == '/') {
        size_t l = 0; while (argv[0][l] && l < sizeof(path) - 1) { path[l] = argv[0][l]; l++; }
        path[l] = 0;
    } else {
        const char *bin = "/bin/"; size_t l = 0;
        while (bin[l]) { path[l] = bin[l]; l++; }
        size_t k = 0;
        while (argv[0][k] && l < sizeof(path) - 1) { path[l++] = argv[0][k++]; }
        path[l] = 0;
    }

    int pipefd[2];
    if (sys_pipe(pipefd) < 0) return 0;

    long pid = sys_fork();
    if (pid < 0) {
        sys_close(pipefd[0]); sys_close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        if (read_mode) {
            sys_close(pipefd[0]); sys_dup2(pipefd[1], 1); sys_close(pipefd[1]);
        } else {
            sys_close(pipefd[1]); sys_dup2(pipefd[0], 0); sys_close(pipefd[0]);
        }
        sys_execve(path, argv, 0);
        sys_exit(127);
    }

    int parent_fd, child_fd;
    if (read_mode) { parent_fd = pipefd[0]; child_fd = pipefd[1]; }
    else           { parent_fd = pipefd[1]; child_fd = pipefd[0]; }
    sys_close(child_fd);

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(parent_fd); return 0; }
    f->fd = parent_fd;
    f->mode = read_mode ? 0 : 1;
    f->bufmode = _IOFBF;
    f->buf = (char *)malloc(BUFSIZ);
    f->bufsize = BUFSIZ;
    f->bufpos = f->buflen = 0;
    f->flags = 0; f->ungot_ch = 0; f->owns_buf = 1;
    if (!f->buf) { free(f); sys_close(parent_fd); return 0; }
    popen_remember(f, pid);
    return f;
}

int pclose(FILE *f) {
    if (!f) return -1;
    long pid = popen_forget(f);
    fflush(f);
    sys_close(f->fd);
    if (f->owns_buf && f->buf) free(f->buf);
    free(f);
    if (pid <= 0) return -1;
    int status = 0;
    sys_waitpid((int)pid, &status);
    return status;
}
int   remove(const char *path) { return sys_unlink(path); }
int   rename(const char *from, const char *to) { (void)from; (void)to; return -1; }

/* Formatted output. */
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char scratch[512];
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    if (n < 0) return n;
    size_t to_write = (size_t)n;
    if (to_write > sizeof(scratch) - 1) to_write = sizeof(scratch) - 1;
    fwrite(scratch, 1, to_write, f);
    return n;
}

int printf(const char *fmt, ...) {
    char scratch[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(scratch, 1, (size_t)n, stdout);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

/* NOTE: ulib.c already defines printf(). Our vfprintf/fprintf above give
   Lua what it needs without shadowing it. */
