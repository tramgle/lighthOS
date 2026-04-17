/* grep <pattern> [file...]: literal substring match, line-oriented.
   Reads stdin when no file argument. Flags: -n prefix with line
   number, -v invert (print non-matching), -i case-insensitive. No
   regex — if we need it later, drop in a minimal NFA. */

#include "syscall.h"
#include "ulib.h"

static int icase;

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int match_at(const char *hay, const char *needle) {
    while (*needle) {
        char a = icase ? lower(*hay) : *hay;
        char b = icase ? lower(*needle) : *needle;
        if (!*hay || a != b) return 0;
        hay++; needle++;
    }
    return 1;
}

static int contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (const char *h = hay; *h; h++) {
        if (match_at(h, needle)) return 1;
    }
    return 0;
}

static void grep_fd(int fd, const char *pat, const char *label,
                    int show_n, int invert, int show_label) {
    char buf[2048];
    char line[1024];
    int line_len = 0;
    int line_no = 0;
    int32_t n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[line_len] = '\0';
                line_no++;
                int hit = contains(line, pat);
                if (invert) hit = !hit;
                if (hit) {
                    if (show_label) printf("%s:", label);
                    if (show_n) printf("%u:", (unsigned)line_no);
                    sys_write(1, line, line_len);
                    sys_write(1, "\n", 1);
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len > 0) {
        line[line_len] = '\0';
        line_no++;
        int hit = contains(line, pat);
        if (invert) hit = !hit;
        if (hit) {
            if (show_label) printf("%s:", label);
            if (show_n) printf("%u:", (unsigned)line_no);
            sys_write(1, line, line_len);
            sys_write(1, "\n", 1);
        }
    }
}

int main(int argc, char **argv) {
    int show_n = 0, invert = 0;
    icase = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'n') show_n = 1;
            else if (*f == 'v') invert = 1;
            else if (*f == 'i') icase = 1;
            else { printf("grep: unknown flag -%c\n", *f); return 2; }
        }
    }
    if (i >= argc) { puts("usage: grep [-nvi] <pattern> [file...]\n"); return 2; }

    const char *pat = argv[i++];
    int file_count = argc - i;

    if (file_count == 0) {
        grep_fd(0, pat, 0, show_n, invert, 0);
    } else {
        int multi = (file_count > 1);
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { printf("grep: %s: not found\n", argv[i]); continue; }
            grep_fd(fd, pat, argv[i], show_n, invert, multi);
            sys_close(fd);
        }
    }
    return 0;
}
