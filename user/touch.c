/* touch <file>...: create-if-missing. We don't track mtimes (no
   timekeeping on filesystems yet), so there's nothing to do for files
   that already exist. */

#include "syscall.h"
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: touch <file>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct vfs_stat st;
        if (sys_stat(argv[i], &st) == 0) continue;
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: cannot create %s\n", argv[i]);
            rc = 1;
        } else {
            sys_close(fd);
        }
    }
    return rc;
}
