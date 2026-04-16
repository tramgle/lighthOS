/* head [-n N] [file]: print the first N lines (default 10). No file
   argument reads stdin, so `cmd | head` works. */

#include "syscall.h"
#include "ulib.h"

static int parse_int(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

static void head_fd(int fd, int max_lines) {
    char buf[1024];
    int32_t n;
    int lines = 0;
    while (lines < max_lines && (n = sys_read(fd, buf, sizeof buf)) > 0) {
        int start = 0;
        for (int i = 0; i < n && lines < max_lines; i++) {
            if (buf[i] == '\n') {
                sys_write(1, buf + start, i - start + 1);
                start = i + 1;
                lines++;
            }
        }
        if (lines < max_lines && start < n) {
            sys_write(1, buf + start, n - start);
        }
    }
}

int main(int argc, char **argv) {
    int max_lines = 10;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
        max_lines = parse_int(argv[i + 1]);
        i += 2;
    }

    if (i >= argc) {
        head_fd(0, max_lines);
    } else {
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { printf("head: %s: not found\n", argv[i]); continue; }
            head_fd(fd, max_lines);
            sys_close(fd);
        }
    }
    return 0;
}
