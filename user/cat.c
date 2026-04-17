/* cat [file...] — write files (or stdin) to stdout. */
#include "ulib_x64.h"

static int cat_fd(int fd) {
    char buf[4096];
    for (;;) {
        long n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) return n < 0 ? 1 : 0;
        long off = 0;
        while (off < n) {
            long w = sys_write(1, buf + off, n - off);
            if (w <= 0) return 1;
            off += w;
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc <= 1) return cat_fd(0);
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            u_puts_n("cat: ");
            u_puts_n(argv[i]);
            u_puts_n(": cannot open\n");
            return 1;
        }
        if (cat_fd(fd) != 0) { sys_close(fd); return 1; }
        sys_close(fd);
    }
    return 0;
}
