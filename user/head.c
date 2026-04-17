/* head [-n N] [file] — first N lines (default 10). */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int n = 10;
    int arg = 1;
    if (arg < argc && u_strcmp(argv[arg], "-n") == 0 && arg + 1 < argc) {
        n = u_atoi(argv[arg + 1]);
        arg += 2;
    }
    int fd = (arg < argc) ? sys_open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) return 1;
    int printed = 0;
    char c;
    while (printed < n && sys_read(fd, &c, 1) == 1) {
        sys_write(1, &c, 1);
        if (c == '\n') printed++;
    }
    if (fd > 0) sys_close(fd);
    return 0;
}
