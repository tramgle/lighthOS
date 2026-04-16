/* assert <name> <expected> <file>: read file, strip trailing
   whitespace/newlines, compare to <expected>. Print PASS/FAIL with
   name label. Exit 0 on pass, 1 on fail. */

#include "syscall.h"
#include "ulib.h"

#define ASSERT_MAX 4096

int main(int argc, char **argv) {
    if (argc != 4) {
        puts("usage: assert <name> <expected> <file>\n");
        return 1;
    }
    const char *name = argv[1];
    const char *expected = argv[2];
    const char *path = argv[3];

    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        printf("FAIL %s: cannot open %s\n", name, path);
        return 1;
    }

    char buf[ASSERT_MAX];
    int len = 0;
    int32_t n;
    while (len < (int)sizeof buf - 1 &&
           (n = sys_read(fd, buf + len, sizeof buf - 1 - len)) > 0) {
        len += n;
    }
    sys_close(fd);
    buf[len] = '\0';

    /* Trim trailing whitespace/newlines. */
    while (len > 0) {
        char c = buf[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { len--; continue; }
        break;
    }
    buf[len] = '\0';

    if (strcmp(buf, expected) == 0) {
        printf("PASS %s\n", name);
        return 0;
    }
    printf("FAIL %s: expected '%s', got '%s'\n", name, expected, buf);
    return 1;
}
