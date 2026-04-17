#include "syscall.h"
#include "ulib.h"

static int cat_fd(int fd) {
    char buf[512];
    int32_t n;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        sys_write(1, buf, n);
    }
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return cat_fd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { printf("cat: %s: not found\n", argv[i]); rc = 1; continue; }
        if (cat_fd(fd) != 0) rc = 1;
        sys_close(fd);
    }
    return rc;
}
