#include "syscall.h"
#include "ulib.h"

static int copy_one(const char *src, const char *dst) {
    int in = sys_open(src, O_RDONLY);
    if (in < 0) { printf("cp: %s: not found\n", src); return 1; }

    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        printf("cp: cannot open %s for writing\n", dst);
        sys_close(in);
        return 1;
    }

    char buf[4096];
    int32_t n;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        int32_t w = sys_write(out, buf, n);
        if (w != n) {
            printf("cp: short write on %s\n", dst);
            sys_close(in); sys_close(out);
            return 1;
        }
    }
    sys_close(in);
    sys_close(out);
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { puts("usage: cp <src> <dst>\n"); return 1; }
    return copy_one(argv[1], argv[2]);
}
