/* wc [-l|-w|-c] [file] — count lines / words / bytes.
 * Default: all three, line count only in our minimal build's single
 * argument form so the test harness can do `wc -w` / `wc -l` / `wc -c`. */
#include "ulib_x64.h"

static int count_fd(int fd, int mode, long *lines, long *words, long *bytes) {
    (void)mode;
    char buf[4096];
    int in_word = 0;
    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n == 0) break;
        *bytes += n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') (*lines)++;
            if (c == ' ' || c == '\t' || c == '\n') { in_word = 0; }
            else if (!in_word)                      { in_word = 1; (*words)++; }
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int show_l = 0, show_w = 0, show_c = 0;
    int arg = 1;
    for (; arg < argc; arg++) {
        if (argv[arg][0] != '-') break;
        const char *flag = argv[arg] + 1;
        while (*flag) {
            switch (*flag) {
            case 'l': show_l = 1; break;
            case 'w': show_w = 1; break;
            case 'c': show_c = 1; break;
            }
            flag++;
        }
    }
    if (!show_l && !show_w && !show_c) { show_l = show_w = show_c = 1; }

    long lines = 0, words = 0, bytes = 0;
    int fd = (arg < argc) ? sys_open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) return 1;
    count_fd(fd, 0, &lines, &words, &bytes);
    if (fd > 0) sys_close(fd);

    int printed = 0;
    if (show_l) { if (printed++) u_putc(' '); u_putdec(lines); }
    if (show_w) { if (printed++) u_putc(' '); u_putdec(words); }
    if (show_c) { if (printed++) u_putc(' '); u_putdec(bytes); }
    u_putc('\n');
    return 0;
}
