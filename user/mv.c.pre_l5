/* mv <src> <dst>: copy then unlink. No vfs_rename yet — even for the
   same filesystem this goes through the full read/write path. Fine for
   the sizes we deal with; once a rename syscall lands we'll shortcut. */

#include "syscall.h"
#include "ulib.h"

static int copy_file(const char *src, const char *dst) {
    int in = sys_open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { sys_close(in); return -1; }

    char buf[4096];
    int32_t n;
    int rc = 0;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        if (sys_write(out, buf, n) != n) { rc = -1; break; }
    }
    if (n < 0) rc = -1;
    sys_close(in);
    sys_close(out);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 3) { puts("usage: mv <src> <dst>\n"); return 1; }
    if (copy_file(argv[1], argv[2]) != 0) {
        printf("mv: %s -> %s failed\n", argv[1], argv[2]);
        return 1;
    }
    if (sys_unlink(argv[1]) != 0) {
        printf("mv: warning — %s removed copy but couldn't unlink source\n", argv[2]);
        return 1;
    }
    return 0;
}
