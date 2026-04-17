/* wc [-l|-w|-c] [file...] — count lines / words / bytes.
 * Default (no flag): print all three. With multiple files, prints
 * one summary line per file plus a trailing "total" line. */
#include "ulib_x64.h"

static void count_fd(int fd, long *lines, long *words, long *bytes) {
    char buf[4096];
    int in_word = 0;
    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        *bytes += n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') (*lines)++;
            if (c == ' ' || c == '\t' || c == '\n') { in_word = 0; }
            else if (!in_word)                      { in_word = 1; (*words)++; }
        }
    }
}

static void print_counts(long lines, long words, long bytes,
                         int show_l, int show_w, int show_c,
                         const char *label) {
    int printed = 0;
    if (show_l) { if (printed++) u_putc(' '); u_putdec(lines); }
    if (show_w) { if (printed++) u_putc(' '); u_putdec(words); }
    if (show_c) { if (printed++) u_putc(' '); u_putdec(bytes); }
    if (label)  { u_putc(' '); u_puts_n(label); } else u_putc('\n');
    if (label)  u_putc('\n');
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

    int file_count = argc - arg;
    long tl = 0, tw = 0, tb = 0;

    if (file_count == 0) {
        long lines = 0, words = 0, bytes = 0;
        count_fd(0, &lines, &words, &bytes);
        print_counts(lines, words, bytes, show_l, show_w, show_c, 0);
        return 0;
    }

    for (int i = 0; i < file_count; i++) {
        const char *path = argv[arg + i];
        int fd = sys_open(path, O_RDONLY);
        if (fd < 0) continue;
        long lines = 0, words = 0, bytes = 0;
        count_fd(fd, &lines, &words, &bytes);
        sys_close(fd);
        print_counts(lines, words, bytes, show_l, show_w, show_c,
                     file_count > 1 ? path : 0);
        if (file_count == 1) {} /* single-file: no label, newline in print_counts */
        tl += lines; tw += words; tb += bytes;
    }
    if (file_count > 1)
        print_counts(tl, tw, tb, show_l, show_w, show_c, "total");
    return 0;
}
