/* grep [-v] [-i] [-n] <pattern> [file...] — literal substring match.
   When more than one file is given, matching lines are prefixed with
   "<path>:" so output from multiple files can be told apart. */
#include "ulib_x64.h"

static char lowc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int contains(const char *hay, const char *needle, int icase) {
    size_t nlen = u_strlen(needle);
    if (nlen == 0) return 1;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = hay[i + j], b = needle[j];
            if (!a) return 0;
            if (icase) { a = lowc(a); b = lowc(b); }
            if (a != b) break;
            j++;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int inv = 0, icase = 0, numbered = 0;
    int arg = 1;
    while (arg < argc && argv[arg][0] == '-' && argv[arg][1] != 0) {
        const char *f = argv[arg] + 1;
        while (*f) {
            switch (*f) {
            case 'v': inv = 1; break;
            case 'i': icase = 1; break;
            case 'n': numbered = 1; break;
            }
            f++;
        }
        arg++;
    }
    if (arg >= argc) return 2;
    const char *pat = argv[arg++];
    int file_count = argc - arg;
    int multi = (file_count > 1);

    int round = 0;
    do {
        int fd;
        const char *label = 0;
        if (file_count == 0) {
            fd = 0;
        } else {
            label = argv[arg + round];
            fd = sys_open(label, O_RDONLY);
            if (fd < 0) { round++; continue; }
        }

        char line[1024];
        int lineno = 0;
        for (;;) {
            long n = u_readline(fd, line, sizeof(line));
            if (n <= 0) break;
            lineno++;
            char saved = 0;
            if (line[n-1] == '\n') { saved = '\n'; line[n-1] = 0; n--; }
            int hit = contains(line, pat, icase);
            if (inv) hit = !hit;
            if (hit) {
                if (multi && label) { u_puts_n(label); u_putc(':'); }
                if (numbered) { u_putdec(lineno); u_putc(':'); }
                sys_write(1, line, n);
                if (saved) u_putc('\n');
            }
            if (saved) { line[n] = '\n'; }
        }
        if (fd > 0) sys_close(fd);
        round++;
    } while (round < file_count);

    return 0;
}
