/* touch <file>... — create each file if missing. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) return 1;
        sys_close(fd);
    }
    return 0;
}
