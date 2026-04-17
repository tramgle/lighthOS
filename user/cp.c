/* cp <src> <dst> — copy a file verbatim. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 3) return 2;
    int in  = sys_open(argv[1], O_RDONLY);
    if (in < 0) return 1;
    int out = sys_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { sys_close(in); return 1; }
    char buf[4096];
    for (;;) {
        long n = sys_read(in, buf, sizeof(buf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            long w = sys_write(out, buf + off, n - off);
            if (w <= 0) { sys_close(in); sys_close(out); return 1; }
            off += w;
        }
    }
    sys_close(in); sys_close(out);
    return 0;
}
