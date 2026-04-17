/* ls [path] — list a directory. Plain listing, one entry per line. */
#include "ulib_x64.h"

struct readdir_out {
    char name[64];
    uint32_t type;
};

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *path = (argc > 1) ? argv[1] : "/";
    struct vfs_stat st;
    if (sys_stat(path, &st) != 0) {
        u_puts_n("ls: "); u_puts_n(path); u_puts_n(": not found\n");
        return 1;
    }
    if (st.type != VFS_DIR) {
        u_puts_n(path); u_putc('\n');
        return 0;
    }
    struct readdir_out e;
    uint32_t i = 0;
    while (_syscall3(SYS_READDIR, (long)(uintptr_t)path, i, (long)(uintptr_t)&e) == 0) {
        u_puts_n(e.name);
        u_putc('\n');
        i++;
    }
    return 0;
}
